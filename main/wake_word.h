#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// 拿到底层 esp-sr 的模型表（AFE 初始化时已经把 model 分区的模型映射好），
// 给离线命令词识别模块复用，避免重复 mmap。
// 返回 void* 是为了不把 esp-sr 的头文件传染到这里，调用方自己转成 srmodel_list_t*。
void* wake_model_list();

// 离线唤醒词（esp-sr WakeNet）+ 唤醒后录音
//
// 用法：
//   WakeWord ww;
//   ww.start(
//     []{ /* 被唤醒 */ },
//     [](const int16_t* pcm, size_t n){ /* 4 秒 16k 单声道 PCM */ });
//
// 引脚、模型名都来自 Kconfig（menuconfig -> Amyai 音频硬件 / Amyai 唤醒词），
// 换板子只需要改配置，不用动代码。
class WakeWord {
public:
    using OnWake = std::function<void()>;
    // 录音完成回调：16kHz / 16bit / 单声道 PCM
    using OnUtterance = std::function<void(const int16_t*, size_t)>;

    // 返回 false 表示配置不全或初始化失败（会在串口说明原因）
    bool start(OnWake on_wake, OnUtterance on_utterance = nullptr);

    void set_enabled(bool en) { enabled_ = en; }
    bool enabled() const { return enabled_; }

    // 音乐播放窗口：开启后不需要唤醒词，直接监听短指令（判定阈值更严，避免音乐自激）
    void set_music_window(bool on) { music_window_ = on; }

    // 主动进入连续聆听（不需要唤醒词），用于"停下音乐后用户还有别的话要说"
    void start_follow_up() { force_follow_up_ = true; }

    // 取消连续聆听（比如没网、ASR 失败时，避免疯狂重录）
    void cancel_follow_up() { cancel_follow_up_ = true; }

private:
    static void task_entry(void* arg);
    void run();

    OnWake on_wake_;
    OnUtterance on_utterance_;
    bool enabled_ = true;
    bool music_window_ = false;
    bool force_follow_up_ = false;
    bool cancel_follow_up_ = false;
};
