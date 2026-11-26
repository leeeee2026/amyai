#pragma once

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// BLE 配网（基于 ESP-IDF 的 wifi_provisioning 组件）
//
// 流程：开机 -> 若 NVS 里没有 Wi-Fi 凭据 -> 广播蓝牙，等手机 App 下发 SSID/密码
//       -> 自动连接 -> 凭据存入 NVS -> 释放蓝牙内存（省 RAM 给 esp-sr）
//
// 手机端 App：ESP BLE Provisioning（乐鑫官方，安卓 / iOS）
//   安卓：应用商店搜 "ESP BLE Provisioning"
//   iOS ：App Store 搜 "ESP BLE Provisioning"
//
// 恢复出厂：开机时按住 BOOT 键（默认 GPIO0）3 秒，会清除已保存的配网信息，
//          下次启动重新进入配网模式。
// ---------------------------------------------------------------------------

// 是否已经配过网（NVS 中存在非空的 STA 配置）
bool prov_is_provisioned();

// 从 NVS 读取已保存的 STA 凭据；没配过网时返回 false
bool prov_load_credentials(std::string& ssid, std::string& password);

// 启动 BLE 配网并阻塞等待。返回后蓝牙已释放。
// 返回值：true = 成功拿到凭据
bool prov_run_ble(uint32_t timeout_ms);

// 清除配网信息（下次开机会重新进入配网）
void prov_reset();

// 开机时检测“恢复出厂”按键是否被长按
bool prov_detect_reset_request();
