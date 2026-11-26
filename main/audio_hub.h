#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2s_std.h"

// ---------------------------------------------------------------------------
// 音频输出中枢
//
// 喇叭只有一路（I2S TX），但声音有好几种来源：唤醒应答、TTS 播报、音乐。
// 这里统一排队，保证：
//   - 高优先级能打断低优先级（比如"已停止"的提示音能打断音乐）
//   - 播放期间自动打开功放，播完关闭（省电、消底噪）
//   - 统一做音量缩放和采样率转换（内部固定 44100Hz 单声道）
// ---------------------------------------------------------------------------

enum AudioPriority {
    AUDIO_PRIO_EMERGENCY = 0,   // 急停提示音，最高
    AUDIO_PRIO_PROMPT    = 1,   // 唤醒应答「在的」
    AUDIO_PRIO_TTS       = 2,   // 聊天/介绍播报
    AUDIO_PRIO_MUSIC     = 3,   // 音乐，最低
};

// 由 wake_word 在 I2S TX 建好后调用一次。
// pa_ctrl: 功放开关回调（wake_word 里的 pa_set），可为 nullptr。
void audio_hub_init(i2s_chan_handle_t tx, void (*pa_ctrl)(bool));

// 音量 0-100
void audio_hub_set_volume(int percent);
int  audio_hub_get_volume();

// 音乐独立音量（0-100，默认 100）。
// 用于"放音乐时压低音乐、方便麦克风听清人说话"——不影响提示音和 TTS。
void audio_hub_set_music_volume(int percent);

// 高频提升系数（0 = 关闭，0.2~0.4 常见）。板载小喇叭高频弱，提一点人声更清楚。
void audio_hub_set_treble(float k);

// 请求抢占：优先级高于 prio 的音频要开始播了，正在播的低优先级会被中断。
void audio_hub_request_preempt(int prio);

// 播放一段 PCM（任意采样率，单声道 16bit），阻塞到播完或被打断。
// 返回 true = 完整播完，false = 被打断/出错。
bool audio_hub_play_pcm(const int16_t* pcm, size_t count, int src_rate, int prio);

// 流式播放：适合 TTS 边下边放。begin -> 多次 write -> end。
bool audio_hub_stream_begin(int prio);
bool audio_hub_stream_write(const int16_t* pcm, size_t count, int src_rate);
void audio_hub_stream_end();

// 当前是否正在播放（任何优先级）
bool audio_hub_busy();
