// 离线命令词识别（esp-sr MultiNet7 中文）
//
// 完整链路：
//   唤醒词命中
//     -> 录音（同时缓存一份 AFE 处理后的音频）
//     -> VAD 检测到人声，记下起点
//     -> 说完 + 450ms 静音，录音结束
//     -> 本模块：喂 [人声起点-300ms, 结尾] + 480ms 尾部静音给 MultiNet
//     -> 命中：返回指令 JSON（<0.6 秒），由 main 下发 UART
//     -> 未命中：回落云端 ASR + 大模型
//
// 两个关键细节（都是实测踩出来的）：
//   1. 必须喂 AFE 处理后的音频，不能喂原始麦克风数据；
//   2. 喂完真实音频必须补 480ms 静音，否则流式解码器不吐最后一个音节
//      （会只识别出 "shui mian mo" 而不是 "shui mian mo shi"）。

#include "diag.h"
#include "local_asr.h"

#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

// 词表由 tools/gen_local_asr.py 从 docs/离线命令词表.md 生成，不要手改
#include "local_asr_cmds.h"
#include "wake_word.h"

namespace {

constexpr const char* kTag = "localasr";

// 一次最多喂 3 秒（16k）。命令词最长 6 个字（约 1.5 秒），3 秒足够且不会截断。
constexpr size_t kMaxFeedSamples = 16000 * 3;

const esp_mn_iface_t* s_mn = nullptr;
model_iface_data_t* s_data = nullptr;
int s_chunk = 0;
bool s_ready = false;

// 按指令 ID 找词条（同一 ID 可能有多条同义词，取第一条即可）
const LocalCmdEntry* find_entry(int id)
{
    for (int i = 0; i < kLocalCmdCount; ++i) {
        if (kLocalCmds[i].id == id) {
            return &kLocalCmds[i];
        }
    }
    return nullptr;
}

}  // namespace

bool local_asr_ready()
{
    return s_ready;
}

bool local_asr_init()
{
    srmodel_list_t* models = static_cast<srmodel_list_t*>(wake_model_list());
    if (models == nullptr) {
        ESP_LOGW(kTag, "模型表为空，离线识别不可用");
        return false;
    }
    char* name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (name == nullptr) {
        ESP_LOGW(kTag, "model 分区里没有中文 MultiNet 模型，离线识别不可用（走云端）");
        return false;
    }
    ESP_LOGI(kTag, "加载离线命令词模型: %s", name);

    s_mn = esp_mn_handle_from_name(name);
    if (s_mn == nullptr) {
        ESP_LOGE(kTag, "esp_mn_handle_from_name 失败");
        return false;
    }
    s_data = s_mn->create(name, 5000);   // 5 秒超时
    if (s_data == nullptr) {
        ESP_LOGE(kTag, "MultiNet create 失败（可能是内存不足）");
        return false;
    }
    // ★ 命中阈值：默认值偏高（实测 0.52 概率才刚过线），调到 0.4 提高命中率。
    //   误识别风险由"140 条同义词表 + 本地指令白名单"兜底。
    if (s_mn->set_det_threshold != nullptr) {
        s_mn->set_det_threshold(s_data, 0.4f);
        AMYAI_DIAGI(kTag, "命中阈值设为 0.40");
    }
    s_chunk = s_mn->get_samp_chunksize(s_data);
    if (s_chunk <= 0) {
        ESP_LOGE(kTag, "get_samp_chunksize 异常: %d", s_chunk);
        s_mn->destroy(s_data);
        s_data = nullptr;
        return false;
    }
    ESP_LOGI(kTag, "MultiNet 就绪：采样率 %d，每次喂 %d 个样本（%.0f ms）",
             s_mn->get_samp_rate(s_data), s_chunk,
             s_chunk * 1000.0f / s_mn->get_samp_rate(s_data));

    // 注册命令词（MultiNet 中文要求拼音，不是汉字）
    esp_mn_commands_alloc(s_mn, s_data);
    esp_mn_commands_clear();
    int ok_cnt = 0;
    for (int i = 0; i < kLocalCmdCount; ++i) {
        const esp_err_t r = esp_mn_commands_add(kLocalCmds[i].id, kLocalCmds[i].pinyin);
        if (r == ESP_OK) {
            ++ok_cnt;
        } else {
            ESP_LOGW(kTag, "命令词「%s」(%s) 注册失败: %s",
                     kLocalCmds[i].display, kLocalCmds[i].pinyin, esp_err_to_name(r));
        }
    }
    esp_mn_error_t* err = esp_mn_commands_update();
    if (err != nullptr && err->num > 0) {
        ESP_LOGW(kTag, "有 %d 条命令词无法建模", err->num);
        for (int i = 0; i < err->num && i < 5; ++i) {
            if (err->phrases[i] != nullptr) {
                ESP_LOGW(kTag, "  无法识别: %s", err->phrases[i]->string);
            }
        }
    }
    ESP_LOGI(kTag, "命令词表就绪：%d/%d 条（%d 个指令）", ok_cnt, kLocalCmdCount,
             kLocalCmdMaxId);

    s_ready = (ok_cnt > 0);
    return s_ready;
}

bool local_asr_recognize(const int16_t* pcm, size_t samples, std::string* out_cmd)
{
    if (!s_ready || pcm == nullptr || samples == 0) {
        return false;
    }

    // 兜底裁剪：正常情况下 wake_afe_pcm() 已经对齐到人声起点，这里基本是空操作；
    // 保留是为了防止上游某天不再做对齐（直接喂原始缓冲）时把前导静音喂进来。
    {
        constexpr int kVoiceThresh = 600;
        constexpr size_t kMargin = 16000 * 300 / 1000;   // 语音起点前保留 300ms
        size_t first = 0;
        while (first < samples && std::abs(static_cast<int>(pcm[first])) < kVoiceThresh) {
            ++first;
        }
        // ★ 只要裁完还剩 100ms 就裁。语音常常在录音后段（用户等提示音播完才开口），
        //   之前用 "first + 余量 + 1600 < samples" 判断，后段语音就不裁，
        //   接着又被 3 秒上限截掉 → 喂进去全是前面的静音（实测峰值只剩 1195）。
        if (first > kMargin && (samples - (first - kMargin)) >= 1600) {
            const size_t skip = first - kMargin;
            AMYAI_DIAGI(kTag, "跳过开头静音 %u ms（保留 300ms 余量），剩余音频 %u ms",
                     static_cast<unsigned>(skip * 1000 / 16000),
                     static_cast<unsigned>((samples - skip) * 1000 / 16000));
            pcm += skip;
            samples -= skip;
        }
    }
    if (samples > kMaxFeedSamples) {
        samples = kMaxFeedSamples;   // 最多 3 秒
    }

    s_mn->clean(s_data);

    // MultiNet 要求按固定块大小喂数据。
    // ★ 关键：喂完真实音频后必须再喂一段静音。流式解码器要靠尾部静音才吐出最后一个音节，
    //   否则会像实测那样只识别出 "shui mian mo"（缺"shi"）、"zhong"（缺"yi dian"）。
    static int16_t silence[512] = {0};
    const int real_chunks = static_cast<int>(samples / static_cast<size_t>(s_chunk));
    const int tail_chunks = 15;   // 15 * 32ms ≈ 480ms 尾部静音
    esp_mn_state_t last_state = ESP_MN_STATE_DETECTING;
    (void)last_state;   // 诊断关闭时避免 "set but not used" 警告
    for (int ci = 0; ci < real_chunks + tail_chunks; ++ci) {
        const int16_t* block = (ci < real_chunks)
                                   ? (pcm + static_cast<size_t>(ci) * s_chunk)
                                   : silence;
        const esp_mn_state_t st = s_mn->detect(s_data, const_cast<int16_t*>(block));
        last_state = st;
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t* res = s_mn->get_results(s_data);
            if (res != nullptr && res->num > 0) {
                const LocalCmdEntry* e = find_entry(res->command_id[0]);
                if (e != nullptr) {
                    if (out_cmd != nullptr) {
                        *out_cmd = e->json;
                    }
                    AMYAI_DIAGI(kTag, "本地命中：「%s」-> %s（概率 %.2f）",
                             e->display, e->json, static_cast<double>(res->prob[0]));
                    return true;
                }
            }
        }
    }
#if AMYAI_DIAG_ENABLE
    {
        // 诊断：喂进去的音频峰值 + 模型内部识别结果
        // （正式版这里的整段会被编译掉，不占 CPU）
        int peak = 0;
        for (size_t i = 0; i < samples; ++i) {
            const int a = std::abs(static_cast<int>(pcm[i]));
            if (a > peak) peak = a;
        }
        esp_mn_results_t* r = s_mn->get_results(s_data);
        AMYAI_DIAGI(kTag, "本地未命中（%u ms，峰值 %d）状态=%d 模型识别=\"%s\" 候选=%d",
                 static_cast<unsigned>(samples * 1000 / 16000), peak,
                 static_cast<int>(last_state),
                 (r != nullptr) ? r->string : "",
                 (r != nullptr) ? r->num : -1);
    }
#endif
    return false;
}
