#pragma once

// 网页配网（模式 A：设备还没有 Wi-Fi 凭据时进入）
//
// 流程：
//   1. 启动 SoftAP（隐藏 SSID + 固定强密码）+ 固定 IP 192.168.4.1
//   2. 起 HTTP 服务，手机扫码连上来后打开配置页
//   3. 用户填 Wi-Fi + 身体信息（都可跳过身体信息）→ 提交
//   4. 保存到 NVS → 重启（重启后自动连 Wi-Fi，进入正常语音模式）
//
// 注意：这个函数是阻塞的，正常情况下不会返回（保存后直接 esp_restart）。
//       配网模式下**不初始化音频/AFE/TTS**，把内部 RAM 全部让给 AP + HTTP。
void web_portal_run();

// 最近一次启动的 AP 状态（给 main 打印用）
bool web_portal_ap_running();

// ---- 阶段 2：正常模式下 AP 常开 + 按需启动配置页 ----

// 启动隐藏 AP（正常模式下常开，只发 beacon，不跑 HTTP）
// channel <= 0 表示跟随 STA 信道
bool web_portal_ap_start(int channel);

// 启动 / 关闭 HTTP 服务（手机接入 AP 时启动，配置完成/断开时关闭）
bool web_portal_http_start();
void web_portal_http_stop();

// 配置是否已完成（页面提交了身体信息）—— 取走标志并清零
bool web_portal_config_done_take();

// 本次配网提交过 Wi-Fi 且已连上 → 取走标志返回 true（用于"连上就退出配网"）
bool web_portal_wifi_done_take();

// 页面是否请求进入"配网模式"（true 时暂停语音、显示 Wi-Fi 表单）
bool web_portal_prov_wanted();

// 强制退出配网模式（超时兜底）
void web_portal_prov_clear();
