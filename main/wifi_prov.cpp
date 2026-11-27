// 配网信息管理（网页配网方案）
//
// 历史：早期用 ESP BLE Provisioning（network_provisioning 组件）配网，
//       现在改成手机扫码 + 网页填写（见 web_portal.cpp），
//       这里只保留凭据读写和恢复出厂，蓝牙相关代码已全部移除。

#include "wifi_prov.h"

#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr const char* kTag = "prov";

}  // namespace

bool prov_load_credentials(std::string& ssid, std::string& password)
{
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return false;
    }
    const char* s = reinterpret_cast<const char*>(cfg.sta.ssid);
    if (s[0] == '\0') {
        return false;
    }
    ssid     = s;
    password = reinterpret_cast<const char*>(cfg.sta.password);
    return true;
}

bool prov_is_provisioned()
{
    std::string ssid;
    std::string password;
    return prov_load_credentials(ssid, password);
}

void prov_reset()
{
    // 只清除 Wi-Fi 凭据，不动其他 NVS 数据
    esp_wifi_restore();
    ESP_LOGW(kTag, "已清除配网信息，设备将以「未配网」状态启动");
}

bool prov_detect_reset_request()
{
#if CONFIG_AMYAI_PROV_RESET_GPIO >= 0
    const gpio_num_t pin = static_cast<gpio_num_t>(CONFIG_AMYAI_PROV_RESET_GPIO);

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << CONFIG_AMYAI_PROV_RESET_GPIO;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;    // 按键低电平有效，内部上拉
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);

    // 按键低电平有效（开发板 BOOT 键接 GND）
    if (gpio_get_level(pin) != 0) {
        return false;
    }

    ESP_LOGW(kTag, "检测到按键按下，保持 %d ms 可恢复出厂设置…",
             CONFIG_AMYAI_PROV_RESET_HOLD_MS);

    constexpr int kStepMs = 100;
    int held = 0;
    while (held < CONFIG_AMYAI_PROV_RESET_HOLD_MS) {
        vTaskDelay(pdMS_TO_TICKS(kStepMs));
        if (gpio_get_level(pin) != 0) {
            ESP_LOGI(kTag, "按键已松开，取消恢复出厂");
            return false;
        }
        held += kStepMs;
    }
    return true;
#else
    return false;
#endif
}
