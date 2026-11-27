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

const char* kSystemPrompt = R"PROMPT(你是按摩椅语音助手。只输出一行 JSON，不要解释、不要 Markdown。
reply 永远是第一个字段（系统要边生成边播报）。
reply 里不要出现英文单引号或双引号，需要引号时用中文「」。

【意图】
控制按摩椅 -> {"reply":"确认语","intent":"control","cmd":...}（多动作用 "actions":[...]）
闲聊问答 -> {"reply":"...","intent":"chat"}（不超过25字，口语，不反问）
问你能做什么 -> {"reply":"...","intent":"intro"}
用户明确要音乐/歌/曲时才用（没提音乐的一律 chat/unknown） -> {"reply":"","intent":"music","action":"play|stop|next","query":"..."}（music 的 reply 必须为空字符串，直接执行不播报）
不懂/无关 -> {"reply":"...","intent":"unknown"}
用户让退出/退下/没事了/不用了 -> {"reply":"","intent":"sleep"}（回复由系统固定播报，reply 必须为空）

【控制指令】
start/stop/pause/resume（这四个起停指令的 reply 必须是空字符串 ""，直接执行不播报）
set_zone zones:["neck","shoulder","back","waist","hip","leg","foot","arm","full"]
set_intensity level:1-5        adjust_intensity delta:-2..2（可带 zones）
set_speed level:1-5            adjust_speed delta:-2..2
set_mode mode:knead|tap|press|knead_tap|shiatsu|stretch
set_heat/set_zero_gravity/set_foot_roller on:true|false
set_airbag level:0-5
set_recline angle:100-170      adjust_recline delta:-30..30
set_leg_rest angle:0-90        adjust_leg_rest delta:-30..30
set_time minutes:5-60
set_program program:sleep|relax|neck|back|waist|leg|foot|full|office|sport|stretch|heat|zero_g|knead|tap|shiatsu|morning|night|elder|female|male|quick|deep|gentle
query_status（状态查询输出 {"intent":"control","cmd":"query_status"}）

【规则】
- "一点/再/太…了"用 adjust_*；"调到N档/最高/最低"用 set_*
- 没提部位不输出 zones
- "疼/太重"用 adjust_intensity delta:-1 或 -2
- 有上下文时，"再讲一个/换一个"要给不同内容
- reply 只复述动作，不承诺效果
- "放倒/降下/躺平"用 adjust_recline 负 delta；"升起/抬起"用正 delta
- "放松一下/放松模式/来点放松的"用 set_program program:relax

【示例】
"脖子好酸" -> {"reply":"好的，按脖子","intent":"control","cmd":"set_zone","zones":["neck"]}
"重一点" -> {"reply":"调重了一点","intent":"control","cmd":"adjust_intensity","delta":1}
"太疼了" -> {"reply":"已经调轻了","intent":"control","cmd":"adjust_intensity","delta":-2}
"腿抬高一点" -> {"reply":"腿托抬高一点","intent":"control","cmd":"adjust_leg_rest","delta":10}
"腿按重一点" -> {"reply":"腿的力度调重了","intent":"control","cmd":"adjust_intensity","delta":1,"zones":["leg"]}
"靠背放倒一点" -> {"reply":"靠背放低了","intent":"control","cmd":"adjust_recline","delta":-10}
"放松一下" -> {"reply":"给你来个放松模式","intent":"control","cmd":"set_program","program":"relax"}
"来个零重力" -> {"reply":"零重力开启","intent":"control","cmd":"set_zero_gravity","on":true}
"你好" -> {"reply":"你好呀，我在呢。","intent":"chat"}
"今天天气怎么样" -> {"reply":"我现在还查不到实时天气。","intent":"chat"}
"你能干什么" -> {"reply":"我能帮你按按摩、调力度、开热敷。","intent":"intro"}
"来点雨声" -> {"reply":"","intent":"music","action":"play","query":"雨声"}
"帮我订机票" -> {"reply":"这个我还做不了。","intent":"unknown"}
)PROMPT";

// 常驻状态：响应缓冲和复用的连接句柄
std::string g_response;
esp_http_client_handle_t g_client = nullptr;
SemaphoreHandle_t g_lock = nullptr;   // 防止多任务同时调用大模型

// 收集响应正文

// ---------------------------------------------------------------- 流式（SSE）
// 逐块解析智谱的 SSE 响应：data: {"choices":[{"delta":{"content":"..."}}]}
// 从累积内容里边解析 "reply" 字段边回调，TTS 就能边生成边播。

struct StreamCtx {
    std::string sse_buf;        // SSE 行缓冲
    std::string content;        // 累积的 delta.content（即模型输出的 JSON 文本）
    size_t reply_begin = 0;     // "reply":" 之后的位置
    size_t emitted = 0;         // 已经回调出去的字符数
    bool reply_started = false;
    bool done = false;
    const std::function<void(const std::string&)>* cb = nullptr;
};

// 当前正在进行的流式请求上下文（复用连接时 user_data 固定为 nullptr，靠它传递）
StreamCtx* s_cur_stream = nullptr;

void emit_reply_delta(StreamCtx* ctx)
{
    if (!ctx->reply_started) {
        const std::string key = "\"reply\":\"";
        const size_t p = ctx->content.find(key);
        if (p == std::string::npos) {
            return;
        }
        ctx->reply_started = true;
        ctx->reply_begin = p + key.size();
        ctx->emitted = 0;
    }

    std::string decoded;
    size_t i = ctx->reply_begin;
    while (i < ctx->content.size()) {
        const char c = ctx->content[i];
        if (c == '\\' && i + 1 < ctx->content.size()) {
            const char n = ctx->content[i + 1];
            switch (n) {
            case '"':  decoded += '"';  break;
            case '\\': decoded += '\\'; break;
            case 'n':  decoded += '\n'; break;
            case 't':  decoded += '\t'; break;
            case 'r':  decoded += '\r'; break;
            case 'u':  i += 6; continue;      // \uXXXX 极少见，跳过
            default:   decoded += n;    break;
            }
            i += 2;
            continue;
        }
        if (c == '"') {
            break;                             // reply 结束
        }
        decoded += c;
        ++i;
    }

    if (decoded.size() > ctx->emitted) {
        const std::string delta = decoded.substr(ctx->emitted);
        ctx->emitted = decoded.size();
        if (ctx->cb != nullptr && *ctx->cb) {
            (*ctx->cb)(delta);
        }
    }
}

esp_err_t stream_event(esp_http_client_event_t* evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    auto* ctx = static_cast<StreamCtx*>(evt->user_data);
    if (ctx == nullptr) {
        ctx = s_cur_stream;   // 流式连接复用：user_data 固定为 nullptr
    }
    if (ctx == nullptr || evt->data == nullptr || evt->data_len <= 0) {
        return ESP_OK;
    }
    ctx->sse_buf.append(static_cast<const char*>(evt->data),
                        static_cast<size_t>(evt->data_len));

    size_t nl = 0;
    while ((nl = ctx->sse_buf.find('\n')) != std::string::npos) {
        std::string line = ctx->sse_buf.substr(0, nl);
        ctx->sse_buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("data: ", 0) != 0) {
            continue;
        }
        const std::string payload = line.substr(6);
        if (payload == "[DONE]") {
            ctx->done = true;
            continue;
        }
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            continue;
        }
        cJSON* ch = cJSON_GetObjectItem(root, "choices");
        cJSON* f  = cJSON_GetArrayItem(ch, 0);
        cJSON* d  = cJSON_GetObjectItem(f, "delta");
        cJSON* c  = cJSON_GetObjectItem(d, "content");
        if (cJSON_IsString(c) && c->valuestring != nullptr) {
            ctx->content += c->valuestring;
            emit_reply_delta(ctx);
        }
        cJSON_Delete(root);
    }
    return ESP_OK;
}

// 流式请求专用连接（和普通请求分开，事件回调不一样）
esp_http_client_handle_t s_stream_client = nullptr;

esp_http_client_handle_t ensure_stream_client(const std::string& auth)
{
    if (s_stream_client != nullptr) {
        esp_http_client_set_header(s_stream_client, "Authorization", auth.c_str());
        return s_stream_client;
    }
    esp_http_client_config_t cfg = {};
    cfg.url               = kUrl;
    cfg.method            = HTTP_METHOD_POST;
    cfg.event_handler     = stream_event;
    cfg.user_data         = nullptr;      // 每次请求通过全局 s_stream_ctx 指定
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = kTimeoutMs;
    cfg.buffer_size       = 1024;         // SSE 小块流式，缓冲小一点更及时
    cfg.keep_alive_enable   = true;
    cfg.keep_alive_idle     = kKeepAliveIdle;
    cfg.keep_alive_interval = kKeepAliveInterval;
    cfg.keep_alive_count    = kKeepAliveCount;

    s_stream_client = esp_http_client_init(&cfg);
    if (s_stream_client == nullptr) {
        return nullptr;
    }
    esp_http_client_set_header(s_stream_client, "Content-Type", "application/json");
    esp_http_client_set_header(s_stream_client, "Accept", "text/event-stream");
    esp_http_client_set_header(s_stream_client, "Authorization", auth.c_str());
    return s_stream_client;
}

void drop_stream_client()
{
    if (s_stream_client != nullptr) {
        esp_http_client_cleanup(s_stream_client);
        s_stream_client = nullptr;
    }
}

// 把最近一轮对话写进历史（和 ask_llm 的行为保持一致）
void push_history(const std::string& user_text, const std::string& reply)
{
    if (reply.empty()) {
        return;
    }
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
    cJSON_AddNumberToObject(root, "max_tokens", 220);   // 太小会把回复截断成非法 JSON

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


// 流式问同一个问题：边收 reply 增量边回调，返回完整 JSON
std::string ask_llm_stream(const std::string& user_text,
                           const std::function<void(const std::string&)>& on_delta)
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
        ESP_LOGE(kTag, "没有配置 API Key");
        return {};
    }

    // ---- 组装请求体（和 ask_llm 一样，只多一个 stream:true）----
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

    cJSON_AddNumberToObject(root, "temperature", 0.7);
    cJSON_AddNumberToObject(root, "max_tokens", 220);   // 太小会把回复截断成非法 JSON
    cJSON_AddBoolToObject(root, "stream", true);

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return {};
    }

    const std::string auth = std::string("Bearer ") + api_key;

    StreamCtx ctx;
    ctx.cb = &on_delta;

    esp_err_t err    = ESP_FAIL;
    int       status = 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        esp_http_client_handle_t client = ensure_stream_client(auth);
        if (client == nullptr) {
            ESP_LOGE(kTag, "创建流式客户端失败");
            free(body);
            return {};
        }
        esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));

        ctx = StreamCtx{};
        ctx.cb = &on_delta;
        s_cur_stream = &ctx;

        err    = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        s_cur_stream = nullptr;

        if (err == ESP_OK && status == 200) {
            break;
        }
        ESP_LOGW(kTag, "流式请求异常(err=%s, status=%d)%s",
                 esp_err_to_name(err), status, attempt == 0 ? "，重建连接重试" : "");
        drop_stream_client();
    }

    free(body);

    if (err != ESP_OK || status != 200) {
        return {};
    }

    push_history(user_text, ctx.content);
    return ctx.content;
}

void llm_reset_history()
{
    s_hist_count = 0;
    for (int i = 0; i < kMaxHistory; ++i) {
        s_hist_user[i].clear();
        s_hist_reply[i].clear();
    }
}
