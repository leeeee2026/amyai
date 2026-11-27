// 大模型客户端：HTTPS POST 到智谱 GLM（OpenAI 兼容接口）
//
// 连接复用：TLS 握手很贵（约 1.5–2 秒），所以 client 句柄常驻、开启 keep-alive，
// 只有第一次请求付握手成本，之后每次请求只花几十到几百毫秒。
//
// 内存提示：TLS 会话 + 证书包大约需要 40~60KB 堆，握手期间峰值更高。

#include "llm_client.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace {

constexpr const char* kTag = "llm";

// 智谱 OpenAI 兼容接口；换成百炼时把这两个常量改掉即可
constexpr const char* kUrl   = "https://open.bigmodel.cn/api/paas/v4/chat/completions";
constexpr const char* kModel = "glm-4-flash";

constexpr int kTimeoutMs = 20000;

// 对话上下文：保留最近几轮，让模型知道"再讲一个"是在接上一句
constexpr int kMaxHistory = 3;
std::string s_hist_user[kMaxHistory];
std::string s_hist_reply[kMaxHistory];
int s_hist_count = 0;

// keep-alive 参数（单位：秒）
constexpr int kKeepAliveIdle     = 60;
constexpr int kKeepAliveInterval = 30;
constexpr int kKeepAliveCount    = 3;

const char* kSystemPrompt = R"PROMPT(你是按摩椅语音助手。只输出一行 JSON，不要解释。

【意图】
控制 -> {"intent":"control","cmd":...,"reply":"确认语"}（多动作放 "actions":[...]）
闲聊 -> {"intent":"chat","reply":"..."}（不超过25字，口语，不反问）
介绍 -> {"intent":"intro"}
音乐 -> {"intent":"music","action":"play|stop|next","query":"..."}
不懂 -> {"intent":"unknown"}

【指令】
start/stop/pause/resume/query_status
set_zone zones:[neck,shoulder,back,waist,hip,leg,foot,arm,full]
set_intensity 1-5 | adjust_intensity delta:-2..2（可带 zones）
set_speed 1-5 | adjust_speed delta:-2..2
set_mode knead|tap|press|knead_tap|shiatsu|stretch
set_heat/set_zero_gravity/set_foot_roller on:bool | set_airbag 0-5
set_recline 100-170 | adjust_recline -30..30 | set_leg_rest 0-90 | adjust_leg_rest -30..30
set_time 5-60
set_program sleep|relax|neck|back|waist|leg|foot|full|office|sport|stretch|heat|zero_g|morning|night|elder|female|male|quick|deep|gentle

【规则】
"一点/再/太"用 adjust_*；"调到N档"用 set_*；没提部位不输出 zones；"疼/太重"用 -1 或 -2；reply 只复述动作。

【示例】
"脖子酸" -> {"intent":"control","cmd":"set_zone","zones":["neck"],"reply":"好的，按脖子"}
"重一点" -> {"intent":"control","cmd":"adjust_intensity","delta":1,"reply":"调重了一点"}
"太疼了" -> {"intent":"control","cmd":"adjust_intensity","delta":-2,"reply":"已经调轻了"}
"腿抬高" -> {"intent":"control","cmd":"adjust_leg_rest","delta":10,"reply":"腿托抬高一点"}
"你好" -> {"intent":"chat","reply":"你好呀，我在呢。"}
"来点雨声" -> {"intent":"music","action":"play","query":"雨声"}
"帮我订机票" -> {"intent":"unknown"}
)PROMPT";

// 常驻状态：响应缓冲和复用的连接句柄
std::string g_response;
esp_http_client_handle_t g_client = nullptr;
SemaphoreHandle_t g_lock = nullptr;   // 防止多任务同时调用大模型

// 收集响应正文
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

// 首次调用时创建，之后一直复用（这是 keep-alive 生效的前提）
esp_http_client_handle_t ensure_client(const std::string& auth)
{
    if (g_client != nullptr) {
        return g_client;
    }

    esp_http_client_config_t cfg = {};
    cfg.url               = kUrl;
    cfg.method            = HTTP_METHOD_POST;
    cfg.event_handler     = on_http_event;
    cfg.user_data         = &g_response;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  // 用内置证书包校验 HTTPS
    cfg.timeout_ms        = kTimeoutMs;
    cfg.buffer_size       = 2048;

    // 关键：打开连接复用，避免每次请求都重新握手
    cfg.keep_alive_enable   = true;
    cfg.keep_alive_idle     = kKeepAliveIdle;
    cfg.keep_alive_interval = kKeepAliveInterval;
    cfg.keep_alive_count    = kKeepAliveCount;

    g_client = esp_http_client_init(&cfg);
    if (g_client == nullptr) {
        return nullptr;
    }

    ESP_LOGI(kTag, "建立新的 HTTPS 连接（首次需要 TLS 握手，较慢）");

    esp_http_client_set_header(g_client, "Content-Type", "application/json");
    esp_http_client_set_header(g_client, "Authorization", auth.c_str());
    return g_client;
}

void drop_client()
{
    if (g_client != nullptr) {
        esp_http_client_cleanup(g_client);
        g_client = nullptr;
    }
}

// 从 OpenAI 风格的响应里取出 choices[0].message.content
std::string extract_content(const std::string& resp)
{
    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        ESP_LOGE(kTag, "响应不是合法 JSON");
        return {};
    }

    std::string text;
    cJSON* choices = cJSON_GetObjectItem(root, "choices");
    cJSON* first   = cJSON_GetArrayItem(choices, 0);
    cJSON* message = cJSON_GetObjectItem(first, "message");
    cJSON* content = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(content)) {
        text = content->valuestring;
    }

    // 出错时把服务端返回的 message 打出来，方便排查
    if (text.empty()) {
        cJSON* error = cJSON_GetObjectItem(root, "error");
        if (error != nullptr) {
            char* err_str = cJSON_PrintUnformatted(error);
            ESP_LOGE(kTag, "服务端报错: %s", err_str ? err_str : "(null)");
            cJSON_free(err_str);
        }
    }

    cJSON_Delete(root);
    return text;
}

}  // namespace

std::string ask_llm(const std::string& user_text)
{
    if (g_lock == nullptr) {
        g_lock = xSemaphoreCreateMutex();
    }
    if (g_lock != nullptr) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
    }
    struct Unlock {
        SemaphoreHandle_t s;
        ~Unlock() { if (s != nullptr) xSemaphoreGive(s); }
    } unlock{g_lock};

    const char* api_key = CONFIG_AMYAI_LLM_API_KEY;
    if (api_key == nullptr || api_key[0] == '\0') {
        ESP_LOGE(kTag, "没有配置 API Key，请 idf.py menuconfig -> Amyai 配置 -> 大模型 API Key");
        return {};
    }

    // ---- 组装请求体 ----
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return {};
    }
    cJSON_AddStringToObject(root, "model", kModel);

    cJSON* messages = cJSON_AddArrayToObject(root, "messages");

    cJSON* sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", kSystemPrompt);
    cJSON_AddItemToArray(messages, sys_msg);

    // ★ 带上最近几轮对话，模型才知道"再讲一个"是在上一句基础上说的
    for (int i = 0; i < s_hist_count; ++i) {
        cJSON* h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "role", "user");
        cJSON_AddStringToObject(h, "content", s_hist_user[i].c_str());
        cJSON_AddItemToArray(messages, h);

        cJSON* a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "role", "assistant");
        cJSON_AddStringToObject(a, "content", s_hist_reply[i].c_str());
        cJSON_AddItemToArray(messages, a);
    }

    cJSON* user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_text.c_str());
    cJSON_AddItemToArray(messages, user_msg);

    // ★ 聊天需要一点变化，否则同一句话永远得到同一个回答（但 0.7 实测控制指令仍稳定）
    cJSON_AddNumberToObject(root, "temperature", 0.7);
    cJSON_AddNumberToObject(root, "max_tokens", 64);

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        ESP_LOGE(kTag, "组装请求失败");
        return {};
    }

    const std::string auth = std::string("Bearer ") + api_key;

    // ---- 发请求（失败时重建连接再试一次）----
    esp_err_t err    = ESP_FAIL;
    int       status = 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        esp_http_client_handle_t client = ensure_client(auth);
        if (client == nullptr) {
            ESP_LOGE(kTag, "创建 HTTP 客户端失败");
            free(body);
            return {};
        }

        g_response.clear();
        esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));

        err    = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);

        // 连接可能已被服务端关闭（keep-alive 超时），重建后再试
        if (err == ESP_OK && status == 200) {
            break;
        }
        if (attempt == 0) {
            ESP_LOGW(kTag, "请求异常(err=%s, status=%d)，重建连接重试一次",
                     esp_err_to_name(err), status);
            drop_client();
        }
    }

    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "请求失败: %s", esp_err_to_name(err));
        return {};
    }
    if (status != 200) {
        ESP_LOGE(kTag, "HTTP 状态码 %d，响应前 300 字节: %.300s", status, g_response.c_str());
        return {};
    }

    const std::string reply = extract_content(g_response);

    // ★ 记录本轮对话，供下一句参考（只保留最近 3 轮）
    if (!reply.empty()) {
        if (s_hist_count >= kMaxHistory) {
            for (int i = 1; i < kMaxHistory; ++i) {
                s_hist_user[i - 1] = s_hist_user[i];
                s_hist_reply[i - 1] = s_hist_reply[i];
            }
            --s_hist_count;
        }
        s_hist_user[s_hist_count] = user_text;
        s_hist_reply[s_hist_count] = reply;
        ++s_hist_count;
    }

    return reply;
}

void llm_reset_history()
{
    s_hist_count = 0;
    for (int i = 0; i < kMaxHistory; ++i) {
        s_hist_user[i].clear();
        s_hist_reply[i].clear();
    }
}
