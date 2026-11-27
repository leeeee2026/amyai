#include "local_asr.h"

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

// 一次最多喂 2.5 秒（16k），命令词都很短，够用还能省 CPU
constexpr size_t kMaxFeedSamples = 16000 * 5 / 2;

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
    if (samples > kMaxFeedSamples) {
        samples = kMaxFeedSamples;   // 只喂前面一段，够识别短命令
    }

    s_mn->clean(s_data);

    // MultiNet 要求按固定块大小喂数据
    size_t off = 0;
    while (off + static_cast<size_t>(s_chunk) <= samples) {
        const esp_mn_state_t st =
            s_mn->detect(s_data, const_cast<int16_t*>(pcm + off));
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t* res = s_mn->get_results(s_data);
            if (res != nullptr && res->num > 0) {
                const LocalCmdEntry* e = find_entry(res->command_id[0]);
                if (e != nullptr) {
                    if (out_cmd != nullptr) {
                        *out_cmd = e->json;
                    }
                    ESP_LOGI(kTag, "本地命中：「%s」-> %s（概率 %.2f）",
                             e->display, e->json, static_cast<double>(res->prob[0]));
                    return true;
                }
            }
        }
        off += static_cast<size_t>(s_chunk);
    }
    return false;
}
