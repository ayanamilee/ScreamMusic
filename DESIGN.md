# ScreamMusic 设计文档

> 状态：草案 v0.1（2026-10-10）。本文档是 macOS 端的唯一架构依据；仓库中其他地方若有冲突描述（例如旧的 `macos/ScreamAudio/` 路径），以本文档为准。

## 1. 目标

把 macOS 上播放器的音频 **bit-perfect** 地通过 Scream 协议（UDP）发给 scream2diretta（s2d）或其他 Scream 接收端。

- 首要场景：**Apple Music**（不会自己切采样率、不会独占设备）。
- 次要场景：能自行选择/独占输出设备的播放器（Roon、Audirvana、Qobuz、Tidal、QQ 音乐等），行为各异，需逐个验证（见 §7）。
- 线上格式与本仓库 `snd-screamalsa.c` 完全一致（extended 6 字节 / original 5 字节），s2d 无需改动即可接收。

非目标（第一版）：TCP、DSD/DoP 解码、多声道、AirPlay。

## 2. 产品形态：一个 App

用户只看到 **ScreamMusic.app**（菜单栏应用）。系统 Sound 设置 / Audio MIDI Setup 里出现一个名为 **ScreamMusic** 的输出设备。

技术上 macOS 只从 `/Library/Audio/Plug-Ins/HAL/` 加载虚拟声卡，所以：

- HAL 驱动 `ScreamMusic.driver` 作为 App 的一部分构建，打包在 `ScreamMusic.app/Contents/Resources/`。
- App 首次启动时提示安装（一次管理员密码，重启 coreaudiod）；App 版本更新时自动提示更新驱动；卸载也在 App 菜单完成。
- 驱动不是独立产品，不单独命名、不单独发布。

```
macos/ScreamMusic/
  DESIGN.md                 本文档
  build.sh                  构建 build/ScreamMusic.app（SwiftPM + clang，ad-hoc 签名，不依赖 xcodeproj）
  Package.swift             App 的 SwiftPM 清单
  App/                      Swift / SwiftUI 菜单栏应用（Sources/、Resources/Info.plist）
  Driver/                   C，AudioServerPlugIn（从 Apple NullAudio 样例起步）
  Shared/scream_protocol.h  包头编码 + 样本转换（C，App 与驱动共用）
  Tools/scream_dump/        本机测试接收端：UDP → 包头日志 + 原始 payload 文件
  Tests/                    协议单元测试、与 snd-screamalsa.c 的对照测试、bit-perfect 比对脚本
```

## 3. 总体架构

```
 播放器 ──► ScreamMusic 设备（HAL 驱动，运行在 coreaudiod 内）
                │  ProcessOutput：按进程区分（仅 Apple Music 模式）
                │  WriteMix：取设备物理格式的数据
                ▼
           转换为线上格式（精确缩放，无抖动）
                ▼
           无锁环形缓冲（SPSC）
                ▼
           驱动内发送线程：按 1152 字节 payload 时长定速 ──► UDP ──► s2d
```

- **网络发送在驱动里完成**：驱动 Info.plist 声明 `AudioServerPlugIn_Network = true`（SDK `AudioServerPlugIn.h` 允许）。不需要麦克风权限，不需要 App 常驻，延迟最低。这与 Linux 上 snd-screamalsa 内核驱动同构。
- **App 负责控制面**：安装驱动、配置目的地址、Apple Music 模式（默认输出接管、采样率跟随、曲目边界处理、其他 App 转发）、状态显示。
- **时钟**：设备时钟就是本机 host clock（`GetZeroTimeStamp` 按标称采样率推进），不跟随任何外部时钟。与 screamalsa 一样，由接收端缓冲吸收漂移。

### 3.1 实时线程约束

`DoIOOperation` 在 coreaudiod 的实时 IO 线程上执行：不加锁、不分配内存、不做系统调用。它只做格式转换并写入环形缓冲。`sendto()` 只在发送线程中调用。

## 4. 驱动（ScreamMusic.driver）

### 4.1 设备与流

| 项 | 值 |
|---|---|
| 设备名 / UID | `ScreamMusic` / `ScreamMusic_UID` |
| 输出流 | 2 声道 |
| 输入流 | 2 声道，仅 Apple Music 模式用于把"其他 App"的声音回送给 App（§6.3）；不做系统默认输入 |
| 采样率 | 44.1k、48k、88.2k、96k、176.4k、192k、352.8k、384k（后续视 s2d 能力加 705.6k/768k） |
| 物理格式 | Float32（可混音，默认）；SInt16、SInt24 packed、SInt32（**不可混音** `kAudioFormatFlagIsNonMixable`，供独占播放器的整数模式） |
| 独占 | 由 HAL 实现 hog mode，驱动无需特殊处理 |

### 4.2 线上位深决策（"按原曲"）

驱动按以下优先级决定线上格式：

1. **物理格式为整数**（播放器独占 + 整数模式）：线上位深 = 物理格式位深，数据原样发出。
   SInt16 → S16_LE，SInt24 → S24_3LE，SInt32 → S32_LE（original 包头下只允许 S32_LE，见 §5）。
2. **物理格式为 Float32，且 App 指定了位深**（Apple Music 模式，属性 `'SMbd'`）：按指定位深精确转换。
3. **物理格式为 Float32，未指定**（其他写 float 的播放器）：**网格检测**——
   观察一段样本，若所有样本 × 2^15 均为整数则判定 16 位，否则 24 位；判定后锁定，直到 IO 停止或采样率变化，避免中途跳变触发 s2d 重配。检测完成前按 24 位发送。

转换规则（必须）：`int = round(x × 2^(N-1))`，钳位到 `[-2^(N-1), 2^(N-1)-1]`，**不加抖动**。
不能用 `× 32767` / `× 8388607` 之类的缩放，否则不是 bit-perfect。

驱动同时统计"离网格样本数"（Float32 输入时，样本 × 2^(N-1) 不是整数的个数），用于 Bit-perfect 检查与诊断。

注：32 位整数源一旦经过 Float32 就无法 bit-perfect，只有路径 1 能做到。

### 4.3 发送

- 环形缓冲按线上字节存储；发送线程每攒满 1152 字节发一包。
  1152 是 S16 立体声（4 B/帧）、S24_3LE（6 B/帧）、S32（8 B/帧）的整数倍。
- **定速**：每包间隔 = 1152 / (rate × 帧字节数)，用 `mach_wait_until` 绝对时间推进；落后则立即追，超前则等待（同 snd-screamalsa 的 `scream_playback_thread`）。避免一个 IO 周期内的几包成串突发。
- 格式（采样率/位深）变化：先用旧格式发一个结束标记，再用新包头发新数据。
- **结束标记**（`byte[4] |= 0x80`，UDP 仅发包头）：在 StopIO、格式变化、App 明确通知"播放结束"（`'SMeo'`）时发送；暂停不发。与 snd-screamalsa / amscream 语义一致。
- **重锁前导**（0.4.5，`'SMli'`）：线上格式与接收端上次收到的不同（采样率、位深或包头方言）时，新 run 先按新格式节拍发至少 N ms 全零，再放音频；音频还没准备好（切换中的回退与预填）时继续发零，最多再 1.5 s，使数据流没有空档。接收端（s2d、apscream 等）→ DAC 在静音里完成重锁，第一个音符不会落在重锁期间。实测动机：自然衔接切换完全干净时，换采样率的曲目开头仍有一声轻微咔嗒，不换采样率重播则没有。Apple Music 模式的切换本身有约 1 s 空档，前导落在其中，不增加延迟；不回退的 live 切换（流媒体起播段升级）由 App 用 `'SMlo'` 关掉本次前导。前导只有发满才算数：被中途打断（设备停了又起）的下一个 run 会补发，期间不发结束标记。同格式重启不加。
- 静音：IO 运行期间播放器送来的静音照常发送；"连续静音 N 秒后停止发包"作为后续可选项（便于 s2d 的上游静默释放）。

### 4.4 自定义属性（设备对象）

| 选择器 | 类型 | 读/写 | 说明 |
|---|---|---|---|
| `'SMds'` | CFDictionary `{ip, port, header}` | 读写 | 目的地址与包头方言（`extended` / `original`）。驱动用 HAL host 的 `WriteToStorage` 持久化，App 不运行时也生效 |
| `'SMmx'` | CFNumber pid | 读写 | Music 的 pid。只把该 pid 的输出送去发送；其他进程转入回送环。0 = 没有 Music 进程（不是关闭模式）。`'SMac'` 非 0 时过滤仍开着，全部转入回送环、不发送。两者都为 0 才是通用模式 |
| `'SMbd'` | CFNumber | 读写 | Float32 输入时的线上位深：0 = 自动（网格检测），16/24/32 = 指定 |
| `'SMeo'` | CFNumber | 写 | 写 1 = 立即发送结束标记 |
| `'SMdl'` | CFNumber 毫秒 | 读写 | 发送延迟（§6.2.3），同时作为设备延迟上报 |
| `'SMhd'` | CFNumber | 读写 | 保持点：0 放行 / 1 闸门 / 2 曲目边界 / 3 丢弃（§6.2.3）。闸门/边界保持超过 4 s、丢弃超过 10 s 未被处理时驱动自动放行 |
| `'SMhr'` | CFNumber | 读 | 1 = 已到达保持点、正在保持 |
| `'SMli'` | CFNumber 毫秒 | 读写 | 换线上格式后先发的数字静音（0–2000，默认 500，0 = 关）。驱动持久化 |
| `'SMlo'` | CFNumber 毫秒 | 写 | 只对**紧接着的那一次**格式切换用这个前导长度（-1 = 取消），3 s 内没有切换就作废，不持久化。App 在流媒体中途升级（48k 起播段→96k，没有回退可以遮住前导）时写 0，免得在 Music 本来就有的约 110 ms 空档上再加 0.5 s 静音 |
| `'SMac'` | CFNumber pid | 读写 | 控制方 App 的 pid：它自己的 IO（读回送输入）不触发 Scream 发送；它退出后驱动自动清除 `'SMmx'`、`'SMhd'`、`'SMdl'` |
| `'SMst'` | CFDictionary | 读 | 状态快照：客户端列表（pid、进程名、是否 hog）、物理格式、线上格式、网格检测结果、离网格计数、已发包数、发送错误、最后错误信息 |

`'SMmx'` 的按进程区分在 `kAudioServerPlugInIOOperationProcessOutput`（HAL 混音前、每客户端一次）中完成，参考 Nativerate `LSOutput.c` 的 `'LSmx'`。

### 4.5 配置持久化

驱动运行在 coreaudiod 的沙盒里，读不到用户的偏好设置。目的地址等由 App 写入 `'SMds'`，驱动经 `AudioServerPlugInHostInterface::WriteToStorage/CopyFromStorage` 保存，coreaudiod 重启后自动恢复。

## 5. 线上协议

与 `snd-screamalsa.c` 2.0.7 完全一致，摘要：

| | extended（默认） | original |
|---|---|---|
| 包头 | 6 字节 | 5 字节 |
| PCM | S16_LE、S24_3LE、S32_LE（S24_LE 容器格式暂不用） | 仅 S32_LE |
| byte[0]/[4] | 倍数低 8 位 / 倍数高 4 位 + `0x10`(44.1k 系) + `0x80`(EOT) | ≥128: 44100×(b0−128)，否则 48000×b0 / `0x80`(EOT) |
| byte[5] | 24 位布局：0 = S24_3LE | 无 |
| payload | 1152 字节 | 1152 字节 |
| 端口 | 默认 4011 | 同左 |

original 方言下，16/24 位源以 S32_LE 发送（左移到高位，数值无损）。

共用实现：`Shared/scream_protocol.h`，以 `snd-screamalsa.c`（唯一经过验证的发送端）的 `scream_fill_header()` 为准用 C 重写。

## 6. App（ScreamMusic.app）

### 6.1 菜单

- 状态：目的地址、当前线上格式（如 `96 kHz / 24-bit`）、正在发送的客户端、Bit-perfect 检查结果。
- 模式：**Apple Music 模式**（开/关）。关闭时设备处于通用模式（§6.4）。
- 目的地址：IP、端口、包头方言。
- 其他 App 输出到：`启动前的输出设备`（默认）/ 指定设备 / 内置扬声器 / 静音丢弃。记住最后一次选择。
- 驱动：安装 / 更新 / 卸载。
- 日志导出。

### 6.2 Apple Music 模式

开启时：

1. 记录当前系统默认输出设备（作为"启动前的输出"）与系统提示音设备。
2. 把系统默认输出设为 ScreamMusic；提示音移到"其他 App 输出到"的设备。
3. 设置 `'SMac'` = App 的 pid，`'SMmx'` = Music 的 pid（Music 未运行时为 0；0.4.2 及更早的驱动把 0 当成关闭过滤，App 在那种驱动上改写一个不会撞上的 pid），`'SMdl'` = 发送延迟（默认 400 ms）。
4. 检查并提示破坏 bit-perfect 的 Music 设置：音量 < 100、Sound Check、EQ、AutoMix/交叉淡入淡出。

关闭（或 App 退出）时：恢复默认输出与提示音设备，清除 `'SMmx'`、`'SMhd'`，`'SMdl'` 恢复通用值。App 异常退出后的下次启动要能恢复（参考 Nativerate `recoverOutput()`）。

#### 6.2.1 为什么必须"暂停 → 切换 → 回退"

Music 总是按设备当前标称采样率渲染，自己不切。App 得知"新曲目采样率不同"时，新曲目开头 0.16–0.5 s 已按旧采样率（经重采样）渲染。这段数据必须丢弃，丢掉的内容只能让 Music 回退重放。这一点与输出端是 DAC 还是 Scream 无关。

与 LosslessSwitcher 的区别：LosslessSwitcher 在曲目开始后才查日志（OSLogStore 回看 5 s、每 2 s 轮询），再改真实设备的采样率，新曲目开头以错误采样率播放，且系统混音器仍在路径上，不保证 bit-perfect。ScreamMusic 采用 Nativerate 的方法：提前识别 + 截住 + 丢弃 + 暂停回退。

#### 6.2.2 采样率识别（App）

沿用 Nativerate 的识别来源与判定规则（在 ScreamMusic 中精简重写，不照搬其代码结构）：

- `log stream --predicate 'process == "Music" AND eventMessage CONTAINS "Input format:"'`：实时解析 `N Hz`、`from N-bit source`、是否 lossless（`lac`）。本地文件的下一首会提前 8–12 s 出现预读行。
- `com.apple.Music.playerInfo` 分布式通知：曲目开始/暂停/停止、PersistentID、名称。
- AppleScript 读取 Music 当前曲目的采样率：与日志冲突时以它为准（日志里可能混有上一首/预读的行）。
- 兜底：本地文件头；都拿不到时按"未知"处理（闸门等待最多 3 s）。
- 判定规则：只接受新曲目开始之后、且不在上一首"自身窗口"内的日志行；流媒体可能先起 lossy 解码器、数秒后升级为 lossless，需要识别升级。

位深：取同一来源的 `from N-bit source`，写入 `'SMbd'`；拿不到时 `'SMbd'` = 0（驱动网格检测）。

**实测（macOS 27，2026-10-11）：Apple Music 流媒体（HLS）开头是低规格起播段。** CoreMedia 日志显示同一个播放项内有两条轨：前 2.56 s 为 48 kHz（`forwardEndTime ... 122880/48000`），之后跳到 96 kHz/24-bit 变体（`will jump to new audio at 2.54`）。AppleScript 的 `sample rate` 有时报的是起播段的格式（如 44.1k），有时为空；每次 seek/暂停后 Music 会把两个变体的解码器都重建一遍，日志里两种采样率交替出现。由此确定的规则：

- **曲目内日志不直接切换（0.3.0）**：解码器日志不带曲目标识，曲目内（开始后 10 s 内，或拖动/恢复后的短窗口）的日志可能是起播段升级、本曲重建，也可能是拖动后一起重建的下一首预读，因此只记为候选；是否切换完全由网格检查决定，切换时**不回退**（回退会重放起播段，导致来回切换的死循环）。
- **自然切换**：用上一首播放期间的预读日志（取最高规格），在曲间静音处只切一次。
- **手动开始**：截住，最多等 1.5 s 拿到本曲第一条无损日志，与 AppleScript 取较高者，只切一次（截住期间若已积压声音，同格式也回到开头再放）；之后起播段升级按上面的"只升不降"处理。
- 起播段那 1–3 s 不可能 bit-perfect（它本身就是另一个变体）。
- **拖动/恢复后的日志不确定**（实测：拖动进度后 Music 同时重建当前曲目的解码器和下一首的预读，0.25 s 内先后出现 96k 与 192k 两行）：拖动或恢复（含 App 自己的回退）后 2 s 内的解码器日志只作为网格检查的候选，不触发立即切换。立即"只升不降"只针对曲目开始后、未发生拖动的日志。
- **网格反馈校正**（实测补充：一首 192k 预读的流媒体实际以 96k 变体播放，"只升不降"把 96k 日志当起播段忽略，整首被升采样）：曲目开始或拖动后 3 s 起，每 0.5 s 用驱动的离网格计数检查；连续 1 s（两个窗口）离网格比例 > 30%，且本曲日志出现过别的格式，就直接把设备切过去（不暂停、不回退），每曲最多 3 次，已失败过的格式不再回去。候选顺序：本曲可靠日志在前，拖动后的不确定日志在后。无损曲目在正确格式下样本必然落在网格上，这是 Music 实际输出的客观证据。

与 LosslessSwitcher（只看日志、曲目开始后才切真实 DAC，开头错误采样率 + 断音）和 Nativerate（以 AppleScript 为准，仅在"有损起播 → 无损"时从当前位置再切一次；AppleScript 报低规格时不会纠正）相比，这套规则针对的正是起播段问题。

**Rate Library（0.4.0，0.4.2 修订；借鉴 Lysoniq Switcher）**：播过的曲目持久记住实测格式（`~/Library/Application Support/ScreamMusic/RateLibrary.json`）。键为 AppleScript 的 persistent ID（流媒体没有时用规范化"艺人 — 专辑 — 曲名"；只有曲名时不记，避免和电台/流媒体互相冲突）。**库只是提示，不是权威**：学习只认"当前格式下连续干净 ≥ 10 s"；有损（AAC）/ Atmos 不记（0.4.0/0.4.1 记 lossy，结果标记自我维持，已取消，旧文件里的 lossy 条目加载时丢弃）。使用规则（`chooseStart`）：自然衔接时本曲预读行优先，库格式只作网格检查候选；手动开始没有预读行，命中库直接切，省掉等解码器日志的时间；库格式没守住（网格检查纠正或放弃）→ 删除该条目，下次按新证据决定，曲目干净结束后重新学习。库有锁，落盘走后台串行队列。AAC（`ACMP4AACBaseDecoder`）与 Atmos（`ACDDPAtmosDecoder`）日志单独识别：有损/渲染内容永远不在网格上，网格检查不动作，面板标注；AAC 起播后 10 s 内没有本曲无损行才认定有损（流媒体常先 AAC 后升级 ALAC）。参考 Lysoniq Switcher 的 Rate Library；其 Pause Point 与 RP/KF 播放项日志关联（私有日志）不采用；Relock Time 的思路 0.4.5 起由驱动的 `'SMli'` 实现（见 §4.3）。

#### 6.2.3 截住与丢弃（驱动）

Nativerate 需要在 App 里维护约 0.35 s 的环形缓冲才能在错误片段到达 DAC 前截住它。ScreamMusic 的驱动在发 UDP 前本来就有发送缓冲，因此截住/丢弃放在驱动里，错误采样率的数据根本不离开 Mac：

- **发送延迟** `'SMdl'`：发送线程只发送"写入时间早于 now − 延迟"的数据。Apple Music 模式 400 ms，通用模式默认 20 ms。设备的 `kAudioDevicePropertyLatency` 同步报告该值，视频播放器据此对齐音画。
- **保持点** `'SMhd'`：
  - `1` 闸门：从下一个非零帧起停止发送（之前的静音照发）。用于"从暂停/停止开始播放、但新曲目采样率还未知"。
  - `2` 曲目边界：从下一段 ≥ 10 ms 的全零帧（Music 的曲间静音）起停止发送。App 在预读到下一首需要不同采样率、旧曲目剩余 1.5 s 时设置；用户跳曲时立即设置。
  - `0` 放行：发送保持点之后的数据（采样率没变，比如最终判定相同）。
  - `3` 丢弃：丢掉保持点之后的全部数据并复位。
  - `'SMst'` 报告保持点是否已到达及其位置。

#### 6.2.4 切换流程（App 驱动）

1. 新曲目 Playing 且需要不同采样率（或闸门期间判定出采样率）。
2. AppleScript 暂停 Music。
3. 等待保持点到达（最多 1 s），`'SMhd'` = 3 丢弃错误片段；驱动发送结束标记。
4. 设置 ScreamMusic 标称采样率（虚拟设备，约 20 ms，无需等待硬件）和 `'SMbd'`。
5. 计算回退位置并 `set player position`，然后播放。规则沿用 Nativerate：已播放时长按暂停时刻计算（+0.05 s，−0.1 s 余量），起点 < 2 s 时设为 0.001（不能设 0，Music 会当作无操作）。
6. 确认 Music 真的在播放（最多重试 3 次；播放 ≥ 0.3 s 后用户自己暂停的不重试）。

s2d 在收到新格式时重建 Diretta 连接（默认 200 ms 冷却），恰好落在 Music 暂停的空档里。

同采样率曲目切换（包括无缝专辑）、暂停/继续、同曲目内跳转：不干预。

#### 6.2.5 不需要的部分

与 Nativerate 相比去掉：DAC 独占与非混音格式、时钟 PLL（`'LSrs'`）、等待 DAC 就绪、DAC 音量转发与软件音量、空闲时让出 DAC。

### 6.3 其他 App 的声音

Apple Music 模式下 ScreamMusic 是系统默认输出，其他 App 的声音也会进来，Music 没在跑时也一样。驱动在 ProcessOutput 中把非 Music 客户端的数据移入回送环，从设备输入流读出；App 读取后播放到"其他 App 输出到"的设备（会经过重采样，不追求 bit-perfect）。选"静音丢弃"时 App 不读输入流，无需麦克风权限。模式开着时这些声音不走 Scream，接收端没开会不会把它们吃掉。

### 6.4 通用模式（其他播放器）

Apple Music 模式关闭时，App 不接管默认输出、不设 `'SMmx'`，驱动对所有客户端一视同仁，按 §4.2 决定线上格式。用户在播放器里选择 ScreamMusic 设备（独占与否由播放器决定）。App 仍提供配置和状态显示。

## 7. 播放器兼容性（待验证）

不同播放器写入设备的方式不同，**不能假设都是整数直通**。用状态面板（`'SMst'`）逐一实测并记录：

| 播放器 | 独占(hog) | 物理格式 | 数据 | 是否改动样本 | 结论 |
|---|---|---|---|---|---|
| Apple Music | 否 | Float32 | float | 音量 100 等条件下不改（macOS 27） | 待测 |
| Audirvana | | | | | 待测 |
| Roon | | | | | 待测 |
| Qobuz | | | | | 待测 |
| Tidal | | | | | 待测 |
| QQ 音乐 | 可能是 | 可能 Float32 | | | 待测 |

## 8. 系统版本

- **macOS 27**：主要开发与测试平台。Music 1.7 输出与文件样本一致（Nativerate 实测）。
- **macOS 26**：支持，需另一台机器验证。已知 Music 1.6.6 输出约 ×0.99999997 的缩放：
  16 位曲目误差 < 0.006 LSB，**按 16 位四舍五入即可完全恢复原值**；24 位曲目误差可达 1.5 LSB，无法恢复。
  这也是"按原曲位深发送"的理由之一：16 位曲目若按 24 位发送，会把这个误差带到线上。
- Music 日志文本可能随版本变化，识别逻辑需按版本适配并保留回退路径（AppleScript 曲目采样率、本地文件头）。
- 部署目标：macOS 26.0。

## 9. 分阶段计划

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P0** ✅ | 目录、`build.sh`（SwiftPM App + 驱动拷入 Resources）；`Shared/scream_protocol.h`；`Tools/scream_dump`；`Tests/run_tests.sh` | 能构建；包头与 snd-screamalsa.c 逐字节一致；scream_dump 端到端测试通过 |
| **P1** ✅ | 驱动：固定采样率（Audio MIDI Setup 手动选择）、Float32 + 整数物理格式、UDP 发送、`'SMds'`、`'SMst'`；App：安装/卸载驱动、配置目的地址、状态显示 | 用测试播放器（整数独占写入 WAV）和 Apple Music（固定采样率）播放 16/24 位测试文件，scream_dump / s2d raw 输出与解码后的源文件逐字节一致 |
| **P2**（顺序调整到 P4 之后） | 网格检测、`'SMbd'`；播放器兼容性实测，填写 §7 | 兼容性表完成 |
| **P3** 🔧 | Apple Music 模式：默认输出接管与恢复、`'SMmx'`、其他 App 转发 | Music 与其他 App 同时出声，Music 路径仍 bit-perfect |
| **P4** 🔧 | 采样率跟随与曲目边界处理、位深跟随；macOS 26 适配 | 混合采样率歌单连续播放，每首首帧即正确采样率 |
| **P5** | 静音停发选项、EOT 细节、开机启动、打包发布 | — |

## 10. 相关问题（本仓库其他部分）

- s2d 未处理结束标记：见 `../scream2diretta/docs/end-of-track-marker.md`。

## 11. 待验证的技术假设

1. 驱动在 coreaudiod 沙盒内声明 `AudioServerPlugIn_Network` 后可以 `sendto()` 局域网地址（macOS 26 / 27，含"本地网络"隐私限制是否作用于 coreaudiod）。
   ✅ macOS 27.0.1：可以发到 127.0.0.1（2026-10-11，`Tests/e2e_hal.sh`）；局域网地址待用 s2d 实测。macOS 26 待测。
2. 播放器独占并设置不可混音整数物理格式时，WriteMix 收到的数据就是播放器写入的整数，未经 float 往返。
   ✅ macOS 27.0.1：hogplay 独占 + int16/int24 播放满幅白噪声，44.1k–192k 逐字节一致；Float32 路径（16/24 位源）同样逐字节一致。
3. Apple Music 在 ScreamMusic 标称采样率变化后能正确重新配置，不会重采样。
4. ad-hoc 签名的驱动可以被 macOS 26 / 27 加载（Nativerate 已验证 ad-hoc 可行）。
   ✅ macOS 27.0.1 可以。macOS 26 待测。

## 12. 许可证

ScreamMusic 计划复用 Nativerate（GPL-3.0）的部分逻辑，因此 `macos/ScreamMusic/` 整体采用 **GPL-3.0**，目录内放独立 LICENSE。驱动骨架来自 Apple NullAudio 样例（MIT，保留其版权声明）。本仓库其余部分（snd-screamalsa，GPL-2.0）不受影响。
