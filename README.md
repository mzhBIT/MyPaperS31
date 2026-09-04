# MyPaperP4

MyPaperP4 是一套面向自定义 **ESP32-P4 单色墨水屏设备**的阅读与音频固件。项目以本地阅读为核心，同时集成 SD 卡文件管理、网页传输、本地音乐、网络收音机和 RSS 阅读，并针对 1216 × 684 墨水屏、PSRAM、ESP32-C6 Hosted Wi-Fi 和 ES8311 音频链路做了适配与性能优化。

> 本项目与具体硬件连接紧密耦合，并不是通用 ESP32-P4 开发板示例。直接移植到其他硬件前，需要重新配置屏幕总线、电源、触摸、SDMMC、Wi-Fi 协处理器和音频引脚。

## 主要功能

### 阅读

- TXT、EPUB 与无 DRM MOBI/PRC 阅读。
- TXT 支持 UTF-8、UTF-16LE、GBK 和 Latin-1 回退。
- EPUB2/EPUB3 目录、封面、正文图片和阅读进度。
- EPUB ZIP 索引与元数据持久化缓存，加快再次打开速度。
- MOBI6/KF7 支持无压缩、PalmDOC 与 HUFF/CDIC 文本，按记录流式读取并缓存解析索引。
- 书架按最近打开顺序展示书籍，并支持 EPUB/MOBI 封面缩略图缓存。
- 共用分页与排版框架：
  - 字号与行距
  - 首行缩进字符数
  - 段间距
  - 左右页边距
  - CJK 标点避头尾与两端排版
  - 图片显示开关
- TTF、OTF 和 EDCBook `.bin` 字体切换。
- `quality`、`text`、`fast`、`fastest` 四种墨水屏刷新模式。

### 音频

- 从 SD 卡递归扫描并播放 MP3、FLAC、WAV、M4A、AAC、Ogg Vorbis 和 Opus。
- 顺序播放、随机播放、单曲循环、上一首、下一首、暂停与音量调节。
- 雪花省电模式可冻结歌词与进度条的周期刷新，不影响播放控制和界面交互。
- 播放进度、已播放时间和总时长显示。
- 封面、滚动歌词和歌曲列表三视图，可在内容区左右滑动切换。
- 支持 FLAC PICTURE、ID3 APIC/PIC、M4A `covr` 内嵌封面，以及同目录封面图片；封面会生成适合硬解显示的方形缩略图。
- 44.1 kHz、48 kHz 音频按原采样率输出；高于 48 kHz 时转换到设备支持范围。
- 相邻歌曲采样率相同时避免重复配置 I²S/Codec。
- 支持 ID3 `SYLT`、`USLT`/`TXXX`、Vorbis Comment 和 M4A `©lyr` 中的同步歌词。
- 没有可用内嵌歌词时，自动查找音频同目录下的同名 `.lrc` 文件。
- 外置 LRC 支持 UTF-8、UTF-16 和 GBK。
- 内置 CNR HLS 网络电台，支持顺序或随机切台。

### 网络与内容

- ESP32-C6 作为 ESP-Hosted Wi-Fi 协处理器，通过 SDIO 与 ESP32-P4 通信。
- 离开网络功能后可停止 Wi-Fi、关闭 C6 电源，降低待机功耗。
- RSS 源列表、新闻列表、正文分页和文章图片缓存。
- Bing 每日壁纸浏览与保存。
- 浏览器批量上传文件，按扩展名自动保存到 SD 卡对应目录。

### 文件与交互

- SD 卡文件浏览器可打开书籍、图片、字体和音频。
- JPG、PNG、BMP 图片浏览。
- 电容触摸：点击、长按、上下左右滑动和边缘返回手势。
- 五键 ADC 键盘：返回、上、下、确认和扩展功能。
- 墨水屏局部脏区刷新，减少无关区域刷新和闪烁。

## 当前界面状态

| 入口 | 状态 |
| --- | --- |
| Book | 按最近打开顺序展示的书架与阅读器 |
| Files / Images | SD 卡文件与图片浏览 |
| Music | 本地音乐、歌词与播放控制 |
| Radio | 网络收音机 |
| News | RSS 与 Bing 壁纸 |
| Transfer | Wi-Fi 网页传输 |
| Setting | Wi-Fi 扫描、密码输入、连接验证与默认网络管理 |
| Clock | 当前仍为占位界面 |

## 硬件与构建配置

| 项目 | 配置 |
| --- | --- |
| 主控 | ESP32-P4，最低芯片版本 1.0 |
| CPU | 360 MHz |
| Flash | 32 MB |
| PSRAM | 200 MHz Hex PSRAM |
| 屏幕 | 1216 × 684 单色 EPD |
| 触摸 | GT911 兼容触摸链路 |
| 存储 | 4-bit SDMMC |
| Wi-Fi | ESP32-C6 + ESP-Hosted SDIO |
| 音频 | ES8311 + I²S + PA |
| SDK | ESP-IDF 5.3.5 |

主要外设引脚如下：

| 外设 | 引脚 |
| --- | --- |
| SDMMC CLK/CMD/D0/D1/D2/D3 | GPIO43/44/39/40/41/42 |
| Audio I²C SDA/SCL | GPIO7/8 |
| I²S MCLK/SCLK/DOUT/LRCK/DIN | GPIO13/12/11/10/9 |
| Audio PA | GPIO53 |
| ESP32-C6 Reset/Enable | GPIO54 |
| ADC keys | GPIO1 / ADC1_CH0 |

屏幕数据总线、电源管理和触摸配置位于仓库内定制的 M5GFX/M5Unified 实现中。

## 构建与烧录

### 1. 准备环境

安装 ESP-IDF 5.3.5，并加载 ESP-IDF 环境。仓库已经包含当前使用的 M5GFX、M5Unified、FreeType，以及经过项目适配的部分 managed components。

```bash
git clone https://github.com/mzhBIT/MyPaperP4.git
cd MyPaperP4
idf.py set-target esp32p4
idf.py build
```

如果仓库中已有正确的 `sdkconfig`，通常可以跳过 `set-target`，直接构建。

### 2. 烧录与串口监视

Windows：

```powershell
idf.py -p COM5 flash monitor
```

Linux/macOS：

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

串口监视默认波特率为 115200。退出 ESP-IDF monitor 使用 `Ctrl-]`。

自定义分区表位于 [`partitions.csv`](partitions.csv)，当前 factory app 分区大小为 8 MB。

## SD 卡准备

建议使用 FAT32，并在卡根目录准备以下结构。应用会在需要时创建大部分目录：

```text
/books/                 TXT、EPUB、MOBI、PRC
/images/                JPG、JPEG、PNG、BMP
/fonts/                 TTF、OTF、EDCBook BIN 字体
/music/                 MP3、FLAC、WAV、M4A、AAC、OGG、OPUS
/rss.txt                自定义 RSS 源
/.reader/               自动生成的应用数据与缓存
```

歌词文件与音频保持同目录、同文件名：

```text
/music/林俊杰 - 江南.flac
/music/林俊杰 - 江南.lrc
```

### Wi-Fi 配置

在主页进入 `Setting > WiFi`，设备会扫描附近网络。选择网络后可通过屏幕虚拟键盘输入密码；只有连接验证成功，SSID 和密码才会写入 NVS，并成为 Radio、RSS 与 Transfer 的默认网络。

扫描、验证以及网络功能退出后会停止 Wi-Fi 并关闭 ESP32-C6，避免设置页面或阅读场景持续耗电。Wi-Fi 密码存放在 NVS；如产品需要更高安全等级，应同时启用 Flash Encryption 与 NVS Encryption。

### RSS 配置

`/sdcard/rss.txt` 每行一个源，可使用 `标题|URL`：

```text
IT之家|https://www.ithome.com/rss
Bing Wallpaper|https://rsshub.app/bing
```

空行和以 `#` 开头的行会被忽略。没有配置文件或没有有效条目时，应用使用内置 RSS 源。

## Wi-Fi 文件传输

打开主页的 **Transfer**：

1. 设备优先尝试连接已配置的路由器。
2. 连接成功后，屏幕显示设备 IP 地址。
3. 若 STA 连接失败，设备回退到 AP 模式；默认 AP 密码为 `12345678`。
4. 手机或电脑访问屏幕显示的 HTTP 地址。
5. 选择一个或多个支持的文件并上传。

上传文件会自动分类：

| 文件类型 | 目标目录 |
| --- | --- |
| TXT、EPUB、MOBI、PRC | `/sdcard/books` |
| JPG、JPEG、PNG、BMP | `/sdcard/images` |
| TTF、OTF、BIN | `/sdcard/fonts` |
| 支持的音频格式 | `/sdcard/music` |

## 缓存与持久化数据

应用数据保存在 `/sdcard/.reader/`：

```text
recent_books             书架记录（按最近打开顺序）
reader_style             阅读排版设置
reader_font              当前字体
txtprogress/             TXT 阅读进度
epubprogress/            EPUB 阅读进度
epub_tmp/                EPUB 图片按需解压缓存
epub_meta/               EPUB ZIP 索引与元数据缓存
mobi_progress/           MOBI 阅读进度
mobi_tmp/                MOBI 图片按需提取缓存
mobi_meta/               MOBI 记录、章节与目录元数据缓存
rss_img/                 RSS 图片缓存
music_cover/             音频封面与方形缩略图缓存
```

缓存文件可以删除，应用会在下次使用时重新生成；删除进度文件会丢失对应阅读位置。

## 项目结构

```text
MyPaperP4/
├─ main/
│  ├─ activities/        页面与交互逻辑
│  ├─ audio/             本地音乐、收音机、Codec 与歌词
│  ├─ input/             触摸与 ADC 按键
│  ├─ parser/            TXT、EPUB、MOBI、HTML 与编码解析
│  ├─ renderer/          分页、文字、图片与封面缩略图
│  ├─ storage/           SD 卡和应用路径
│  ├─ ui/                字体、样式和位图图标
│  └─ wifi/              ESP-Hosted Wi-Fi 生命周期
├─ components/           定制 M5GFX、M5Unified、FreeType
├─ managed_components/   项目使用并做过适配的 Espressif 组件
├─ Assets/               SVG 设计与图标源文件
├─ tools/                图标转换等开发工具
├─ sdkconfig.defaults
└─ partitions.csv
```

应用内部采用 `Activity`/`ActivityManager` 管理页面。更详细的源码模块说明见 [`main/README.md`](main/README.md)。

## 注意事项

- 本项目不会在 SD 卡挂载失败时自动格式化存储卡。
- 网络电台地址和 RSS 服务可能因第三方服务变化而失效。
- 墨水屏快速局刷会产生一定残影；Music、Radio 等页面首次进入时使用高质量模式清屏。
- Progressive JPEG 解码设有内存预算，大分辨率图片会优先缩放或使用紧凑解码路径。
- 单扬声器设备最终听到的声道由硬件布线决定；软件音频管线当前使用双声道 I²S 格式输出。
- `managed_components/` 中包含项目级修订，升级 ESP-IDF 或组件版本时需要重新核对这些改动。

## 开发状态

项目仍在持续开发中。提交问题时建议附上：

- 使用的提交版本
- ESP-IDF 版本
- 完整串口日志
- 触发问题的文件格式与操作步骤
- 是否同时启用了 EPD、SD、Wi-Fi 或音频
