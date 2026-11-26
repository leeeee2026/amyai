#include "uart_link.h"

#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_AMYAI_UART_ENABLE

namespace {

constexpr const char* kTag = "uart";

constexpr uint8_t kHead0 = 0xA5;
constexpr uint8_t kHead1 = 0x5A;
constexpr uint8_t kVer = 0x01;

constexpr uint8_t kTypeCmd = 0x01;
constexpr uint8_t kTypeAck = 0x02;
constexpr uint8_t kTypeHeartbeat = 0x03;
constexpr uint8_t kTypeEstop = 0x04;

constexpr uart_port_t kPort =
#if CONFIG_AMYAI_UART_ON_CONSOLE
    UART_NUM_0;   // 调试串口（和日志共用，无需额外接线，仅验证用）
#else
    UART_NUM_1;   // 独立主控串口
#endif
constexpr int kTxBufSize = 1024;
constexpr int kRxBufSize = 2048;

SemaphoreHandle_t s_lock = nullptr;
uint8_t s_seq = 0;

// ---------------------------------------------------------------- CRC
uint16_t crc16_ccitt(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

// ---------------------------------------------------------------- 组帧
int build_frame(uint8_t* out, size_t cap, uint8_t type, uint8_t seq,
                const uint8_t* payload, size_t payload_len)
{
    if (payload_len > 4096 || cap < payload_len + 9) {
        return 0;
    }
    size_t n = 0;
    out[n++] = kHead0;
    out[n++] = kHead1;
    out[n++] = kVer;
    out[n++] = type;
    out[n++] = seq;
    out[n++] = static_cast<uint8_t>(payload_len & 0xFF);
    out[n++] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
    if (payload_len > 0) {
        std::memcpy(out + n, payload, payload_len);
        n += payload_len;
    }
    const uint16_t crc = crc16_ccitt(out + 2, n - 2);
    out[n++] = static_cast<uint8_t>(crc & 0xFF);
    out[n++] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    return static_cast<int>(n);
}

// ---------------------------------------------------------------- 收帧
// 从串口缓冲里找出一个完整且 CRC 正确的帧；匹配到 ACK 就返回 true
bool wait_for_ack(uint8_t expect_seq, int timeout_ms)
{
    uint8_t buf[256];
    size_t have = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(kPort, buf + have, sizeof(buf) - have, pdMS_TO_TICKS(20));
        if (n > 0) {
            have += static_cast<size_t>(n);

            size_t i = 0;
            while (i + 9 <= have) {
                if (buf[i] != kHead0 || buf[i + 1] != kHead1) {
                    ++i;
                    continue;
                }
                const size_t len = static_cast<size_t>(buf[i + 5]) | (static_cast<size_t>(buf[i + 6]) << 8);
                const size_t total = 7 + len + 2;
                if (i + total > have) {
                    break;  // 半帧，继续等
                }
                const uint16_t crc_rx = static_cast<uint16_t>(buf[i + 7 + len]) |
                                        (static_cast<uint16_t>(buf[i + 8 + len]) << 8);
                const uint16_t crc_calc = crc16_ccitt(buf + i + 2, 5 + len);
                if (crc_calc == crc_rx) {
                    const uint8_t type = buf[i + 3];
                    const uint8_t seq = buf[i + 4];
                    if (type == kTypeAck && seq == expect_seq) {
                        return true;
                    }
                    ESP_LOGI(kTag, "收到主控帧 type=0x%02X seq=%u", type, seq);
                } else {
                    ESP_LOGW(kTag, "CRC 校验失败，丢弃一帧");
                }
                i += total;
            }
            if (i > 0) {
                std::memmove(buf, buf + i, have - i);
                have -= i;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------- 心跳
void heartbeat_task(void* /*arg*/)
{
    const uint8_t payload[] = "{\"stat\":\"idle\"}";
    uint8_t frame[64];
    while (true) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
            const int n = build_frame(frame, sizeof(frame), kTypeHeartbeat, 0,
                                      payload, sizeof(payload) - 1);
            if (n > 0) {
                uart_write_bytes(kPort, frame, n);
            }
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_AMYAI_UART_HEARTBEAT_MS));
    }
}

}  // namespace

#if CONFIG_AMYAI_UART_PIN_SCAN
// 引脚扫描：这块板子上只有 IO5/IO6/IO8/IO18 是空闲的，
// 逐个把 UART1 的 TX 换过去发帧，哪根线接着，那边的串口助手就能看到数据。
static void start_pin_scan()
{
    static const int kPins[] = {5, 6, 8, 18};
    static const int kPinCount = sizeof(kPins) / sizeof(kPins[0]);

    uart_set_pin(kPort, kPins[0], UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate([](void*) {
        uint8_t frame[128];
        for (int idx = 0;; ++idx) {
            const int pin = kPins[idx % kPinCount];
            uart_set_pin(kPort, pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

            char payload[32];
            const int len = snprintf(payload, sizeof(payload), "{\"scan\":%d}", pin);
            const int n = build_frame(frame, sizeof(frame), kTypeHeartbeat, 0,
                                      reinterpret_cast<const uint8_t*>(payload),
                                      static_cast<size_t>(len));
            ESP_LOGW(kTag, "扫描：TX -> IO%d，连发 3 帧（2 秒后换下一个）", pin);
            for (int i = 0; i < 3 && n > 0; ++i) {
                uart_write_bytes(kPort, frame, n);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            vTaskDelay(pdMS_TO_TICKS(1800));
        }
    }, "uart_scan", 3072, nullptr, 4, nullptr);

    ESP_LOGW(kTag, "================ 引脚扫描模式 ================");
    ESP_LOGW(kTag, "TX 依次尝试 IO5 -> IO6 -> IO8 -> IO18，每个 2 秒");
    ESP_LOGW(kTag, "打开串口助手（看板子 TX 那一路），出现数据时看日志里当时是哪个引脚");
    ESP_LOGW(kTag, "=============================================");
}
#endif

bool uart_link_init()
{
    uart_config_t cfg = {};
    cfg.baud_rate = CONFIG_AMYAI_UART_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(kPort, kRxBufSize, kTxBufSize, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "UART 驱动安装失败: %s", esp_err_to_name(err));
        return false;
    }
    if (uart_param_config(kPort, &cfg) != ESP_OK) {
        ESP_LOGE(kTag, "UART 参数配置失败");
        return false;
    }
#if !CONFIG_AMYAI_UART_ON_CONSOLE
#if CONFIG_AMYAI_UART_PIN_SCAN
    start_pin_scan();
    s_lock = xSemaphoreCreateMutex();
    return true;
#else
    if (uart_set_pin(kPort, CONFIG_AMYAI_UART_TX_GPIO, CONFIG_AMYAI_UART_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(kTag, "UART 引脚设置失败");
        return false;
    }
#endif
#endif

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        ESP_LOGE(kTag, "创建互斥锁失败");
        return false;
    }

#if CONFIG_AMYAI_UART_ON_CONSOLE
    ESP_LOGW(kTag, "主控协议走调试串口 UART0（%d baud）—— 日志与帧混用，仅用于验证",
             CONFIG_AMYAI_UART_BAUD);
#else
    ESP_LOGI(kTag, "UART1 就绪: TX=GPIO%d RX=GPIO%d baud=%d",
             CONFIG_AMYAI_UART_TX_GPIO, CONFIG_AMYAI_UART_RX_GPIO, CONFIG_AMYAI_UART_BAUD);
#endif

#if CONFIG_AMYAI_UART_HEARTBEAT
    xTaskCreate(heartbeat_task, "uart_hb", 2560, nullptr, 3, nullptr);
    ESP_LOGI(kTag, "心跳已启动，每 %d ms 一次", CONFIG_AMYAI_UART_HEARTBEAT_MS);
#endif
    return true;
}

bool uart_link_send_command(const std::string& json)
{
    if (s_lock == nullptr) {
        ESP_LOGE(kTag, "UART 还没初始化");
        return false;
    }

    uint8_t frame[512];
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(kTag, "等锁超时");
        return false;
    }

    const uint8_t seq = s_seq++;
    const int n = build_frame(frame, sizeof(frame), kTypeCmd, seq,
                              reinterpret_cast<const uint8_t*>(json.data()), json.size());
    if (n <= 0) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(kTag, "组帧失败（payload 太长？）");
        return false;
    }

    bool ok = false;
    for (int attempt = 1; attempt <= CONFIG_AMYAI_UART_RETRIES; ++attempt) {
        uart_flush_input(kPort);
        uart_write_bytes(kPort, frame, n);
        ESP_LOGI(kTag, "发送指令 seq=%u (%d 字节，第 %d 次): %s",
                 seq, n, attempt, json.c_str());

        if (wait_for_ack(seq, CONFIG_AMYAI_UART_ACK_TIMEOUT_MS)) {
            ESP_LOGI(kTag, "主控已确认 seq=%u", seq);
            ok = true;
            break;
        }
        ESP_LOGW(kTag, "%d ms 没有 ACK，重试", CONFIG_AMYAI_UART_ACK_TIMEOUT_MS);
    }

    xSemaphoreGive(s_lock);
    if (!ok) {
        ESP_LOGE(kTag, "重传 %d 次仍无 ACK，指令未送达", CONFIG_AMYAI_UART_RETRIES);
    }
    return ok;
}

void uart_link_send_estop()
{
    if (s_lock == nullptr) {
        return;
    }
    uint8_t frame[32];
    const uint8_t payload[] = "{\"cmd\":\"stop\"}";
    const int n = build_frame(frame, sizeof(frame), kTypeEstop, 0,
                              payload, sizeof(payload) - 1);
    if (n > 0) {
        uart_write_bytes(kPort, frame, n);
        ESP_LOGW(kTag, "!!! 已发送急停帧（0x04）");
    }
}

bool uart_link_is_estop_text(const std::string& text)
{
    static const char* kWords[] = {"停一下", "停下", "别按", "急停", "快停"};
    for (const char* w : kWords) {
        if (text.find(w) != std::string::npos) {
            return true;
        }
    }
    // 单独一个「停」也算
    return text == "停" || text == "停下来";
}

#else  // !CONFIG_AMYAI_UART_ENABLE

bool uart_link_init() { return false; }
bool uart_link_send_command(const std::string&) { return false; }
void uart_link_send_estop() {}
bool uart_link_is_estop_text(const std::string&) { return false; }

#endif
