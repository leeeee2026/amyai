#pragma once

#include <cstdint>

// 用户身体信息（全部可跳过，0 表示未填）
// 存在 NVS 里永久保留，后续用于按摩力度/时长/程序的个性化推荐。
struct UserProfile {
    uint8_t height = 0;   // cm
    uint8_t weight = 0;   // kg
    uint8_t age    = 0;   // 岁
    uint8_t gender = 0;   // 0 未填 / 1 男 / 2 女
};

// 读取用户信息（没有记录时返回 false，profile 保持默认 0）
bool user_profile_load(UserProfile* out);

// 保存用户信息（永久写入 NVS）
bool user_profile_save(const UserProfile& in);

// 保存 Wi-Fi 凭据到 NVS（同时写进 WiFi 驱动的 STA 配置，重启后自动连接）
bool user_wifi_save(const char* ssid, const char* password);

// 读取 Wi-Fi 凭据（只读我们自己的记录；没有记录返回 false）
bool user_wifi_load(char* ssid, size_t ssid_len, char* password, size_t pwd_len);

// 清空所有用户数据（恢复出厂用）
void user_data_erase();
