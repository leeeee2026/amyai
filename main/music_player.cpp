#include "music_player.h"

#include <atomic>
#include <cstring>
#include <string>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "audio_hub.h"

namespace {

constexpr const char* kTag = "music";

// 关键词 -> 文件名（先硬编码，正式期改成从服务器拉 JSON 清单）
struct Track {
    const char* keyword;
    const char* file;
    const char* name;
};

const Track kTracks[] = {
    // 用户提供的真实音乐（Pixabay 免费商用）
    {"轻音乐", "bg_soft.wav", "轻音乐"},
    {"音乐", "bg_soft.wav", "轻音乐"},
    {"歌", "bg_soft.wav", "轻音乐"},
    {"背景", "bg_soft.wav", "背景音乐"},
    {"春天", "bg_light.wav", "春日轻快"},
    {"轻快", "bg_light.wav", "春日轻快"},
    {"动感", "bg_up.wav", "动感节奏"},
    {"节奏", "bg_up.wav", "动感节奏"},
    // 合成音效（占位，等真实素材）
    {"雨", "rain_16k.wav", "雨声"},
    {"海浪", "ocean_16k.wav", "海浪"},
    {"海", "ocean_16k.wav", "海浪"},
    {"森林", "forest_16k.wav", "森林"},
    {"鸟", "forest_16k.wav", "森林鸟鸣"},
    {"助眠", "sleep_16k.wav", "助眠"},
    {"睡", "sleep_16k.wav", "助眠"},
    {"放松", "sleep_16k.wav", "放松"},
    {"白噪", "rain_16k.wav", "白噪音"},
    {"噪音", "rain_16k.wav", "白噪音"},
    {"小星星", "music_16k.wav", "小星星"},
    {"钢琴", "music_16k.wav", "轻音乐"},
    // 模型有时会把查询翻译成英文，补一组
    {"song", "bg_soft.wav", "轻音乐"},
    {"music", "bg_soft.wav", "轻音乐"},
    {"piano", "bg_soft.wav", "轻音乐"},
    {"rain", "rain_16k.wav", "雨声"},
    {"ocean", "ocean_16k.wav", "海浪"},
    {"wave", "ocean_16k.wav", "海浪"},
    {"forest", "forest_16k.wav", "森林"},
    {"bird", "forest_16k.wav", "森林鸟鸣"},
    {"sleep", "sleep_16k.wav", "助眠"},
    {"relax", "sleep_16k.wav", "放松"},
    {"white noise", "rain_16k.wav", "白噪音"},
};

const char* const kDefaultFile = "bg_soft.wav";
const char* const kDefaultName = "轻音乐";

// 轮播顺序（"换一首/下一首" 时按这个顺序切）
struct RotEntry { const char* file; const char* name; };
const RotEntry kRotation[] = {
    {"bg_soft.wav",   "轻音乐"},
    {"bg_light.wav",  "春日轻快"},
    {"bg_up.wav",     "动感节奏"},
    {"rain_16k.wav",  "雨声"},
    {"ocean_16k.wav", "海浪"},
    {"forest_16k.wav","森林"},
    {"sleep_16k.wav", "助眠"},
};
int s_rot_idx = -1;

// ---------------------------------------------------------------- 环形缓冲
// 网络任务只负责往这里灌数据（可能卡顿），播放任务只负责匀速往外取。
// 中间有 6 秒缓冲，网络抖动就吸收掉了。
constexpr size_t kRingBytes = 64 * 1024;    // 2 秒 @16kHz 16bit（6 秒太占内存）

struct Ring {
    uint8_t* buf = nullptr;
    size_t cap = 0;
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
    SemaphoreHandle_t lock = nullptr;
    // 注：不加额外信号量，播放任务用轮询 + 缓冲水位控制
    SemaphoreHandle_t has_space = nullptr;  // 有新数据写入（唤醒播放任务）
    bool eof = false;
};

Ring s_ring;

// 每个曲目独立保存的参数（供播放任务使用）
struct Playing {
    int rate = 16000;
    int channels = 1;
    bool rate_known = false;
};

std::atomic<bool> s_playing{false};
std::atomic<bool> s_stop{false};
const char* s_cur_name = "";
std::string s_url;
Playing s_fmt;

size_t ring_free()
{
    return s_ring.cap - s_ring.count;
}

void ring_push(const uint8_t* data, size_t len)
{
    if (s_ring.lock == nullptr || len == 0) {
        return;
    }
    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    size_t room = s_ring.cap - s_ring.count;
    if (len > room) {
        len = room;
    }
    const size_t first = (len < s_ring.cap - s_ring.tail) ? len : (s_ring.cap - s_ring.tail);
    std::memcpy(s_ring.buf + s_ring.tail, data, first);
    if (len > first) {
        std::memcpy(s_ring.buf, data + first, len - first);
    }
    s_ring.tail = (s_ring.tail + len) % s_ring.cap;
    s_ring.count += len;
    xSemaphoreGive(s_ring.lock);
}

size_t ring_pop(uint8_t* dst, size_t len)
{
    if (s_ring.lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    if (len > s_ring.count) {
        len = s_ring.count;
    }
    const size_t first = (len < s_ring.cap - s_ring.head) ? len : (s_ring.cap - s_ring.head);
    std::memcpy(dst, s_ring.buf + s_ring.head, first);
    if (len > first) {
        std::memcpy(dst + first, s_ring.buf, len - first);
    }
    s_ring.head = (s_ring.head + len) % s_ring.cap;
    s_ring.count -= len;
    xSemaphoreGive(s_ring.lock);
    return len;
}

// ---------------------------------------------------------------- WAV 头解析
struct WavInfo {
    int rate = 16000;
    int channels = 1;
    size_t data_off = 44;
};

bool parse_wav(const uint8_t* p, size_t n, WavInfo* out)
{
    if (n < 44 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
        return false;
    }
    size_t pos = 12;
    bool fmt_ok = false;
    while (pos + 8 <= n) {
        const char* id = reinterpret_cast<const char*>(p + pos);
        const uint32_t sz = static_cast<uint32_t>(p[pos + 4]) | (static_cast<uint32_t>(p[pos + 5]) << 8) |
                            (static_cast<uint32_t>(p[pos + 6]) << 16) | (static_cast<uint32_t>(p[pos + 7]) << 24);
        const size_t body = pos + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= n) {
            out->channels = static_cast<int>(p[body + 2] | (p[body + 3] << 8));
            out->rate = static_cast<int>(p[body + 4] | (p[body + 5] << 8) | (p[body + 6] << 16) |
                                         (p[body + 7] << 24));
            fmt_ok = true;
        } else if (std::memcmp(id, "data", 4) == 0 && fmt_ok) {
            out->data_off = body;
            return true;
        }
        pos = body + sz + (sz & 1);
    }
    return false;
}

// ---------------------------------------------------------------- 读取任务
// 只做一件事：从 HTTP 拉数据塞进环形缓冲，拉完就重连循环。
void reader_task(void*)
{
    static uint8_t buf[4096];
    ESP_LOGI(kTag, "读取任务启动：%s", s_url.c_str());

    while (!s_stop) {
        const std::string url = s_url;
        esp_http_client_config_t cfg = {};
        cfg.url = url.c_str();
        cfg.timeout_ms = 15000;
        cfg.buffer_size = 4096;
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (cli == nullptr) {
            break;
        }
        esp_http_client_set_header(cli, "Connection", "close");

        if (esp_http_client_open(cli, 0) != ESP_OK) {
            ESP_LOGE(kTag, "连接失败，1 秒后重试");
            esp_http_client_cleanup(cli);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        esp_http_client_fetch_headers(cli);
        ESP_LOGI(kTag, "连接成功，开始灌数据");

        // 读头部
        int got = esp_http_client_read(cli, reinterpret_cast<char*>(buf), sizeof(buf));
        WavInfo wi;
        if (got <= 0 || !parse_wav(buf, static_cast<size_t>(got), &wi)) {
            ESP_LOGE(kTag, "不是合法 WAV（%d 字节）", got);
            esp_http_client_close(cli);
            esp_http_client_cleanup(cli);
            break;
        }
        s_fmt.rate = wi.rate;
        s_fmt.channels = wi.channels;
        s_fmt.rate_known = true;
        ESP_LOGI(kTag, "WAV: %d Hz / %d ch", wi.rate, wi.channels);

        // 头部剩余的 PCM 也灌进去
        if (static_cast<size_t>(got) > wi.data_off + 1) {
            ring_push(buf + wi.data_off, static_cast<size_t>(got) - wi.data_off);
        }

        while (!s_stop) {
            const int n = esp_http_client_read(cli, reinterpret_cast<char*>(buf), sizeof(buf));
            if (n <= 0) {
                break;                      // 文件播完，重连循环
            }
            // 缓冲满就等播放任务消耗一点
            int guard = 0;
            while (ring_free() < static_cast<size_t>(n) && !s_stop && ++guard < 300) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            ring_push(buf, static_cast<size_t>(n));
        }

        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        ESP_LOGI(kTag, "本轮读完，重新连接以循环播放");
    }

    s_ring.eof = true;
    ESP_LOGI(kTag, "读取任务退出");
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------- 播放任务
// 从环形缓冲取 PCM，匀速喂 I2S。缓冲攒够 1 秒才开播，避免开头断音。
void player_task(void*)
{
    static int16_t pcm[2048];
    constexpr size_t kPrebufferBytes = 8000;   // 约 250ms（16kHz 16bit），够起播即可

    ESP_LOGI(kTag, "播放任务启动，等待预缓冲…");
    int wait_ms = 0;
    while (!s_stop && s_ring.count < kPrebufferBytes && s_ring.eof == false && wait_ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        wait_ms += 20;
    }
    if (s_stop || s_ring.count < 2) {
        ESP_LOGE(kTag, "预缓冲失败（%d ms 内没拿到音频），放弃播放", wait_ms);
        s_playing = false;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kTag, "预缓冲 %u 字节，开始播放", (unsigned)s_ring.count);

    if (!audio_hub_stream_begin(AUDIO_PRIO_MUSIC)) {
        ESP_LOGE(kTag, "占用音频通道失败");
        s_playing = false;
        vTaskDelete(nullptr);
        return;
    }

    while (!s_stop) {
        const size_t n = ring_pop(reinterpret_cast<uint8_t*>(pcm), sizeof(pcm));
        if (n < 2) {
            if (s_ring.eof && s_ring.count == 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const int rate = s_fmt.rate;
        const int ch = s_fmt.channels;
        if (ch > 1) {
            const size_t frames = (n / 2) / static_cast<size_t>(ch);
            for (size_t i = 0; i < frames; ++i) {
                pcm[i] = pcm[i * ch];
            }
            if (!audio_hub_stream_write(pcm, frames, rate)) {
                break;
            }
        } else {
            if (!audio_hub_stream_write(pcm, n / 2, rate)) {
                break;
            }
        }
    }

    audio_hub_stream_end();
    s_playing = false;
    ESP_LOGI(kTag, "播放任务退出");
    vTaskDelete(nullptr);
}

// 只重置状态，不释放内存/信号量——避免任务还在退出时被删掉导致崩溃
void music_reset_ring()
{
    if (s_ring.lock != nullptr) {
        xSemaphoreTake(s_ring.lock, portMAX_DELAY);
        s_ring.count = s_ring.head = s_ring.tail = 0;
        s_ring.eof = false;
        xSemaphoreGive(s_ring.lock);
    }
}

// 一次性分配
bool music_ensure_ring()
{
    if (s_ring.buf != nullptr && s_ring.lock != nullptr) {
        return true;
    }
    s_ring.cap = kRingBytes;
    s_ring.buf = static_cast<uint8_t*>(heap_caps_malloc(kRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_ring.buf == nullptr) {
        s_ring.buf = static_cast<uint8_t*>(malloc(kRingBytes));
    }
    if (s_ring.lock == nullptr) {
        s_ring.lock = xSemaphoreCreateMutex();
    }
    if (s_ring.buf == nullptr || s_ring.lock == nullptr) {
        ESP_LOGE(kTag, "环形缓冲创建失败");
        return false;
    }
    return true;
}

}  // namespace

static bool start_track(const char* file, const char* name)
{
    if (s_playing) {
        music_stop();
        // ★ 旧任务（reader/player）必须彻底退出后才能开新曲目：
        //   否则两套任务会共用同一份环形缓冲和 static PCM 缓冲，
        //   旧任务的 audio_hub_stream_end() 还会把新曲目的音频流关掉。
        if (s_playing) {
            ESP_LOGW(kTag, "上一曲的播放任务还没退出，本轮拒绝启动新曲目");
            return false;
        }
    }
    if (!music_ensure_ring()) {
        return false;
    }
    music_reset_ring();

    s_url = std::string(CONFIG_AMYAI_MUSIC_BASE_URL) + "/" + file;
    s_cur_name = name;
    s_fmt = Playing{};
    s_stop = false;
    s_playing = true;

    ESP_LOGI(kTag, "曲目「%s」-> %s", name, s_url.c_str());
    if (xTaskCreate(reader_task, "music_rd", 6144, nullptr, 3, nullptr) != pdPASS ||
        xTaskCreate(player_task, "music_pl", 6144, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "创建任务失败");
        s_stop = true;
        s_playing = false;
        return false;
    }
    return true;
}

bool music_next()
{
    const int n = static_cast<int>(sizeof(kRotation) / sizeof(kRotation[0]));
    s_rot_idx = (s_rot_idx + 1) % n;
    ESP_LOGI(kTag, "换下一首（第 %d/%d 首）", s_rot_idx + 1, n);
    return start_track(kRotation[s_rot_idx].file, kRotation[s_rot_idx].name);
}

bool music_play(const std::string& query)
{
    if (query.empty()) {
        return false;
    }
    // "换一首 / 下一首 / 换个别的" 这类：直接切到轮播的下一首
    if (query.find("换") != std::string::npos || query.find("下一") != std::string::npos ||
        query.find("别的") != std::string::npos || query.find("其他") != std::string::npos ||
        query.find("其它") != std::string::npos || query.find("next") != std::string::npos) {
        return music_next();
    }

    const Track* hit = nullptr;
    for (const auto& t : kTracks) {
        if (std::strstr(query.c_str(), t.keyword) != nullptr) {
            hit = &t;
            break;
        }
    }
    if (hit != nullptr) {
        // 记住轮播位置，方便下次"下一首"接得上
        const int n = static_cast<int>(sizeof(kRotation) / sizeof(kRotation[0]));
        for (int i = 0; i < n; ++i) {
            if (std::strstr(kRotation[i].file, hit->file) != nullptr) {
                s_rot_idx = i;
                break;
            }
        }
        return start_track(hit->file, hit->name);
    }

    ESP_LOGW(kTag, "曲库里没有匹配「%s」，改播默认轻音乐", query.c_str());
    s_rot_idx = 0;
    return start_track(kDefaultFile, kDefaultName);
}

void music_stop()
{
    if (!s_playing) {
        return;
    }
    s_stop = true;
    for (int i = 0; i < 30 && s_playing; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_playing) {
        // 3 秒还没退出：不硬删任务（会踩已持有的锁），只记录，下次 start_track 会拒绝启动
        ESP_LOGE(kTag, "播放任务 3 秒内未退出（s_playing 仍为真）");
    }
    music_reset_ring();
    audio_hub_set_music_volume(100);   // 监听时压低过音乐音量，停止后恢复
}

bool music_is_playing()
{
    return s_playing;
}

const char* music_current()
{
    return s_cur_name;
}