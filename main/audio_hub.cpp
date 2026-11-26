#include "audio_hub.h"

#include <cmath>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr const char* kTag = "audio";

// 硬件固定参数（NS4168 功放 + 数字麦克风都接在 I2S 上，统一 44100Hz）
constexpr int kHwRate = 44100;
constexpr int kChunkFrames = 256;     // 每次写 I2S 的帧数

i2s_chan_handle_t s_tx = nullptr;
void (*s_pa_ctrl)(bool) = nullptr;
SemaphoreHandle_t s_lock = nullptr;

int s_volume = 100;                   // 总音量 0-100（拉满）
int s_music_volume = 100;             // 音乐专用音量（ducking 用，不影响 TTS/提示音）
float s_treble = 0.15f;               // 高频提升系数，只做轻微补偿，避免染色

// 响度增益：小喇叭音量有限，靠"增益 + tanh 压缩"提升听感响度（1.0 = 不增强）
constexpr double kLoudnessGain = 1.0;   // 不额外加增益（会改变音色），音量靠 DAC 寄存器
volatile int s_preempt_prio = 1000;   // 大于此值的优先级会被打断（1000 = 不打断任何）
volatile bool s_busy = false;
volatile int s_current_prio = -1;

int s_stream_prio = -1;
int16_t s_out[kChunkFrames * 2];      // 立体声：每帧 2 个 int16（左右各一份）
float s_prev_sample = 0.0f;           // 高频提升用的一阶差分状态

// ★ 流式重采样要跨块保持相位，否则每块开头都会重复一个采样点（听感：周期性咔哒）
double s_res_pos = 0.0;               // 相对当前输入块的位置（可为负，表示要接上一块末尾）
int16_t s_res_last = 0;               // 上一块最后一个输入样本

void pa(bool on)
{
    if (s_pa_ctrl != nullptr) {
        s_pa_ctrl(on);
    }
}

// 线性插值重采样到 44100
void write_resampled(const int16_t* pcm, size_t count, int src_rate, float* phase, bool* aborted)
{
    if (count == 0) {
        return;
    }
    const double step = (src_rate > 0) ? static_cast<double>(src_rate) / kHwRate : 1.0;
    const int vol = (s_current_prio == AUDIO_PRIO_MUSIC) ? (s_volume * s_music_volume / 100) : s_volume;
    const bool streaming = (s_stream_prio >= 0);

    // 流式播放时用跨块连续的相位；一次性播放每次从头开始
    double pos = streaming ? s_res_pos : 0.0;
    int filled = 0;

    while (pos < static_cast<double>(count - 1)) {
        if (*aborted) {
            return;
        }
        // pos 可能为负（跨块插值要借用上一块最后一个样本）
        const int i0 = static_cast<int>(std::floor(pos));
        const double frac = pos - i0;
        const int16_t a = (i0 < 0) ? (streaming ? s_res_last : pcm[0]) : pcm[i0];
        const int16_t b = (i0 + 1 < 0) ? (streaming ? s_res_last : pcm[0])
                                       : ((i0 + 1 >= static_cast<int>(count)) ? pcm[count - 1]
                                                                              : pcm[i0 + 1]);
        double v = a + (b - a) * frac;

        // 高频提升（一阶差分）：小喇叭低频重、高频弱，提一下辅音会清楚很多
        if (s_treble > 0.001f) {
            const double boosted = v + s_treble * (v - s_prev_sample);
            s_prev_sample = static_cast<float>(v);
            v = boosted;
        }

        v = v * vol / 100.0;

        // ★ 响度提升：小喇叭本身音量有限，这里先加固定增益，
        //   再用 tanh 软压缩把峰值压回满量程——听感上明显变响，又不会破音。
        v *= kLoudnessGain;
        constexpr double kKnee = 32000.0;   // 只削真正的过载峰，不做压缩
        const double abs_v = (v < 0) ? -v : v;
        if (abs_v > kKnee) {
            const double room = 32767.0 - kKnee;
            const double over = abs_v - kKnee;
            const double out = kKnee + room * std::tanh(over / room);
            v = (v < 0) ? -out : out;
        }
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;

        // ★ I2S 是立体声，必须左右声道各写一份（单声道数据要复制到两个 slot）
        s_out[filled * 2]     = static_cast<int16_t>(v);
        s_out[filled * 2 + 1] = static_cast<int16_t>(v);
        ++filled;

        if (filled >= kChunkFrames) {
            size_t written = 0;
            i2s_channel_write(s_tx, s_out, filled * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(500));
            filled = 0;

            if (s_current_prio > s_preempt_prio) {
                *aborted = true;
                ESP_LOGW(kTag, "播放被打断（优先级 %d > %d）", s_current_prio, s_preempt_prio);
                return;
            }
        }
        pos += step;
    }

    if (filled > 0) {
        size_t written = 0;
        i2s_channel_write(s_tx, s_out, filled * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(500));
    }

    if (streaming) {
        s_res_pos = pos - static_cast<double>(count);   // 相对下一块的起始偏移
        s_res_last = pcm[count - 1];
    }
}

}  // namespace

// 保活任务：平时没人往 I2S TX 写数据，DMA 会空转。
// 这块板子的 I2S 时钟疑似依赖 TX 在跑，所以要定期喂一点静音，别让 TX 停摆。
static void keepalive_task(void*)
{
    static int16_t sil[256] = {};   // 128 帧立体声静音
    static int quiet_rounds = 0;
    while (true) {
        // 只有在没人播放、且通道确认可用时才喂
        if (!s_busy && s_tx != nullptr) {
            if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
                if (!s_busy && s_tx != nullptr) {
                    size_t written = 0;
                    const esp_err_t r = i2s_channel_write(s_tx, sil, sizeof(sil), &written, 0);
                    if (r != ESP_OK) {
                        // 通道被临时禁用（恢复流程），别刷屏
                        if (++quiet_rounds <= 2) {
                            ESP_LOGD(kTag, "保活写入暂不可用");
                        }
                    } else {
                        quiet_rounds = 0;
                    }
                }
                xSemaphoreGive(s_lock);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void audio_hub_init(i2s_chan_handle_t tx, void (*pa_ctrl)(bool))
{
    s_tx = tx;
    s_pa_ctrl = pa_ctrl;
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
    }
    ESP_LOGI(kTag, "AudioHub 就绪（音量 %d%%）", s_volume);

    // 启动 TX 保活（防止 I2S 主时钟因 TX 空转而停摆）
    static bool started = false;
    if (!started) {
        started = true;
        xTaskCreate(keepalive_task, "audio_keep", 2560, nullptr, 3, nullptr);
    }
}

void audio_hub_set_music_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_music_volume = percent;
    ESP_LOGI(kTag, "音乐音量设为 %d%%（总音量 %d%%）", s_music_volume, s_volume);
}

void audio_hub_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    ESP_LOGI(kTag, "音量设为 %d%%", s_volume);
}

int audio_hub_get_volume()
{
    return s_volume;
}

void audio_hub_set_treble(float k)
{
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    s_treble = k;
    ESP_LOGI(kTag, "高频提升设为 %.2f", s_treble);
}

void audio_hub_request_preempt(int prio)
{
    s_preempt_prio = prio;
}

bool audio_hub_busy()
{
    return s_busy;
}

bool audio_hub_play_pcm(const int16_t* pcm, size_t count, int src_rate, int prio)
{
    if (s_tx == nullptr || pcm == nullptr || count == 0) {
        return false;
    }
    // ★ 先请求抢占：正在播的低优先级流（音乐）会在下一个分片检测到并主动退出，
    //   否则只能干等锁 3 秒——唤醒应答「在的」就是这么丢的。
    s_preempt_prio = prio;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        s_preempt_prio = 1000;
        ESP_LOGW(kTag, "等音频锁超时");
        return false;
    }

    s_busy = true;
    s_current_prio = prio;
    s_preempt_prio = 1000;

    pa(true);
    vTaskDelay(pdMS_TO_TICKS(50));   // 功放稳定

    bool aborted = false;
    float phase = 0.0f;
    s_prev_sample = 0.0f;
    s_res_pos = 0.0;
    s_res_last = 0;
    write_resampled(pcm, count, src_rate, &phase, &aborted);

    if (!aborted) {
        // 尾部补静音，避免咔哒声
        std::memset(s_out, 0, sizeof(s_out));
        size_t written = 0;
        i2s_channel_write(s_tx, s_out, sizeof(s_out), &written, pdMS_TO_TICKS(500));
        vTaskDelay(pdMS_TO_TICKS(60));
    }

    pa(false);
    s_busy = false;
    s_current_prio = -1;
    s_preempt_prio = 1000;
    xSemaphoreGive(s_lock);
    return !aborted;
}

bool audio_hub_stream_begin(int prio)
{
    if (s_tx == nullptr) {
        return false;
    }
    // 同样先请求抢占，让低优先级流（音乐）让出，TTS 不用干等 3 秒
    s_preempt_prio = prio;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        s_preempt_prio = 1000;
        return false;
    }
    s_busy = true;
    s_stream_prio = prio;
    s_current_prio = prio;
    s_preempt_prio = 1000;   // 抢到了，恢复默认（不再打断别人）
    s_prev_sample = 0.0f;
    s_res_pos = 0.0;
    s_res_last = 0;
    pa(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

bool audio_hub_stream_write(const int16_t* pcm, size_t count, int src_rate)
{
    if (!s_busy || s_stream_prio < 0 || pcm == nullptr || count == 0) {
        return false;
    }
    bool aborted = false;
    float phase = 0.0f;
    write_resampled(pcm, count, src_rate, &phase, &aborted);
    return !aborted;
}

void audio_hub_stream_end()
{
    if (s_stream_prio < 0) {
        return;
    }
    std::memset(s_out, 0, sizeof(s_out));
    size_t written = 0;
    i2s_channel_write(s_tx, s_out, sizeof(s_out), &written, pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(60));
    pa(false);
    s_busy = false;
    s_stream_prio = -1;
    s_current_prio = -1;
    s_preempt_prio = 1000;
    xSemaphoreGive(s_lock);
}
