#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// 与主控板的串口通信（双芯片协议的语音板侧）
//
// 帧格式：A5 5A | ver | type | seq | len(2, 小端) | payload(JSON) | crc16(2)
//   type: 0x01 指令  0x02 应答  0x03 心跳  0x04 急停  0x05 状态上报
//   crc : CRC-16/CCITT-FALSE，覆盖 ver..payload
//
// 规则（见 docs/语音交互模块设计方案.md 第 6 节）：
//   - 发指令后 300ms 内没收到 ACK 就重发，最多 3 次
//   - 每秒发一次心跳，主控 3 秒收不到会停机（安全兜底）
//   - 急停（0x04）优先级最高，绕过大模型直接发
// ---------------------------------------------------------------------------

// 初始化 UART 并启动心跳任务。返回 false 表示配置没启用或初始化失败。
bool uart_link_init();

// 把一条 JSON 指令发给主控，内部负责组帧、等 ACK、重传。
// 返回 true = 主控确认收到。
bool uart_link_send_command(const std::string& json);

// 发急停帧（0x04），不等 ACK，立即返回。
void uart_link_send_estop();

// 语音识别出来的文字里是否包含急停词（停 / 别按 / 急停 …）
bool uart_link_is_estop_text(const std::string& text);
