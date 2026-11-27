// 离线唤醒词：数字 I2S 麦克风(44.1kHz) -> 降采样到 16kHz -> esp-sr AFE + WakeNet
//
// 2026-11-25 实测确定的硬件事实（正点原子 DNESP32S3B）：
//   I2C:  SDA=IO48  SCL=IO45   XL9555(0x20) 控制功放使能
//   I2S:  BCLK=IO21  WS=IO13  DIN=IO47  DOUT=IO14  MCLK 不用
//   采样率: 44.1kHz（其他采样率 codec 不出数据）
//   关键: I2S 必须同时创建 TX 和 RX 通道，否则时钟不跑、麦克风无数据
//   麦克风: MSM261S4030H0R 数字 I2S 麦克风（数据在右声道）

#include "diag.h"
#include "wake_word.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "model_path.h"

#include "prompt_zai_de.h"
#include "audio_hub.h"
#include "music_player.h"

namespace {

constexpr const char* kTag = "wake";

constexpr int kHwRate  = 44100;   // codec 只认这个
constexpr int kAfeRate = 16000;   // esp-sr 只认这个
constexpr float kStep  = static_cast<float>(kHwRate) / kAfeRate;  // 2.75625

constexpr int kTaskStack = 20 * 1024;   // TLS 证书校验很吃栈，留足
constexpr int kTaskPrio  = 5;

i2s_chan_handle_t s_tx = nullptr;
// ★ 提示音播放期间暂停录音：否则喇叭播报会被麦克风录进去，
//   下一句的音频就和"好的马上为您调整"混在一起，识别必然失败。
//   用计数器而不是 bool：万一两个提示音重叠，先结束的那个不会把后来者的暂停清掉。
//   另外带 5 秒超时自恢复：任何异常导致暂停没被解除，也不会让设备永远听不见。
int s_listen_pause_cnt = 0;
int64_t s_pause_since_us = 0;

// 跨任务读写用原子操作，避免数据竞争，也避开 volatile 自增的编译告警
inline int pause_cnt_get() { return __atomic_load_n(&s_listen_pause_cnt, __ATOMIC_ACQUIRE); }
inline void pause_cnt_set(int v) { __atomic_store_n(&s_listen_pause_cnt, v, __ATOMIC_RELEASE); }
constexpr int64_t kPauseTimeoutUs = 5LL * 1000 * 1000;

const int16_t* s_afe_pcm = nullptr;   // 最近一次录音的 AFE 输出（给离线命令词）
size_t s_afe_pcm_len = 0;
size_t s_afe_pcm_start = 0;           // 人声起点（样本下标）：VAD 判定人声的位置 - 300ms
i2s_chan_handle_t s_rx = nullptr;
i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_xl = nullptr;   // XL9555 @0x20，控制功放
const esp_afe_sr_iface_t* s_afe = nullptr;
esp_afe_sr_data_t* s_afe_data = nullptr;
srmodel_list_t* s_models = nullptr;
char s_model_name[64] = {0};

// ---------------------------------------------------------------- I2S
esp_err_t init_i2s()
{
    const int mclk = CONFIG_AMYAI_I2S_MCLK;
    const int bclk = CONFIG_AMYAI_I2S_BCLK;
    const int ws   = CONFIG_AMYAI_I2S_WS;
    const int din  = CONFIG_AMYAI_I2S_DIN;
    const int dout = CONFIG_AMYAI_I2S_DOUT;

    if (bclk < 0 || ws < 0 || din < 0) {
        ESP_LOGE(kTag, "I2S 引脚未配置：MCLK=%d BCLK=%d WS=%d DIN=%d DOUT=%d",
                 mclk, bclk, ws, din, dout);
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t chan_cfg = {};
    chan_cfg.id = I2S_NUM_0;
    chan_cfg.role = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear_after_cb = true;

    // ★ 必须 TX+RX 双通道，只开 RX 时钟不跑，麦克风没数据
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_new_channel 失败: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std = {};
    std.clk_cfg.sample_rate_hz = kHwRate;
    std.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std.slot_cfg.ws_pol = false;
    std.slot_cfg.bit_shift = true;
    std.slot_cfg.left_align = true;
    std.gpio_cfg.mclk = (mclk < 0) ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(mclk);
    std.gpio_cfg.bclk = static_cast<gpio_num_t>(bclk);
    std.gpio_cfg.ws   = static_cast<gpio_num_t>(ws);
    std.gpio_cfg.din  = static_cast<gpio_num_t>(din);
    std.gpio_cfg.dout = (dout < 0) ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(dout);

    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK ||
        i2s_channel_init_std_mode(s_rx, &std) != ESP_OK) {
        ESP_LOGE(kTag, "i2s 通道初始化失败");
        return ESP_FAIL;
    }
    if (i2s_channel_enable(s_tx) != ESP_OK || i2s_channel_enable(s_rx) != ESP_OK) {
        ESP_LOGE(kTag, "i2s 通道使能失败");
        return ESP_FAIL;
    }

    ESP_LOGI(kTag, "I2S 就绪 @%d Hz: MCLK=%d BCLK=%d WS=%d DIN=%d DOUT=%d",
             kHwRate, mclk, bclk, ws, din, dout);
    return ESP_OK;
}

void pa_set(bool on);   // 前向声明（定义在后面）

// ---------------------------------------------------------------- 初始化
// 这块板子（DNESP32S3B）没有 ES8311 之类的音频 codec 芯片：
//   功放 NS4168 直接吃 I2S 数字输入；
//   麦克风 MSM261S4030H0R 是数字 I2S 麦克风，直接输出到 I2S DIN。
// 所以这里只需要把 I2C 总线起起来，给 XL9555（功放使能 SPK_CTRL）用。
esp_err_t init_codec()
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(CONFIG_AMYAI_I2C_SDA);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(CONFIG_AMYAI_I2C_SCL);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(kTag, "I2C 总线初始化失败");
        return ESP_FAIL;
    }

    i2c_device_config_t xl_cfg = {};
    xl_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    xl_cfg.device_address = 0x20;          // XL9555
    xl_cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(s_bus, &xl_cfg, &s_xl) != ESP_OK) {
        ESP_LOGE(kTag, "XL9555 挂在 I2C 上失败，功放将无法控制");
        s_xl = nullptr;
    }

    // 先把功放关掉，避免上电爆音
    pa_set(false);
    ESP_LOGI(kTag, "音频硬件就绪（NS4168 功放 + 数字麦克风，无 codec 芯片）");
    return ESP_OK;
}


// ---------------------------------------------------------------- 功放 + 提示音
// 手册 33/34 章：SPK_CTRL 接 XL9555 的 IO0_5
static bool s_pa_state = false;

void pa_set(bool on)
{
    if (s_xl == nullptr) {
        return;
    }
    uint8_t b[3];
    // config0: bit5 设为输出(0)，其余保持输入(1)
    b[0] = 0x06;
    b[1] = static_cast<uint8_t>(0xFF & ~(1u << 5));
    b[2] = 0xFF;
    for (int i = 0; i < 2; ++i) {
        if (i2c_master_transmit(s_xl, b, 3, 100) == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    // output0: bit5 拉高/拉低（NS4168: CTRL 高电平 = 使能 + 右声道）
    b[0] = 0x02;
    b[1] = on ? static_cast<uint8_t>(1u << 5) : 0x00;
    b[2] = 0xFF;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3; ++i) {
        err = i2c_master_transmit(s_xl, b, 3, 100);
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "功放控制写入失败: %s", esp_err_to_name(err));
    } else if (on != s_pa_state) {
        s_pa_state = on;
        ESP_LOGI(kTag, "功放 %s", on ? "ON" : "OFF");
    }
}

// 把「在的」播出去（走 AudioHub）
// 放慢到 0.85 倍速播放：小喇叭上太快会听不清，稍微拉长并降低音调更好辨认
void play_prompt()
{
    // 延迟 500ms 再播：和本地应答保持一致的节奏
    vTaskDelay(pdMS_TO_TICKS(500));
    const int slow_rate = kPromptZaiDeRate * 85 / 100;
    audio_hub_play_pcm(kPromptZaiDe, kPromptZaiDeLen, slow_rate, AUDIO_PRIO_PROMPT);
}

// ---------------------------------------------------------------- AFE
esp_err_t init_afe()
{
    s_models = esp_srmodel_init("model");
    if (s_models == nullptr || s_models->num <= 0) {
        ESP_LOGE(kTag, "model 分区加载失败");
        return ESP_FAIL;
    }
    if (esp_srmodel_filter(s_models, ESP_WN_PREFIX, nullptr) == nullptr) {
        ESP_LOGE(kTag, "model 分区里没有 WakeNet 模型");
        return ESP_FAIL;
    }

    std::strncpy(s_model_name, CONFIG_AMYAI_WAKE_MODEL, sizeof(s_model_name) - 1);

    afe_config_t* cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (cfg == nullptr) {
        ESP_LOGE(kTag, "afe_config_init 失败");
        return ESP_FAIL;
    }
    cfg->wakenet_init = true;
    cfg->wakenet_model_name = s_model_name;
    cfg->pcm_config.sample_rate = kAfeRate;

    s_afe = esp_afe_handle_from_config(cfg);
    if (s_afe == nullptr) {
        ESP_LOGE(kTag, "AFE 句柄获取失败");
        afe_config_free(cfg);
        return ESP_FAIL;
    }
    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (s_afe_data == nullptr) {
        ESP_LOGE(kTag, "AFE 创建失败（内存不足？）");
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "AFE 就绪 @%d Hz，唤醒词 = \"%s\"", kAfeRate, CONFIG_AMYAI_WAKE_TEXT);
    return ESP_OK;
}

}  // namespace

void WakeWord::task_entry(void* arg)
{
    static_cast<WakeWord*>(arg)->run();
}

void WakeWord::run()
{
    const int chunk = s_afe->get_feed_chunksize(s_afe_data);
    const int ch    = s_afe->get_feed_channel_num(s_afe_data);
    const int need  = chunk * ch;

    ESP_LOGI(kTag, "AFE: chunk=%d 通道=%d 每次喂 %d 个样本", chunk, ch, need);

    // 硬件采集缓冲：44.1kHz 立体声
    constexpr int kHwFrames = 512;
    static int16_t hw[kHwFrames * 2];

    int16_t* feed = static_cast<int16_t*>(
        heap_caps_malloc(need * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (feed == nullptr) {
        feed = static_cast<int16_t*>(malloc(need * sizeof(int16_t)));
    }
    if (feed == nullptr) {
        ESP_LOGE(kTag, "缓冲分配失败");
        vTaskDelete(nullptr);
    }

    // 降采样状态
    float pos = 0.0f;      // 下一个输出样本在输入块中的位置
    int16_t tail = 0;      // 上一块最后一个样本（用于跨块插值）
    int filled = 0;        // feed 里已填多少
    int silent_secs = 0;   // 连续静音秒数，用于断流自愈
    int read_fail = 0;     // I2S 读取失败次数（诊断用）
    int good_secs = 0;     // 连续有真实音频的秒数（判定恢复用）
    int log_sec = 0;       // 日志节拍（每 5 秒打一次电平）
    int fed_chunks = 0;    // 已喂给 AFE 的块数（诊断）
    int res_cnt = 0;       // AFE 返回结果的次数（诊断）
    int last_st = 0;       // 最近一次 AFE 状态（诊断）
    (void)last_st;
    int l_pk = 0, r_pk = 0;  // 左右声道峰值（诊断）
    bool follow_up_mode = false;   // 连续对话：回复完继续聆听，不用再喊唤醒词
    int follow_up_turns = 0;       // 连续对话轮数
    // ★ 麦克风声道判定：这块板子的数字麦固定只输出在某一路，开机统计 1 秒后锁定，
    //   不要用"开机至今累加"来比较（会 int 溢出，导致选到空声道被误判成断流）
    uint32_t ch_vote_left = 0;
    uint32_t ch_vote_right = 0;
    bool ch_locked = false;
    bool use_left = false;

    // 麦克风电平统计（每秒打印一次，用来确认音频真的进来了）
    uint32_t lvl_sum = 0;
    uint32_t lvl_max = 0;
    int lvl_cnt = 0;

    // 唤醒后的录音缓冲（最长 6 秒 @16kHz = 96000 样本 = 192KB，放 PSRAM）
    // 正常会用 VAD 自动断句（说完就停），只有一直没停才录满 6 秒
    constexpr int kRecordSamples = kAfeRate * 6;
    auto* rec = static_cast<int16_t*>(
        heap_caps_malloc(kRecordSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (rec == nullptr) {
        rec = static_cast<int16_t*>(malloc(kRecordSamples * sizeof(int16_t)));
    }
    // ★ 同时缓存一份 AFE 处理后的音频（降噪+增益），给离线命令词识别用。
    //   官方要求 MultiNet 吃 AFE 输出而不是原始麦克风数据，识别率差很多。
    auto* afe_pcm = static_cast<int16_t*>(
        heap_caps_malloc(kRecordSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (afe_pcm == nullptr) {
        afe_pcm = static_cast<int16_t*>(malloc(kRecordSamples * sizeof(int16_t)));
    }
    int afe_fill = 0;
    s_afe_pcm = afe_pcm;
    s_afe_pcm_len = 0;
    AMYAI_DIAGI(kTag, "录音缓冲: rec=%s(%d KB) afe_pcm=%s(%d KB)",
             rec ? "OK" : "NULL", kRecordSamples * 2 / 1024,
             afe_pcm ? "OK" : "NULL", kRecordSamples * 2 / 1024);

    bool recording = false;
    int rec_fill = 0;

    // VAD 断句状态
    int speech_start = 0;      // 本次录音中人声起点（AFE 缓冲下标）
    int dbg_pk_max = 0;        // 本秒内 10ms 块的最大峰值（诊断）
    int vad_acc = 0;           // 当前 10ms 块的能量累加
    int vad_n = 0;             // 当前块已累计样本数
    bool speech_seen = false;  // 本次录音是否已经检测到人声
    int quiet_chunks = 0;      // 人声之后连续安静的 10ms 块数
    int total_chunks = 0;      // 本次录音总块数（用于最短录音保护）
    int speech_chunks = 0;     // 连续有声的块数（要求持续 150ms 才算开始说话）
    int noise_pk = 1200;       // 环境底噪峰值估计（自适应）
    uint32_t vad_pk = 0;       // 当前 10ms 块的峰值

    while (true) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, hw, sizeof(hw), &got, pdMS_TO_TICKS(1000)) != ESP_OK || got < 8) {
            ++read_fail;
            continue;
        }
        const int frames = static_cast<int>(got / 4);  // 16bit × 2 声道

        // 判断麦克风在哪一路（左右声道能量对比）
        int le = 0, re = 0;
        for (int i = 0; i < frames; ++i) {
            const int l = hw[i * 2];
            const int r = hw[i * 2 + 1];
            const int la = (l < 0) ? -l : l;
            const int ra = (r < 0) ? -r : r;
            le += la;
            re += ra;
            if (la > l_pk) l_pk = la;
            if (ra > r_pk) r_pk = ra;
        }
        // 声道判定：前 1 秒（约 86 块）投票，之后锁定，运行期不再切换
        if (!ch_locked) {
            if (le > re) {
                ++ch_vote_left;
            } else if (re > le) {
                ++ch_vote_right;
            }
            if (ch_vote_left + ch_vote_right >= 86) {
                use_left = (ch_vote_left >= ch_vote_right);
                ch_locked = true;
                ESP_LOGI(kTag, "麦克风声道锁定：%s声道（左票 %u / 右票 %u）",
                         use_left ? "左" : "右",
                         static_cast<unsigned>(ch_vote_left),
                         static_cast<unsigned>(ch_vote_right));
            }
        }

        // 线性插值降采样 44.1k -> 16k
        while (pos < frames) {
            // pos 可能是负数（跨块插值借用上一块最后一个样本），必须向下取整
            const int i0 = static_cast<int>(std::floor(pos));
            const float frac = pos - i0;
            const int16_t a = (i0 < 0) ? tail : hw[i0 * 2 + (use_left ? 0 : 1)];
            const int16_t b = (i0 + 1 < frames) ? hw[(i0 + 1) * 2 + (use_left ? 0 : 1)] : a;
            const int16_t v = static_cast<int16_t>(a + (b - a) * frac);

            feed[filled++] = v;

            // 录音状态：把降采样后的 16k 样本存进缓冲
            const bool listen_paused = (pause_cnt_get() > 0);
            if (recording && listen_paused) {
                // 提示音正在播：这期间的音频丢掉，播完从零开始一个新窗口，
                // 避免喇叭声被录进来污染下一句。
                rec_fill = 0;
                afe_fill = 0;
                speech_start = 0;
                vad_acc = 0;
                vad_n = 0;
                speech_seen = false;
                quiet_chunks = 0;
                total_chunks = 0;
                speech_chunks = 0;
            }
            if (recording && rec != nullptr && !listen_paused) {
                // ★ 硬保护：绝不能写越界（越界会破坏堆，导致状态机卡死）
                if (rec_fill < kRecordSamples) {
                    rec[rec_fill++] = v;
                }

                // ---- VAD 断句：用"峰值"判断，说话峰值远大于底噪（实测 7000+ vs 1700）----
                const uint32_t av = static_cast<uint32_t>(v < 0 ? -v : v);
                vad_acc += av;
                if (av > vad_pk) {
                    vad_pk = av;
                }
                if (++vad_n >= kAfeRate / 100) {          // 10ms 一块
                    const int pk = static_cast<int>(vad_pk);
                    vad_acc = 0;
                    vad_pk = 0;
                    vad_n = 0;
                    ++total_chunks;

                    // 底噪峰值自适应：慢速跟随最小值
                    if (pk < noise_pk) {
                        noise_pk = (noise_pk * 7 + pk) / 8;
                    } else if (pk < noise_pk * 2) {
                        noise_pk = (noise_pk * 31 + pk) / 32;
                    }
                    if (noise_pk < 800) noise_pk = 800;
                    if (pk > dbg_pk_max) dbg_pk_max = pk;

                    // 说话阈值：正常要明显高于底噪；音乐窗口要更高（避免喇叭声自激）
                    const int speech_pk_thresh = music_window_
                                                     ? 8000
                                                     : ((noise_pk * 3 > 2500) ? (noise_pk * 3) : 2500);
                    const int quiet_pk_thresh = music_window_
                                                    ? 4000
                                                    : ((noise_pk * 2 > 2000) ? (noise_pk * 2) : 2000);

                    if (total_chunks > 20) {
                        if (pk > speech_pk_thresh) {
                            ++speech_chunks;
                            quiet_chunks = 0;
                            if (speech_chunks >= (music_window_ ? 8 : 10)) {
                                if (!speech_seen && music_window_) {
                                    ESP_LOGI(kTag, "音乐窗口：检测到人声（峰值 %d）", pk);
                                }
                                if (!speech_seen) {
                                    // ★ 记下人声起点：VAD 判定的这一刻往回留 300ms，
                                    //   之后直接把 [起点, 结尾] 交给离线识别，不用再猜裁剪位置。
                                    constexpr int kPreMargin = kAfeRate * 300 / 1000;
                                    const int at = afe_fill - kPreMargin;
                                    speech_start = (at > 0) ? at : 0;
                                    AMYAI_DIAGI(kTag, "人声起点：第 %d 样本（%d ms）",
                                             speech_start, speech_start * 1000 / kAfeRate);
                                }
                                speech_seen = true;
                            }
                        } else {
                            speech_chunks = 0;
                            if (speech_seen && pk < quiet_pk_thresh) {
                                ++quiet_chunks;
                            }
                        }
                    }
                }
                // 结束条件：确实说过话 + 静音（音乐窗口 500ms，正常 600ms）+ 最短录音；或录满上限
                const int quiet_need = music_window_ ? 45 : 45;   // 450ms 静音即断句（原 600ms，快了 150ms）
                const int min_chunks = music_window_ ? 30 : 80;   // 音乐窗口至少要 300ms 人声
                const bool vad_done = speech_seen && quiet_chunks >= quiet_need && total_chunks >= min_chunks;
                const int rec_limit = music_window_ ? (kAfeRate * 5 / 2) : kRecordSamples;   // 音乐窗口最多 2.5 秒

                // ★ 连续对话模式：设定时长内一直没人说话，就退出聆听、回到等唤醒词
                const int kFollowUpChunks = CONFIG_AMYAI_FOLLOW_UP_SEC * 100;   // 100 块 = 1 秒
                if (follow_up_mode && !music_window_ && !speech_seen && total_chunks >= kFollowUpChunks) {
                    recording = false;
                    follow_up_mode = false;
                    rec_fill = 0;
                    afe_fill = 0;
                    enabled_ = true;
                    ESP_LOGI(kTag, "连续对话窗口结束（没听到说话），回到唤醒词模式");
                } else if (vad_done || rec_fill >= rec_limit) {
                    recording = false;
                    // ★ 整段都没检测到人声：不送云端识别（否则 ASR 返回乱码，白花一次调用）
                    if (!speech_seen) {
                        rec_fill = 0;
                        afe_fill = 0;
                        vad_acc = 0;
                        vad_n = 0;
                        quiet_chunks = 0;
                        total_chunks = 0;
                        speech_chunks = 0;
                        if (music_window_) {
                            static int s_idle = 0;
                            if ((++s_idle % 5) == 1) {
                                ESP_LOGI(kTag, "音乐窗口：这一轮没听到人声，继续监听（第 %d 轮）", s_idle);
                            }
                            recording = true;      // 音乐窗口：继续听
                            noise_pk = 1200;
                        } else if (follow_up_mode && ++follow_up_turns < 200) {
                            recording = true;      // 继续听下一句
                            noise_pk = 1200;
                        } else {
                            enabled_ = true;
                            follow_up_mode = false;
                            ESP_LOGI(kTag, "没听到人声，回到唤醒词模式");
                        }
                    } else {
                        AMYAI_DIAGI(kTag, "录音结束：%d 样本（%.1f 秒）%s", rec_fill,
                                 static_cast<float>(rec_fill) / kAfeRate,
                                 vad_done ? "，VAD 断句" : "，录满上限");

                        if (on_utterance_) {
                            s_afe_pcm_len = static_cast<size_t>(afe_fill);
                            s_afe_pcm_start = static_cast<size_t>(speech_start);
                            on_utterance_(rec, static_cast<size_t>(rec_fill));
#if AMYAI_DIAG_ENABLE
                            // 处理完一轮后打一次资源水位：查栈溢出 / 内存泄漏
                            AMYAI_DIAGI(kTag, "[资源] 唤醒任务剩余栈 %u 字节 | 内部RAM %u 空闲(最大块 %u) | PSRAM %u 空闲",
                                        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
#endif
                        }
                        rec_fill = 0;
                        afe_fill = 0;
                        speech_start = 0;
                        // 处理完（ASR+大模型，几秒）再恢复唤醒检测，并丢弃期间堆积的音频
                        size_t drained = 0;
                        while (i2s_channel_read(s_rx, hw, sizeof(hw), &drained, 0) == ESP_OK && drained > 0) {
                        }
                        // ★ 连续对话：处理完直接进入下一轮录音（不需要唤醒词）
                        //   但如果正在放音乐，不能进连续聆听（否则喇叭声会被当成说话）
                        if (music_window_) {
                            // 音乐窗口：处理完继续监听下一条短指令
                            rec_fill = 0;
                            afe_fill = 0;
                            vad_acc = 0;
                            vad_n = 0;
                            speech_seen = false;
                            quiet_chunks = 0;
                            total_chunks = 0;
                            speech_chunks = 0;
                            recording = true;
                            enabled_ = true;            // 音乐窗口里也允许说唤醒词
                            noise_pk = 1200;
                            audio_hub_set_music_volume(20);   // 只压低音乐（TTS/提示音不受影响）
                        } else if (cancel_follow_up_) {
                            cancel_follow_up_ = false;
                            recording = false;
                            follow_up_mode = false;
                            enabled_ = true;
                            ESP_LOGI(kTag, "已取消连续聆听，回到唤醒词模式");
                        } else if (follow_up_mode && !music_is_playing() && ++follow_up_turns < 200) {
                            rec_fill = 0;
                            afe_fill = 0;
                            vad_acc = 0;
                            vad_n = 0;
                            speech_seen = false;
                            quiet_chunks = 0;
                            total_chunks = 0;
                            speech_chunks = 0;
                            recording = true;   // ★ 必须真的开始录音，否则只是"看起来在听"
                            noise_pk = 1200;
                            ESP_LOGI(kTag, "继续聆听（连续对话第 %d 轮），可以直接说话", follow_up_turns);
                        } else {
                            enabled_ = true;
                            follow_up_mode = false;
                        }
                    }
                }
            }

            {
                const uint32_t a = static_cast<uint32_t>(v < 0 ? -v : v);
                lvl_sum += a;
                if (a > lvl_max) lvl_max = a;
                if (++lvl_cnt >= kAfeRate) {
                    const uint32_t mean = lvl_sum / lvl_cnt;
                    const bool silent_now = (lvl_max == 0);
                    // 用均值判断"真实音频"：单个 32768 尖峰的均值只有个位数，真实说话在几百以上
                    const bool has_real = (mean > 50);
                    ++log_sec;
                    // 暂停超时自恢复：防止提示音任务异常导致设备永久"失聪"
                    if (pause_cnt_get() > 0 &&
                        esp_timer_get_time() - s_pause_since_us > kPauseTimeoutUs) {
                        ESP_LOGE(kTag, "提示音暂停超过 5 秒未解除，强制恢复录音");
                        pause_cnt_set(0);
                    }
                    if (recording) {
                        AMYAI_DIAGI(kTag, "[录音诊断] fill=%d speech=%d quiet=%d chunks=%d noise=%d 本秒峰值=%d 连续语音块=%d",
                                 rec_fill, speech_seen ? 1 : 0, quiet_chunks,
                                 total_chunks, noise_pk, dbg_pk_max, speech_chunks);
                        dbg_pk_max = 0;
                        // ★ 保险：缓冲写满却没结束，说明判定逻辑异常，强制收尾（防止卡死）
                        if (rec_fill >= kRecordSamples) {
                            AMYAI_DIAGE(kTag, "录音缓冲写满仍未结束，强制收尾（丢弃本次录音）");
                            recording = false;
                            rec_fill = 0;
                            afe_fill = 0;
                            vad_acc = 0;
                            vad_n = 0;
                            speech_seen = false;
                            quiet_chunks = 0;
                            total_chunks = 0;
                            speech_chunks = 0;
                            enabled_ = true;
                            follow_up_mode = false;
                        }
                    }
                    if ((log_sec % 5) == 0) {
                        AMYAI_DIAGI(kTag, "MIC 峰值=%u 均值=%u | L=%d R=%d 用%s | AFE: 结果 %d, 状态 %d",
                                 static_cast<unsigned>(lvl_max),
                                 static_cast<unsigned>(mean),
                                 l_pk, r_pk, use_left ? "左" : "右",
                                 res_cnt, last_st);
                        l_pk = 0;
                        r_pk = 0;
                    }
                    lvl_sum = 0;
                    lvl_max = 0;
                    lvl_cnt = 0;



                    // 断流判定：只有"真实音频"才算恢复。
                    // 重放配置会产生一个 32768 的瞬态尖峰，不能当成有声音。
                    if (has_real) {
                        ++good_secs;
                    } else {
                        good_secs = 0;
                    }
                    if (good_secs >= 3) {
                        // 连续 3 秒有真实声音：确认已恢复
                        silent_secs = 0;
                    } else if (silent_now) {
                        ++silent_secs;
                    }

                    if (silent_secs >= 3) {
                        // 实测：寄存器重放 / 长时钟停顿 / I2S 重建 / 换电源 / 断开 WiFi 都救不回来，
                        // 只有整机重启有效。所以别浪费时间反复尝试，直接重启（约 3 秒恢复）。
                        ESP_LOGE(kTag, "麦克风断流（%d 秒全 0），直接重启整机", silent_secs);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        esp_restart();
                    }
                }
            }
            if (filled >= need) {
                s_afe->feed(s_afe_data, feed);
                filled = 0;
                ++fed_chunks;

                afe_fetch_result_t* res = s_afe->fetch(s_afe_data);
                if (res != nullptr && res->ret_value != -1) {
                    ++res_cnt;
                    last_st = res->wakeup_state;
                    // 录音期间把 AFE 处理后的音频存一份（给 MultiNet 用）
                    if (recording && pause_cnt_get() == 0 && res->data != nullptr &&
                        res->data_size > 0 && afe_pcm != nullptr) {
                        const int n = res->data_size / static_cast<int>(sizeof(int16_t));
                        if (afe_fill + n <= kRecordSamples) {
                            std::memcpy(afe_pcm + afe_fill, res->data, res->data_size);
                            afe_fill += n;
                            s_afe_pcm_len = afe_fill;
                        }
                    }
                    if (res->wakeup_state == WAKENET_DETECTED) {
                        AMYAI_DIAGW(kTag, "AFE 检测到唤醒词！enabled_=%d", enabled_ ? 1 : 0);
                    }
                    // ★ 只要听到唤醒词就一定回应：即使正在录音/聆听中也重新开始一轮
                    if (res->wakeup_state == WAKENET_DETECTED) {
                        if (!enabled_) {
                            ESP_LOGI(kTag, "录音中再次听到唤醒词，重新开始一轮");
                        }
                        ESP_LOGI(kTag, "★ 唤醒命中: \"%s\"", CONFIG_AMYAI_WAKE_TEXT);
                        enabled_ = false;   // 录音+处理期间不再响应唤醒，避免连续触发
                        if (on_wake_) {
                            on_wake_();
                        }
                        play_prompt();          // 先回一句「在的」
                        recording = true;
                        rec_fill = 0;
                        afe_fill = 0;
                        vad_acc = 0;
                        vad_n = 0;
                        speech_seen = false;
                        quiet_chunks = 0;
                        total_chunks = 0;
                        speech_chunks = 0;
                        follow_up_mode = true;   // 回复完之后继续聆听
                        follow_up_turns = 0;
                        ESP_LOGI(kTag, "开始录音（说完自动结束，最长 6 秒）");
                    }
                }
            }
            // 音乐播放窗口：不需要唤醒词，直接开始监听短指令（"停""关掉"）
            // 监听期间把音乐音量压低到 20%，否则喇叭声会盖住人说话，云端识别只能得到乱码
            if (music_window_ && enabled_ && !recording && s_tx != nullptr) {
                recording = true;
                rec_fill = 0;
                afe_fill = 0;
                vad_acc = 0;
                vad_n = 0;
                speech_seen = false;
                quiet_chunks = 0;
                total_chunks = 0;
                speech_chunks = 0;
                noise_pk = 1200;
                enabled_ = true;            // 音乐窗口里也允许说唤醒词
                audio_hub_set_music_volume(20);
                ESP_LOGI(kTag, "音乐窗口：开始监听短指令（音乐压低到 20%%，否则识别不出人声）");
            }

            // 外部要求"接着说"（比如刚停下音乐）：直接进入连续聆听，不用唤醒词
            if (force_follow_up_ && enabled_ && !recording && !music_is_playing() && s_tx != nullptr) {
                force_follow_up_ = false;
                follow_up_mode = true;
                follow_up_turns = 0;
                recording = true;
                rec_fill = 0;
                afe_fill = 0;
                vad_acc = 0;
                vad_n = 0;
                speech_seen = false;
                quiet_chunks = 0;
                total_chunks = 0;
                speech_chunks = 0;
                noise_pk = 1200;
                ESP_LOGI(kTag, "自动进入连续聆听，可以直接说话");
            }
            pos += kStep;
        }
        pos -= frames;
        tail = hw[(frames - 1) * 2 + (use_left ? 0 : 1)];
    }
}

bool WakeWord::start(OnWake on_wake, OnUtterance on_utterance)
{
    on_wake_ = std::move(on_wake);
    on_utterance_ = std::move(on_utterance);

    if (init_codec() != ESP_OK) return false;
    if (init_i2s() != ESP_OK) return false;
    if (init_afe() != ESP_OK) return false;

    // I2S TX 已就绪，把功放开关交给 AudioHub 托管
    audio_hub_init(s_tx, &pa_set);

    if (xTaskCreate(WakeWord::task_entry, "wake_word", kTaskStack, this, kTaskPrio, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "创建任务失败");
        return false;
    }
    ESP_LOGI(kTag, "唤醒监听已启动，请说：\"%s\"", CONFIG_AMYAI_WAKE_TEXT);
    return true;
}

// 供离线命令词模块复用模型表（AFE 初始化时已 mmap 好 model 分区）
void* wake_model_list() { return static_cast<void*>(s_models); }

// 提示音播放期间暂停录音：防止喇叭播报被麦克风录进来，污染下一句识别
void wake_pause_listening(bool pause)
{
    const int cur = pause_cnt_get();
    if (pause) {
        if (cur == 0) {
            s_pause_since_us = esp_timer_get_time();
        }
        pause_cnt_set(cur + 1);
    } else if (cur > 0) {
        pause_cnt_set(cur - 1);
    }
}

// 取最近一次录音的 AFE 处理后音频（16k 单声道），给离线命令词识别用
const int16_t* wake_afe_pcm(size_t* samples)
{
    // ★ 直接从人声起点返回，调用方不用再做静音裁剪
    if (s_afe_pcm == nullptr || s_afe_pcm_len <= s_afe_pcm_start) {
        if (samples != nullptr) *samples = 0;
        return s_afe_pcm;
    }
    if (samples != nullptr) *samples = s_afe_pcm_len - s_afe_pcm_start;
    return s_afe_pcm + s_afe_pcm_start;
}