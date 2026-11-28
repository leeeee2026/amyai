// 网页配网（模式 A）
//
// 用户手机扫码 → 自动连上设备 AP → 自动弹出配置页 → 填 Wi-Fi 和身体信息 → 保存重启。
//
// 内存策略：配网模式下不初始化音频/AFE/TTS，内部 RAM 全部留给 AP + HTTP 服务。
// 实测配置：AP ~12KB、HTTP ~8KB，配网时内部 RAM 空闲 ~60KB，余量充足。

#include "web_portal.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "device_cfg.h"
#include "diag.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "uart_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web_page.h"
#include "wifi_prov.h"

namespace {

constexpr const char* kTag = "portal";

// ---- AP 参数（二维码里印的就是这一组）----
constexpr const char* kApSsid = "AMYAI-CHAIR";
constexpr const char* kApPass = "Amyai2k8m4x7p";   // 13 位强密码（无易混字符）
constexpr bool        kApHidden = false;           // 广播 SSID：手机能直接搜到，扫码也能加
constexpr uint8_t     kApChannel = 1;
constexpr uint8_t     kApMaxConn = 2;

httpd_handle_t s_server = nullptr;
esp_netif_t*   s_ap_netif = nullptr;
bool           s_ap_running = false;

// 扫描结果缓存（进页面时扫一次，避免反复扫描打断手机连接）
struct ApItem {
    std::string ssid;
    int rssi;
    int channel;
};
std::vector<ApItem> s_scan_cache;

// 身体信息保存标记（保存后重启进入正常模式）
volatile bool s_profile_saved = false;
// Wi-Fi 已提交，需要去连接（配网页提交后触发，不重启）
volatile bool s_need_connect = false;
// STA 连接结果（页面轮询用）
volatile bool s_sta_connected = false;
volatile bool s_sta_failed = false;
// 本次配网是否提交过 Wi-Fi：连上后据此自动退出配置模式
volatile bool s_wifi_saved = false;
// 页面请求进入"配网模式"：只有这个为 true 时才暂停语音
volatile bool s_prov_req = false;

// ---------------------------------------------------------------- STA 连接
esp_netif_t* sta_netif()
{
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

bool sta_has_ip()
{
    esp_netif_t* n = sta_netif();
    if (n == nullptr) {
        return false;
    }
    esp_netif_ip_info_t ip{};
    return esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0;
}

// 从扫描缓存里查某个 SSID 的信道（找不到返回 0）
int find_channel(const char* ssid)
{
    if (ssid == nullptr) {
        return 0;
    }
    for (const auto& ap : s_scan_cache) {
        if (ap.ssid == ssid) {
            return ap.channel;
        }
    }
    return 0;
}

// 把 SoftAP 切到指定信道。
// ★ 必须做：ESP32 只有一个射频，AP 和 STA 必须在同一信道才能连上。
//   手机连在我们的 AP 上（比如 1 信道），家里路由在 6 信道，
//   不切信道的话 STA 永远连不上（实测就是这样失败的）。
bool switch_ap_channel(int channel)
{
    if (channel <= 0 || channel > 13) {
        return false;
    }
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) != ESP_OK) {
        return false;
    }
    if (cfg.ap.channel == static_cast<uint8_t>(channel)) {
        return true;   // 已经是这个信道，不用动
    }
    const uint8_t old_ch = cfg.ap.channel;
    cfg.ap.channel = static_cast<uint8_t>(channel);
    if (esp_wifi_set_config(WIFI_IF_AP, &cfg) != ESP_OK) {
        ESP_LOGW(kTag, "切换 AP 信道失败");
        return false;
    }
    ESP_LOGW(kTag, "AP 信道已从 %u 切到 %d（和家里路由一致，否则 STA 连不上）",
             static_cast<unsigned>(old_ch), channel);
    return true;
}

// ---------------------------------------------------------------- Wi-Fi 扫描
void do_scan()
{
    s_scan_cache.clear();

    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    cfg.scan_type   = WIFI_SCAN_TYPE_ACTIVE;

    esp_err_t err = esp_wifi_scan_start(&cfg, true);   // 阻塞扫描
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "扫描失败: %s", esp_err_to_name(err));
        return;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        return;
    }
    if (num > 24) {
        num = 24;   // 最多取 24 个，够用且省内存
    }
    auto* recs = static_cast<wifi_ap_record_t*>(calloc(num, sizeof(wifi_ap_record_t)));
    if (recs == nullptr) {
        return;
    }
    uint16_t got = num;
    if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
        for (uint16_t i = 0; i < got; ++i) {
            const char* s = reinterpret_cast<const char*>(recs[i].ssid);
            if (s[0] == '\0') {
                continue;
            }
            s_scan_cache.push_back({std::string(s), recs[i].rssi, recs[i].primary});
        }
    }
    free(recs);
    ESP_LOGI(kTag, "扫描到 %u 个 Wi-Fi", static_cast<unsigned>(s_scan_cache.size()));
}

// ---------------------------------------------------------------- HTTP 处理
// 设备是否已有 Wi-Fi 凭据（决定显示哪个页面）
bool has_wifi_credentials()
{
    // 新方案：我们自己的 NVS 记录
    char ssid[33] = {0};
    if (user_wifi_load(ssid, sizeof(ssid), nullptr, 0)) {
        return true;
    }
    // 兜底：老版本用 BLE 配网时凭据存在 WiFi 驱动里，这类设备也算"已配网"
    std::string legacy_ssid;
    std::string legacy_pwd;
    return prov_load_credentials(legacy_ssid, legacy_pwd);
}

esp_err_t handle_root(httpd_req_t* req)
{
    ESP_LOGI(kTag, "→ GET / （页面请求）");
    // 单页面：Wi-Fi 配置和身体信息都在同一页里，
    // 由页面 JS 根据 /state 决定显示哪一块，服务端不做分页。
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kWebPage, HTTPD_RESP_USE_STRLEN);
}

// URL 解码（GET 提交时 ssid/pwd 会被百分号编码）
void url_decode(char* s)
{
    char* w = s;
    for (const char* r = s; *r != '\0'; ++r) {
        if (*r == '%' && r[1] != '\0' && r[2] != '\0') {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int h = hex(r[1]);
            const int l = hex(r[2]);
            if (h >= 0 && l >= 0) {
                *w++ = static_cast<char>((h << 4) | l);
                r += 2;
                continue;
            }
        }
        if (*r == '+') {
            *w++ = ' ';
            continue;
        }
        *w++ = *r;
    }
    *w = '\0';
}

// 兜底接口：用 GET 提交（部分手机浏览器在无外网环境下会重置 POST 连接）
esp_err_t handle_save_get(httpd_req_t* req)
{
    char query[320] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_sendstr(req, R"({"ok":false,"error":"缺少参数"})");
        return ESP_OK;
    }
    char ssid[64] = {0};
    char pwd[80] = {0};
    if (httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        httpd_resp_sendstr(req, R"({"ok":false,"error":"Wi-Fi 名称不能为空"})");
        return ESP_OK;
    }
    httpd_query_key_value(query, "pwd", pwd, sizeof(pwd));
    url_decode(ssid);   // GET 参数是百分号编码的
    url_decode(pwd);
    ESP_LOGW(kTag, "→ GET /save2：Wi-Fi=\"%s\"", ssid);
    if (!user_wifi_save(ssid, pwd)) {
        httpd_resp_sendstr(req, R"({"ok":false,"error":"保存失败"})");
        return ESP_OK;
    }
    s_sta_connected = false;
    s_sta_failed    = false;
    s_need_connect  = true;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, R"({"ok":true})");
    return ESP_OK;
}

// 配网进度查询：页面提交 Wi-Fi 后轮询这个，连上了就自动跳到身体信息页
esp_err_t handle_state(httpd_req_t* req)
{
    // ★ s_sta_connected 只在"网页里提交 Wi-Fi 后主动连接"的流程里更新；
    //   设备开机自己连上的情况它是 false，所以这里必须查驱动的真实状态，
    //   否则页面一直显示"未连接网络"。
    wifi_ap_record_t ap{};
    const bool really_connected = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    ESP_LOGI(kTag, "→ GET /state (provisioned=%d connected=%d failed=%d)",
             has_wifi_credentials() ? 1 : 0, really_connected ? 1 : 0, s_sta_failed ? 1 : 0);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "provisioned", has_wifi_credentials());
    cJSON_AddBoolToObject(o, "connected", really_connected);
    cJSON_AddBoolToObject(o, "failed", s_sta_failed);
    cJSON_AddBoolToObject(o, "prov", s_prov_req);
    if (really_connected) {
        cJSON_AddStringToObject(o, "ssid", reinterpret_cast<const char*>(ap.ssid));
        cJSON_AddNumberToObject(o, "rssi", ap.rssi);
    }
    char* out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, out != nullptr ? out : "{}");
    if (out != nullptr) {
        cJSON_free(out);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- 偏好设置 JSON
namespace {
int jget(cJSON* o, const char* k, int def)
{
    cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}
int jclamp(int v, int lo, int hi, int def)
{
    return (v >= lo && v <= hi) ? v : def;
}
// 枚举型参数：不在允许集合里就回退默认值，避免主控收到无意义参数
int jpick(int v, const int* allowed, int n, int def)
{
    for (int i = 0; i < n; ++i) {
        if (v == allowed[i]) {
            return v;
        }
    }
    return def;
}
}  // namespace

cJSON* prefs_to_json(const UserPrefs& p)
{
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "program",      p.program);
    cJSON_AddNumberToObject(o, "strength",     p.strength);
    cJSON_AddNumberToObject(o, "speed",        p.speed);
    cJSON_AddNumberToObject(o, "duration",     p.duration_min);
    cJSON_AddNumberToObject(o, "parts",        p.parts_mask);
    cJSON_AddNumberToObject(o, "air_neck",     p.air_neck);
    cJSON_AddNumberToObject(o, "air_shoulder", p.air_shoulder);
    cJSON_AddNumberToObject(o, "air_arm",      p.air_arm);
    cJSON_AddNumberToObject(o, "air_leg",      p.air_leg);
    cJSON_AddNumberToObject(o, "air_foot",     p.air_foot);
    cJSON_AddNumberToObject(o, "foot_roller",  p.foot_roller);
    cJSON_AddNumberToObject(o, "heat",         p.heat_mask);
    cJSON_AddNumberToObject(o, "heat_temp",    p.heat_temp);
    cJSON_AddNumberToObject(o, "volume",       p.volume);
    cJSON_AddNumberToObject(o, "tts_speed",    p.tts_speed);
    cJSON_AddNumberToObject(o, "tts_voice",    p.tts_voice);
    cJSON_AddNumberToObject(o, "follow_up",    p.follow_up_sec);
    cJSON_AddNumberToObject(o, "ack",          p.ack_enabled);
    cJSON_AddNumberToObject(o, "child_lock",   p.child_lock);
    cJSON_AddNumberToObject(o, "sedentary",    p.sedentary_min);
    return o;
}

void json_to_prefs(cJSON* o, UserPrefs* p)
{
    if (o == nullptr || p == nullptr) {
        return;
    }
    UserPrefs d = *p;
    const int kDur[]   = {10, 20, 30, 45};
    const int kTemp[]  = {40, 45, 50};
    const int kFollow[] = {0, 15, 60, 600};
    const int kSed[]   = {0, 30, 60, 120};

    d.program       = static_cast<uint8_t>(jclamp(jget(o, "program",      d.program),       0,   5, d.program));
    d.strength      = static_cast<uint8_t>(jclamp(jget(o, "strength",     d.strength),      1,   5, d.strength));
    d.speed         = static_cast<uint8_t>(jclamp(jget(o, "speed",        d.speed),         0,   2, d.speed));
    d.duration_min  = static_cast<uint8_t>(jpick(jget(o, "duration",      d.duration_min),  kDur, 4, d.duration_min));
    d.parts_mask    = static_cast<uint8_t>(jclamp(jget(o, "parts",        d.parts_mask),    0, 127, d.parts_mask));
    d.air_neck      = static_cast<uint8_t>(jclamp(jget(o, "air_neck",     d.air_neck),      0,   2, d.air_neck));
    d.air_shoulder  = static_cast<uint8_t>(jclamp(jget(o, "air_shoulder", d.air_shoulder),  0,   2, d.air_shoulder));
    d.air_arm       = static_cast<uint8_t>(jclamp(jget(o, "air_arm",      d.air_arm),       0,   2, d.air_arm));
    d.air_leg       = static_cast<uint8_t>(jclamp(jget(o, "air_leg",      d.air_leg),       0,   2, d.air_leg));
    d.air_foot      = static_cast<uint8_t>(jclamp(jget(o, "air_foot",     d.air_foot),      0,   2, d.air_foot));
    d.foot_roller   = static_cast<uint8_t>(jclamp(jget(o, "foot_roller",  d.foot_roller),   0,   3, d.foot_roller));
    d.heat_mask     = static_cast<uint8_t>(jclamp(jget(o, "heat",         d.heat_mask),     0,  31, d.heat_mask));
    d.heat_temp     = static_cast<uint8_t>(jpick(jget(o, "heat_temp",     d.heat_temp),     kTemp, 3, d.heat_temp));
    d.volume        = static_cast<uint8_t>(jclamp(jget(o, "volume",       d.volume),        0, 100, d.volume));
    d.tts_speed     = static_cast<uint8_t>(jclamp(jget(o, "tts_speed",    d.tts_speed),     0,   2, d.tts_speed));
    d.tts_voice     = static_cast<uint8_t>(jclamp(jget(o, "tts_voice",    d.tts_voice),     0,   2, d.tts_voice));
    d.follow_up_sec = static_cast<uint16_t>(jpick(jget(o, "follow_up",    d.follow_up_sec), kFollow, 4, d.follow_up_sec));
    d.ack_enabled   = static_cast<uint8_t>(jclamp(jget(o, "ack",          d.ack_enabled),   0,   1, d.ack_enabled));
    d.child_lock    = static_cast<uint8_t>(jclamp(jget(o, "child_lock",   d.child_lock),    0,   1, d.child_lock));
    d.sedentary_min = static_cast<uint8_t>(jpick(jget(o, "sedentary",     d.sedentary_min), kSed, 4, d.sedentary_min));
    *p = d;
}

// 返回已保存的用户信息 + 当前 Wi-Fi，页面加载时回填用
esp_err_t handle_user(httpd_req_t* req)
{
    ESP_LOGI(kTag, "→ GET /user");
    UserProfile prof{};
    user_profile_load(&prof);

    char ssid[33] = {0};
    user_wifi_load(ssid, sizeof(ssid), nullptr, 0);

    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "height", prof.height);
    cJSON_AddNumberToObject(o, "weight", prof.weight);
    cJSON_AddNumberToObject(o, "age",    prof.age);
    cJSON_AddNumberToObject(o, "gender", prof.gender);
    cJSON_AddStringToObject(o, "ssid", ssid);

    UserPrefs prefs{};
    user_prefs_load(&prefs);
    cJSON_AddItemToObject(o, "prefs", prefs_to_json(prefs));

    char* out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out != nullptr ? out : "{}");
    if (out != nullptr) {
        cJSON_free(out);
    }
    return ESP_OK;
}

esp_err_t handle_scan(httpd_req_t* req)
{
    ESP_LOGI(kTag, "→ GET /scan"); 
    do_scan();

    cJSON* arr = cJSON_CreateArray();
    for (const auto& ap : s_scan_cache) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", ap.ssid.c_str());
        cJSON_AddNumberToObject(item, "rssi", ap.rssi);
        cJSON_AddItemToArray(arr, item);
    }
    char* out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (out == nullptr) {
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return ESP_OK;
}

// 读取请求体（限制在 1KB 内，页面只会发很小的 JSON）
int read_body(httpd_req_t* req, char* buf, size_t cap)
{
    const size_t want = (req->content_len < cap - 1) ? req->content_len : cap - 1;
    size_t got = 0;
    while (got < want) {
        const int n = httpd_req_recv(req, buf + got, want - got);
        if (n <= 0) {
            return -1;
        }
        got += static_cast<size_t>(n);
    }
    buf[got] = '\0';
    return static_cast<int>(got);
}

esp_err_t handle_save(httpd_req_t* req)
{
    ESP_LOGW(kTag, "→ POST /save 收到请求，content_len=%u", static_cast<unsigned>(req->content_len));
    char body[1024];
    if (read_body(req, body, sizeof(body)) <= 0) {
        httpd_resp_sendstr(req, R"({"ok":false,"error":"请求为空"})");
        return ESP_OK;
    }

    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        httpd_resp_sendstr(req, R"({"ok":false,"error":"数据格式错误"})");
        return ESP_OK;
    }

    cJSON* j_ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON* j_pwd  = cJSON_GetObjectItem(root, "password");
    // ★ 必须拷贝成 std::string：后面会 cJSON_Delete(root)，
    //   直接用 valuestring 指针会变成悬空指针（实测存进去的 SSID 是乱码）
    const std::string ssid = cJSON_IsString(j_ssid) ? j_ssid->valuestring : "";
    const std::string pwd  = cJSON_IsString(j_pwd) ? j_pwd->valuestring : "";

    // 注意：ssid 为空是合法的 —— 表示这是"身体信息页"的提交

    // 身体信息（全部可选）
    UserProfile prof{};
    user_profile_load(&prof);   // 先读旧的，避免用户这次没填就清空
    cJSON* h = cJSON_GetObjectItem(root, "height");
    cJSON* w = cJSON_GetObjectItem(root, "weight");
    cJSON* a = cJSON_GetObjectItem(root, "age");
    cJSON* g = cJSON_GetObjectItem(root, "gender");
    auto clamp_u8 = [](int v, int lo, int hi) -> uint8_t {
        if (v < lo || v > hi) return 0;          // 超出合理范围当"未填"
        return static_cast<uint8_t>(v);
    };
    if (cJSON_IsNumber(h)) prof.height = clamp_u8(h->valueint, 80, 220);
    if (cJSON_IsNumber(w)) prof.weight = clamp_u8(w->valueint, 25, 200);
    if (cJSON_IsNumber(a)) prof.age    = clamp_u8(a->valueint, 10, 100);
    if (cJSON_IsNumber(g)) {
        const int gv = g->valueint;
        prof.gender = (gv == 1 || gv == 2) ? static_cast<uint8_t>(gv) : 0;
    }

    // 偏好设置（可选）：解析 + 记录原始 JSON 用于下发主控
    UserPrefs prefs{};
    user_prefs_load(&prefs);
    bool has_prefs = false;
    std::string prefs_uart;
    cJSON* j_prefs = cJSON_GetObjectItem(root, "prefs");
    if (cJSON_IsObject(j_prefs)) {
        json_to_prefs(j_prefs, &prefs);
        has_prefs = true;
        char* s = cJSON_PrintUnformatted(j_prefs);
        if (s != nullptr) {
            prefs_uart = std::string(R"({"cmd":"set_cfg","prefs":)") + s + "}";
            cJSON_free(s);
        }
    }
    cJSON_Delete(root);

    ESP_LOGI(kTag, "收到配置：Wi-Fi=\"%s\" 身高=%u 体重=%u 年龄=%u 性别=%u",
             ssid.c_str(), prof.height, prof.weight, prof.age, prof.gender);

    // ★ 两种提交分开处理：
    //   - 带 ssid：Wi-Fi 配置页（首次配网）→ 保存后重启
    //   - 不带 ssid：身体信息页（已配网）→ 只存信息，不重启
    if (!ssid.empty()) {
        const bool ok_wifi = user_wifi_save(ssid.c_str(), pwd.c_str());
        if (!ok_wifi) {
            httpd_resp_sendstr(req, R"({"ok":false,"error":"Wi-Fi 保存失败，请重试"})");
            return ESP_OK;
        }
        user_profile_save(prof);      // 顺手保存（页面没填就是保持原值）
        if (has_prefs) {
            user_prefs_save(prefs);
            if (!prefs_uart.empty()) {
                uart_link_send_notify(prefs_uart);
            }
        }
        // ★ 不重启：马上发起 STA 连接，连上后页面会自动跳到身体信息页
        s_sta_connected = false;
        s_sta_failed    = false;
        s_wifi_saved    = true;      // 记下"这次配网提交过 Wi-Fi"，连上后好自动退出
        s_need_connect  = true;
        ESP_LOGW(kTag, "Wi-Fi 配置已保存，开始连接（不重启，连上后跳到身体信息页）");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, R"({"ok":true})");
        return ESP_OK;
    }

    // 身体信息：只保存，不回重启
    if (!user_profile_save(prof)) {
        httpd_resp_sendstr(req, R"({"ok":false,"error":"保存失败，请重试"})");
        return ESP_OK;
    }
    s_profile_saved = true;
    ESP_LOGI(kTag, "身体信息已保存");

    // 偏好设置：存 NVS + 下发主控（不等 ACK，主控没接也不阻塞页面）
    if (has_prefs) {
        if (!user_prefs_save(prefs)) {
            // NVS 写失败要如实告诉页面，否则用户以为存上了
            ESP_LOGE(kTag, "偏好保存失败");
            httpd_resp_sendstr(req, R"({"ok":false,"error":"设置保存失败，请重试"})");
            return ESP_OK;
        }
        ESP_LOGI(kTag, "偏好已保存，开始下发主控");
        if (!prefs_uart.empty()) {
            uart_link_send_notify(prefs_uart);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, R"({"ok":true})");
    return ESP_OK;
}

// 页面请求进入/退出"配网模式"
//   {"on":true}  -> 暂停语音，Wi-Fi 卡显示表单
//   {"on":false} -> 恢复语音，Wi-Fi 卡显示当前网络
esp_err_t handle_prov(httpd_req_t* req)
{
    char body[128];
    bool on = false;
    if (read_body(req, body, sizeof(body)) > 0) {
        cJSON* r = cJSON_Parse(body);
        if (r != nullptr) {
            cJSON* j = cJSON_GetObjectItem(r, "on");
            if (cJSON_IsBool(j)) {
                on = cJSON_IsTrue(j);
            } else if (cJSON_IsNumber(j)) {
                on = (j->valueint != 0);
            }
            cJSON_Delete(r);
        }
    }
    s_prov_req = on;
    ESP_LOGW(kTag, "→ POST /prov 配网模式=%s", on ? "开" : "关");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, on ? R"({"ok":true,"prov":true})" : R"({"ok":true,"prov":false})");
    return ESP_OK;
}

// 恢复出厂：清空 NVS 用户数据后重启
esp_err_t handle_reset(httpd_req_t* req){
    ESP_LOGW(kTag, "→ GET /reset 恢复出厂设置");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, R"({"ok":true,"msg":"正在恢复出厂并重启"})");
    vTaskDelay(pdMS_TO_TICKS(400));   // 让响应先发出去
    user_data_erase();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

// Captive portal 探测接口：让手机连上后自动弹出配置页
esp_err_t handle_captive(httpd_req_t* req){
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// ---------------------------------------------------------------- 启动 AP
bool start_ap_impl(int channel)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == nullptr) {
        ESP_LOGE(kTag, "创建 AP 网络接口失败");
        return false;
    }

    // APSTA：AP 给手机连，STA 暂时不连（但要有 STA 接口才能扫描）
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_cfg = {};
    std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), kApSsid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len       = static_cast<uint8_t>(std::strlen(kApSsid));
    std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), kApPass,
                 sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.channel        = (channel > 0) ? static_cast<uint8_t>(channel) : kApChannel;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = kApMaxConn;
    ap_cfg.ap.ssid_hidden    = kApHidden ? 1 : 0;   // 隐藏 SSID，邻居搜不到
    ap_cfg.ap.beacon_interval = 200;

    if (esp_wifi_set_config(WIFI_IF_AP, &ap_cfg) != ESP_OK) {
        ESP_LOGE(kTag, "配置 AP 失败");
        return false;
    }
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(kTag, "启动 Wi-Fi 失败");
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_ap_running = true;
    ESP_LOGW(kTag, "======== 网页配网已启动 ========");
    ESP_LOGW(kTag, "Wi-Fi 名称: %s（%s）", kApSsid, kApHidden ? "隐藏网络" : "已广播");
    ESP_LOGW(kTag, "Wi-Fi 密码: %s", kApPass);
    ESP_LOGW(kTag, "配置页地址: http://192.168.4.1/");
    ESP_LOGW(kTag, "二维码内容: WIFI:T:WPA;S:%s;P:%s;H:%s;;", kApSsid, kApPass,
             kApHidden ? "true" : "false");

    // AP 下的手机由 ESP32 的 DHCP 服务自动分配 192.168.4.x
    esp_netif_ip_info_t ip{};
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        ESP_LOGI(kTag, "AP 网关: " IPSTR, IP2STR(&ip.ip));
    }
    return true;
}

bool start_http_impl()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;   // 8 个业务接口 + 6 个 captive 探测，留余量
    cfg.lru_purge_enable = true;
    cfg.server_port      = 80;
    cfg.stack_size       = 6144;   // 处理 JSON 用，稍微给足一点

    if (s_server != nullptr) {
        return true;   // 已经在跑
    }
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(kTag, "启动 HTTP 服务失败");
        s_server = nullptr;
        return false;
    }

    httpd_uri_t u_root = {};
    u_root.uri = "/";
    u_root.method = HTTP_GET;
    u_root.handler = handle_root;
    httpd_register_uri_handler(s_server, &u_root);

    httpd_uri_t u_scan = {};
    u_scan.uri = "/scan";
    u_scan.method = HTTP_GET;
    u_scan.handler = handle_scan;
    httpd_register_uri_handler(s_server, &u_scan);

    httpd_uri_t u_state = {};
    u_state.uri = "/state";
    u_state.method = HTTP_GET;
    u_state.handler = handle_state;
    httpd_register_uri_handler(s_server, &u_state);

    httpd_uri_t u_save2 = {};
    u_save2.uri = "/save2";
    u_save2.method = HTTP_GET;
    u_save2.handler = handle_save_get;
    httpd_register_uri_handler(s_server, &u_save2);

    httpd_uri_t u_user = {};
    u_user.uri = "/user";
    u_user.method = HTTP_GET;
    u_user.handler = handle_user;
    httpd_register_uri_handler(s_server, &u_user);

    httpd_uri_t u_reset = {};
    u_reset.uri = "/reset";
    u_reset.method = HTTP_POST;   // 状态变更用 POST，避免被 <img src> 之类的 GET 误触发
    u_reset.handler = handle_reset;
    httpd_register_uri_handler(s_server, &u_reset);

    httpd_uri_t u_prov = {};
    u_prov.uri = "/prov";
    u_prov.method = HTTP_POST;
    u_prov.handler = handle_prov;
    httpd_register_uri_handler(s_server, &u_prov);

    httpd_uri_t u_save = {};
    u_save.uri = "/save";
    u_save.method = HTTP_POST;
    u_save.handler = handle_save;
    httpd_register_uri_handler(s_server, &u_save);

    // captive portal 探测（安卓 / iOS 各一个，都 302 到首页）
    static const char* kProbe[] = {
        "/generate_204",
        "/gen_204",
        "/hotspot-detect.html",
        "/library/test/success.html",
        "/ncsi.txt",
        "/connecttest.txt",
    };
    for (const char* p : kProbe) {
        httpd_uri_t u = {};
        u.uri = p;
        u.method = HTTP_GET;
        u.handler = handle_captive;
        httpd_register_uri_handler(s_server, &u);
    }

    ESP_LOGI(kTag, "HTTP 服务已启动（端口 80）");
    return true;
}

}  // namespace

bool web_portal_ap_running()
{
    return s_ap_running;
}

// ---- 阶段 2 接口 ----

bool web_portal_ap_start(int channel)
{
    if (s_ap_running) {
        return true;
    }
    ESP_LOGW(kTag, "启动常驻 AP（用于手机扫码/连接进入配置页）");
    return start_ap_impl(channel);
}

bool web_portal_http_start()
{
    return start_http_impl();
}

void web_portal_http_stop()
{
    if (s_server != nullptr) {
        httpd_stop(s_server);
        s_server = nullptr;
        ESP_LOGI(kTag, "HTTP 服务已关闭");
    }
}

bool web_portal_config_done_take()
{
    if (s_profile_saved) {
        s_profile_saved = false;
        return true;
    }
    return false;
}

// 本次配网提交过 Wi-Fi 且现在已连上 → 返回 true（取走标志，只触发一次）
bool web_portal_wifi_done_take()
{
    if (s_wifi_saved && s_sta_connected) {
        s_wifi_saved = false;
        return true;
    }
    return false;
}

// 页面是否请求进入"配网模式"（此时暂停语音）
bool web_portal_prov_wanted()
{
    return s_prov_req;
}

// 强制退出配网模式（超时兜底用）
void web_portal_prov_clear()
{
    s_prov_req = false;
}

void web_portal_run()
{
    ESP_LOGW(kTag, "没有 Wi-Fi 凭据，进入网页配网模式（不初始化音频，内存全留给配网）");

    if (!start_ap_impl(0)) {
        ESP_LOGE(kTag, "配网启动失败，重启重试");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    if (!start_http_impl()) {
        ESP_LOGE(kTag, "HTTP 启动失败，重启重试");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // 先扫一次，用户打开页面就有列表可选
    do_scan();

    AMYAI_DIAGI(kTag, "[内存] 配网模式：内部 %u 空闲（最大块 %u）| PSRAM %u 空闲",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    // 主循环：等用户提交
    //   ① 提交 Wi-Fi -> 立即连接（不重启），连上后页面自动跳身体信息页
    //   ② 提交身体信息 -> 保存后重启进入正常工作模式
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (s_need_connect) {
            s_need_connect = false;
            s_sta_failed   = false;
            s_sta_connected = false;

            // ★ 先把 AP 切到目标路由的信道，否则 AP+STA 不同信道时 STA 连不上
            {
                char saved_ssid[33] = {0};
                user_wifi_load(saved_ssid, sizeof(saved_ssid), nullptr, 0);
                const int ch = find_channel(saved_ssid);
                if (ch > 0) {
                    ESP_LOGW(kTag, "目标 Wi-Fi \"%s\" 在 %d 信道", saved_ssid, ch);
                    switch_ap_channel(ch);
                    vTaskDelay(pdMS_TO_TICKS(800));   // 等 AP 换好信道
                } else {
                    ESP_LOGW(kTag, "扫描缓存里没找到目标信道，直接尝试连接");
                }
            }

            ESP_LOGW(kTag, "开始连接 Wi-Fi…");
            esp_wifi_connect();

            // 最多等 25 秒（密码错/信号差就给页面报失败，让用户重试）
            const int64_t deadline = esp_timer_get_time() + 25LL * 1000 * 1000;
            bool ok = false;
            while (esp_timer_get_time() < deadline) {
                if (sta_has_ip()) {
                    ok = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            s_sta_connected = ok;
            if (ok) {
                s_wifi_saved = false;    // 已经连上，不用再等
                s_prov_req   = false;    // 配网完成：页面切回"当前网络"，语音恢复
                esp_netif_ip_info_t ip{};
                if (esp_netif_get_ip_info(sta_netif(), &ip) == ESP_OK) {
                    ESP_LOGW(kTag, "Wi-Fi 连接成功！IP: " IPSTR "（页面切到身体信息页）", IP2STR(&ip.ip));
                }
            } else {
                s_sta_failed = true;
                ESP_LOGE(kTag, "Wi-Fi 连接失败（密码错误或信号太差）");
                esp_wifi_disconnect();
            }
        }

        if (s_profile_saved) {
            s_profile_saved = false;
            ESP_LOGW(kTag, "身体信息已保存，2 秒后重启进入正常工作模式…");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
    }
}
