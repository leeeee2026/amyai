#pragma once

#include <string>

// ---------------------------------------------------------------------------
// 阿里云百炼（DashScope）语音合成
//
// 两步：
//   1) POST 合成请求 -> 返回一个 WAV 文件 URL（24 小时有效）
//   2) HTTPS GET 那个 URL -> 拿到 WAV -> 解析 -> 送 AudioHub 播放
//
// 模型 qwen3-tts-flash，单次最多 600 字符，超长会自动分段播。
// ---------------------------------------------------------------------------

// 初始化（目前只是打日志，鉴权走 Kconfig）
void tts_init();

// 把一段文字念出来，阻塞到播完。返回 false 表示失败。
bool tts_speak(const std::string& text);

// 最近一次失败原因（给上层打日志用）
const char* tts_last_error();
