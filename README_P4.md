# MyPaperP4

面向自定义 **ESP32-P4 墨水屏设备**的阅读、通信与音频固件，与 MyPaperS31 共用应用层、阅读器和 2D 刷新引擎，采用独立板级驱动与构建配置。项目主介绍已转为 [MyPaperS31 README](README.md)，本文保留 P4 专属的硬件、通信及构建说明。

## 功能

P4 保留本地阅读、阅读首页、多风格书架、文件管理、音乐、网络收音机、RSS、微信读书、DeepSeek、在线/离线地图、股票与科学计算器。书籍格式、SD 卡目录、系统字体及共用应用说明见 [主 README](README.md)。

P4 的主要硬件功能包括：

- ESP32-C6 作为 ESP-Hosted Wi-Fi 协处理器，经 SDIO 与 P4 通信；支持常开 / 按需生命周期。
- ML307A 提供 4G 数据、短信和电话，设置中可选择 Wi-Fi 或 4G 数据网络。
- ES8311 + I²S + PA 扬声器输出，以及 UAC 1.0 / UAC 2.0 USB 耳机或 DAC。
- Wi-Fi 浏览器传输、热点传输及重启隔离的 USB MSC 模式。
- 前光、系统日期时间、网络校时、三语界面和系统字体设置。

S31 的独立模拟器固件、PCF8563 / BQ27220 驱动和电源锁存关机属于 S31 板级实现，不应据此认为 P4 也具备相同硬件功能。P4 控制中心保留关闭按钮，不提供 S31 的硬件关机按钮。

### 短信与电话

- ML307A 短信收发，按号码归并对话；中文短信使用 UCS-2 PDU，发件记录保存到 SD 卡。
- 电话拨打、来电接听、挂断、通话中 DTMF、通话计时和本地通话记录。
- 可从 SD 卡 `/ringtone/` 选择来电铃声。通话音频走模组模拟音频引脚，与普通音乐的 ES8311 路径不同。
- 模组经 USB RNDIS 提供数据网络，AT 通道负责控制。

时间显示默认北京时间 UTC+8。当前适配的 ML307A `AT+CCLK?` 数字实测为 UTC，默认按 UTC 数字解释；短信接收时间按短信自身时区字段解析。相关设置可在日期与时间菜单调整。

### 音频

支持 MP3、FLAC、WAV、M4A、AAC、Ogg Vorbis 和 Opus，本地音乐有封面、歌词、列表三视图，同名 `.lrc` 可作外置歌词。收音机支持 HLS / MP3 电台。

ES8311 外放输出最高 48 kHz；USB 按 DAC 描述符协商，设备支持时可使用更高原始采样率和 16 / 24 / 32-bit 格式，具体格式兼容性以设备实测为准。架构见 [USB 音频输出](docs/USB_AUDIO_OUTPUT_DESIGN.md)。

## 硬件

| 项目 | P4 配置 |
| --- | --- |
| 主控 | ESP32-P4，最低芯片版本 1.0 |
| CPU | 默认 360 MHz |
| Flash | 32 MiB |
| PSRAM | 200 MHz Hex PSRAM |
| 屏幕 | 1216 × 684 墨水屏 |
| 触摸 | GT911 |
| 存储 | 4-bit SDMMC |
| Wi-Fi | ESP32-C6 + ESP-Hosted SDIO |
| 4G / 短信 / 电话 | ML307A |
| 音频 | ES8311 + I²S + PA |
| USB | HS 端口按功能切换 Host / Device |
| SDK | ESP-IDF 5.3.5，目标 `esp32p4` |

主要外设引脚：

| 外设 | 引脚 |
| --- | --- |
| SDMMC CLK / CMD / D0 / D1 / D2 / D3 | GPIO43 / 44 / 39 / 40 / 41 / 42 |
| Audio I²C SDA / SCL | GPIO7 / 8 |
| I²S MCLK / SCLK / DOUT / LRCK / DIN | GPIO13 / 12 / 11 / 10 / 9 |
| Audio PA | GPIO53 |
| ESP32-C6 Reset / Enable | GPIO54 |
| 五键 ADC | GPIO1 / ADC1_CH0 |

屏幕、电源和触摸配置位于仓库内定制的 M5GFX / M5Unified。固件不是通用 P4 开发板示例，移植前需逐项核对硬件连接。

## 构建与烧录

在独立终端激活 **ESP-IDF 5.3.5**，然后在仓库根目录显式选择 P4。当前默认 Board 是 S31，仅切换 SDK 环境不足以切换目标。

```powershell
# 先在当前终端激活本机 ESP-IDF 5.3.5 环境
./scripts/build-board.ps1 -Board MyPaperP4 -Action build
./scripts/build-board.ps1 -Board MyPaperP4 -Action flash -Port COM10
./scripts/build-board.ps1 -Board MyPaperP4 -Action monitor -Port COM10
```

`COM10` 改为实际串口；默认烧录 921600 baud，可用 `-Baud 460800` 降速。监视为 115200 baud，`Ctrl-]` 退出。flash 只烧录已有产物，源代码更新后需先 build。

| 配置 | 构建目录 | 参数 |
| --- | --- | --- |
| Current | `build-p4` | 默认配置 |
| Debug | `build-p4-debug` | `-Profile Debug` |
| Release | `build-p4-release` | `-Profile Release` |

烧录时指定与构建相同的 Profile。固件为对应目录中的 `MyPaperP4.bin`。VS Code 保留 P4 专用任务；默认的 ESP-IDF Build 指向 S31，不能直接当作 P4 构建按钮。

P4 新构建使用根目录 [sdkconfig.defaults](sdkconfig.defaults) 加 [P4 defaults](configs/mypaperp4/sdkconfig.defaults)，分区见 [P4 partitions.csv](configs/mypaperp4/partitions.csv)，factory 应用为 8 MiB。原 `build` / 根目录 `sdkconfig` 可作为既有配置保留，勿在其中执行 S31 的 set-target；新脚本入口使用独立 `build-p4`。

S31 与 P4 的 SDK、目标、构建目录和配置必须分别管理，不要互刷固件。仓库内 `managed_components` 包含项目适配，升级 SDK 或依赖前应核对这些修改。

## SD 卡与持久化

通用目录为 `/books`、`/fonts`、`/images`、`/music`、`/map`，RSS 源可由应用管理或放在 `/rss.txt`；P4 另有可选的 `/ringtone`。

设备数据保存在 `/sdcard/.reader/`，包括书架记录、阅读位置与时长、封面缓存、书籍解析索引及应用缓存。P4 短信发件记录位于 `sms/sent.tsv`，通话记录为 `calls.tsv`。缓存可按需重建，但删除阅读记录、进度或通信记录会丢失相应数据。

USB MSC 模式会重启并将 SD 卡交给电脑独占，退出前请先安全弹出。具体见 [USB U 盘设计](docs/USB_MASS_STORAGE_TRANSFER_DESIGN.md)。

## 相关资料

- [主 README：共用应用、阅读与字体](README.md)
- [短信设计](docs/ML307_SMS_DESIGN.md)
- [USB 音频输出](docs/USB_AUDIO_OUTPUT_DESIGN.md)
- [系统字体与内存预算](docs/SYSTEM_FONT.md)
- [2D 刷新框架](docs/EPD_2D_WAVEFORM_ENGINE.md)
- [源码模块说明](main/README.md)

反馈问题请明确写出 **MyPaperP4**、提交版本、IDF 版本及日志，避免与 S31 不同的 Wi-Fi、音频和电源通路混淆。第三方组件、字体及图标沿用各自授权与声明。
