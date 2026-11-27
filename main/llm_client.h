#pragma once

#include <functional>
#include <string>

// 问大模型：返回模型输出的 JSON 字符串（失败返回空串）
std::string ask_llm(const std::string& user_text);

// 流式问大模型：边收边回调 reply 字段的增量文本。
// on_delta 收到的是 reply 字段里新增的那一段（已去 JSON 转义，可直接送 TTS）。
// 返回值仍然是完整 JSON（用来解析 intent/cmd），失败返回空串。
std::string ask_llm_stream(const std::string& user_text,
                           const std::function<void(const std::string&)>& on_delta);

// 清空多轮对话上下文
void llm_reset_history();
