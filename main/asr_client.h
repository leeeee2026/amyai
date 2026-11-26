#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// 把 16kHz / 16bit / 单声道 PCM 送给智谱 GLM-ASR-2512，返回识别出的文字。
// 失败返回空字符串（原因会打到串口）。
std::string asr_transcribe(const int16_t* pcm, size_t samples);
