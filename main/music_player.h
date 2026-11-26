#pragma once

#include <string>

// ---------------------------------------------------------------------------
// 音乐播放（网络拉流）
//
// 当前只支持 WAV（PCM），不需要解码器；后续加 MP3 再引入 esp_audio_codec。
// 音频来自 HTTP 服务器（开发期用电脑上的本地服务，正式期换成 CDN/OSS）。
// ---------------------------------------------------------------------------

// 按关键词（雨声/海浪/森林/助眠…）选曲并开始播放。返回 false 表示没匹配到或启动失败。
bool music_play(const std::string& query);

// 切到轮播列表的下一首（"换一首/下一首"用）
bool music_next();

// 停止播放（会等播放任务退出）
void music_stop();

// 是否正在播放
bool music_is_playing();

// 当前曲目名（给日志用）
const char* music_current();
