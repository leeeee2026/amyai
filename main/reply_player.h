#pragma once

#include <string>

// ---------------------------------------------------------------------------
// 流式回复播放器
//
// 用途：大模型边生成，这边边切句、边合成播报，用户不用等整段回复生成完。
//
// 流程：
//   reply_player_begin()                 开始新一轮
//   reply_player_feed(delta) x N         边收边喂（内部按标点切句）
//   reply_player_finish()                流结束，把剩余文本推出去
//   reply_player_wait_done(timeout)      等播完
//
// 抖动缓冲：拿到第一个完整句子后不立刻播，先等 kStartGraceMs 攒一点后续内容，
// 这样网络抖一下也不会出现"播一句卡一下"。等不到就按现有的播。
// ---------------------------------------------------------------------------

// 初始化（创建队列和播放任务），只需调用一次
bool reply_player_init();

// 开始新一轮：清空状态、重置计数
void reply_player_begin();

// 喂入 reply 字段的增量文本（已去掉 JSON 转义）
void reply_player_feed(const std::string& delta);

// 流结束：剩下的文本按一句处理，然后通知播放任务收尾
void reply_player_finish();

// 等待本轮全部播完（返回 false 表示超时）
bool reply_player_wait_done(int timeout_ms);

// 本轮是否有内容被播报过（用于决定要不要播兜底语）
bool reply_player_spoke_anything();
