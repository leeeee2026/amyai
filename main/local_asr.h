#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// 离线命令词识别（esp-sr MultiNet7 中文）
//
// 定位：唤醒词之后的"快路径"。
//   「小鹿小鹿」+「重一点」 → 本地 300ms 内识别并执行（不联网、不花调用费）
//   「小鹿小鹿」+「讲个笑话」 → 本地说没听懂 → 回落到云端 ASR + 大模型
//
// 用法：
//   local_asr_init();                                  // 开机一次
//   std::string cmd;
//   if (local_asr_recognize(pcm, n, &cmd)) { 执行 cmd }
// ---------------------------------------------------------------------------

// 初始化。返回 false 表示模型分区里没有 MultiNet 模型（此时功能自动降级为纯云端）
bool local_asr_init();

// 对一段 16kHz / 16bit / 单声道 PCM 做命令词识别。
// 命中返回 true，out_cmd 填入与云端口径一致的 JSON 指令（如 {"cmd":"start"}）。
// 只喂前 2.5 秒，命中即返回，避免长语音拖慢回落路径。
bool local_asr_recognize(const int16_t* pcm, size_t samples, std::string* out_cmd);

// 是否初始化成功（没成功就走云端，不影响主流程）
bool local_asr_ready();
