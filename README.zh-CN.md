# ScreamMusic

[English](README.md)

macOS 菜单栏应用，带一个小音频驱动。系统里多一块名为 **ScreamMusic** 的声卡，让 Apple Music 按每首曲目自己的采样率和位深 bit-perfect。播进去的 PCM 用 Scream 协议（UDP）原样发出。

需要 macOS 26 或更新。协议与 [screamalsa](https://github.com/ayanamilee/screamalsa) 一致。License: GPL-3.0。

## 下载

[Releases](https://github.com/ayanamilee/ScreamMusic/releases) 里的 zip。ad-hoc 签名，未公证：解压后右键 App → 打开。

**请用 0.4.7。** 0.5.0 和 0.5.1 有已知问题：设备时钟走慢，接收端欠载，声音是怪声。这两个版本保留作记录，不要安装。

## 接收端

在 App 里填 IP 和端口，点 Apply，Header 按接收端选：

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

## Apple Music 模式

打开后：

1. 系统默认输出变成 ScreamMusic（记住之前的输出，关掉模式时恢复）。只有这样才捕获得到 Apple Music 的声音。
2. 设备按每首曲目的采样率和位深切换。换采样率时，在两首之间的静音处拦住，切好设备，回退重播——开头那一小段旧采样率的声音不会到接收端。
3. 其他 App 的声音转到你选的输出（启动前的、内置的、或指定的），也可以静音。

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
