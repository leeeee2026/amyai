#include "reply_player.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "diag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "tts_client.h"

namespace {

constexpr const char* kTag = "reply";

// 拿到第一个完整句子后，先等这么久攒一点后续内容（抖动缓冲）
constexpr int kStartGraceMs = 250;

// 单句最长字符数：超过就在逗号处切开，避免一句话太长迟迟不播
constexpr size_t kMaxSentenceChars = 22;

QueueHandle_t s_queue = nullptr;        // 待播句子（char*）
SemaphoreHandle_t s_lock = nullptr;     // 保护下面的状态
SemaphoreHandle_t s_done = nullptr;     // 本轮播完
bool s_done_flag = false;               // 本轮是否已经播完（让 wait_done 幂等）

std::string s_text;                     // 还没切出完整句子的残留文本
std::vector<std::string> s_pending;     // 已切好、还没送进队列的句子
bool s_started = false;                 // 是否已经开始播（抖动缓冲结束）
bool s_finished = false;                // 流是否已结束
int  s_spoken = 0;                      // 已送进队列的句子数

bool is_sentence_end(char c)
{
    return c == '\xe3' ? false :  // 中文标点是多字节，单独处理，见 split_sentences
           (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n');
}

// 按中文/英文标点切句。中文标点（。！？；）是 3 字节 UTF-8，逐字节比对。
void split_sentences()
{
    size_t i = 0;
    while (i < s_text.size()) {
        size_t cut = std::string::npos;
        size_t cut_len = 0;

        for (size_t k = i; k < s_text.size(); ++k) {
            const unsigned char c = static_cast<unsigned char>(s_text[k]);
            // 。 = E3 80 82 / ！ = EF BC 81 / ？ = EF BC 9F / ； = EF BC 9B
            if (c == 0xE3 && k + 2 < s_text.size() &&
                static_cast<unsigned char>(s_text[k + 1]) == 0x80 &&
                static_cast<unsigned char>(s_text[k + 2]) == 0x82) {
                cut = k + 3;
                cut_len = 3;
                break;
            }
            if (c == 0xEF && k + 2 < s_text.size()) {
                const unsigned char c1 = static_cast<unsigned char>(s_text[k + 1]);
                const unsigned char c2 = static_cast<unsigned char>(s_text[k + 2]);
                if (c1 == 0xBC && (c2 == 0x81 || c2 == 0x9F || c2 == 0x9B)) {
                    cut = k + 3;
                    cut_len = 3;
                    break;
                }
            }
            if (is_sentence_end(static_cast<char>(c))) {
                cut = k + 1;
                cut_len = 1;
                break;
            }
            // 太长就在逗号处切开：， = EF BC 8C / , = 0x2C
            if (k - i >= kMaxSentenceChars) {
                if (c == 0xEF && k + 2 < s_text.size() &&
                    static_cast<unsigned char>(s_text[k + 1]) == 0xBC &&
                    static_cast<unsigned char>(s_text[k + 2]) == 0x8C) {
                    cut = k + 3;
                    cut_len = 3;
                    break;
                }
                if (c == ',') {
                    cut = k + 1;
                    cut_len = 1;
                    break;
                }
            }
        }
        if (cut == std::string::npos) {
            break;   // 没有完整句子，等下一批
        }
        (void)cut_len;
        std::string sent = s_text.substr(i, cut - i);
        s_text.erase(i, cut - i);
        // 去掉纯标点/空白
        bool has_content = false;
        for (char c : sent) {
            if (static_cast<unsigned char>(c) > 0x20) {
                has_content = true;
                break;
            }
        }
        if (has_content) {
            s_pending.push_back(sent);
        }
        // erase 后 i 不变，继续从当前位置切
    }
}

void push_to_queue(const std::string& text)
{
    if (text.empty()) {
        return;
    }
    char* copy = static_cast<char*>(malloc(text.size() + 1));
    if (copy == nullptr) {
        return;
    }
    std::memcpy(copy, text.c_str(), text.size() + 1);
    if (xQueueSend(s_queue, &copy, pdMS_TO_TICKS(3000)) != pdTRUE) {
        free(copy);
        ESP_LOGW(kTag, "句子队列满，丢弃：%s", text.c_str());
        return;
    }
    ++s_spoken;
}

// 把 pending 里的句子全部送进队列（调用方需持锁）
void flush_pending_locked()
{
    for (auto& s : s_pending) {
        push_to_queue(s);
    }
    s_pending.clear();
}

void consumer_task(void*)
{
    while (true) {
        // 还没开始播、但已经有句子在等：先等一小段（抖动缓冲）再开播
        if (!s_started) {
            bool has_pending = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            has_pending = !s_pending.empty();
            xSemaphoreGive(s_lock);
            if (has_pending) {
                vTaskDelay(pdMS_TO_TICKS(kStartGraceMs));
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (!s_started) {
                    s_started = true;
                    flush_pending_locked();
                }
                xSemaphoreGive(s_lock);
            }
        }

        char* item = nullptr;
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(30)) != pdTRUE) {
            continue;
        }
        if (item == nullptr) {
            // 结束标记：本轮播完
            s_done_flag = true;
            xSemaphoreGive(s_done);
            continue;
        }
        tts_speak(item);
        free(item);
    }
}

}  // namespace

bool reply_player_init()
{
    if (s_queue != nullptr) {
        return true;
    }
    s_queue = xQueueCreate(12, sizeof(char*));
    s_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (s_queue == nullptr || s_lock == nullptr || s_done == nullptr) {
        ESP_LOGE(kTag, "流式播放器初始化失败（内存不足）");
        return false;
    }
    // ★ 任务栈放 PSRAM：这个任务要跑 TTS 的 TLS 握手，实测需要 20KB
    //   （12KB 会在 mbedtls_ssl_handshake 里栈溢出 panic）。
    //   放 PSRAM 是为了不挤爆内部 RAM（内部堆崩了会导致 lwip 分配失败、DNS 崩溃）。
    if (xTaskCreateWithCaps(consumer_task, "reply_play", 20 * 1024, nullptr, 4,
                            nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(kTag, "播放任务创建失败");
        return false;
    }
    return true;
}

void reply_player_begin()
{
    if (s_lock == nullptr || s_queue == nullptr) {
        return;   // 初始化失败：直接不播，不影响主流程
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_text.clear();
    s_pending.clear();
    s_started = false;
    s_finished = false;
    s_spoken = 0;
    // 清掉上一轮可能残留的队列项
    char* item = nullptr;
    while (xQueueReceive(s_queue, &item, 0) == pdTRUE) {
        if (item != nullptr) {
            free(item);
        }
    }
    xSemaphoreGive(s_lock);
    s_done_flag = false;
    xSemaphoreTake(s_done, 0);   // 清掉旧的完成信号
}

void reply_player_feed(const std::string& delta)
{
    if (delta.empty() || s_queue == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_text += delta;
    split_sentences();
    // 已经开始播了就直接续上；没开始就交给消费任务做抖动缓冲
    if (s_started) {
        flush_pending_locked();
    }
    xSemaphoreGive(s_lock);
}

void reply_player_finish()
{
    if (s_queue == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // 剩余不成句的文本当最后一句
    bool has_content = false;
    for (char c : s_text) {
        if (static_cast<unsigned char>(c) > 0x20) {
            has_content = true;
            break;
        }
    }
    if (has_content) {
        s_pending.push_back(s_text);
    }
    s_text.clear();
    s_started = true;
    flush_pending_locked();
    s_finished = true;
    xSemaphoreGive(s_lock);

    // 结束标记
    char* sentinel = nullptr;
    xQueueSend(s_queue, &sentinel, pdMS_TO_TICKS(3000));
}

bool reply_player_wait_done(int timeout_ms)
{
    if (s_done == nullptr) {
        return true;
    }
    // 幂等：本轮已经播完的话直接返回。
    // 否则第二次调用会等满整个超时（音乐分支和回调结尾各调一次，实测卡 20 秒）。
    if (s_done_flag) {
        return true;
    }
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        s_done_flag = true;
        return true;
    }
    return false;
}

bool reply_player_spoke_anything()
{
    return s_queue != nullptr && s_spoken > 0;
}
