#include "tts_client.h"
#include "wake_word.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#include "audio_hub.h"

namespace {

constexpr const char* kTag = "tts";

// 百炼的接口（北京地域）
constexpr const char* kTtsUrl =
    "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";

constexpr int kMaxChars = 550;     // 接口上限 600，留点余量

char s_last_error[160] = "";

void set_error(const char* msg)
{
    std::snprintf(s_last_error, sizeof(s_last_error), "%s", msg);
    if (msg[0] != '\0') {
        ESP_LOGE(kTag, "%s", msg);
    }
}

// ---------------------------------------------------------------- 播放管道
// 网络回调只负责把 PCM 塞进环形缓冲（很快），另一个任务按实时速度往 I2S 喂。
// 之前直接在 HTTP 回调里写 I2S，会把网络接收堵住，导致声音一卡一卡。
struct PcmPipe {
    uint8_t* buf = nullptr;
    size_t cap = 0;
    size_t head = 0;      // 读位置
    size_t tail = 0;      // 写位置
    size_t count = 0;
    SemaphoreHandle_t lock = nullptr;
    SemaphoreHandle_t wake = nullptr;
    volatile bool eos = false;     // 生产结束
    volatile bool active = false;
    int rate = 24000;
    int channels = 1;
    int total = 0;
    volatile int inflight = 0;     // 正在写 I2S 的块数（收尾时要等它归零）
};

PcmPipe s_pipe;
float s_stream_gain = 1.0f;   // TTS 自动增益（每段重置）

void pipe_start();   // 前向声明

void pipe_reset(int rate, int channels)
{
    if (s_pipe.lock == nullptr || s_pipe.wake == nullptr || s_pipe.buf == nullptr) {
        pipe_start();
        if (s_pipe.lock == nullptr || s_pipe.buf == nullptr) {
            return;
        }
    }
    xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
    s_pipe.head = s_pipe.tail = s_pipe.count = 0;
    s_pipe.rate = rate;
    s_pipe.channels = channels;
    s_pipe.total = 0;
    s_pipe.eos = false;
    s_pipe.active = true;
    s_stream_gain = 1.0f;      // 每段语音重新估计响度
    xSemaphoreGive(s_pipe.lock);
}

void pipe_push(const uint8_t* data, size_t len)
{
    if (s_pipe.buf == nullptr || len == 0) {
        return;
    }
    xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
    const size_t room = s_pipe.cap - s_pipe.count;
    if (len > room) {
        len = room;                    // 缓冲满就丢（正常情况下不会）
    }
    const size_t first = std::min(len, s_pipe.cap - s_pipe.tail);
    std::memcpy(s_pipe.buf + s_pipe.tail, data, first);
    if (len > first) {
        std::memcpy(s_pipe.buf, data + first, len - first);
    }
    s_pipe.tail = (s_pipe.tail + len) % s_pipe.cap;
    s_pipe.count += len;
    xSemaphoreGive(s_pipe.lock);
    xSemaphoreGive(s_pipe.wake);
}

// 消费者：按实时速度喂 I2S
void pipe_consumer(void*)
{
    if (s_pipe.lock == nullptr || s_pipe.wake == nullptr || s_pipe.buf == nullptr) {
        ESP_LOGE(kTag, "播放管道未就绪，任务退出");
        vTaskDelete(nullptr);
        return;
    }
    static int16_t mono[2048];
    while (true) {
        if (s_pipe.count > 0) {
            xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
            const size_t bytes_raw = std::min(s_pipe.count, (size_t)sizeof(mono));
            const size_t bytes = bytes_raw & ~static_cast<size_t>(1);   // ★ 必须是偶数，否则会把 16bit 采样切成两半
            const size_t first = std::min(bytes, s_pipe.cap - s_pipe.head);
            std::memcpy(mono, s_pipe.buf + s_pipe.head, first);
            if (bytes > first) {
                std::memcpy(reinterpret_cast<uint8_t*>(mono) + first, s_pipe.buf, bytes - first);
            }
            s_pipe.head = (s_pipe.head + bytes) % s_pipe.cap;
            s_pipe.count -= bytes;
            const int ch = s_pipe.channels;
            const int rate = s_pipe.rate;
            xSemaphoreGive(s_pipe.lock);

            s_pipe.inflight = s_pipe.inflight + 1;
            if (ch > 1) {
                const size_t frames = bytes / (2 * ch);
                for (size_t i = 0; i < frames; ++i) {
                    mono[i] = mono[i * ch];
                }
                audio_hub_stream_write(mono, frames, rate);
            } else {
                audio_hub_stream_write(mono, bytes / 2, rate);
            }
            s_pipe.inflight = s_pipe.inflight - 1;
            s_pipe.total += static_cast<int>(bytes);
        } else if (s_pipe.eos) {
            vTaskDelay(pdMS_TO_TICKS(20));   // 等最后一点播完
            if (s_pipe.count == 0 && s_pipe.inflight == 0) {
                s_pipe.active = false;
                continue;
            }
        } else {
            xSemaphoreTake(s_pipe.wake, pdMS_TO_TICKS(20));
        }
    }
}

void pipe_start()
{
    static bool started = false;
    if (started) {
        return;
    }
    // ★ 先建好同步对象和缓冲，再启动任务；否则任务一跑就拿到空的资源
    if (s_pipe.lock == nullptr) {
        s_pipe.lock = xSemaphoreCreateMutex();
    }
    if (s_pipe.wake == nullptr) {
        s_pipe.wake = xSemaphoreCreateBinary();
    }
    if (s_pipe.buf == nullptr) {
        s_pipe.cap = 256 * 1024;
        s_pipe.buf = static_cast<uint8_t*>(
            heap_caps_malloc(s_pipe.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_pipe.buf == nullptr) {
            s_pipe.buf = static_cast<uint8_t*>(malloc(s_pipe.cap));
        }
    }
    if (s_pipe.lock == nullptr || s_pipe.wake == nullptr || s_pipe.buf == nullptr) {
        ESP_LOGE(kTag, "播放管道创建失败");
        return;
    }
    s_pipe.eos = false;
    s_pipe.head = s_pipe.tail = s_pipe.count = 0;
    started = true;
    xTaskCreate(pipe_consumer, "tts_play", 4096, nullptr, 4, nullptr);
    ESP_LOGI(kTag, "播放管道就绪（缓冲 %u KB）", (unsigned)(s_pipe.cap / 1024));
}

// ---------------------------------------------------------------- WAV 头解析
struct WavInfo {
    const uint8_t* pcm = nullptr;
    size_t pcm_len = 0;
    int rate = 24000;
    int channels = 1;
    int bits = 16;
};

bool parse_wav(const std::string& buf, WavInfo* info)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buf.data());
    const size_t n = buf.size();
    if (n < 44 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
        return false;
    }

    size_t pos = 12;
    bool got_fmt = false;
    while (pos + 8 <= n) {
        const char* id = reinterpret_cast<const char*>(p + pos);
        const uint32_t sz = static_cast<uint32_t>(p[pos + 4]) |
                            (static_cast<uint32_t>(p[pos + 5]) << 8) |
                            (static_cast<uint32_t>(p[pos + 6]) << 16) |
                            (static_cast<uint32_t>(p[pos + 7]) << 24);
        const size_t body = pos + 8;

        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= n) {
            info->channels = static_cast<int>(p[body + 2] | (p[body + 3] << 8));
            info->rate = static_cast<int>(p[body + 4] | (p[body + 5] << 8) |
                                          (p[body + 6] << 16) | (p[body + 7] << 24));
            info->bits = static_cast<int>(p[body + 14] | (p[body + 15] << 8));
            got_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (!got_fmt) {
                return false;
            }
            info->pcm = p + body;
            info->pcm_len = sz;
            if (body + sz > n) {          // 流式 WAV 的长度字段是假的
                info->pcm_len = n - body;
            }
            return info->pcm_len > 0;
        }
        pos = body + sz + (sz & 1);
    }
    return false;                        // 头还没收全
}

// ---------------------------------------------------------------- SSE 流式播放
struct SseCtx {
    std::string line;        // SSE 行缓冲
    std::string head;        // WAV 头 + 首批数据（够解析就开播）
    uint8_t* b64 = nullptr;  // base64 解码缓冲
    size_t b64_cap = 0;
    bool playing = false;
    int rate = 24000;
    int channels = 1;
    int total_pcm = 0;
    int chunks = 0;
    bool error = false;
};

// ★ 连接复用：TLS 握手要 400~600ms，每句话都重连太浪费。
//   SSE 回调的 user_data 在 init 时固定，所以用全局指针在每次请求时切换。
esp_http_client_handle_t s_tts_cli = nullptr;
int64_t g_tts_t0_us = 0;   // 本次 TTS 开始时刻（算首包延迟）
SseCtx* s_cur_ctx = nullptr;

// 把一段 PCM 丢进播放管道（非阻塞）
// 顺便做自动增益：百炼返回的音频峰值只有 0.4~0.5，直接播会比提示音小很多，
// 这里按峰值慢速归一化到 0.85，让所有语音响度一致。
void feed_pcm(SseCtx* ctx, const uint8_t* data, size_t len)
{
    if (len < 2 || !ctx->playing) {
        return;
    }
    const int16_t* src = reinterpret_cast<const int16_t*>(data);
    const size_t total = len / 2;

    // 统计本片峰值
    int peak = 0;
    for (size_t i = 0; i < total; ++i) {
        const int v = (src[i] < 0) ? -src[i] : src[i];
        if (v > peak) peak = v;
    }
    if (peak > 300) {
        float want = 0.85f * 32767.0f / static_cast<float>(peak);
        if (want > 3.5f) want = 3.5f;
        if (want < 0.6f) want = 0.6f;
        s_stream_gain = s_stream_gain * 0.85f + want * 0.15f;   // 慢速跟随，避免忽大忽小
    }

    static int16_t scaled[4096];
    for (size_t off = 0; off < total;) {
        size_t cnt = total - off;
        if (cnt > sizeof(scaled) / sizeof(scaled[0])) cnt = sizeof(scaled) / sizeof(scaled[0]);
        for (size_t i = 0; i < cnt; ++i) {
            int v = static_cast<int>(src[off + i] * s_stream_gain);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            scaled[i] = static_cast<int16_t>(v);
        }
        pipe_push(reinterpret_cast<const uint8_t*>(scaled), cnt * 2);
        off += cnt;
    }
    ctx->total_pcm += static_cast<int>(len);
}

// 处理一条 SSE 的 data: 负载
void handle_sse_data(SseCtx* ctx, const char* json)
{
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        return;
    }
    cJSON* out = cJSON_GetObjectItem(root, "output");
    cJSON* audio = cJSON_GetObjectItem(out, "audio");
    cJSON* d = cJSON_GetObjectItem(audio, "data");
    if (!cJSON_IsString(d) || d->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return;
    }

    const size_t b64_len = std::strlen(d->valuestring);
    // 注意：base64 解码后只有 3/4 大小，要按解码后长度判断，否则合法分片会被误丢
    const size_t decoded_est = b64_len / 4 * 3 + 4;
    if (decoded_est > ctx->b64_cap) {
        ESP_LOGW(kTag, "分片解码后过大（base64 %u -> 约 %u 字节，上限 %u），忽略",
                 (unsigned)b64_len, (unsigned)decoded_est, (unsigned)ctx->b64_cap);
        cJSON_Delete(root);
        return;
    }
    size_t out_len = 0;
    const int rc = mbedtls_base64_decode(ctx->b64, ctx->b64_cap, &out_len,
                                         reinterpret_cast<const unsigned char*>(d->valuestring),
                                         b64_len);
    cJSON_Delete(root);
    if (rc != 0 || out_len == 0) {
        return;
    }
    ++ctx->chunks;

    if (!ctx->playing) {
        // 还没开播：累积到能解析出 WAV 头为止
        ctx->head.append(reinterpret_cast<const char*>(ctx->b64), out_len);
        WavInfo wi;
        if (!parse_wav(ctx->head, &wi)) {
            return;                      // 头没收全，等下一片
        }
        if (wi.bits != 16) {
            ESP_LOGE(kTag, "只支持 16bit，收到 %d", wi.bits);
            ctx->error = true;
            return;
        }
        ctx->rate = wi.rate;
        ctx->channels = wi.channels;
        pipe_start();
        pipe_reset(wi.rate, wi.channels);
        if (!audio_hub_stream_begin(AUDIO_PRIO_TTS)) {
            ctx->error = true;
            return;
        }
        ctx->playing = true;
        if (g_tts_t0_us > 0) {
            ESP_LOGI(kTag, "[耗时] TTS 首包 %lld ms（含 TLS/排队）",
                     static_cast<long long>((esp_timer_get_time() - g_tts_t0_us) / 1000));
        }
        ESP_LOGI(kTag, "首包开播（第 %d 片，%d Hz / %d ch）", ctx->chunks, wi.rate, wi.channels);
        feed_pcm(ctx, wi.pcm, wi.pcm_len);
        ctx->head.clear();
    } else {
        feed_pcm(ctx, ctx->b64, out_len);
    }
}

esp_err_t sse_event(esp_http_client_event_t* evt)
{
    auto* ctx = static_cast<SseCtx*>(evt->user_data);
    if (ctx == nullptr) {
        ctx = s_cur_ctx;   // 复用连接时 user_data 固定为 nullptr，用当前请求的 ctx
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA || ctx == nullptr || ctx->error) {
        return ESP_OK;
    }
    ctx->line.append(static_cast<const char*>(evt->data), static_cast<size_t>(evt->data_len));

    size_t start = 0;
    while (true) {
        const size_t nl = ctx->line.find('\n', start);
        if (nl == std::string::npos) {
            break;
        }
        std::string row = ctx->line.substr(start, nl - start);
        start = nl + 1;
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        if (row.rfind("data:", 0) != 0) {
            continue;                    // id:/event:/注释行都跳过
        }
        std::string payload = row.substr(5);
        while (!payload.empty() && payload.front() == ' ') {
            payload.erase(payload.begin());
        }
        if (payload == "[DONE]") {
            continue;
        }
        handle_sse_data(ctx, payload.c_str());
    }
    if (start > 0) {
        ctx->line.erase(0, start);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- 单段合成
bool speak_once(const std::string& text)
{
    const char* key = CONFIG_AMYAI_TTS_API_KEY;
    if (key == nullptr || key[0] == '\0') {
        set_error("没配置 TTS API Key（CONFIG_AMYAI_TTS_API_KEY）");
        return false;
    }

    g_tts_t0_us = esp_timer_get_time();

    // 构造请求
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_AMYAI_TTS_MODEL);
    cJSON* input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "text", text.c_str());
    cJSON_AddStringToObject(input, "voice", CONFIG_AMYAI_TTS_VOICE);
    cJSON_AddStringToObject(input, "language_type", "Chinese");
    cJSON_AddItemToObject(root, "input", input);
    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "speech_rate", CONFIG_AMYAI_TTS_RATE_PCT / 100.0);
    cJSON_AddItemToObject(root, "parameters", params);
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        set_error("构造请求失败");
        return false;
    }

    SseCtx ctx;
    ESP_LOGI(kTag, "TTS 前内存: 剩余 %u，内部最大块 %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ctx.b64_cap = 32 * 1024;   // 分片约 17KB，32KB 足够
    ctx.b64 = static_cast<uint8_t*>(heap_caps_malloc(ctx.b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ctx.b64 == nullptr) {
        ctx.b64 = static_cast<uint8_t*>(malloc(ctx.b64_cap));
    }
    if (ctx.b64 == nullptr) {
        cJSON_free(body);
        set_error("内存不足");
        return false;
    }
    ctx.head.reserve(96 * 1024);

    esp_http_client_config_t cfg = {};
    cfg.url = kTtsUrl;
    cfg.method = HTTP_METHOD_POST;
    cfg.event_handler = sse_event;
    cfg.user_data = nullptr;              // 用全局 s_cur_ctx，便于复用连接
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 4096;
    cfg.keep_alive_enable = true;         // ★ 复用 TLS 连接，省掉每次 400~600ms 握手

    esp_err_t err = ESP_FAIL;
    int status = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (s_tts_cli == nullptr) {
            s_tts_cli = esp_http_client_init(&cfg);
            if (s_tts_cli == nullptr) {
                break;
            }
        }
        esp_http_client_set_header(s_tts_cli, "Content-Type", "application/json");
        esp_http_client_set_header(s_tts_cli, "X-DashScope-SSE", "enable");
        std::string auth = std::string("Bearer ") + key;
        esp_http_client_set_header(s_tts_cli, "Authorization", auth.c_str());
        esp_http_client_set_post_field(s_tts_cli, body, static_cast<int>(std::strlen(body)));

        s_cur_ctx = &ctx;
        ctx.error = false;
        err = esp_http_client_perform(s_tts_cli);
        status = esp_http_client_get_status_code(s_tts_cli);
        s_cur_ctx = nullptr;

        if (err == ESP_OK && status == 200) {
            break;
        }
        // 连接可能已被服务端关闭，扔掉重连一次
        ESP_LOGW(kTag, "第 %d 次合成失败(err=%s status=%d)，重连重试",
                 attempt + 1, esp_err_to_name(err), status);
        if (s_tts_cli != nullptr) {
            esp_http_client_cleanup(s_tts_cli);
            s_tts_cli = nullptr;
        }
        ctx.line.clear();
        ctx.head.clear();
        ctx.playing = false;
        ctx.chunks = 0;
    }
    cJSON_free(body);

    if (ctx.playing) {
        // 等管道里的音频播完（最多等 15 秒），再关功放
        s_pipe.eos = true;
        const int64_t deadline = esp_timer_get_time() + 15LL * 1000 * 1000;
        while (s_pipe.count > 0 && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        audio_hub_stream_end();
    }
    free(ctx.b64);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(kTag, "流式合成失败 err=%d status=%d", err, status);
        set_error("TTS 合成请求失败");
        return false;
    }
    if (!ctx.playing || ctx.error) {
        set_error("合成没有返回音频");
        return false;
    }
    ESP_LOGI(kTag, "播放完成：%d 片，%d 字节 PCM（%.1f 秒）", ctx.chunks, ctx.total_pcm,
             static_cast<float>(ctx.total_pcm) / 2.0f / ctx.rate);
    return true;
}

// 按字节数切分（UTF-8 安全）
size_t split_pos(const std::string& s, size_t max_bytes)
{
    if (s.size() <= max_bytes) return s.size();
    size_t p = max_bytes;
    while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) {
        --p;
    }
    if (p == 0) p = max_bytes;
    return p;
}

}  // namespace

void tts_init()
{
    if (CONFIG_AMYAI_TTS_API_KEY[0] == '\0') {
        ESP_LOGW(kTag, "TTS 未配置 Key，聊天播报不可用");
        return;
    }
    pipe_start();   // 开机就把播放管道建好，避免第一次播报时现分配
    ESP_LOGI(kTag, "TTS 就绪（流式）：模型 %s，音色 %s", CONFIG_AMYAI_TTS_MODEL,
             CONFIG_AMYAI_TTS_VOICE);
}

const char* tts_last_error()
{
    return s_last_error;
}

bool tts_speak(const std::string& text)
{
    if (text.empty()) return false;
    set_error("");

    // ★ 播报期间暂停录音：否则麦克风会把喇叭声录进去，ASR 认出"自己说的话"
    //   当成新的用户输入 —— 实测会出现"它把自己讲的段子当成用户在问"的自激。
    wake_pause_listening(true);
    struct PauseGuard {
        ~PauseGuard() { wake_pause_listening(false); }
    } pause_guard;

    size_t total_chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) ++total_chars;
    }

    if (total_chars <= kMaxChars) {
        return speak_once(text);
    }

    ESP_LOGW(kTag, "文本 %u 字，超过上限，分段播报", (unsigned)total_chars);
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t cut = split_pos(text.substr(pos), kMaxChars * 3);
        const std::string seg = text.substr(pos, cut);
        if (!speak_once(seg)) {
            return false;
        }
        pos += cut;
    }
    return true;
}
