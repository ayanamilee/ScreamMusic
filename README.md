# ScreamMusic

macOS menu-bar app that sends stereo PCM **bit-perfect** over the [Scream](https://github.com/igor63r/scream) protocol (UDP) to [scream2diretta](https://github.com/ayanamilee/scream2diretta) or any Scream receiver.

菜单栏应用。系统里出现一块名为 **ScreamMusic** 的虚拟声卡，播放进去的立体声 PCM 按 Scream 协议原样发出，不做混音、不做重采样（格式对上时）。主要给 Apple Music 用，能自己选输出设备的播放器也可以直接选这块声卡。

当前版本 **0.3.0**。还在打磨 Apple Music 流媒体的采样率跟随，见下方限制。协议与 [screamalsa](https://github.com/ayanamilee/screamalsa) 2.0.7 的线上格式一致。

License: GPL-3.0（`LICENSE`）。需要 **macOS 26 或更新**。主要在 macOS 27 上测试。

## 它做什么

| | |
|---|---|
| 设备 | 立体声输出，44.1–384 kHz |
| 物理格式 | Float32（可混音）；SInt16 / SInt24 / SInt32（不可混音，给独占整数播放器） |
| 传输 | 仅 UDP，默认端口 **4011** |
| 包头 | `extended`（6 字节，默认）或 `original`（5 字节，仅 S32） |
| 负载 | 1152 字节 / 包 |

Apple Music 自己不会切换输出设备的采样率。打开 **Apple Music 模式** 后，ScreamMusic 会：

- 把系统默认输出切到 ScreamMusic，退出或关闭模式时恢复；
- 只把 Music 的声音发出去，其他 App 和提示音转到模式开启前的输出（可改，会记住）；
- 按曲目采样率和位深切换设备，同格式的曲目无缝衔接。

不打开这个模式时，在播放器里手动选择 ScreamMusic 即可。独占并选了整数格式的播放器，样本原样上线。

## 安装

1. 克隆后构建（需要 Xcode 命令行工具，macOS 26+）：

   ```sh
   git clone https://github.com/ayanamilee/ScreamMusic.git
   cd ScreamMusic
   ./build.sh
   ```

   产物是 `build/ScreamMusic.app`（ad-hoc 签名，未公证）。

2. 第一次双击打开。若系统拦截，在 Finder 里右键 → 打开。菜单栏图标若被 Bartender 一类工具收起，再双击一次会弹出窗口。

3. 点 **Install Driver…**，输入管理员密码。系统会重启音频服务，所有声音中断一两秒。设备名是 **ScreamMusic**。

4. 填接收端 IP 和端口（scream2diretta 请用 `-p 4011`，它自己的默认端口是 4010），点 Apply。

5. 打开 **Apple Music mode**。系统会要两项权限：

   - **自动化（控制 Music）**：暂停、回退、继续，用于换采样率；
   - **麦克风**：把其他 App 的声音转到另一块输出。拒绝的话其他 App 静音，Music 不受影响。

音量请在功放或 Diretta 端调。这块声卡故意没有音量控制，否则就不是 bit-perfect。

卸载：菜单里 **Remove Driver…**，或删掉 `/Library/Audio/Plug-Ins/HAL/ScreamMusic.driver` 后执行 `sudo killall coreaudiod`。

## 和 scream2diretta 配对

```sh
# 接收端
scream2diretta -p 4011
```

ScreamMusic 里 Receiver 填那台机器的 IP，端口 4011，Header 用 Extended。原版 5 字节接收端（`scream -L`、s2d `--legacy`）把 Header 改成 Original；此时线上只有 S32。

## Bit-perfect 的条件

- Music 音量 100，声音检查、均衡器、歌曲过渡、杜比全景声都关掉。
- 设备采样率、位深和曲目一致。对上之后，16/24 位样本在 Float32 里可以精确还原，驱动按 `round(x × 2^(N-1))` 打包，不加抖动。
- 面板在本次播放的样本全部落在网格上时显示绿色。日志在 `~/Library/Logs/ScreamMusic.log`。

macOS 27 上，格式正确时已用满幅测试文件和 Apple Music 曲目逐字节核对过。流媒体的**起播段**（开头约 1–3 秒的低规格版本）不可能 bit-perfect，那段之后可以。

## 已知限制

- **流媒体采样率跟随还在打磨。** Apple Music 的 HLS 常先放一小段低规格，再跳到正式版本；拖动进度时还会把下一首的预读和解码器日志混在一起。0.3.0 起，曲目进行中不再凭日志立刻切换，由驱动统计的「样本是否在网格上」决定要不要改格式。同格式专辑的自然衔接不受影响。
- **换采样率时接收端可能咔一下。** 发送的数据在切换点是静音后平滑进入的；咔声来自 scream2diretta 重建 Diretta 连接、以及 DAC 锁定新时钟。同采样率切歌没有这一下。
- 仅立体声、仅 UDP、没有 DSD。
- ad-hoc 签名，未公证。macOS 26 的代码路径在，但还没在 26 上完整验证。
- 更新驱动会重启 `coreaudiod`。App 会在驱动重新加载后自动接回 Apple Music 模式。

## 从源码构建与测试

```sh
./build.sh            # release，驱动打进 App 的 Resources
DEBUG=1 ./build.sh    # debug
Tests/run_tests.sh    # 协议单元测试、与 screamalsa 包头对照、驱动进程内测试
```

`Tests/run_tests.sh` 里和 `snd-screamalsa.c` 的逐字节对照，需要本目录的上两级是 screamalsa 源码树（`../../snd-screamalsa.c`）。单独克隆本仓库时，这一项会跳不过去；其余测试不依赖它。

驱动装好之后：

```sh
Tests/e2e_hal.sh      # 经真实 HAL 播放测试 WAV，逐字节比对
```

工具（由 `Tests/run_tests.sh` 编到 `build/`）：

| 工具 | 作用 |
|---|---|
| `scream_dump` | 本机收包，按格式分段写成 raw |
| `smctl` | 读写驱动属性：`status`、`dest`、`bits`、`delay`、`rate`、`eot`、`mirror` |
| `hogplay` | 像独占播放器一样把 WAV 写进设备 |
| `driver_harness` | 不安装，在进程内加载驱动做比对 |

设计笔记在 `DESIGN.md`（中文，给改代码的人看）。

## 许可与来源

GPL-3.0。虚拟声卡的骨架来自 Apple 的 NullAudio 示例（MIT，版权声明保留在驱动源码里）。线上包头以 screamalsa 的 `scream_fill_header()` 为准。

Apple Music 的采样率识别没有公开 API，只能读 Music 的日志并在曲目边界暂停、回退。这个做法参考了 [LosslessSwitcher](https://github.com/vincentneo/LosslessSwitcher) 和 [Nativerate](https://github.com/dizzysound/Nativerate) 的思路，驱动和发送路径是另外写的。
