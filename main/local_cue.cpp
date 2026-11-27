#include "local_cue.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_hub.h"
#include "wake_word.h"
#include "prompt_hao_de.h"
#include "prompt_hao_ok.h"
#include "prompt_no_net.h"

namespace {

constexpr const char* kTag = "cue";

// 按摩控制类关键词：命中任意一个就认为"这是在操作按摩椅"
// 注意不要放太泛的词（比如单个"热"），否则聊天时也会误播「好的」
const char* const kControlWords[] = {
    // 力度
    "重一点", "重点", "重些", "轻一点", "轻点", "轻些", "力度", "力气",
    "力大", "力小", "用力", "加力", "减力", "太疼", "太重", "太轻", "太紧",
    // 部位
    "脖子", "颈部", "脖子", "肩", "后背", "背部", "腰", "臀部", "腿", "脚",
    "足", "手臂", "胳膊", "全身", "颈肩",
    // 角度
    "抬高", "放低", "升高", "降下", "躺", "坐起", "坐起来", "靠背", "腿托",
    // 功能
    "热敷", "加热", "零重力", "气囊", "足底", "滚轮",
    // 速度
    "快一点", "慢一点", "节奏", "速度快", "速度慢",
    // 时间
    "分钟", "半小时", "小时",
    // 起停
    "开始", "停", "暂停", "继续",
    // 手法
    "揉捏", "揉一揉", "敲", "捶", "指压", "推拿", "拉伸", "揉",
};

}  // namespace

bool local_is_control(const std::string& text)
{
    if (text.empty()) {
        return false;
    }
    for (const char* w : kControlWords) {
        if (std::strstr(text.c_str(), w) != nullptr) {
            return true;
        }
    }
    return false;
}

bool local_is_stop_command(const std::string& text)
{
    if (text.empty()) {
        return false;
    }
    // 判定从严：只认短句（说"停"这种），长句里碰巧有"停"不算
    // 中文字符按 3 字节算，10 字以内 ≈ 30 字节；再放宽到 36 字节
    if (text.size() > 36) {
        return false;
    }
    static const char* const kStopWords[] = {
        "停", "暂停", "停止", "关掉", "关了", "关一下", "关闭",
        "别放", "不要放", "不放了", "别播", "别唱", "安静",
        "小声", "小声点", "关音乐", "停音乐", "停止播放", "别响了",
    };
    for (const char* w : kStopWords) {
        if (std::strstr(text.c_str(), w) != nullptr) {
            return true;
        }
    }
    return false;
}

// 所有本地应答都延迟 500ms 再播：用户刚说完话，太快的应答显得很突兀
constexpr int kLocalAckDelayMs = 500;

void local_play_ack()
{
    // 用独立任务播放，避免阻塞主链路（云端请求要马上发出去）
    xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(kLocalAckDelayMs));
            wake_pause_listening(true);    // 播放期间别录音，否则喇叭声会被录进去
            audio_hub_play_pcm(kPromptHaoDe, kPromptHaoDeLen, kPromptHaoDeRate,
                               AUDIO_PRIO_PROMPT);
            wake_pause_listening(false);
            vTaskDelete(nullptr);
        },
        "ack", 3072, nullptr, 4, nullptr);
}

// 本地命令词命中：「好的，马上为您调整」
void local_play_ok()
{
    xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(kLocalAckDelayMs));
            wake_pause_listening(true);
            audio_hub_play_pcm(kPromptHaoOk, kPromptHaoOkLen, kPromptHaoOkRate,
                               AUDIO_PRIO_PROMPT);
            wake_pause_listening(false);
            vTaskDelete(nullptr);
        },
        "ackok", 3072, nullptr, 4, nullptr);
}

void local_play_no_net()
{
    // 冷却 15 秒，防止用户反复说话时一直播提示音
    static int64_t last_ms = 0;
    const int64_t now = esp_timer_get_time() / 1000;
    if (now - last_ms < 15000) {
        return;
    }
    last_ms = now;

    xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(kLocalAckDelayMs));
            wake_pause_listening(true);
            audio_hub_play_pcm(kPromptNoNet, kPromptNoNetLen, kPromptNoNetRate,
                               AUDIO_PRIO_PROMPT);
            wake_pause_listening(false);
            vTaskDelete(nullptr);
        },
        "nonet", 4096, nullptr, 4, nullptr);
}
