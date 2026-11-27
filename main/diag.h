#pragma once

// ---------------------------------------------------------------------------
// 诊断日志开关
//
// 开发阶段打开（能看到录音电平、VAD 状态、本地识别细节、各阶段耗时），
// 上线前把 AMYAI_DIAG_ENABLE 改成 0，所有诊断日志一次性消失，
// 不用去各文件里一行行删。
// ---------------------------------------------------------------------------

#include "esp_log.h"

#ifndef AMYAI_DIAG_ENABLE
#define AMYAI_DIAG_ENABLE 1
#endif

#if AMYAI_DIAG_ENABLE
#define AMYAI_DIAGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define AMYAI_DIAGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define AMYAI_DIAGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#define AMYAI_DIAGI(tag, fmt, ...) ((void)0)
#define AMYAI_DIAGW(tag, fmt, ...) ((void)0)
#define AMYAI_DIAGE(tag, fmt, ...) ((void)0)
#endif
