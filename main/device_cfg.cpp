#include "device_cfg.h"

#include <cstring>

#include "diag.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr const char* kTag = "cfg";

// 用户身体信息
constexpr const char* kNsUser = "user";
constexpr const char* kKeyProf = "profile";
constexpr const char* kKeyPrefs = "prefs";

// Wi-Fi 凭据（我们自己的副本，和 WiFi 驱动内部存储互相独立）
constexpr const char* kNsWifi = "amyai_wifi";
constexpr const char* kKeySsid = "ssid";
constexpr const char* kKeyPwd  = "pwd";

}  // namespace

bool user_profile_load(UserProfile* out)
{
    if (out == nullptr) {
        return false;
    }
    nvs_handle_t h = 0;
    if (nvs_open(kNsUser, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(UserProfile);
    UserProfile tmp{};
    const esp_err_t err = nvs_get_blob(h, kKeyProf, &tmp, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(UserProfile)) {
        return false;
    }
    *out = tmp;
    return true;
}

bool user_profile_save(const UserProfile& in)
{
    nvs_handle_t h = 0;
    if (nvs_open(kNsUser, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(kTag, "打开 NVS(user) 失败");
        return false;
    }
    const esp_err_t err = nvs_set_blob(h, kKeyProf, &in, sizeof(UserProfile));
    if (err == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "保存用户信息失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "已保存用户信息：身高 %u cm / 体重 %u kg / 年龄 %u / 性别 %u",
             in.height, in.weight, in.age, in.gender);
    return true;
}

bool user_wifi_save(const char* ssid, const char* password)
{
    if (ssid == nullptr || ssid[0] == '\0') {
        return false;
    }

    // ① 写我们自己的记录（开机判断"有没有配过网"用这个，不依赖 WiFi 驱动的内部存储）
    nvs_handle_t h = 0;
    bool ok = false;
    if (nvs_open(kNsWifi, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t e1 = nvs_set_str(h, kKeySsid, ssid);
        esp_err_t e2 = nvs_set_str(h, kKeyPwd, password != nullptr ? password : "");
        if (e1 == ESP_OK && e2 == ESP_OK) {
            nvs_commit(h);
            ok = true;
        }
        nvs_close(h);
    }

    // ② 同时写进 WiFi 驱动的 STA 配置，重启后 WiFi 驱动能直接用它连接
    wifi_config_t cfg{};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid, sizeof(cfg.sta.ssid) - 1);
    if (password != nullptr) {
        std::strncpy(reinterpret_cast<char*>(cfg.sta.password), password,
                     sizeof(cfg.sta.password) - 1);
    }
    cfg.sta.threshold.authmode = (password != nullptr && password[0] != '\0')
                                     ? WIFI_AUTH_WPA2_PSK
                                     : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable  = true;
    cfg.sta.pmf_cfg.required = false;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        ESP_LOGW(kTag, "写入 WiFi 驱动配置失败（记录已保存，重启后仍可重试）");
        ok = false;
    }

    ESP_LOGI(kTag, "已保存 Wi-Fi 凭据: \"%s\" (%s)", ssid, ok ? "OK" : "部分失败");
    return ok;
}

bool user_wifi_load(char* ssid, size_t ssid_len, char* password, size_t pwd_len)
{
    if (ssid == nullptr || ssid_len == 0) {
        return false;
    }
    nvs_handle_t h = 0;
    if (nvs_open(kNsWifi, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = ssid_len;
    const esp_err_t err = nvs_get_str(h, kKeySsid, ssid, &len);
    if (err == ESP_OK && password != nullptr && pwd_len > 0) {
        size_t plen = pwd_len;
        if (nvs_get_str(h, kKeyPwd, password, &plen) != ESP_OK) {
            password[0] = '\0';
        }
    }
    nvs_close(h);
    return err == ESP_OK && ssid[0] != '\0';
}

void user_data_erase()
{
    nvs_handle_t h = 0;
    if (nvs_open(kNsUser, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open(kNsWifi, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(kTag, "用户数据已清空");
}

// ---------------------------------------------------------------- 偏好设置
namespace {
constexpr uint8_t kPrefsMagic = 0xA7;   // 结构版本，改字段时改这里
}

void user_prefs_defaults(UserPrefs* out)
{
    if (out == nullptr) {
        return;
    }
    UserPrefs d{};
    d.program       = 0;      // 全身放松
    d.strength      = 3;
    d.speed         = 1;      // 中
    d.duration_min  = 20;
    d.parts_mask    = 0x07;   // 颈+肩+背
    d.air_neck      = 1;
    d.air_shoulder  = 1;
    d.air_arm       = 1;
    d.air_leg       = 1;
    d.air_foot      = 1;
    d.foot_roller   = 2;      // 中
    d.heat_mask     = 0x03;   // 腰背+腿
    d.heat_temp     = 45;
    d.volume        = 70;
    d.tts_speed     = 1;
    d.tts_voice     = 1;
    d.follow_up_sec = 600;
    d.ack_enabled   = 1;
    d.child_lock    = 0;
    d.sedentary_min = 0;
    d.magic         = kPrefsMagic;
    *out = d;
}

bool user_prefs_load(UserPrefs* out)
{
    if (out == nullptr) {
        return false;
    }
    user_prefs_defaults(out);   // 先给默认值，读失败就是出厂状态

    nvs_handle_t h = 0;
    if (nvs_open(kNsUser, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    UserPrefs tmp{};
    size_t len = sizeof(UserPrefs);
    const esp_err_t err = nvs_get_blob(h, kKeyPrefs, &tmp, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(UserPrefs) || tmp.magic != kPrefsMagic) {
        return false;   // 没保存过，或结构变了
    }
    *out = tmp;
    return true;
}

bool user_prefs_save(const UserPrefs& in)
{
    UserPrefs v = in;
    v.magic = kPrefsMagic;

    nvs_handle_t h = 0;
    if (nvs_open(kNsUser, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(kTag, "打开 NVS(user) 失败");
        return false;
    }
    const esp_err_t err = nvs_set_blob(h, kKeyPrefs, &v, sizeof(UserPrefs));
    if (err == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "保存偏好失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "偏好已保存：力度=%u 速度=%u 时长=%u 部位=0x%02X 音量=%u",
             v.strength, v.speed, v.duration_min, v.parts_mask, v.volume);
    return true;
}
