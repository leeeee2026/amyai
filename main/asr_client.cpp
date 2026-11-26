// 智谱 GLM-ASR-2512 语音识别
//
// 接口：POST https://open.bigmodel.cn/api/paas/v4/audio/transcriptions
//       multipart/form-data
//         model = glm-asr-2512
//         file  = 一个 wav 文件（≤25MB，≤30 秒）
// 返回：{"text": "识别结果"}
//
// 实现要点：
//   - 现场拼一个 44 字节的 WAV 头（PCM 16bit 单声道 16kHz）
//   - 手工构造 multipart 请求体（不引额外依赖）
//   - 整个 body 放 PSRAM，避免撑爆内部 RAM

#include "asr_client.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace {

constexpr const char* kTag = "asr";

// 国内站点；海外可换成 https://api.z.ai/api/paas/v4/audio/transcriptions
constexpr const char* kUrl = "https://open.bigmodel.cn/api/paas/v4/audio/transcriptions";
constexpr const char* kModel = "glm-asr-2512";
constexpr const char* kBoundary = "----amyaiBoundary7d3f9a";

constexpr int kTimeoutMs = 30000;

esp_err_t on_http_event(esp_http_client_event_t* evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr) {
        auto* out = static_cast<std::string*>(evt->user_data);
        if (evt->data != nullptr && evt->data_len > 0) {
            out->append(static_cast<const char*>(evt->data),
                        static_cast<size_t>(evt->data_len));
        }
    }
    return ESP_OK;
}

void put_le16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void put_le32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// 44 字节标准 WAV 头
void fill_wav_header(uint8_t* h, uint32_t data_bytes, uint32_t rate)
{
    std::memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    put_le32(h + 16, 16);          // fmt 块大小
    put_le16(h + 20, 1);           // PCM
    put_le16(h + 22, 1);           // 单声道
    put_le32(h + 24, rate);        // 采样率
    put_le32(h + 28, rate * 2);    // 字节率 (rate * channels * 2)
    put_le16(h + 32, 2);           // 块对齐
    put_le16(h + 34, 16);          // 位深
    std::memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

}  // namespace

std::string asr_transcribe(const int16_t* pcm, size_t samples)
{
    const char* key = CONFIG_AMYAI_LLM_API_KEY;
    if (key == nullptr || key[0] == '\0') {
        ESP_LOGE(kTag, "没有配置 API Key");
        return {};
    }

    const uint32_t rate = 16000;
    const uint32_t data_bytes = static_cast<uint32_t>(samples * sizeof(int16_t));
    if (data_bytes == 0) {
        return {};
    }

    // ---- 构造 multipart 请求体 ----
    char head[256];
    const int head_len = snprintf(
        head, sizeof(head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        kBoundary, kModel, kBoundary);

    char tail[64];
    const int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", kBoundary);

    const size_t total = static_cast<size_t>(head_len) + 44 + data_bytes + static_cast<size_t>(tail_len);

    auto* body = static_cast<uint8_t*>(
        heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (body == nullptr) {
        body = static_cast<uint8_t*>(malloc(total));
    }
    if (body == nullptr) {
        ESP_LOGE(kTag, "请求体分配失败（%u 字节）", static_cast<unsigned>(total));
        return {};
    }

    size_t off = 0;
    std::memcpy(body + off, head, head_len);
    off += head_len;
    fill_wav_header(body + off, data_bytes, rate);
    off += 44;
    std::memcpy(body + off, pcm, data_bytes);
    off += data_bytes;
    std::memcpy(body + off, tail, tail_len);
    off += tail_len;

    ESP_LOGI(kTag, "上传 %u 字节音频（%.1f 秒）",
             static_cast<unsigned>(data_bytes),
             static_cast<double>(samples) / rate);

    // ---- 发请求 ----
    std::string response;
    std::string auth = std::string("Bearer ") + key;
    std::string ctype = std::string("multipart/form-data; boundary=") + kBoundary;

    esp_http_client_config_t cfg = {};
    cfg.url = kUrl;
    cfg.method = HTTP_METHOD_POST;
    cfg.event_handler = on_http_event;
    cfg.user_data = &response;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = kTimeoutMs;
    cfg.buffer_size = 4096;

    // 弱网下 TLS 握手容易超时，失败就重试一次（重建连接）
    esp_err_t err = ESP_FAIL;
    int status = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == nullptr) {
            free(body);
            ESP_LOGE(kTag, "HTTP 客户端创建失败");
            return {};
        }
        esp_http_client_set_header(client, "Content-Type", ctype.c_str());
        esp_http_client_set_header(client, "Authorization", auth.c_str());
        esp_http_client_set_post_field(client, reinterpret_cast<const char*>(body), static_cast<int>(total));

        response.clear();
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK && status == 200) {
            break;
        }
        ESP_LOGW(kTag, "第 %d 次识别失败(err=%s status=%d)，重试一次",
                 attempt + 1, esp_err_to_name(err), status);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "请求失败: %s", esp_err_to_name(err));
        return {};
    }
    if (status != 200) {
        ESP_LOGE(kTag, "HTTP %d，响应: %.300s", status, response.c_str());
        return {};
    }

    // ---- 解析 {"text": "..."} ----
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(kTag, "响应不是合法 JSON");
        return {};
    }
    std::string text;
    cJSON* t = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(t)) {
        text = t->valuestring;
    } else {
        char* raw = cJSON_PrintUnformatted(root);
        ESP_LOGE(kTag, "没有 text 字段: %.300s", raw ? raw : "(null)");
        cJSON_free(raw);
    }
    cJSON_Delete(root);
    return text;
}
