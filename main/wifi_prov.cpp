#include "wifi_prov.h"

#include "audio_hub.h"
#include "prompt_prov.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

// ESP-IDF 6.x 起，配网组件从核心移出并改名为 network_provisioning
// （由 main/idf_component.yml 里的 espressif/network_provisioning 拉取）
#include "network_provisioning/manager.h"
#include "network_provisioning/network_config.h"
#include "network_provisioning/scheme_ble.h"

namespace {

constexpr const char* kTag = "prov";

EventGroupHandle_t s_ev = nullptr;
constexpr EventBits_t kProvEndBit = BIT0;

// 用 MAC 后 3 字节拼一个唯一的蓝牙名字，例如 AMYAI_A1B2C3
void make_device_name(char* out, size_t out_len)
{
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        std::snprintf(out, out_len, "%s", CONFIG_AMYAI_PROV_NAME_PREFIX);
        return;
    }
    std::snprintf(out, out_len, "%s_%02X%02X%02X", CONFIG_AMYAI_PROV_NAME_PREFIX,
                  mac[3], mac[4], mac[5]);
}

void prov_event_handler(void* /*arg*/, esp_event_base_t base, int32_t id, void* data)
{
    if (base != NETWORK_PROV_EVENT) {
        return;
    }

    switch (id) {
    case NETWORK_PROV_START:
        ESP_LOGI(kTag, "配网服务已启动，等待手机连接…");
        break;

    case NETWORK_PROV_WIFI_CRED_RECV: {
        const auto* cfg = static_cast<const wifi_sta_config_t*>(data);
        ESP_LOGI(kTag, "收到 Wi-Fi 凭据：SSID=\"%s\"",
                 reinterpret_cast<const char*>(cfg->ssid));
        break;
    }

    case NETWORK_PROV_WIFI_CRED_FAIL: {
        const auto* reason = static_cast<const network_prov_wifi_sta_fail_reason_t*>(data);
        ESP_LOGE(kTag, "配网失败：%s",
                 (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR)
                     ? "密码错误（认证被拒）"
                     : "找不到这个 AP（名称错/信号弱/只有 5GHz）");
        break;
    }

    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(kTag, "Wi-Fi 认证并连接成功");
        break;

    case NETWORK_PROV_END:
        ESP_LOGI(kTag, "配网流程结束");
        if (s_ev != nullptr) {
            xEventGroupSetBits(s_ev, kProvEndBit);
        }
        break;

    default:
        break;
    }
}

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

bool prov_run_ble(uint32_t timeout_ms)
{
    if (s_ev == nullptr) {
        s_ev = xEventGroupCreate();
        if (s_ev == nullptr) {
            ESP_LOGE(kTag, "创建事件组失败");
            return false;
        }
    }

    esp_event_handler_instance_t instance = nullptr;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, nullptr, &instance));

    network_prov_mgr_config_t cfg = {};
    cfg.scheme               = network_prov_scheme_ble;
    // 配网结束后释放 BT 控制器内存，把 RAM 留给 esp-sr
    cfg.scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
    cfg.app_event_handler    = NETWORK_PROV_EVENT_HANDLER_NONE;

    esp_err_t err = network_prov_mgr_init(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "初始化配网管理器失败: %s", esp_err_to_name(err));
        esp_event_handler_instance_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, instance);
        return false;
    }

    char device_name[33] = {};
    make_device_name(device_name, sizeof(device_name));

    ESP_LOGW(kTag, "================ 进入 BLE 配网模式 ================");
    ESP_LOGW(kTag, "蓝牙名称: %s", device_name);
    ESP_LOGW(kTag, "配网密码(PoP): %s", CONFIG_AMYAI_PROV_POP);
    ESP_LOGW(kTag, "内存: 总剩余 %u，内部 %u（蓝牙需要足够的内部 RAM）",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGW(kTag, "手机装 ESP BLE Provisioning App：选到 \"%s\"，输入上面的 PoP，", device_name);
    ESP_LOGW(kTag, "再选家里的 2.4GHz Wi-Fi 并填密码。");

    err = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1,
                                              static_cast<const void*>(CONFIG_AMYAI_PROV_POP),
                                              device_name, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "启动配网失败: %s", esp_err_to_name(err));
        network_prov_mgr_deinit();
        esp_event_handler_instance_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, instance);
        return false;
    }

    // 语音提示：本地播放「请用手机蓝牙帮我配网吧」（不依赖网络，用独立任务避免阻塞配网）
    xTaskCreate(
        [](void*) {
            audio_hub_play_pcm(kPromptProv, kPromptProvLen, kPromptProvRate, AUDIO_PRIO_PROMPT);
            vTaskDelete(nullptr);
        },
        "prov_voice", 4096, nullptr, 4, nullptr);

    EventBits_t bits = xEventGroupWaitBits(s_ev, kProvEndBit, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    const bool ok = (bits & kProvEndBit) != 0;

    if (!ok) {
        ESP_LOGW(kTag, "配网超时（%u 秒没有完成），关闭蓝牙",
                 static_cast<unsigned>(timeout_ms / 1000));
        network_prov_mgr_stop_provisioning();
        xEventGroupWaitBits(s_ev, kProvEndBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
    } else {
        ESP_LOGI(kTag, "配网完成，正在关闭蓝牙以释放内存");
    }

    network_prov_mgr_deinit();
    esp_event_handler_instance_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, instance);
    return ok;
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
