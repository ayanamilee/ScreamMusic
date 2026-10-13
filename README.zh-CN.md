# ScreamMusic

[English](README.md)

macOS 菜单栏应用，带一个小音频驱动。系统里多一块名为 **ScreamMusic** 的声卡，让 Apple Music 按每首曲目自己的采样率和位深 bit-perfect。声音可以发到网络上的 Scream 接收端（UDP），也可以到一个本地输入设备，供 HQPlayer 这类软件读取。

需要 macOS 26 或更新。协议与 [screamalsa](https://github.com/ayanamilee/screamalsa) 一致。License: GPL-3.0。

## 下载

[Releases](https://github.com/ayanamilee/ScreamMusic/releases) 里的 zip。ad-hoc 签名，未公证：解压后右键 App → 打开。

## 两种用法

在面板的 Receiver 下拉框里选一种，互斥。

### Scream（网络）

Apple Music 用 UDP 发到 Scream 接收端。填它的 IP 和端口，点 Apply，Header 按接收端选：

| 接收端 | Header |
|---|---|
| scream2diretta（s2d，不加 `--legacy`） | **Extended**（6 字节，默认） |
| 原版 scream、`scream -L`、apscream | **Original**（5 字节） |

s2d 和原版 scream 的默认端口是 **4010**。本应用默认发 **4011**。两边填同一个端口。

Original 只发 32 位 PCM。Extended 按曲目发 16 / 24 / 32 位。

### Loopback（ScreamMusic AM）

Apple Music 不发到网络，而是出现在第二个设备 **ScreamMusic AM** 里：一个只有输入的立体声设备，采样率跟着曲目变。能选输入设备的软件（比如 HQPlayer）直接选它。

换曲拦截、丢弃错误采样率的开头、重锁静音前导都照常生效。设备里是 Float32，16 位和 24 位的样本原样装得下（测试逐位一致），不影响音质。macOS 会要求那个软件的麦克风权限。

原来的 **ScreamMusic** 设备不变：它是输出，输入里是"其他 App"的声音。

替 Music 播放的路由软件（SoundSource、Loopback 之类）会让 Music 不再直连 ScreamMusic，ScreamMusic AM 就收不到它的声音。用 Loopback 时请让 Music 绕过这些软件。

## Apple Music 模式

打开后：

1. 系统默认输出变成 ScreamMusic（记住之前的输出，关掉模式时恢复）。
2. 设备按每首曲目的采样率和位深切换。换采样率时，在两首之间的静音处拦住，切好设备，回退重播——开头那一小段旧采样率的声音不会到接收端。
3. 其他 App 的声音转到你选的输出（启动前的、内置的、或指定的），也可以静音。

音量在功放或接收端调。这块声卡没有音量，否则就不是 bit-perfect。

只去声卡用 **Remove Driver…**；连 App 一起删（恢复输出、移除驱动、清设置和日志、删除 App 本体）用 **Uninstall…**。

## 说明

- 立体声，仅 UDP。采样率 44.1–384 kHz。
- bit-perfect 需要：Music 音量 100，声音检查、均衡器、歌曲过渡、杜比全景声关掉。
- Apple Music 流媒体开头一两秒可能是低规格起播段（48 kHz 几秒后升到 96 kHz），升级的那一下 Music 自己有约 0.1 秒空档。同采样率切歌无缝。
- 换采样率时，驱动先发一段新采样率的静音（默认 0.5 秒），让接收端和 DAC 在静音里重锁。Advanced 里可调（Off / 250 / 500 / 1000 ms）；要更新到最新驱动才有。
- 播过的曲目会记住实测格式（Rate Library，存在 `~/Library/Application Support/ScreamMusic/`），手动点播时不用再等解码器日志。只是提示：自然衔接以曲目自己的预读为准，库里的格式不对会被网格检查纠正并删掉。Advanced 里可以清空。
- 拖动进度条后偶尔有一声：Music 对新位置没有淡入，是 Music 自己的行为，为保持 bit-perfect 不处理。
- 更新驱动会重启 coreaudiod，系统声音会停一两分钟；若迟迟不恢复，可执行 `sudo killall AirPlayXPCHelper`。
- 菜单栏图标若被收起，再双击 App 会弹出窗口。

从源码构建：`./build.sh`，产物 `build/ScreamMusic.app`。设计笔记见 `DESIGN.md`。
