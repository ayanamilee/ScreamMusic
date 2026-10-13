# ScreamMusic

macOS 菜单栏应用。系统里多一块名为 **ScreamMusic** 的立体声声卡，播放进去的 PCM 用 Scream 协议（UDP）原样发出。给 Apple Music，也给能自己选输出设备的播放器。

需要 macOS 26 或更新。协议与 [screamalsa](https://github.com/ayanamilee/screamalsa) 一致。License: GPL-3.0。

## 下载

[Releases](https://github.com/ayanamilee/ScreamMusic/releases) 里的 zip。ad-hoc 签名，未公证：解压后右键 App → 打开。

## 接收端

在 App 里填 IP 和端口，Header 按接收端选：

| 接收端 | Header |
|---|---|
| scream2diretta（s2d，不加 `--legacy`） | **Extended**（6 字节，默认） |
| 原版 scream、`scream -L`、apscream | **Original**（5 字节） |

s2d 和原版 scream 的默认端口是 **4010**。本应用默认发 **4011**。两边填同一个端口。

Original 只发 32 位 PCM。Extended 按曲目发 16 / 24 / 32 位。

## 使用

1. 打开 App，点 **Install Driver…**（要管理员密码，系统声音会断一两秒）。
2. 填接收端，点 Apply。
3. **Apple Music**：打开 Apple Music mode。允许控制 Music；若要把其他 App 转到另一块声卡，再允许麦克风。

音量在功放或接收端调。这块声卡没有音量，否则就不是 bit-perfect。

关掉模式或退出后，系统默认输出会恢复。只去声卡用 **Remove Driver…**；连 App 一起删（恢复输出、移除驱动、清设置和日志、删除 App 本体）用 **Uninstall…**。

## 说明

- 立体声，仅 UDP。采样率 44.1–384 kHz。
- Music 音量 100，声音检查、均衡器、歌曲过渡、杜比全景声关掉，格式才是 bit-perfect。
- Apple Music 流媒体开头一两秒可能是低规格起播段（48 kHz 几秒后升到 96 kHz），升级的那一下 Music 自己有约 0.1 秒空档。同采样率切歌无缝。
- 换采样率时，驱动先发一段新采样率的静音（默认 0.5 秒），让接收端和 DAC 在静音里重锁，音乐开头不再咔一下。Advanced 里可调（Off / 250 / 500 / 1000 ms）；要更新到最新驱动才有。
- **Loopback 模式**（Receiver 下拉框里选 "Loopback: ScreamMusic AM"，需要 0.5.0 的驱动）：Apple Music 的声音不发到网络，而是出现在一个叫 **ScreamMusic AM** 的输入设备里，HQPlayer 之类能选输入设备的软件直接选它。和发 Scream 互斥。换曲拦截、丢弃错误采样率的开头、换采样率时的静音前导都照常生效，采样率跟着曲目变。设备里是 Float32，16 位和 24 位的样本原样装得下，不影响音质（前提还是 Music 音量 100 等 bit-perfect 条件）。macOS 会要求那个软件的麦克风权限。原来的 **ScreamMusic** 设备不变：它是输出，输入里是"其他 App"的声音。
- **和 SoundSource、Loopback 等路由软件同用，可能让路由混乱**：它们会替 Music 播放，Music 就不再是直连 ScreamMusic 的进程，ScreamMusic AM 里就收不到它的声音。要用 Loopback 模式，请让 Music 绕过这些软件，自己负责配置。
- 播过的曲目会记住实测格式（Rate Library，存在 `~/Library/Application Support/ScreamMusic/`），手动点播时不用再等解码器日志。只是提示：自然衔接以曲目自己的预读为准，库里的格式不对会被网格检查纠正并删掉。Advanced 里可以清空。
- 拖动进度条后偶尔有一声：Music 对新位置没有淡入，是 Music 自己的行为，为保持 bit-perfect 不处理。
- 更新驱动会重启 coreaudiod，系统声音会停一两分钟；若迟迟不恢复，可执行 `sudo killall AirPlayXPCHelper`。
- 菜单栏图标若被收起，再双击 App 会弹出窗口。

从源码构建：`./build.sh`，产物 `build/ScreamMusic.app`。设计笔记见 `DESIGN.md`。
