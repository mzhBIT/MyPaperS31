# MyPaperS31

本项目完美适配 **无名科技 4.7 寸墨水屏设备 Metalio-E-INK4-Plus**。

面向 **ESP32-S31 墨水屏设备**的阅读与多应用固件。以本地书籍阅读为核心，提供书架、文件管理、音乐、网络收音机、RSS、微信读书、DeepSeek、在线/离线地图、股票和计算器，并支持重启切换到独立的 GB 模拟器固件。

当前默认构建目标为 **MyPaperS31 / ESP-IDF 6.1**。同一仓库保留 MyPaperP4 支持，P4 的硬件、4G 通信和构建方式见 [README_P4.md](README_P4.md)。仓库目录即使仍叫 `MyPaperP4`，也不影响选择 S31 构建。

## S31 硬件

| 项目 | 配置 |
| --- | --- |
| 主控 | ESP32-S31，原生 Wi-Fi |
| Flash | 16 MiB，DIO / 80 MHz |
| PSRAM | 16 MiB Octal PSRAM，当前默认 250 MHz |
| 屏幕 | 1216 × 684 墨水屏，常用竖屏界面为 684 × 1216 |
| 触摸 | FT6336U |
| 存储 | SD 卡，4-bit SDMMC |
| 外放 | BH1098G，UART 控制、I²S 音频及功放控制 |
| USB | Type-C 数据通路切换，支持 USB MSC 与 USB 数字耳机 |
| 前光 | 冷暖双通道 PWM，亮度和色温可调 |
| 时钟与电源 | PCF8563 RTC、BQ27220 电池计、CX25601N 充电管理 |
| SDK | ESP-IDF 6.1，目标 `esp32s31` |

这是与具体硬件连接配套的固件。外设引脚见 [S31 Pins.h](main/boards/MyPaperS31/Pins.h)，显示与电源驱动位于定制 M5GFX 中。其他板卡或硬件批次应核对引脚、电源和 PSRAM 时钟配置。

## 主要功能

### 本地阅读与书架

- 支持 TXT、Markdown、EPUB、FB2、DOCX，以及无 DRM 的 MOBI / PRC / AZW / AZW3。
- 支持 ZIP / CBZ 图片漫画，包含 JPG、JPEG、PNG、BMP，可带子目录；按文件名自然排序，每张图片一页，无需整包解压。
- 支持 EPUB 目录、封面、正文图片，MOBI6 / KF7 文本和 AZW3 / KF8 章节重建。
- 支持横排、竖排、ruby 小字、行内批注与链接脚注；具体呈现取决于原书标记。
- 正文字体支持 SD 卡 TTF / OTF 和 EDCBook BIN；可调整字号、字间距、行距、段间距、页边距、首行缩进与图片显示。
- 支持简繁转换、阅读背景及封面配置。简繁转换需准备相应字典资源，见 [简繁转换说明](docs/CHINESE_SCRIPT_CONVERSION_DESIGN.md)。
- 自动翻页间隔 5～300 秒，每次递增 5 秒；翻页动画可选关闭、快、慢，支持点击区域及滑动方向判定。
- 下一页排版、字形、图片和独立缓冲预绘制，减少翻页时的正文绘制等待。
- 阅读进度与累计阅读时长在退出阅读时保存，阅读时长按分钟展示。
- 独立阅读首页、最近阅读、多种书架风格及文件标签页；封面优先使用无压缩 4bpp BIN 缓存，缺失时从图片生成。

刷新模式包括 `epd_fastest`、`epd_fast`、`epd_text`、`epd_hd` 和 `epd_quality`。正文默认 `epd_text`、默认显示图片；书架使用 `epd_hd`，从书架首次打开书籍使用一次 `epd_quality`。主系统使用共用 2D 刷新引擎及驱动帧流水线。

### 文件管理与传输

- 浏览 SD 卡目录，打开书籍、图片、字体与音频；支持文件操作、重命名（含扩展名）和图片设为待机壁纸。
- 文件页右上角“+”提供传输入口，系统主页也有独立传输应用。
- Wi-Fi 网页传输：连接路由器，或开启设备热点，通过浏览器上传和管理文件。
- USB U 盘模式：重启后将 SD 卡交给电脑独占，退出前应先在电脑上安全弹出磁盘。
- 全局文本输入框支持移动光标，中文输入可切换九宫格。

### 音乐与网络收音机

- 本地音乐支持 MP3、FLAC、WAV、M4A、AAC、Ogg Vorbis 和 Opus；提供列表、封面、歌词视图及播放控制。
- 支持内嵌封面和歌词，也可读取音频同目录、同名的 `.lrc` 歌词文件。
- 音乐与收音机均接入 BH1098G 外放，以及 UAC 1.0 / UAC 2.0 Type-C 数字耳机；输出可选 `SPK / USB / AUTO`。
- 外放按 BH1098G 实际时钟重采样；USB 按耳机描述符协商格式，不能直接套用 P4 外放采样率。
- 收音机支持 HLS 与 MP3 电台，使用后台网络缓冲降低断流影响。第三方电台可用性仍取决于网络和源站。

USB 输出仅支持带 DAC 的数字耳机，不支持被动模拟 Type-C 转接头。三字节 24-bit 等扩展格式需对应耳机实测，详见 [S31 USB 音频](docs/S31_USB_AUDIO.md)。

### 网络应用

| 应用 | 功能 |
| --- | --- |
| 新闻 | RSS 订阅源增删改、新闻列表、图文分页、图片缓存，以及 Bing 壁纸浏览与保存 |
| 微信读书 | 登录、书架、章节阅读与评论；联网失败时使用已有缓存离线阅读 |
| DeepSeek | 对话及图片提问，在设置中配置 API 信息 |
| 地图 | 高德在线地图、SD 卡离线分块地图、地点搜索、起终点路线规划与出行方式缓存 |
| 股票 | A 股、美股自选和报价详情，腾讯网页行情接口，无需 API Key |
| 计算器 | 普通 / 科学模式、DEG / RAD、计算历史 |

地图支持混合来源：Z16 及以下使用离线包，Z17～Z19 使用在线地图。搜索、地址查询和路线规划仍需网络及高德 Key，离线包制作见 [离线地图格式与工具](docs/OFFLINE_MAP_PACK.md)。

微信读书离线模式只读取已有缓存，不等同于自动缓存整本；部分内容需要有效账号权限。S31 不提供 P4 的 ML307A 4G、短信和电话功能，对应主页入口已隐藏。时钟应用目前仍为占位入口，状态栏时间与 RTC 功能可用。

### 系统设置与控制中心

- 简体中文、繁体中文、英文界面。
- 内置 TTF 字符子集；可选择 SD 卡系统字体，系统字号 24～48 px，默认 28 px，与正文字号独立。
- Wi-Fi 常开 / 按需两种生命周期；S31 默认 b/g/n/ax 自动协商、5 dBm 发射功率上限。
- 亮度与色温连续滑条；全局顶部下拉控制中心还显示电量百分比、充电状态、剩余 PSRAM / SRAM，并提供软件关机。
- 日期与时间设置：RTC 初始化、离线设置日期时间、网络校时、时区和 12 / 24 小时制；开机后首次联网自动尝试一次校时。
- 电源键待机 / 唤醒、长按关机，待机关闭前光，可选择待机壁纸；音量键可调节播放音量。
- 恢复默认设置带确认，清除保存的 Wi-Fi 凭据和 DeepSeek / 高德 API 配置；具体保留范围见 [恢复默认设置](docs/RESTORE_DEFAULT_SETTINGS.md)。

内置 UI 字体只包含界面所需字符。阅读正文、任意书名、文件名和歌词建议安装完整字体；系统字体的共享缓存及内存预算见 [系统字体说明](docs/SYSTEM_FONT.md)。

### 独立 GB 模拟器

模拟器是 **仅供 S31 使用的独立固件**，通过重启与主系统切换，采用 Paperboy 式独立连续行扫描，不使用主系统的 2D 刷新通路。

- ROM 放在 SD 卡 `/roms/`，支持普通 GB 和具有 DMG 兼容模式的 `.gbc`，不支持 CGB-only 或 GBA。
- 提供触摸游戏按键、物理按键、暂停菜单、卡带存档与手动状态存档。
- 暂停菜单选择“返回主系统”后保存并自动重启；正常返回不需要按 BOOT。
- 已接入独立 BH1098G 外放，游戏兼容性与音频效果仍需逐项实机验证。

安装及操作见 [独立模拟器固件](docs/DUAL_FIRMWARE_EMULATOR.md)。

## 构建与烧录

### 主系统

安装并激活 **ESP-IDF 6.1** 环境，然后在仓库根目录执行。使用其他 IDF 版本或只激活普通 Python 环境不能替代对应的 IDF 环境。

```powershell
git clone https://github.com/mzhBIT/MyPaperS31.git
cd MyPaperS31

# 先在当前终端激活本机的 ESP-IDF 6.1 环境
./scripts/build-board.ps1 -Board MyPaperS31 -Action build
./scripts/build-board.ps1 -Board MyPaperS31 -Action flash -Port COM5
./scripts/build-board.ps1 -Board MyPaperS31 -Action monitor -Port COM5
```

`COM5` 改为实际串口。烧录默认 921600 baud，可追加 `-Baud 460800` 降速；串口监视为 115200 baud，`Ctrl-]` 退出。烧录前关闭占用端口的串口监视器。

| 配置 | 构建目录 | 构建参数 |
| --- | --- | --- |
| Current | `build-s31` | 默认配置 |
| Debug | `build-s31-debug` | `-Profile Debug` |
| Release | `build-s31-release` | `-Profile Release` |

不同 Profile 使用独立配置和产物；烧录时应指定与构建相同的 `-Profile`。`-Action flash` 只烧录已有产物，不重新编译。主固件路径为对应目录下的 `MyPaperS31.bin`。

VS Code 工作区默认指向 S31，可使用 ESP-IDF Build 或仓库的“编译 MyPaperS31”任务。本仓库 `.vscode` 中的 SDK、Python 和串口路径需按本机安装修改，不要使用 C/C++ 的“编译当前文件”任务构建固件。

### 模拟器固件（可选）

先构建并烧录主系统，使设备具备双固件分区表，再执行：

```powershell
./scripts/build-emulator.ps1 -Action build
./scripts/build-emulator.ps1 -Action flash -Port COM5
```

构建产物为 `build-s31-emulator/MyPaperEmulatorS31.bin`。安装脚本核对设备真实分区表和镜像身份，仅写入模拟器槽；完成后从主系统主页第二页进入“模拟器”。

### Flash 分区

以 [S31 分区表](configs/mypapers31/partitions.csv) 为准：

| 分区 | 地址 | 大小 |
| --- | --- | --- |
| NVS | `0x9000` | 24 KiB |
| PHY | `0xF000` | 4 KiB |
| 主系统 factory | `0x10000` | 8 MiB |
| 模拟器 emulator | `0x810000` | 4 MiB |
| 启动选择 otadata | `0xC10000` | 8 KiB |

这是两个用途不同的固件槽，不是主系统 A/B OTA 升级布局。SDK 可能提示主系统镜像放不进 4 MiB 模拟器槽；应同时检查主系统的 factory 容量检查，真正超出所属槽才是溢出。

## SD 卡准备

建议使用 FAT32。电脑看到的是卡根目录，设备日志会在这些路径前加 `/sdcard`：

```text
/books/       TXT、MD、EPUB、FB2、DOCX、MOBI、PRC、AZW、AZW3、ZIP、CBZ
/fonts/       完整 TTF / OTF / EDCBook BIN 字体
/images/      JPG、JPEG、PNG、BMP 图片与壁纸
/music/       音乐及同名 LRC 歌词
/map/         离线地图包与 catalog.mpc（在地图应用中选择）
/roms/        GB ROM 与模拟器存档
/rss.txt      可选的自定义 RSS 源
/.reader/     自动生成的阅读记录、封面和应用缓存
```

RSS 可在应用内通过右下角“+”添加，长按修改或删除；也可编辑 `/rss.txt`，每行 `名称|URL`，例如：

```text
IT之家|https://www.ithome.com/rss
```

仅在配置及备份都不存在时启用内置源；删除全部订阅后会保持空列表，见 [RSS 源管理](docs/RSS_FEED_MANAGEMENT.md)。

### 首次使用

1. 在设置中选择语言、系统字体和前光，在“网络”中配置 Wi-Fi。
2. 将书籍和完整字体复制到 SD 卡，通过文件页打开书籍，之后可从书架或最近阅读继续。
3. 网页传输时访问设备屏幕显示的地址；热点模式需先连接屏幕显示的 SSID，默认热点密码为 `12345678`。
4. 配置 DeepSeek / 高德 Key 后使用相应在线应用；微信读书按界面完成登录。

Type-C 口用于 U 盘或数字耳机时会切换数据通路，不能同时使用同一个端口的 USB Serial/JTAG 日志。切换功能前注意电脑挂载及耳机输出状态。

## 开发与资料

```text
main/                  共用应用、阅读器、系统服务及分板硬件后端
components/            定制 M5GFX / M5Unified、FreeType、USB Host 等
managed_components/    项目依赖，部分包含适配修改
firmware/emulator/     S31 独立模拟器及第三方核心
configs/               S31 / P4 默认配置、分区表与日志 Profile
scripts/               分板构建、烧录及模拟器安装入口
tools/                 字体子集、图标、地图转换与 PC 验证工具
docs/                  功能设计、实现边界及验证记录
```

- [P4 设备说明](README_P4.md)
- [源码模块说明](main/README.md)
- [系统字体与新增文案的字符覆盖检查](docs/SYSTEM_FONT.md)
- [书架](docs/PICO_BOOKSHELF.md) / [阅读首页](docs/READING_HOME.md) / [文件管理](docs/FILE_BROWSER_UI.md)
- [DOCX](docs/DOCX_READING.md) / [ZIP 漫画](docs/ZIP_COMIC_READING.md) / [竖排与批注](docs/VERTICAL_ANNOTATED_READER_DESIGN.md)
- [2D 波形与驱动帧](docs/EPD_2D_WAVEFORM_ENGINE.md) / [正文预绘制](docs/READER_PAGE_BUFFER.md)
- [S31 USB U 盘](docs/S31_USB_MASS_STORAGE_DESIGN.md) / [收音机](docs/S31_RADIO_PLAYBACK.md) / [Wi-Fi 功率测试](docs/S31_WIFI_POWER_RESULTS.md)

设计文档中可能保留阶段性方案与历史验证记录；当前默认参数以源码、`configs` 及实际构建目录的 `sdkconfig` 为准。修改 defaults 不会自动覆盖已有 sdkconfig 中的值。

新增界面文案后，应执行 `python tools/subset_system_font.py --check`，缺字时重新生成并提交内置字体子集。第三方库、字体、图标和模拟器核心应保留各自授权及声明；模拟器来源见 [vendor NOTICE](firmware/emulator/vendor/NOTICE.md)。

## 已知边界

- 不支持 DRM 书籍、KFX、Print Replica、旧版 DOC；DOCX 为重排阅读，不还原 Word 分页。漫画不支持 RAR / CBR、加密 ZIP 或 ZIP64。
- 内置 UI 字体不覆盖任意文本；缺字应安装合适的完整字体。
- S31 的 16 MiB PSRAM 需要同时容纳驱动帧、字体、图片与网络缓存，大文件仍可能触及内存限制。
- 快速墨水屏刷新可能残影；动画、图片灰阶及刷新间隔需按实际面板效果选择。
- 网络电台、RSS、行情及在线接口可能变更；股票报价可能延迟，不保证接口长期可用。

反馈问题时请附提交版本、IDF 版本、完整日志、硬件型号，以及触发文件和操作步骤。
