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
- Apple Music 流媒体开头一两秒可能是低规格起播段。同采样率切歌无缝；换采样率时，接收端或 DAC 重锁时钟可能咔一下。
- 菜单栏图标若被收起，再双击 App 会弹出窗口。

从源码构建：`./build.sh`，产物 `build/ScreamMusic.app`。设计笔记见 `DESIGN.md`。
