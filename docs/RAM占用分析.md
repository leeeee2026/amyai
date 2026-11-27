# RAM 占用分析

日期：2026-11-27
硬件：正点原子 DNESP32S3B（ESP32-S3，512KB 内部 SRAM + 8MB PSRAM）
数据来源：固件 map 文件（`build/amyai.map`）+ 串口运行时日志实测

---

## 一、结论速览

| 内存 | 总量 | 已用 | 剩余 | 状态 |
|---|---|---|---|---|
| **内部 RAM** | ~239 KB（可用堆）| ~189 KB | **50 KB** | ⚠️ 紧张，最大连续块仅 20 KB |
| **PSRAM** | 8 MB | ~3.9 MB | **4.1 MB** | ✅ 宽裕 |

**内部 RAM 是唯一瓶颈**，PSRAM 很富余。

---

## 二、静态占用（编译期固定）

### 内部 RAM

| 段 | 大小 | 说明 |
|---|---|---|
| `.dram0.data` | 35 KB | 已初始化全局变量 |
| `.dram0.bss` | 69 KB | 零初始化全局变量 / 静态数组 |
| `.dram0.dummy` | 106 KB | 预留占位（不实际消耗）|
| `.iram0.vectors` | 1 KB | 中断向量 |
| `.iram0.text` | 121 KB | 放在内部 RAM 的代码（中断 / 高频函数）|

静态数据合计约 **104 KB**。

### Flash（不占 RAM）

| 段 | 大小 |
|---|---|
| `.flash.text` | 1594 KB |
| `.flash.rodata` | 855 KB |
| 合计 | **~2.47 MB** |

> 4 个本地提示音数组（在的 / 好的 / 好的马上为您调整 / 好的有需要随时叫我）
> 共约 1.2 MB，**全部在 flash，不占 RAM**。

---

## 三、运行期实测（串口日志）

### 内部 RAM 分阶段

| 阶段 | 空闲 | 最大连续块 | 说明 |
|---|---|---|---|
| 开机堆初始化 | ~239 KB | — | 179+21+32+7 KiB 四个池 |
| **连网前** | **130 KB** | 51 KB | AFE + 各缓冲 + 任务栈已分配 |
| **连网并交互后** | **50 KB** | **20 KB** | WiFi + lwip 额外吃掉 ~80 KB |

日志原文：

```
[内存] 连网前：内部 130343 空闲（最大块 51200）| PSRAM 7324268 空闲
[资源] 唤醒任务剩余栈 15980 字节 | 内部RAM 50003 空闲(最大块 20480) | PSRAM 4281692 空闲
```

### PSRAM 分阶段

| 阶段 | 空闲 | 已用 |
|---|---|---|
| 开机（加入堆）| 8 MB | 0 |
| 连网前 | 7,324 KB | ~868 KB |
| **交互后（MultiNet 已加载）** | **4,281 KB** | **~3.9 MB** |

---

## 四、PSRAM 3.9 MB 的去向

| 模块 | 用途 | 大小 |
|---|---|---|
| **esp-sr MultiNet** | mn7_cn 模型解码状态（144 条命令）| **~2.9 MB** |
| esp-sr AFE | 1MIC + WakeNet 内部缓冲 | ~238 KB |
| tts_client | TTS 播放管道（流式 PCM）| 256 KB |
| wake_word | rec 录音缓冲（6 秒 @16k）| 187 KB |
| wake_word | afe_pcm AFE 输出缓冲（6 秒 @16k）| 187 KB |
| music_player | 音乐环形缓冲 | 64 KB |
| asr_client / tts | base64、请求体等临时缓冲 | ~100 KB |
| **合计** | | **~3.9 MB**（与实测吻合）|

---

## 五、内部 RAM 的去向

| 项 | 大小 | 说明 |
|---|---|---|
| 静态数据（.data + .bss）| 104 KB | 全局变量/静态数组 |
| WiFi + lwip | ~80 KB | 协议栈、缓冲区、控制块 |
| 任务栈 | ~40 KB | 唤醒(20K) + asr_init(8K) + tts_play(4K) + 其他 |
| AFE / I2S / MultiNet 小对象 | ~15 KB | 各种控制结构 |
| 其余零散堆 | ~10 KB | 字符串、cJSON、事件队列 |

---

## 六、已有的内存保护措施

| 措施 | 位置 | 效果 |
|---|---|---|
| 录音缓冲写越界硬保护 | `wake_word.cpp` | 写满强制收尾，绝不越界破坏堆 |
| AFE 拷贝前置容量判断 | `wake_word.cpp` | `afe_fill + n <= kRecordSamples` |
| 环形缓冲先算余量 | `tts_client.cpp` / `music_player.cpp` | `len` 先钳到 `room` 再拷贝 |
| 全项目禁用 `strcpy/strcat/sprintf` | 全 | 统一 `snprintf` |
| 所有 `malloc` 都有 null 检查 | 全 | 失败降级或报错退出 |
| 提示音任务参数 free | `local_cue.cpp` | 任务末尾释放 |
| 流式播放任务栈放 PSRAM | `reply_player.cpp` | 20KB 不占内部 RAM |
| asr_init 任务栈降到 8KB | `main.cpp` | 只加载模型，不跑 TLS |
| 暂停/播报状态用原子操作 | `wake_word.cpp` | 避免数据竞争 |

---

## 七、踩过的内存坑（血的教训）

### 1. 任务栈吃光内部 RAM → lwip 崩

给流式播放任务开了 6KB **内部 RAM** 栈，直接挤爆堆：

```
E lwip_arch: thread_sem_init: out of memory
Guru Meditation → netconn_gethostbyname → memcmp(NULL) → 重启循环
```

**结论**：新增任务一律 `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`。

### 2. 任务栈太小 → TLS 握手栈溢出

流式播放任务栈 12KB（PSRAM）时，TTS 需要**新建 TLS 连接**就会崩：

```
Guru Meditation → mbedtls_ssl_handshake_step → memcpy
  ← speak_once → tts_speak → reply_player 任务
```

**结论**：要跑 TLS 的任务栈至少 **20KB**（连接复用命中时不握手，所以表现为"偶发崩溃"）。

### 3. 模型分区 2MB 装不下 2.9MB 模型

```
Guru Meditation (Cache error) → 启动崩溃循环
```

**结论**：`partitions.csv` 的 model 分区必须 ≥ 4MB（当前就是 4MB）。

---

## 八、如果以后不够用，可省的地方

| 方案 | 可省 | 代价 |
|---|---|---|
| `rec` / `afe_pcm` 从 6 秒降到 3 秒 | **-187 KB PSRAM** | 只影响超长句（VAD 一般 3 秒内断句）|
| 音乐环形缓冲 64KB → 32KB | -32 KB PSRAM | 预缓冲时间减半，弱网易卡 |
| TTS 管道 256KB → 128KB | -128 KB PSRAM | 首包缓冲变短 |
| 关掉诊断日志（`AMYAI_DIAG_ENABLE 0`）| 少量 | 上线前本来就要关 |
| 减少并发 TLS 连接数 | 内部 RAM | 需要串行化请求 |

### 想加 AEC 的话

AEC 预计需要额外 **1~2 MB PSRAM + 部分内部 RAM**：
- PSRAM 完全够（还剩 4.1 MB）
- **内部 RAM 是风险点**（只剩 50KB / 最大块 20KB），需要先把某些任务栈或缓冲优化掉

---

## 九、监控方法

固件里已埋好日志，随时可查：

```
# 开机时（连网前）
[内存] 连网前：内部 xxx 空闲（最大块 xxx）| PSRAM xxx 空闲

# 每轮交互后
[资源] 唤醒任务剩余栈 xxx 字节 | 内部RAM xxx 空闲(最大块 xxx) | PSRAM xxx 空闲
```

**判断标准**：
- 唤醒任务剩余栈 > 8KB 安全（实测 15.9KB）
- 内部 RAM 空闲 > 30KB 安全（实测 50KB）
- 内部 RAM 最大连续块 < 16KB 时要警惕（TLS 握手可能失败）
