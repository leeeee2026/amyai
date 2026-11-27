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
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "sdkconfig.h"

#include "cJSON.h"
#include "asr_client.h"
#include "diag.h"
#include "llm_client.h"
#include "reply_player.h"
#include "tts_client.h"
#include "local_cue.h"
#include "local_asr.h"
#include "music_player.h"
#include "uart_link.h"
#include "wake_word.h"
#include "audio_hub.h"
#include "device_cfg.h"
#include "web_portal.h"
#include "wifi_prov.h"

namespace {

constexpr const char* kTag = "amyai";

// 本轮录音是不是用户主动喊唤醒词触发的（用来区分"音乐窗口自动监听"）
static bool g_from_wake = false;

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

// 手机是否接入了我们自己的 AP（阶段 2：扫码进入配置页的触发条件）
volatile bool g_ap_client_connected = false;

void on_ap_client_event(void* /*arg*/, esp_event_base_t base, int32_t id, void* data)
{
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        g_ap_client_connected = true;
        ESP_LOGW(kTag, "检测到手机接入设备 AP");
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        g_ap_client_connected = false;
        ESP_LOGW(kTag, "手机已断开设备 AP");
    }
    (void)data;
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

    // ================= 配网判定（必须在音频初始化之前）=================
    // 网页配网模式不初始化音频 / AFE / TTS，把内部 RAM 全让给 AP + HTTP 服务。
    // 凭据来源优先级：编译期配置 > 网页配网保存的 NVS > 旧 BLE 配网留下的配置
    static WifiStation sta;
    if (sta.init() != ESP_OK) {
        ESP_LOGE(kTag, "Wi-Fi 初始化失败");
        return;
    }

#if CONFIG_AMYAI_PROV_ENABLE
    // 开机时按住 BOOT 键 -> 恢复出厂：清掉凭据后重启，下次启动自动进配网
    if (prov_detect_reset_request()) {
        prov_reset();
        user_data_erase();
        ESP_LOGW(kTag, "已恢复出厂设置，1 秒后重启…");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
#endif

    std::string ssid     = CONFIG_AMYAI_WIFI_SSID;
    std::string password = CONFIG_AMYAI_WIFI_PASSWORD;
    if (ssid.empty()) {
        char s_buf[33] = {0};
        char p_buf[65] = {0};
        if (user_wifi_load(s_buf, sizeof(s_buf), p_buf, sizeof(p_buf))) {
            ssid     = s_buf;
            password = p_buf;
            ESP_LOGI(kTag, "使用网页配网保存的 Wi-Fi 凭据: \"%s\"", ssid.c_str());
        }
    }
#if CONFIG_AMYAI_PROV_ENABLE
    if (ssid.empty() && prov_load_credentials(ssid, password)) {
        ESP_LOGI(kTag, "使用旧的 BLE 配网凭据: \"%s\"", ssid.c_str());
    }
#endif

    // 没有任何 Wi-Fi 凭据 -> 进入网页配网（阻塞，保存后重启）
    if (ssid.empty()) {
        web_portal_run();
        return;
    }

    // 主控通信：先初始化 UART，后面识别出的指令直接走它
    if (uart_link_init()) {
        ESP_LOGI(kTag, "主控通信已就绪（UART1）");
    } else {
        ESP_LOGW(kTag, "主控通信未启用，指令只会打印到日志");
    }

    tts_init();
    reply_player_init();   // 流式回复播放器（边生成边播报）


    // 唤醒词：不依赖 Wi-Fi，先启动。引脚没配好时只报警告，不影响后面的联网流程。
#if CONFIG_AMYAI_AUDIO_PROBE
    // ★ 诊断：I2S 初始化前，把麦克风数据脚（IO47）当普通 GPIO 读电平，
    //   判断是"线悬空（麦没驱动）"还是"被拉低（短路/麦不出数据）"。
    {
        const gpio_num_t kMicPin = static_cast<gpio_num_t>(CONFIG_AMYAI_I2S_DIN);
        auto probe = [&](bool pull_up, const char* name) {
            gpio_config_t io = {};
            io.pin_bit_mask = 1ULL << kMicPin;
            io.mode         = GPIO_MODE_INPUT;
            io.pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
            io.pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
            io.intr_type    = GPIO_INTR_DISABLE;
            gpio_config(&io);
            int high = 0;
            for (int i = 0; i < 200; ++i) {
                if (gpio_get_level(kMicPin)) {
                    ++high;
                }
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            ESP_LOGW(kTag, "[麦克风线] 内部%s时 IO47 高电平 %d/200", name, high);
        };
        probe(true,  "上拉");
        probe(false, "下拉");
        ESP_LOGW(kTag, "[麦克风线] 判读：上拉读 1 且下拉读 0 = 线悬空（麦没驱动）；"
                       "两次都 0 = 被拉低；两次都 1 = 被拉高");
    }

#endif

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
                    // ★ 退出唤醒：只回一句告别语，不下发主控，之后进入休眠
                    if (local_cmd.find("\"sleep\"") != std::string::npos) {
                        ESP_LOGI(kTag, "【本地】退出唤醒（退下）");
                        if (music_is_playing()) {
                            music_stop();
                            wake.set_music_window(false);
                        }
                        local_play_bye();       // 「好的，有需要随时叫我」
                        wake.sleep_now();       // 停录音，等重新唤醒
                        return;
                    }
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
                // ★ 音乐窗口只处理两种事，其余一律忽略、不走云端：
                //   1) 停止类：直接停，静默
                //   2) 换歌类：直接换，静默
                //   音乐声被 ASR 识别出的碎片（"听不清"、""#" 等）太多，
                //   一旦送去云端就会冒出莫名其妙的回复，甚至误判成"放歌"。
                if (local_is_stop_command(text)) {
                    ESP_LOGI(kTag, "【音乐窗口】停止「%s」", text.c_str());
                    music_stop();
                    wake.set_music_window(false);
                    return;   // 返回时 guard 会自动开启连续聆听
                }
                if (local_is_music_command(text)) {
                    ESP_LOGI(kTag, "【音乐窗口】换歌「%s」", text.c_str());
                    if (music_next()) {
                        ESP_LOGI(kTag, "【音乐】换到：%s", music_current());
                    }
                    return;   // 保持音乐窗口，继续听
                }
                ESP_LOGI(kTag, "【音乐窗口】「%s」忽略（不是停止/换歌指令，音乐继续）", text.c_str());
                return;
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

            // 3. 文字 -> 带意图的 JSON（★ 流式：边生成边播报 reply）
            const int64_t t_llm0 = esp_timer_get_time();
            int64_t t_first_delta_us = 0;
            reply_player_begin();
            const std::string reply = ask_llm_stream(
                text, [&t_first_delta_us](const std::string& delta) {
                    if (t_first_delta_us == 0) {
                        t_first_delta_us = esp_timer_get_time();   // 第一段文字到达时间
                    }
                    reply_player_feed(delta);   // 切句 + 抖动缓冲 + 送 TTS
                });
            // 首字延迟：从发起请求到第一段文字到达（这才是流式的收益点）
            // 如果没有回调过，说明一次就返回了整段（模型很快或没走流式）
            const int64_t t_llm_begin = (t_first_delta_us > 0)
                                            ? (t_first_delta_us - t_llm0) / 1000
                                            : -1;
            reply_player_finish();
            const int64_t t_llm = (esp_timer_get_time() - t_llm0) / 1000;
            ESP_LOGI(kTag, "[耗时] ASR=%lld ms, LLM首字=%lld ms, LLM收完=%lld ms, 录音结束→LLM完成=%lld ms",
                     static_cast<long long>(t_asr), static_cast<long long>(t_llm_begin),
                     static_cast<long long>(t_llm),
                     static_cast<long long>((esp_timer_get_time() - t_cb0) / 1000));
            if (reply.empty()) {
                ESP_LOGE(kTag, "大模型调用失败（流式）");
                reply_player_wait_done(3000);
                tts_speak("网络不太好。");
                return;
            }
            ESP_LOGI(kTag, "模型返回: %s", reply.c_str());

            // 3. 按意图分流
            // ★ 模型偶尔会输出 \' （JSON 里非法的转义，多见于讲笑话时引用对话），
            //   先清洗掉，否则整条回复解析失败。
            std::string json_sane = reply;
            {
                size_t p = 0;
                while ((p = json_sane.find("\\'", p)) != std::string::npos) {
                    json_sane.replace(p, 2, "'");
                    ++p;
                }
            }
            cJSON* root = cJSON_Parse(json_sane.c_str());
            if (root == nullptr) {
                // JSON 不合法（最常见原因：回复太长被 max_tokens 截断）。
                // reply 字段的内容已经流式播报过了，这里不要再播兜底语（会打断播报），
                // 也不要下发指令（避免误动作）。
                ESP_LOGW(kTag, "模型返回不是合法 JSON（可能被 max_tokens 截断），保留已播报内容");
                reply_player_wait_done(20000);
                return;
            }
            cJSON* intent_item = cJSON_GetObjectItem(root, "intent");
            const char* intent = cJSON_IsString(intent_item) ? intent_item->valuestring : "control";

            if (std::strcmp(intent, "sleep") == 0) {
                // 云端也支持"退下"：回告别语后休眠
                ESP_LOGI(kTag, "【休眠】");
                if (music_is_playing()) {
                    music_stop();
                    wake.set_music_window(false);
                }
                local_play_bye();
                wake.sleep_now();
                cJSON_Delete(root);
                reply_player_wait_done(20000);
                return;
            }
            if (std::strcmp(intent, "chat") == 0) {
                cJSON* r = cJSON_GetObjectItem(root, "reply");
                if (cJSON_IsString(r) && r->valuestring[0] != '\0') {
                    ESP_LOGI(kTag, "【聊天】%s", r->valuestring);   // 已在流式阶段播过
                } else {
                    ESP_LOGW(kTag, "chat 意图但没给 reply");
                    tts_speak("这个我还没想好。");
                }
            } else if (std::strcmp(intent, "intro") == 0) {
                ESP_LOGI(kTag, "【产品介绍】");
                if (!reply_player_spoke_anything()) {
                    tts_speak(kIntroText);
                }
            } else if (std::strcmp(intent, "music") == 0) {
                // ★ 必须等回复播完再动音乐：流式播报还在响的时候开播，
                //   TTS（高优先级）会抢占音频通道，音乐任务 write 失败会直接退出。
                reply_player_wait_done(15000);
                cJSON* act = cJSON_GetObjectItem(root, "action");
                cJSON* q = cJSON_GetObjectItem(root, "query");
                const char* action = cJSON_IsString(act) ? act->valuestring : "play";
                if (std::strcmp(action, "stop") == 0 || std::strcmp(action, "pause") == 0) {
                    if (music_is_playing()) {
                        music_stop();
                        wake.set_music_window(false);
                        ESP_LOGI(kTag, "【音乐】已停止");   // 停下就好，不播报（用户要求静默）
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
                if (!reply_player_spoke_anything()) {
                    tts_speak("没听懂，你再说一次。");
                }
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
                        // 确认语已经在流式阶段播过了，这里只打个日志
                        cJSON* ack_text = cJSON_GetObjectItem(root, "reply");
                        if (cJSON_IsString(ack_text) && ack_text->valuestring[0] != '\0') {
                            ESP_LOGI(kTag, "【确认】%s", ack_text->valuestring);
                        }
#endif
                    } else {
                        ESP_LOGE(kTag, "主控没有确认，指令可能没送到");
                        tts_speak("指令没送到。");
                    }
                }
            }
            cJSON_Delete(root);
            // 等流式播报播完，再回去继续聆听
            reply_player_wait_done(20000);
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
        "asr_init", 8 * 1024, nullptr, 2, nullptr);

    if (wake_ok) {
        ESP_LOGI(kTag, "等待唤醒词: \"%s\"", CONFIG_AMYAI_WAKE_TEXT);
    } else {
        ESP_LOGW(kTag, "唤醒功能未启动（多半是 I2S 引脚没配置），继续跑联网部分");
    }


    // 开机内存快照：内部 RAM 是本板最紧张的资源（WiFi + TLS 都从这里要）
    AMYAI_DIAGI(kTag, "[内存] 连网前：内部 %u 空闲（最大块 %u）| PSRAM %u 空闲",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));


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

    // ================= 阶段 2：AP 常开（手机随时可扫码进入配置页）=================
    {
        uint8_t primary = 0;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&primary, &second);   // AP 必须跟随 STA 信道
        web_portal_ap_start(primary);
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                            &on_ap_client_event, nullptr, nullptr);
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                            &on_ap_client_event, nullptr, nullptr);
        ESP_LOGW(kTag, "AP 已常开（隐藏网络）。扫码连上后会自动进入配置模式，"
                       "可修改身体信息；配置完成后需要重新唤醒");
    }

    // 主循环：500ms 一拍，负责配置模式切换 + 网络看门狗
    constexpr int64_t kCfgTimeoutUs = 5LL * 60 * 1000 * 1000;   // 配置模式最长 5 分钟
    int net_fail = 0;
    int tick = 0;
    bool cfg_active = false;
    bool wait_disconnect = false;   // 退出配置模式后，等手机真正断开才允许再进入
    int64_t cfg_start_us = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));

        // ---- 每 2 秒处理一次配置模式 ----
        if (++tick % 4 == 0) {
            // 兜底：如果手机在网络就绪之前就连上了，事件会漏掉，这里主动查一次
            if (!g_ap_client_connected && !cfg_active) {
                wifi_sta_list_t sta_list = {};
                if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num > 0) {
                    g_ap_client_connected = true;
                    ESP_LOGW(kTag, "检测到手机接入设备 AP（轮询兜底）");
                }
            }
            const bool want = g_ap_client_connected;
            if (!want) {
                wait_disconnect = false;   // 手机断开了，下次接入可以重新进入
            }
            if (want && !cfg_active && !wait_disconnect) {
                cfg_active = true;
                cfg_start_us = esp_timer_get_time();
                ESP_LOGW(kTag, "==== 进入配置模式：暂停语音 + 启动配置页 ====");
                if (music_is_playing()) {
                    music_stop();
                    wake.set_music_window(false);
                }
                wake.pause();                  // 唤醒任务完全暂停（不录音不检测）
                web_portal_http_start();
            } else if (cfg_active &&
                       (!want || web_portal_config_done_take() ||
                        esp_timer_get_time() - cfg_start_us > kCfgTimeoutUs)) {
                cfg_active = false;
                wait_disconnect = true;        // 手机还连着的话不要立刻又进来
                web_portal_http_stop();
                wake.resume();
                wake.sleep_now();              // 回到"需要重新唤醒"状态
                ESP_LOGW(kTag, "==== 退出配置模式：恢复语音，需要重新喊唤醒词 ====");
            }
        }

        // ---- 每 60 秒做一次网络检查（配置模式下跳过，避免误重启）----
        if (tick % 120 == 0 && !cfg_active) {
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
}
