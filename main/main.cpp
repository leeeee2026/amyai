// amyai —— ESP32-S3 Wi-Fi 上网示例（C++）
//
// 流程：NVS -> netif/event loop -> Wi-Fi STA -> 扫描或连接 -> 获取 IP -> DNS + HTTP 验证上网
// 未配置 SSID 时会自动扫描附近网络并打印列表，方便直接照着填。
// 断线自动重连；联网后每 15 秒打印一次连接质量并重新验证外网。
//
// 编译前请先执行 `idf.py menuconfig` -> "Amyai 配置" 填写 Wi-Fi 名称与密码。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "sdkconfig.h"

#include "cJSON.h"
#include "asr_client.h"
#include "llm_client.h"
#include "tts_client.h"
#include "local_cue.h"
#include "local_asr.h"
#include "music_player.h"
#include "uart_link.h"
#include "wake_word.h"
#include "wifi_prov.h"

namespace {

constexpr const char* kTag = "amyai";

// 本轮录音是不是用户主动喊唤醒词触发的（用来区分"音乐窗口自动监听"）
static bool g_from_wake = false;

// ASR 偶尔会把音乐/噪声识别成 "#"、"嗯"、"啊" 之类的碎片，这种不能当指令
bool text_is_garbage(const std::string& t)
{
    int cjk = 0;
    int alnum = 0;
    for (unsigned char ch : t) {
        if (ch >= 0xE4 && ch <= 0xE9) {
            ++cjk;                     // 中文字符的 UTF-8 首字节大致范围
        } else if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                   (ch >= 'A' && ch <= 'Z')) {
            ++alnum;
        }
    }
    return (cjk < 2 && alnum < 3);
}

// 网络是否已就绪（连上 WiFi 且拿到 IP 后才允许语音交互）
static volatile bool g_net_ready = false;

// 产品介绍（问"你能干什么"时播报）
constexpr const char* kIntroText =
    "我可以用声音控制按摩椅。比如说：按按肩膀、力度重一点、腿抬高一点、"
    "开个热敷、来个二十分钟，我都能听懂。";

// 用来验证外网的 HTTP 站点（纯 HTTP，不需要证书）
constexpr const char* kTestHost = "www.baidu.com";
constexpr const char* kTestUrl  = "http://www.baidu.com/";

constexpr EventBits_t kGotIpBit  = BIT0;
constexpr EventBits_t kFailedBit = BIT1;

constexpr int kMaxRetry         = 8;
constexpr int kRetryDelayMs     = 2000;
constexpr int kConnectTimeoutMs = 30000;
constexpr int kHttpTimeoutMs    = 8000;
constexpr int kPsramCheckMs     = 15000;

// ---------------------------------------------------------------------------
// Wi-Fi STA
// ---------------------------------------------------------------------------
// 把常见的断开原因码翻译成人话
const char* reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:          return "找不到这个 AP（名称写错/信号太弱/只有 5GHz）";
    case WIFI_REASON_AUTH_FAIL:            return "认证失败（密码错误）";
    case WIFI_REASON_ASSOC_FAIL:           return "关联失败";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:    return "握手超时（密码错误或信号差）";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "四次握手超时（密码错误）";
    case WIFI_REASON_CONNECTION_FAIL:      return "连接失败";
    case WIFI_REASON_BEACON_TIMEOUT:       return "心跳超时（AP 掉线/信号差）";
    case WIFI_REASON_AUTH_EXPIRE:          return "认证过期";
    default:                               return "其他原因";
    }
}

class WifiStation {
public:
    // 网络栈 + Wi-Fi 初始化，不涉及具体 AP（也不启动 Wi-Fi）
    esp_err_t init();

    // 启动 Wi-Fi。配网流程内部会自己调用 esp_wifi_start()，
    // 所以非配网路径才需要显式调这个；重复调用是安全的。
    void start();

    // 设置目标 AP 并开始连接
    esp_err_t connect(const char* ssid, const char* password);

    // 等待拿到 IP，超时返回 false
    bool wait_for_ip(TickType_t ticks);

    bool connected() const { return connected_; }

    // 打印当前 IP / 网关 / 信号强度
    void log_link_info() const;

    // 扫描附近网络并打印（未配置 SSID 或连接失败时用）
    void scan_nearby();

private:
    static void on_wifi_event(void* ctx, esp_event_base_t base, int32_t id, void* data);
    static void on_ip_event(void* ctx, esp_event_base_t base, int32_t id, void* data);

    esp_netif_t* netif_    = nullptr;
    EventGroupHandle_t ev_ = nullptr;
    bool connected_        = false;
    bool has_config_       = false;
    int retry_             = 0;
};

esp_err_t WifiStation::init()
{
    ev_ = xEventGroupCreate();
    if (ev_ == nullptr) {
        ESP_LOGE(kTag, "创建事件组失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "创建默认事件循环失败: %s", esp_err_to_name(err));
        return err;
    }

    netif_ = esp_netif_create_default_wifi_sta();
    if (netif_ == nullptr) {
        ESP_LOGE(kTag, "创建 STA 网络接口失败");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::on_wifi_event, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiStation::on_ip_event, this, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    return ESP_OK;
}

void WifiStation::start()
{
    // 注意：network_prov_mgr_start_provisioning() 内部会自己 esp_wifi_start()，
    // 如果这里提前启动过，配网会因为 "Failed to start Wi-Fi" 直接失败。
    // 所以 init() 只初始化，真正启动放在这里，且允许重复调用。
    const esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(kTag, "启动 Wi-Fi 失败: %s", esp_err_to_name(err));
        return;
    }

    // 关闭省电模式，降低延迟
    esp_wifi_set_ps(WIFI_PS_NONE);
}

esp_err_t WifiStation::connect(const char* ssid, const char* password)
{
    wifi_config_t wifi_cfg{};
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid), ssid,
                 sizeof(wifi_cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password), password,
                 sizeof(wifi_cfg.sta.password) - 1);

    if (password == nullptr || password[0] == '\0') {
        ESP_LOGW(kTag, "密码为空，按开放网络处理");
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    wifi_cfg.sta.pmf_cfg.capable  = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    has_config_ = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    ESP_LOGI(kTag, "正在连接 Wi-Fi: \"%s\" ...", ssid);
    return esp_wifi_connect();
}

bool WifiStation::wait_for_ip(TickType_t ticks)
{
    EventBits_t bits = xEventGroupWaitBits(ev_, kGotIpBit | kFailedBit,
                                           pdFALSE, pdFALSE, ticks);
    return (bits & kGotIpBit) != 0;
}

void WifiStation::on_wifi_event(void* ctx, esp_event_base_t /*base*/, int32_t id, void* data)
{
    auto* self = static_cast<WifiStation*>(ctx);

    if (id == WIFI_EVENT_STA_START) {
        // 只有配置好 AP 才自动连接，否则留给扫描流程
        if (self->has_config_) {
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        self->connected_ = false;
        if (!self->has_config_) {
            return;
        }
        const auto* evt = static_cast<const wifi_event_sta_disconnected_t*>(data);
        ESP_LOGW(kTag, "连接断开，原因码=%d (%s)", evt->reason, reason_name(evt->reason));
        if (self->retry_ < kMaxRetry) {
            self->retry_++;
            ESP_LOGW(kTag, "第 %d/%d 次重连...", self->retry_, kMaxRetry);
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            esp_wifi_connect();
        } else {
            ESP_LOGE(kTag, "重连 %d 次仍失败，停止重试", kMaxRetry);
            xEventGroupSetBits(self->ev_, kFailedBit);
        }
    }
}

void WifiStation::on_ip_event(void* ctx, esp_event_base_t /*base*/, int32_t id, void* data)
{
    auto* self = static_cast<WifiStation*>(ctx);

    if (id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(data);
        self->connected_ = true;
        self->retry_     = 0;
        ESP_LOGI(kTag, "已获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(self->ev_, kGotIpBit);
    }
}

void WifiStation::log_link_info() const
{
    esp_netif_ip_info_t info{};
    if (netif_ != nullptr && esp_netif_get_ip_info(netif_, &info) == ESP_OK) {
        ESP_LOGI(kTag, "IP=" IPSTR "  网关=" IPSTR "  掩码=" IPSTR,
                 IP2STR(&info.ip), IP2STR(&info.gw), IP2STR(&info.netmask));
    }

    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(kTag, "AP=\"%s\"  信道=%d  信号=%d dBm",
                 reinterpret_cast<const char*>(ap.ssid), ap.primary, ap.rssi);
    }
}

void WifiStation::scan_nearby()
{
    // 先停掉连接/重连，否则扫描会和连接流程互相干扰，可能扫不到任何 AP
    has_config_ = false;
    connected_  = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));

    wifi_scan_config_t scan_cfg{};
    scan_cfg.show_hidden = false;

    ESP_LOGI(kTag, "开始扫描附近 2.4GHz 网络（约 3 秒）...");
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "扫描失败: %s", esp_err_to_name(err));
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        ESP_LOGW(kTag, "没有扫到任何网络");
        return;
    }

    auto* records = static_cast<wifi_ap_record_t*>(
        std::calloc(ap_num, sizeof(wifi_ap_record_t)));
    if (records == nullptr) {
        ESP_LOGE(kTag, "内存不足");
        return;
    }

    esp_wifi_scan_get_ap_records(&ap_num, records);

    ESP_LOGI(kTag, "附近共 %u 个网络：", static_cast<unsigned>(ap_num));
    for (int i = 0; i < ap_num; ++i) {
        ESP_LOGI(kTag, "  [%2d] %-32s %4d dBm  信道 %2d  %s",
                 i + 1,
                 reinterpret_cast<const char*>(records[i].ssid),
                 records[i].rssi,
                 records[i].primary,
                 records[i].authmode == WIFI_AUTH_OPEN ? "开放" : "加密");
    }

    std::free(records);
    ESP_LOGI(kTag, "把要连的名称和密码填到 menuconfig -> Amyai 配置");
}

// ---------------------------------------------------------------------------
// 联网检测：DNS 解析 + HTTP GET
// ---------------------------------------------------------------------------
bool resolve_host(const char* host)
{
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    const int rc = getaddrinfo(host, "80", &hints, &res);
    if (rc != 0 || res == nullptr) {
        ESP_LOGE(kTag, "DNS 解析 %s 失败", host);
        return false;
    }
    freeaddrinfo(res);
    ESP_LOGI(kTag, "DNS 解析 %s 成功", host);
    return true;
}

bool http_get_test(const char* url, int* status_code)
{
    esp_http_client_config_t cfg{};
    cfg.url               = url;
    cfg.timeout_ms        = kHttpTimeoutMs;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(kTag, "创建 HTTP 客户端失败");
        return false;
    }

    bool ok = false;
    // 只取响应头，不下载网页正文（正文有几百 KB，纯属浪费）
    if (esp_http_client_open(client, 0) == ESP_OK) {
        const int content_len = esp_http_client_fetch_headers(client);
        *status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(kTag, "HTTP GET %s -> 状态码 %d，正文 %d 字节（未下载）",
                 url, *status_code, content_len);
        esp_http_client_close(client);
        ok = (*status_code > 0);
    } else {
        ESP_LOGE(kTag, "HTTP GET %s 连接失败", url);
    }

    esp_http_client_cleanup(client);
    return ok;
}

bool check_internet()
{
    if (!resolve_host(kTestHost)) {
        ESP_LOGE(kTag, "结论：未连通外网（DNS 失败）");
        return false;
    }

    int status = 0;
    if (http_get_test(kTestUrl, &status)) {
        ESP_LOGI(kTag, "结论：外网可用 ✅");
        return true;
    }
    ESP_LOGE(kTag, "结论：DNS 通了但 HTTP 不通（可能有强制门户/防火墙）");
    return false;
}

// 模型偶尔会把同一个动作输出两遍（会让力度加两次），这里去重
void dedupe_actions(cJSON* obj)
{
    cJSON* acts = cJSON_GetObjectItem(obj, "actions");
    if (!cJSON_IsArray(acts)) {
        return;
    }
    cJSON* uniq = cJSON_CreateArray();
    const int n = cJSON_GetArraySize(acts);
    for (int i = 0; i < n; ++i) {
        cJSON* a = cJSON_GetArrayItem(acts, i);
        char* sa = cJSON_PrintUnformatted(a);
        bool dup = false;
        const int m = cJSON_GetArraySize(uniq);
        for (int j = 0; j < m; ++j) {
            char* sb = cJSON_PrintUnformatted(cJSON_GetArrayItem(uniq, j));
            if (sb != nullptr && sa != nullptr && std::strcmp(sa, sb) == 0) {
                dup = true;
            }
            if (sb != nullptr) cJSON_free(sb);
            if (dup) break;
        }
        if (sa != nullptr) cJSON_free(sa);
        if (!dup) {
            cJSON_AddItemToArray(uniq, cJSON_Duplicate(a, 1));
        }
    }
    const int removed = n - cJSON_GetArraySize(uniq);
    cJSON_DeleteItemFromObject(obj, "actions");
    cJSON_AddItemToObject(obj, "actions", uniq);
    if (removed > 0) {
        ESP_LOGW(kTag, "模型输出了 %d 个重复动作，已去重", removed);
    }
}

// ---------------------------------------------------------------------------
// 大模型指令校验（v2：部位/力度/速度/手法/热敷/零重力/足底滚轮/角度/程序）
// ---------------------------------------------------------------------------
namespace {

const char* const kZones[] = {"neck", "shoulder", "back", "waist", "hip",
                              "leg", "foot", "arm", "full"};
const char* const kModes[] = {"knead", "tap", "press", "knead_tap", "shiatsu", "stretch"};
const char* const kPrograms[] = {
    "sleep", "relax", "neck", "back", "waist", "leg", "foot", "full",
    "office", "sport", "stretch", "heat", "zero_g", "knead", "tap", "shiatsu",
    "morning", "night", "elder", "female", "male", "quick", "deep", "gentle",
};

bool in_list(const char* v, const char* const* arr, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (std::strcmp(v, arr[i]) == 0) return true;
    }
    return false;
}

// 取整数并检查范围
bool need_int(cJSON* o, const char* key, int lo, int hi)
{
    cJSON* it = cJSON_GetObjectItem(o, key);
    if (!cJSON_IsNumber(it)) {
        ESP_LOGW(kTag, "[校验] [REJECT] 缺少或非数字字段 %s", key);
        return false;
    }
    const int v = it->valueint;
    if (v < lo || v > hi) {
        ESP_LOGW(kTag, "[校验] [REJECT] %s=%d 超出范围 %d..%d", key, v, lo, hi);
        return false;
    }
    return true;
}

bool need_bool(cJSON* o, const char* key)
{
    cJSON* it = cJSON_GetObjectItem(o, key);
    if (!cJSON_IsBool(it)) {
        ESP_LOGW(kTag, "[校验] [REJECT] 缺少或非布尔字段 %s", key);
        return false;
    }
    return true;
}

bool need_enum(cJSON* o, const char* key, const char* const* arr, size_t n)
{
    cJSON* it = cJSON_GetObjectItem(o, key);
    if (!cJSON_IsString(it) || !in_list(it->valuestring, arr, n)) {
        ESP_LOGW(kTag, "[校验] [REJECT] 字段 %s 取值非法", key);
        return false;
    }
    return true;
}

bool check_zones(cJSON* o, bool required)
{
    cJSON* z = cJSON_GetObjectItem(o, "zones");
    if (!cJSON_IsArray(z) || cJSON_GetArraySize(z) == 0) {
        if (required) {
            ESP_LOGW(kTag, "[校验] [REJECT] 缺少 zones 数组");
            return false;
        }
        return true;   // 可选
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, z) {
        if (!cJSON_IsString(item) || !in_list(item->valuestring, kZones,
                                              sizeof(kZones) / sizeof(kZones[0]))) {
            ESP_LOGW(kTag, "[校验] [REJECT] zones 里有非法部位");
            return false;
        }
    }
    return true;
}

bool validate_obj(cJSON* o)
{
    cJSON* cmd = cJSON_GetObjectItem(o, "cmd");
    if (!cJSON_IsString(cmd)) {
        ESP_LOGW(kTag, "[校验] [REJECT] 缺少 cmd");
        return false;
    }
    const char* c = cmd->valuestring;

    if (!std::strcmp(c, "start") || !std::strcmp(c, "stop") || !std::strcmp(c, "pause") ||
        !std::strcmp(c, "resume") || !std::strcmp(c, "query_status") || !std::strcmp(c, "unknown")) {
        return true;
    }
    if (!std::strcmp(c, "set_zone"))   return check_zones(o, true);
    if (!std::strcmp(c, "set_intensity"))    return need_int(o, "level", 1, 5);
    if (!std::strcmp(c, "adjust_intensity")) return need_int(o, "delta", -2, 2) && check_zones(o, false);
    if (!std::strcmp(c, "set_speed"))        return need_int(o, "level", 1, 5);
    if (!std::strcmp(c, "adjust_speed"))     return need_int(o, "delta", -2, 2);
    if (!std::strcmp(c, "set_mode"))
        return need_enum(o, "mode", kModes, sizeof(kModes) / sizeof(kModes[0]));
    if (!std::strcmp(c, "set_heat") || !std::strcmp(c, "set_zero_gravity") ||
        !std::strcmp(c, "set_foot_roller")) {
        return need_bool(o, "on");
    }
    if (!std::strcmp(c, "set_airbag"))    return need_int(o, "level", 0, 5);
    if (!std::strcmp(c, "set_recline"))   return need_int(o, "angle", 100, 170);
    if (!std::strcmp(c, "adjust_recline")) return need_int(o, "delta", -30, 30);
    if (!std::strcmp(c, "set_leg_rest"))  return need_int(o, "angle", 0, 90);
    if (!std::strcmp(c, "adjust_leg_rest")) return need_int(o, "delta", -30, 30);
    if (!std::strcmp(c, "set_time"))      return need_int(o, "minutes", 5, 60);
    if (!std::strcmp(c, "set_program")) {
        return need_enum(o, "program", kPrograms, sizeof(kPrograms) / sizeof(kPrograms[0]));
    }

    ESP_LOGW(kTag, "[校验] [REJECT] 未知 cmd=\"%s\"", c);
    return false;
}

}  // namespace

bool validate_command(const std::string& text)
{
    cJSON* root = cJSON_Parse(text.c_str());
    if (root == nullptr) {
        ESP_LOGW(kTag, "[校验] [REJECT] 不是合法 JSON");
        return false;
    }

    bool ok = true;
    cJSON* actions = cJSON_GetObjectItem(root, "actions");
    if (cJSON_IsArray(actions)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, actions) {
            if (!validate_obj(item)) ok = false;
        }
        if (cJSON_GetArraySize(actions) == 0) ok = false;
    } else {
        ok = validate_obj(root);
    }

    if (ok) {
        ESP_LOGI(kTag, "[校验] [OK] 合法指令: %s", text.c_str());
    } else {
        ESP_LOGW(kTag, "[校验] [REJECT] 指令不合法，不下发");
    }

    cJSON_Delete(root);
    return ok;
}


}  // namespace

// ---------------------------------------------------------------------------
extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "================ amyai 启动 ================");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 主控通信：先初始化 UART，后面识别出的指令直接走它
    if (uart_link_init()) {
        ESP_LOGI(kTag, "主控通信已就绪（UART1）");
    } else {
        ESP_LOGW(kTag, "主控通信未启用，指令只会打印到日志");
    }

    tts_init();   // 语音合成（聊天/播报用）

    // 唤醒词：不依赖 Wi-Fi，先启动。引脚没配好时只报警告，不影响后面的联网流程。
    static WakeWord wake;
    const bool wake_ok = wake.start(
        [&] {
            ESP_LOGI(kTag, ">>> 唤醒成功，正在录音…");
            g_from_wake = true;     // 标记：这一轮是用户主动唤醒的，不是音乐窗口自动监听
            llm_reset_history();    // 新的一段会话，清空对话上下文
        },
        [&](const int16_t* pcm, size_t samples) {
            // 0. 网络没就绪时，仍然给离线命令词一个机会（这正是本地识别最大的价值）
            if (!g_net_ready) {
                std::string local_cmd;
                if (!music_is_playing() && local_asr_recognize(pcm, samples, &local_cmd)) {
                    ESP_LOGI(kTag, "【本地】网络未就绪，但本地识别命中，直接执行");
                    local_play_ack();
                    if (validate_command(local_cmd)) {
                        uart_link_send_command(local_cmd);
                    }
                    wake.cancel_follow_up();
                    return;
                }
                ESP_LOGW(kTag, "网络还没连上，播放本地提示音");
                local_play_no_net();   // 本地音频，不依赖网络；15 秒冷却
                wake.cancel_follow_up();
                return;
            }

            // ★ 1. 本地命令词快路径：不联网、不花调用费、约 200~400ms
            //   音乐播放中不走这条（避免和"停音乐/换歌"混淆，那条仍交给云端）
            //   喂给 MultiNet 的是 AFE 处理后的音频（降噪+增益），不是原始录音
            bool local_acked = false;
            if (!music_is_playing()) {
                size_t afe_len = 0;
                const int16_t* afe_pcm = wake_afe_pcm(&afe_len);
                std::string local_cmd;
                if (afe_pcm != nullptr && local_asr_recognize(afe_pcm, afe_len, &local_cmd)) {
                    local_play_ok();     // 「好的，马上为您调整」（延迟 500ms 播）
                    if (validate_command(local_cmd)) {
                        if (uart_link_send_command(local_cmd)) {
                            ESP_LOGI(kTag, "【本地】指令已下发主控: %s", local_cmd.c_str());
                        } else {
                            ESP_LOGW(kTag, "【本地】下发失败: %s", local_cmd.c_str());
                        }
                    } else {
                        ESP_LOGW(kTag, "【本地】指令校验未通过: %s", local_cmd.c_str());
                    }
                    return;                              // 不再走云端
                }
                // 本地没命中：先回「好的」，然后照常走云端（延迟 500ms 播）
                local_play_ack();
                local_acked = true;
            }

            // 2. 语音 -> 文字（云端）
            const int64_t t_cb0 = esp_timer_get_time();
            const std::string text = asr_transcribe(pcm, samples);
            const int64_t t_asr = (esp_timer_get_time() - t_cb0) / 1000;
            if (text.empty()) {
                ESP_LOGE(kTag, "语音识别失败");
                wake.cancel_follow_up();   // 别疯狂重录
                return;
            }
            ESP_LOGI(kTag, "你说: \"%s\"", text.c_str());

            // ★ 音乐播放窗口：不用唤醒词也能下指令
            //   停止词走本地快路径；换歌/调力度等继续走大模型。
            //   处理完自动进入连续聆听（用户停下音乐通常还有别的话要说）。
            const bool from_wake = g_from_wake;
            g_from_wake = false;
            const bool from_music = music_is_playing() && !from_wake;

            struct FollowUpGuard {
                bool on;
                WakeWord* w;
                ~FollowUpGuard() { if (on && w != nullptr) w->start_follow_up(); }
            } follow_guard{from_music, &wake};

            if (from_music) {
                // 停止类指令优先判断（"停"只有一个字，不能走乱码过滤）
                if (local_is_stop_command(text)) {
                    ESP_LOGI(kTag, "【音乐窗口】停止指令「%s」", text.c_str());
                    music_stop();
                    wake.set_music_window(false);
                    tts_speak("停了。");
                    return;   // 返回时 guard 会自动开启连续聆听
                }
                // 音乐声触发的识别结果常是 "#" 之类的碎片，忽略掉继续放音乐
                if (text_is_garbage(text)) {
                    ESP_LOGI(kTag, "【音乐窗口】识别乱码「%s」，忽略（音乐继续）", text.c_str());
                    return;
                }
                ESP_LOGI(kTag, "【音乐窗口】收到「%s」，先停音乐", text.c_str());
                music_stop();
                wake.set_music_window(false);
                // 其他指令继续往下走完整流程
            }

            // 主动唤醒且音乐在放：先把音乐停掉，把喇叭让给 TTS
            if (music_is_playing() && from_wake) {
                ESP_LOGI(kTag, "用户主动唤醒，先停掉背景音乐");
                music_stop();
                wake.set_music_window(false);
            }

            // 1.5 急停词本地直通：不联网、不走大模型，立刻发 0x04
            if (uart_link_is_estop_text(text)) {
                uart_link_send_estop();
                return;
            }

            // 2.5 老的"控制类秒回"只在没走过本地快路径时才需要（音乐窗口等场景）
            if (!local_acked && local_is_control(text)) {
                ESP_LOGI(kTag, "本地命中控制类关键词，先播「好的」");
                local_play_ack();
            }

            // 3. 文字 -> 带意图的 JSON（照常走云端，拿到完整回复）
            const int64_t t_llm0 = esp_timer_get_time();
            const std::string reply = ask_llm(text);
            const int64_t t_llm = (esp_timer_get_time() - t_llm0) / 1000;
            ESP_LOGI(kTag, "[耗时] ASR=%lld ms, LLM=%lld ms, 录音结束→LLM完成=%lld ms",
                     static_cast<long long>(t_asr), static_cast<long long>(t_llm),
                     static_cast<long long>((esp_timer_get_time() - t_cb0) / 1000));
            if (reply.empty()) {
                ESP_LOGE(kTag, "大模型调用失败");
                tts_speak("网络不太好。");
                return;
            }
            ESP_LOGI(kTag, "模型返回: %s", reply.c_str());

            // 3. 按意图分流
            cJSON* root = cJSON_Parse(reply.c_str());
            cJSON* intent_item = cJSON_GetObjectItem(root, "intent");
            const char* intent = cJSON_IsString(intent_item) ? intent_item->valuestring : "control";

            if (std::strcmp(intent, "chat") == 0) {
                cJSON* r = cJSON_GetObjectItem(root, "reply");
                if (cJSON_IsString(r) && r->valuestring[0] != '\0') {
                    ESP_LOGI(kTag, "【聊天】%s", r->valuestring);
                    tts_speak(r->valuestring);
                } else {
                    ESP_LOGW(kTag, "chat 意图但没给 reply");
                    tts_speak("这个我还没想好。");
                }
            } else if (std::strcmp(intent, "intro") == 0) {
                ESP_LOGI(kTag, "【产品介绍】");
                tts_speak(kIntroText);
            } else if (std::strcmp(intent, "music") == 0) {
                cJSON* act = cJSON_GetObjectItem(root, "action");
                cJSON* q = cJSON_GetObjectItem(root, "query");
                const char* action = cJSON_IsString(act) ? act->valuestring : "play";
                if (std::strcmp(action, "stop") == 0 || std::strcmp(action, "pause") == 0) {
                    if (music_is_playing()) {
                        music_stop();
                        wake.set_music_window(false);
                        ESP_LOGI(kTag, "【音乐】已停止");
                        tts_speak("停了。");
                    } else {
                        tts_speak("现在没在放音乐。");
                    }
                } else if (std::strcmp(action, "next") == 0) {
                    if (music_next()) {
                        wake.set_music_window(true);
                        ESP_LOGI(kTag, "【音乐】换到：%s", music_current());
                    }
                } else {
                    const char* query = cJSON_IsString(q) ? q->valuestring : "";
                    if (music_play(query)) {
                        wake.set_music_window(true);   // 开启免唤醒词监听（只认"停/关掉"）
                        ESP_LOGI(kTag, "【音乐】开始播放：%s", music_current());
                    } else {
                        ESP_LOGW(kTag, "【音乐】曲库没有「%s」", query);
                        tts_speak("这个我这儿还没有。");
                    }
                }
            } else if (std::strcmp(intent, "unknown") == 0) {
                ESP_LOGW(kTag, "【未知】");
                tts_speak("没听懂，你再说一次。");
            } else {
                // control（或老格式没有 intent 字段）
                if (!validate_command(reply)) {
                    ESP_LOGW(kTag, "指令不合法，不下发给主控");
                    tts_speak("这个我不太确定，先不动。");
                } else {
                    // reply 是念给用户听的确认语，下发给主控前要剥掉，保持报文干净
                    std::string cmd_payload = reply;
                    cJSON* clean = cJSON_Parse(reply.c_str());
                    if (clean != nullptr) {
                        cJSON_DeleteItemFromObject(clean, "reply");
                        dedupe_actions(clean);   // 去掉模型偶发的重复动作
                        char* s = cJSON_PrintUnformatted(clean);
                        if (s != nullptr) {
                            cmd_payload = s;
                            cJSON_free(s);
                        }
                        cJSON_Delete(clean);
                    }

                    if (uart_link_send_command(cmd_payload)) {
                        ESP_LOGI(kTag, "指令已送达主控");
#if CONFIG_AMYAI_TTS_CONFIRM
                        // 主控确认后再播报，确保"说的话"和"执行的事"一致
                        cJSON* ack_text = cJSON_GetObjectItem(root, "reply");
                        if (cJSON_IsString(ack_text) && ack_text->valuestring[0] != '\0') {
                            ESP_LOGI(kTag, "【确认】%s", ack_text->valuestring);
                            tts_speak(ack_text->valuestring);
                        }
#endif
                    } else {
                        ESP_LOGE(kTag, "主控没有确认，指令可能没送到");
                        tts_speak("指令没送到。");
                    }
                }
            }
            cJSON_Delete(root);
        });
    // ★ 离线命令词识别（MultiNet）：放到独立任务里延迟初始化。
    //   原因：MultiNet 加载模型要 1.5~2 秒，如果放在唤醒监听启动的同一个上下文里跑，
    //   会把 I2S 读取堵住，麦克风直接断流重启。延迟到系统稳定后再建。
    xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(6000));   // 等麦克风声道锁定、网络起来
            if (local_asr_init()) {
                ESP_LOGI(kTag, "离线命令词识别已启用（唤醒后先本地识别，未命中再走云端）");
            } else {
                ESP_LOGW(kTag, "离线命令词识别未启用，全部走云端");
            }
            vTaskDelete(nullptr);
        },
        "asr_init", 10 * 1024, nullptr, 2, nullptr);

    if (wake_ok) {
        ESP_LOGI(kTag, "等待唤醒词: \"%s\"", CONFIG_AMYAI_WAKE_TEXT);
    } else {
        ESP_LOGW(kTag, "唤醒功能未启动（多半是 I2S 引脚没配置），继续跑联网部分");
    }

    static WifiStation sta;
    if (sta.init() != ESP_OK) {
        ESP_LOGE(kTag, "Wi-Fi 初始化失败");
        return;
    }

#if CONFIG_AMYAI_PROV_ENABLE
    // 开机时按住 BOOT 键 -> 恢复出厂：清掉凭据后重启，下次启动自动进配网
    if (prov_detect_reset_request()) {
        prov_reset();
        ESP_LOGW(kTag, "已恢复出厂设置，1 秒后重启…");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
#endif

    std::string ssid     = CONFIG_AMYAI_WIFI_SSID;
    std::string password = CONFIG_AMYAI_WIFI_PASSWORD;

    // 编译期没填 SSID 时，优先用上次配网保存在 NVS 里的凭据
    if (ssid.empty()) {
        if (prov_load_credentials(ssid, password)) {
            ESP_LOGI(kTag, "使用已保存的 Wi-Fi 凭据: \"%s\"", ssid.c_str());
        }
    }

#if CONFIG_AMYAI_PROV_ENABLE
    // 还是没有任何凭据 -> 进入 BLE 配网
    if (ssid.empty()) {
        ESP_LOGW(kTag, "没有可用的 Wi-Fi 凭据，进入 BLE 配网流程");
        prov_run_ble(static_cast<uint32_t>(CONFIG_AMYAI_PROV_TIMEOUT_SEC) * 1000U);

        if (!prov_load_credentials(ssid, password)) {
            ESP_LOGE(kTag, "配网没完成，先扫描附近网络供你核对 SSID/信号");
            sta.start();
            sta.scan_nearby();
            return;
        }
        ESP_LOGI(kTag, "配网完成，目标 Wi-Fi: \"%s\"", ssid.c_str());

        // ★ 配网用完蓝牙后重启一次：让 BT 占用的内部内存彻底还回来，
        //   避免后面 TLS 建连时因为内存碎片失败（重启后就是纯 STA 模式，内存最干净）。
        ESP_LOGW(kTag, "配网完成，重启一次以释放蓝牙内存…");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
#endif

#if CONFIG_AMYAI_SKIP_WIFI_FOR_TEST
    // 排查用：完全不启动 WiFi，观察麦克风断流是否消失。
    // 如果消失 -> 断流与 WiFi/射频/供电有关；如果照旧 -> 与 WiFi 无关。
    ESP_LOGW(kTag, "=== 测试模式：跳过 WiFi，只跑麦克风 ===");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
#endif

    if (ssid.empty()) {
        ESP_LOGW(kTag, "尚未配置 Wi-Fi 名称，先扫描一下附近有什么网络");
        sta.start();
        sta.scan_nearby();
        return;
    }

    // 配网路径里 Wi-Fi 已经被配网管理器启动过，这里重复调用是安全的
    sta.start();

    // 配网流程内部可能已经连上并拿到 IP，已经连通就不再重复连接
    if (!sta.connected()) {
        if (sta.connect(ssid.c_str(), password.c_str()) != ESP_OK) {
            ESP_LOGE(kTag, "发起连接失败");
            return;
        }

        if (!sta.wait_for_ip(pdMS_TO_TICKS(kConnectTimeoutMs))) {
            ESP_LOGE(kTag, "连接超时：%d 秒内没拿到 IP", kConnectTimeoutMs / 1000);
            ESP_LOGW(kTag, "下面扫描一下 2.4GHz 频段实际能看到哪些网络，用于核对 SSID：");
            sta.scan_nearby();
            return;
        }
    }

    sta.log_link_info();
    check_internet();
    g_net_ready = true;   // 到这里才算真正可用了，之前唤醒会提示"网络未就绪"
    ESP_LOGI(kTag, "======== 网络就绪，可以开始语音交互 ========");
    // （原来那个固定语句的联调循环已经去掉，避免和语音链路抢 HTTP 连接）

    // 之后周期性打印链路质量并复测外网
    // 网络看门狗：连续多次连不上外网就整机重启（WiFi 偶尔会卡死，重启最快恢复）
    int net_fail = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kPsramCheckMs));
        sta.log_link_info();
        if (check_internet()) {
            net_fail = 0;
        } else {
            ++net_fail;
            ESP_LOGW(kTag, "连续 %d 次连不上外网", net_fail);
            if (net_fail >= 3) {
                ESP_LOGE(kTag, "网络持续不可用，重启整机恢复");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
    }
}
