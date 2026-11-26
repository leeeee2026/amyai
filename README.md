# amyai

ESP32-S3 的 Wi-Fi 上网示例工程（C++ / ESP-IDF v6.1.1）。

连接路由器后自动获取 IP，然后做 DNS 解析 + HTTP GET 验证外网是否真的通，
断线自动重连，联网后每 15 秒打印一次链路质量（IP / 网关 / 信道 / 信号强度）。

## 目录结构

```
amyai/
├── CMakeLists.txt          顶层工程定义
├── sdkconfig.defaults      默认配置（芯片型号、Flash 容量、Wi-Fi 账号）
└── main/
    ├── CMakeLists.txt      组件定义（SRCS = main.cpp）
    ├── Kconfig.projbuild   menuconfig 里的 "Amyai 配置" 菜单
    └── main.cpp            全部业务代码
```

## 第一步：填写 Wi-Fi 账号

**方式 A：menuconfig（推荐）**

```powershell
idf.py menuconfig
```

进入 `Amyai 配置` → 填写 `Wi-Fi 名称 (SSID)` 和 `Wi-Fi 密码` → `S` 保存 → `Q` 退出。

**方式 B：直接改 sdkconfig.defaults**

```ini
CONFIG_AMYAI_WIFI_SSID="你的WiFi名称"
CONFIG_AMYAI_WIFI_PASSWORD="你的WiFi密码"
```

> 注意：该文件只在生成 sdkconfig 时生效。如果已经编译过，改完后要先删掉
> `sdkconfig` 再编译，或者直接用方式 A。

## 第二步：编译、烧录、看日志

```powershell
idf.py build
idf.py -p COM3 flash monitor
```

`Ctrl+]` 退出监视器。

## 预期输出

```
I (1234) amyai: ================ amyai 启动 ================
I (1300) amyai: 正在连接 Wi-Fi: "MyWiFi" ...
I (2500) amyai: 已获取 IP: 192.168.1.23
I (2510) amyai: IP=192.168.1.23  网关=192.168.1.1  掩码=255.255.255.0
I (2520) amyai: AP="MyWiFi"  信道=6  信号=-45 dBm
I (2650) amyai: DNS 解析 www.baidu.com 成功
I (2900) amyai: HTTP GET http://www.baidu.com/ -> 状态码 200，返回 -1 字节
I (2910) amyai: 结论：外网可用 ✅
```

## 代码要点

| 位置 | 说明 |
|---|---|
| `WifiStation::start()` | 初始化 netif / 事件循环 / Wi-Fi STA，注册事件回调 |
| `WifiStation::on_wifi_event()` | 处理 `STA_START` 与 `STA_DISCONNECTED`，实现自动重连 |
| `WifiStation::on_ip_event()` | 拿到 IP 后用事件组通知主流程 |
| `check_internet()` | 先 `getaddrinfo` 验证 DNS，再 `esp_http_client` 发 HTTP GET |
| `app_main()` | C++ 里必须写 `extern "C"`，否则链接器找不到入口 |

## 常见问题

**一直连不上 / 超时**
- 确认是 2.4GHz 网络（ESP32 不支持 5GHz）
- 确认 SSID 和密码正确，注意大小写
- 路由器如果开了 MAC 过滤，需要把板子 MAC 加白名单

**连上了但没有外网**
- 日志里 `DNS 解析 ... 失败` → 路由器没给 DNS，或网络本身不通
- `DNS 通了但 HTTP 不通` → 常见于需要网页认证的公共 Wi-Fi

**想换测试网址**
改 `main.cpp` 顶部的 `kTestHost` / `kTestUrl` 两个常量即可。

---

# 大模型意图解析

当前版本已接入大模型：联网成功后，会把几句固定的话发给
**智谱 GLM-4-Flash**（OpenAI 兼容接口），把返回文本打到串口并做白名单校验。

## 配置 API Key

```powershell
idf.py menuconfig
```

`Amyai 配置` → `大模型 API Key`，填形如 `xxxxx.yyyyy` 的 Key。

也可以直接改 `sdkconfig`（该文件已在 `.gitignore` 中，不会提交）：

```ini
CONFIG_AMYAI_LLM_API_KEY="xxxxx.yyyyy"
```

## 串口输出示例

```
I (6510) amyai: ================ 大模型联调开始 ================
I (8670) amyai: 用户: 我脖子好酸
I (8670) amyai: 模型返回: {"cmd":"set_zone","zone":"neck"}
I (8670) amyai: [校验] [OK] 合法指令: {"cmd":"set_zone","zone":"neck"}
I (12310) amyai: 用户: 力气太大了
I (12310) amyai: 模型返回: {"cmd":"set_level","level":2}
I (12310) amyai: [校验] [OK] 合法指令: {"cmd":"set_level","level":2}
I (21320) amyai: ================ 大模型联调结束 ================
```

## 代码结构

| 文件 | 职责 |
|---|---|
| `main/llm_client.cpp` | HTTPS POST、证书校验、解析 `choices[0].message.content` |
| `main/main.cpp` 的 `validate_command()` | 白名单校验，模拟固件侧安全把关 |
| `main/idf_component.yml` | cJSON 托管组件（IDF 6.1 起 cJSON 已移出核心） |

## 换模型

`llm_client.cpp` 顶部两个常量改掉即可（都是 OpenAI 兼容格式）：

| 平台 | `kUrl` | `kModel` |
|---|---|---|
| 智谱（当前） | `https://open.bigmodel.cn/api/paas/v4/chat/completions` | `glm-4-flash` |
| 阿里云百炼 | `https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions` | `qwen-flash` |
| 火山引擎 | `https://ark.cn-beijing.volces.com/api/v3/chat/completions` | `doubao-lite-32k` |

## 延迟实测（ESP32-S3，keep-alive 已开启）

| 请求 | 耗时 | 说明 |
|---|---|---|
| 第 1 次 | 1780 ms | 含完整 TLS 握手 |
| 第 2 次 | 1222 ms | 复用连接 |
| 第 3 次 | 1268 ms | 复用连接 |
| 第 4 次 | 1191 ms | 复用连接 |

`llm_client.cpp` 里 `g_client` 常驻并设置 `keep_alive_enable = true`，
串口只会出现一次「建立新的 HTTPS 连接」，之后都走同一条 TCP/TLS 连接。

参数：`keep_alive_idle = 60s` —— 空闲超过 60 秒连接会被服务端关闭，
下次请求自动重建再重试一次（代码里已处理）。

## 已知限制

- **不要把 Key 烧进量产固件**，`esptool read_flash` 能直接读出来，正式版应走自有中转服务
- 目前只有文本输入，语音识别（ASR）还没接
- 单次约 1.2 秒里，大部分是模型推理 + 网络往返，端侧能压缩的空间已经不多

---

# 离线唤醒词（esp-sr WakeNet）

已集成乐鑫 `esp-sr` 的 WakeNet 离线唤醒，**不需联网、不上传音频**。

当前唤醒词：**小鹿小鹿**（`wn9_xiaoluxiaolu_tts2`）。
选它是因为节奏和「小莫小莫」一致（四字叠词），定制模型到位后只改配置即可。

## 硬件前提

| 项 | 值 | 状态 |
|---|---|---|
| 芯片 | ESP32-S3-WROOM-1-N16R8 | ✅ |
| PSRAM | 8MB 八线 @80MHz | ✅ |
| 板型 | **正点原子 DNESP32S3B (ESP32S3 BOX)** | ✅ |
| 音频 codec | ES8311 @ I2C 0x18 | ✅ |
| **I2S 引脚** | **BCLK=IO21, WS=IO13, DIN=IO47, DOUT=IO14** | ✅ **实测确认** |

### 实测确认的配置（2026-11-25）

```
I2C:  SDA=IO48  SCL=IO45
I2S:  BCLK=IO21  WS=IO13  DIN=IO47  DOUT=IO14  MCLK=不用
采样率: 44100 Hz（这块板的 ES8311 只在这个频率出数据）
麦克风: ES8311 的模拟输入，digital_mic=false，增益 42dB
```

### 三个曾经卡死我们的坑（务必记住）

1. **I2S 必须同时创建 TX 和 RX 通道**
   只创建 RX 时，即使角色是 MASTER，时钟也不会真正跑起来，读到的永远是静音。
   TX 必须绑定 DOUT 引脚（IO14），哪怕一个字节都不写。

2. **采样率只能是 44100**
   16k / 24k / 48k 全部读不到数据（ES8311 的时钟分频在这个硬件上只对 44.1k 成立）。
   esp-sr 需要 16k，所以代码里做了 **44.1k → 16k 线性插值降采样**。

3. **麦克风增益必须拉高**
   默认增益下峰值只有 ~100（几乎等于底噪）；`set_mic_gain(42.0f)` 后峰值 1600–3500，
   WakeNet 才能识别。

4. **codec 打开后不能提前 close**
   早期测试在读取之前就把 codec 关了，导致整轮扫描全是 0。

## 配置引脚

```powershell
idf.py menuconfig
```

进入 `Amyai 音频硬件`，填：

| 配置项 | 本板取值 |
|---|---|
| `I2C SDA` | 48 |
| `I2C SCL` | 45 |
| `I2S MCLK 引脚` | -1（不用） |
| `I2S BCLK 引脚` | 21 |
| `I2S WS/LRCK 引脚` | 13 |
| `I2S DIN 引脚` | 47 |
| `I2S DOUT 引脚` | 14 |
| `功放使能引脚` | -1 |

## 实测输出


## 唤醒词配置

| 配置项 | 当前值 | 说明 |
|---|---|---|
| `Amyai 唤醒词 -> WakeNet 模型名` | `wn9_xiaoluxiaolu_tts2` | 换成别的模型时改这里 |
| `Amyai 唤醒词 -> 唤醒词文字` | `小鹿小鹿` | 仅用于日志显示 |

⚠️ 换模型要**两处一起改**：
1. `menuconfig -> ESP Speech Recognition` 里勾选对应模型（如 `SR_WN_WN9_XIAOLONGXIAOLONG_TTS`）
2. `Amyai 唤醒词 -> WakeNet 模型名` 填模型名

否则 model 分区里打包的还是旧模型。

可选的中文叠词模型（都在 esp-sr 里）：

| 唤醒词 | 模型名 |
|---|---|
| 小鹿小鹿 | `wn9_xiaoluxiaolu_tts2` |
| 小龙小龙 | `wn9_xiaolongxiaolong_tts` |
| 小特小特 | `wn9_xiaotexiaote_tts2` |
| 小鸭小鸭 | `wn9_xiaoyaxiaoya_tts2` |
| 小箭小箭 | `wn9_xiaojianxiaojian_tts2` |

## 代码结构

| 文件 | 职责 |
|---|---|
| `main/wake_word.cpp` | I2S 采集 → AFE(AEC/降噪/VAD) → WakeNet → 回调 |
| `main/Kconfig.projbuild` | 引脚、模型名、唤醒词文字的配置项 |
| `partitions.csv` | 新增 2MB `model` 分区，烧录时自动写入 `srmodels.bin` |

唤醒命中后目前只打印日志，下一步接「录音 → 云端 ASR」。

## 引脚未配置时的行为

不会崩：`wake.start()` 返回 false，串口打印

```
E wake: I2S 引脚还没配置！请在 menuconfig -> Amyai 音频硬件 里填
E wake: 当前: MCLK=-1 BCLK=-1 WS=-1 DIN=-1 DOUT=-1
W amyai: 唤醒功能未启动（多半是 I2S 引脚没配置），继续跑联网部分
```

Wi-Fi 和大模型部分照常工作，方便单独调试。

---

# 语音识别（ASR）与完整语音链路

## 用的还是智谱，同一个 Key

| 环节 | 模型 | 接口 |
|---|---|---|
| 语音 → 文字 | `glm-asr-2512` | `POST /paas/v4/audio/transcriptions`（multipart） |
| 文字 → 指令 | `glm-4-flash` | `POST /paas/v4/chat/completions` |

**不需要再注册别的平台**，`CONFIG_AMYAI_LLM_API_KEY` 一把钥匙两个接口通用。

## 完整交互时序（实测）

```
用户：小鹿小鹿
  ↓ 唤醒（本地，<300ms）
设备：播放「在的」（预录人声，0.4 秒，不花钱）
  ↓
用户：我脖子好酸
  ↓ 录音 4 秒 @44.1kHz → 降采样 16kHz
  ↓ 上传 WAV（128KB）到 GLM-ASR
设备：识别出「我脖子好酸」
  ↓ 送 GLM-4-Flash
设备：得到 {"cmd":"set_zone","zone":"neck"}
  ↓ 白名单校验通过
（下一步：通过 UART 发给按摩椅主控）
```

实测日志：

```
I (4757)  wake: ★ 唤醒命中: "小鹿小鹿"
I (5007)  wake: 已播放提示音「在的」
I (13937) amyai: 你说: "我脖子好酸"
I (16007) amyai: 模型返回: {"cmd":"set_zone","zone":"neck"}
I (16007) amyai: [校验] [OK] 合法指令
```

## 应答提示音怎么做的

不是云端 TTS，是**预录人声内嵌到固件里**：

1. 用微软 Edge 在线 TTS（`edge-tts`，音色 `zh-CN-XiaoxiaoNeural`）合成「在的」
2. 自动裁剪首尾静音、加 8ms 淡入淡出、归一化到 -1.4dB，转成 C 数组 `main/prompt_zai_de.h`
3. 设备端播放时 22050Hz → 44100Hz 升采样，写 I2S TX → ES8311 DAC → NS4150B 功放

好处：**零延迟、零成本、断网也能用**。

换提示语或换音色：

```powershell
set HTTPS_PROXY=http://127.0.0.1:7897    # 访问微软语音服务需要
python tools\gen_prompt_edge.py
```

脚本顶部可改 `VOICE` / `TEXT` / `RATE`（语速）。候选音色试听文件在
`tools\candidates\`（Xiaoxiao / Xiaoyi，「在的」「我在」等几种），
选定后把对应文字填进脚本的 `TEXT` 重跑即可。

> 早期版本用的是 Windows SAPI（Microsoft Huihui），合成偏机械、字头发闷，
> 在 ES8311 + 小喇叭上听不清，已换成现在的方案。

功放使能：手册说 `SPK_CTRL` 接在 XL9555 的 `IO0_5`，代码里通过 I2C(0x20) 操作，只在播放时打开，播完关掉避免底噪。

## 新增文件

| 文件 | 作用 |
|---|---|
| `main/asr_client.cpp` | 拼 WAV + multipart，调 GLM-ASR |
| `main/prompt_zai_de.h` | 「在的」的 PCM 数据（8756 样本） |
| `main/wake_word.cpp` | 加了唤醒后录音 4 秒 + 播放提示音 |

## 已知限制

- 录音固定 4 秒（还没做 VAD 自动断句，说完要等一下）
- 单次交互约 4-8 秒（录音 4s + ASR 1.2s + LLM 1.2s）
- 提示音只有「在的」一句；「没听懂」「好的」还需要再生成

---

# BLE 配网（手机配对 Wi-Fi）
不用再把 Wi-Fi 账号烧进固件了。设备第一次开机（或按 BOOT 键恢复出厂后）会自动进入
**蓝牙配网模式**，手机连上去填 SSID 和密码即可，凭据保存在 NVS 里，下次开机自动连。

## 怎么配

1. 手机装 **ESP BLE Provisioning**（乐鑫官方）
   - 安卓：应用商店 / [GitHub Release](https://github.com/espressif/esp-idf-provisioning-android/releases)
   - iOS：App Store 搜 "ESP BLE Provisioning"
2. 设备上电，串口会打印：

   ```
   W prov: ================ 进入 BLE 配网模式 ================
   W prov: 蓝牙名称: AMYAI_A1B2C3
   W prov: 配网密码(PoP): abcd1234
   ```

3. 手机打开 App → `Provision New Device` → 选 **BLE** → 找到 `AMYAI_A1B2C3`
4. 输入 PoP `abcd1234` → 选家里的 **2.4GHz** Wi-Fi → 填密码 → 开始配网
5. 串口会打印 `配网完成，正在关闭蓝牙以释放内存`，随后自动连上并打印 IP

> 蓝牙开着的内存开销约 50 KB，配网一结束就整块释放，把 RAM 留给 esp-sr。

## 恢复出厂 / 重新配网

开机时**按住 BOOT 键（GPIO0）3 秒**，会清除已保存的 Wi-Fi 凭据，重新进入配网。

改动时长或换成别的按键：

```ini
CONFIG_AMYAI_PROV_RESET_GPIO=0        # -1 = 禁用这个功能
CONFIG_AMYAI_PROV_RESET_HOLD_MS=3000
```

## 配置项（menuconfig → Amyai 配网（BLE））

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `启用 BLE 配网` | 开 | 关掉就退回「SSID 写死在 menuconfig」的老方式 |
| `配网密码 (PoP)` | `abcd1234` | 量产应改成每台唯一的随机值 |
| `蓝牙设备名前缀` | `AMYAI` | 实际名字是 `前缀_MAC后6位` |
| `配网等待超时` | 300 秒 | 超时就关蓝牙，避免一直占内存 |
| `恢复出厂按键 GPIO` | 0（BOOT） | 低电平有效 |
| `恢复出厂按住时长` | 3000 ms | |

## 三种凭据来源的优先级

1. `menuconfig → Amyai 配置 → Wi-Fi 名称` 填了 → 直接用（开发调试方便，可跳过配网）
2. 没填 → 用 NVS 里上次配网保存的凭据
3. 都没有 → 进入 BLE 配网

## 代码位置

| 文件 | 职责 |
|---|---|
| `main/wifi_prov.cpp` | 封装 `wifi_prov_mgr`：启动、事件处理、超时、恢复出厂 |
| `main/wifi_prov.h` | 对外接口：`prov_run_ble()` / `prov_load_credentials()` / `prov_reset()` |
| `main/main.cpp` | `app_main()` 里按优先级决定「直连 / 读 NVS / 进配网」 |

## 常见问题

**手机搜不到设备**
- 确认设备串口打印了「进入 BLE 配网模式」
- 安卓需要开蓝牙和定位权限（Android 12 以下蓝牙扫描依赖定位权限）
- iOS 需要在 App 首次启动时允许蓝牙

**提示密码错误 / 认证失败**
- 90% 是 Wi-Fi 密码错，或连的是 5GHz 网络（ESP32 只支持 2.4GHz）
- 路由器开了 MAC 白名单时，先把板子加进去

**配网成功后还是连不上**
- 串口会打印原因码，`认证失败` 基本就是密码问题
- 公司/校园网需要网页认证（Captive Portal）的，设备连上也没外网，属于网络本身限制




---

# 主控通信（UART）

语音板解析出的指令，按双芯片协议经 UART 发给主控板。

## 接线

| 语音板 | 方向 | 主控板 |
|---|---|---|
| GPIO17 (TX) | → | RX |
| GPIO18 (RX) | ← | TX |
| GND | — | GND（**必须共地**） |

默认波特率 115200。`menuconfig → Amyai 主控通信（UART）` 可改引脚和波特率。

## 协议

```
A5 5A | ver=01 | type | seq | len(2, LE) | payload(JSON) | crc16(2, LE)
```

- CRC：CRC-16/CCITT-FALSE（poly 0x1021, init 0xFFFF），覆盖 `ver` 到 `payload`
- type：`01` 指令 / `02` 应答 / `03` 心跳 / `04` 急停 / `05` 状态上报

## 固件里的两条规则

1. **ACK + 重传**：发出后 300ms 没收到匹配 seq 的 ACK，重发，最多 3 次
2. **急停直通**：识别出的文字含「停一下 / 停下 / 别按 / 急停」时，
   **不联网、不走大模型**，直接发 `0x04`，全程亚秒级

> **关于心跳**：产品需求确定"只在有数据时通信"，所以**不发心跳帧**。
> 代价是链路断开无法主动发现，只能靠 ACK 超时间接判断。
> 如果以后要做断线保护，把 `CONFIG_AMYAI_UART_HEARTBEAT` 打开即可（代码还在）。

## 调试工具

| 脚本 | 用途 |
|---|---|
| `tools/send_uart.py` | PC 直接发帧（需要串口空闲） |
| `tools/uart_lab.py` | 角色 A（发+等ACK）/ 角色 B（主控模拟器，会回 ACK） |
| `tools/sscom_send.py` | 把帧注入 sscom，由它从已占用的串口发出 |
| `tools/pc_voice_test.py` | PC 麦克风 → ASR → 大模型 → 帧 全链路 |

```powershell
# 主控板模拟器：收帧、校验 CRC 和白名单、回 ACK、发心跳
python tools\uart_lab.py --port COM3 --role b --heartbeat 1

# 帧逻辑自测（不需要串口和硬件）
python tools\uart_lab.py --selftest
```

---

# 当前状态（2026-11-26）

## 已完成并验证

| 功能 | 状态 |
|---|---|
| Wi-Fi 配网（BLE） | ✅ 实测通过（手机 App，设备名 `AMYAI_xxxxxx`） |
| 唤醒词「小鹿小鹿」 | ✅ 实测通过 |
| 录音 → 智谱 ASR | ✅ 实测通过 |
| 大模型指令解析（v2 指令集） | ✅ 实测通过，21 条指令 |
| UART 协议下发到主控（IO5/IO6） | ✅ 实测通过，CRC/ACK 正常 |
| 语音合成（百炼 `qwen3-tts-flash`，**流式**） | ✅ 实测通过，首包 0.5 秒开播 |
| 意图路由（控制/聊天/介绍/音乐/未知） | ✅ 已实现 |
| AudioHub（音频仲裁/音量/高频提升/保活） | ✅ 已实现 |
| **VAD 自动断句**（说完就停，峰值判定） | ✅ 实测通过，录音 3.5 秒左右 |
| **连续对话**（回复后继续聆听，可配 10 分钟） | ✅ 实测通过 |
| **对话上下文**（最近 3 轮，避免答非所问） | ✅ 实测通过 |
| **本地秒回「好的」**（控制类指令 0.3 秒先回应） | ✅ 实测通过 |
| 动作去重（模型偶发重复动作） | ✅ 已实现 |
| 主控板固件（`F:\work\main_ctrl`） | ✅ 编译通过，未实测 |

## 交互时序（实测）

```
唤醒「小鹿小鹿」→「在的」        0.5s
用户说话                        ~2s
VAD 断句（静音 600ms）          0.6s
ASR                             1.0~1.4s
── 控制类到这一步先播「好的」 ──   ← 本地音频，0 延迟
LLM 意图解析                     1.5~3.1s
TTS 首包开播                     1.1s
──────────────────────────────
控制类总延迟                    约 7~9 秒
聊天总延迟                      约 8~12 秒（网络波动）
```

> 最初版本是 15 秒。优化项：VAD 断句（-1s）、提示词压缩 34%（-1.5s）、
> 流式 TTS（-2.5s）、本地秒回（主观延迟降到 0.3s）。

## 已知问题

| 问题 | 说明 |
|---|---|
| ~~麦克风约 45–70 秒后断流~~ | ✅ **已修复**：根因是麦克风 PGA 增益 42dB 过载，降到 **24dB** 后连续 3 分钟无断流，唤醒/ASR 均正常。详见 `docs/麦克风断流问题排查记录.md` |
| 音乐播放 / 点歌 | 未实现（方案见 `docs/音乐与聊天功能设计.md`） |
| 24 个自动程序名、6 种手法名 | 用占位 id，待提供实际清单 |

## 文档索引

| 文档 | 内容 |
|---|---|
| `docs/语音交互模块设计方案.md` | 双芯片协议帧格式、CRC、ACK 机制 |
| `docs/语音指令集设计_v2.md` | 指令 schema、行业维度、口语映射表 |
| `docs/音乐与聊天功能设计.md` | 音乐音源、TTS 选型、意图路由架构 |
| `docs/麦克风断流问题排查记录.md` | 硬件问题完整排查记录 + 新硬件设计建议 |

## 关键配置（sdkconfig）

```ini
CONFIG_AMYAI_WAKE_TEXT="小鹿小鹿"
CONFIG_AMYAI_UART_TX_GPIO=5          # 扩展口 IO5 -> 主控 RX
CONFIG_AMYAI_UART_RX_GPIO=6          # 扩展口 IO6 <- 主控 TX
CONFIG_AMYAI_UART_HEARTBEAT=n        # 不发心跳，只在有数据时通信
CONFIG_AMYAI_TTS_MODEL="qwen3-tts-flash"
CONFIG_AMYAI_TTS_VOICE="Cherry"
CONFIG_AMYAI_TTS_RATE_PCT=90
CONFIG_AMYAI_TTS_CONFIRM=y           # 指令送达后语音确认
```

> 注意：这块板子上真正空闲可用的 IO 只有 **IO5 / IO6 / IO8 / IO18**，
> 其余全被 LCD、触摸、音频、USB、SPI 占用（详见硬件参考手册第 10 页的 IO 分配表）。
