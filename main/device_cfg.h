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

// ---------------------------------------------------------------------------
// 按摩偏好 / 智能体 / 安全设置
//
// 这些是"用户设置的默认值"，和语音临时指令（"轻一点"）是两回事：
// 语音指令只影响当前这一次，这里的值决定下次开机后的默认行为。
// 全部存 NVS，掉电不丢。
// ---------------------------------------------------------------------------
struct UserPrefs {
    uint8_t  program;        // 默认程序：0全身 1肩颈 2腰臀 3睡眠 4舒展 5运动恢复
    uint8_t  strength;       // 力度 1-5
    uint8_t  speed;          // 速度 0慢 1中 2快
    uint8_t  duration_min;   // 时长 10/20/30/45 分钟
    uint8_t  parts_mask;     // 部位位掩码：bit0颈 bit1肩 bit2背 bit3腰 bit4臀 bit5腿 bit6足
    uint8_t  air_neck;       // 气囊 0弱 1中 2强
    uint8_t  air_shoulder;
    uint8_t  air_arm;
    uint8_t  air_leg;
    uint8_t  air_foot;
    uint8_t  foot_roller;    // 足底滚轮 0关 1低 2中 3高
    uint8_t  heat_mask;      // 热敷部位：bit0腰背 bit1腿 bit2足 bit3手 bit4颈
    uint8_t  heat_temp;      // 40 / 45 / 50 ℃
    uint8_t  volume;         // 语音音量 0-100
    uint8_t  tts_speed;      // 语速 0慢 1标准 2快
    uint8_t  tts_voice;      // 音色 0女声 1温柔 2沉稳
    uint16_t follow_up_sec;  // 连聊时长 0关 / 15 / 60 / 600
    uint8_t  ack_enabled;    // 应答音 0关 1开
    uint8_t  child_lock;     // 儿童锁 0关 1开
    uint8_t  sedentary_min;  // 久坐提醒 0关 / 30 / 60 / 120
    uint8_t  magic;          // 结构版本标记，用于识别"从未保存过"
};

// 读取偏好设置（没保存过则返回 false，out 保持默认值）
bool user_prefs_load(UserPrefs* out);

// 保存偏好设置（永久写入 NVS）
bool user_prefs_save(const UserPrefs& in);

// 把默认值填进 out（出厂状态）
void user_prefs_defaults(UserPrefs* out);
