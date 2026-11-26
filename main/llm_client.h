#pragma once

#include <string>

// 把用户说的话发给大模型，返回模型输出的指令文本（通常是 JSON 字符串）。
// 失败（未配置 Key / 网络错误 / 解析失败）返回空字符串。
// 内部会自动记录最近 3 轮对话作为上下文。
std::string ask_llm(const std::string& user_text);

// 清空对话上下文（重新唤醒时调用，开始一段新会话）
void llm_reset_history();
