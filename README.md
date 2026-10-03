# MyPaperP4

MyPaperP4 是一套面向自定义 **ESP32-P4 单色墨水屏设备**的阅读、通信与音频固件。项目以本地阅读为核心，同时集成 SD 卡文件管理、Wi-Fi/USB 传输、本地音乐、网络收音机、在线内容，以及通过 ML307A 实现的 4G、短信和电话功能，并针对 1216 × 684 墨水屏、PSRAM、ESP32-C6 Hosted Wi-Fi 和 ES8311 音频链路做了适配与性能优化。

> 本项目与具体硬件连接紧密耦合，并不是通用 ESP32-P4 开发板示例。直接移植到其他硬件前，需要重新配置屏幕总线、电源、触摸、SDMMC、Wi-Fi 协处理器和音频引脚。

## 主要功能

### 计算器

- 主页第二页计算器入口，支持四则优先级、小数、正负号、百分比、退格及连续运算。
- 五行四列大按键，三语言界面；最近20条计算历史保存在RAM，重启清空，无网络及SD卡写入。
- 页眉切换科学模式：括号、乘方/平方/立方、根号、对数/指数、三角/反三角函数、π/e、绝对值、倒数和DEG/RAD，切换保留算式及结果。
- 结果区域局刷，定期Quality清理；详见 [计算器设计](docs/CALCULATOR_APPLICATION_DESIGN.md)。

### 股票行情

- 主页 Stocks/股票入口，支持 A 股、美股自选列表与报价详情，最多20只自选，配置保存在NVS。
- 通过腾讯网页行情接口批量获取价格、涨跌幅和报价时间，无需API Key；后台请求、手动刷新，失败保留旧报价。
- 此接口没有公开稳定性承诺，行情可能延迟，仅供个人查看，不作为交易依据。见 [股票应用设计](docs/STOCKS_APPLICATION_DESIGN.md)。

### 阅读

- TXT、Markdown、EPUB、FB2、DOCX 与无 DRM MOBI/PRC/AZW/AZW3 阅读。
- DOCX 正文、标题目录、粗体/斜体、列表、嵌入图片与脚注/尾注重排；表格线性化阅读，不还原 Word 原始分页布局，不支持加密 DOCX。
- ZIP/CBZ 图片漫画阅读：支持 JPG/JPEG、PNG、BMP 和子目录，按文件名自然排序，每张图片一页，复用图片预解码、居中显示、阅读进度及计时，不需整包解压；不支持加密 ZIP、ZIP64 或 RAR/CBR。
- TXT 支持 UTF-8、UTF-16LE、GBK 和 Latin-1 回退。
- EPUB2/EPUB3 目录、封面、正文图片和阅读进度。
- EPUB ZIP 索引与元数据持久化缓存，加快再次打开速度。
- MOBI6/KF7 支持无压缩、PalmDOC 与 HUFF/CDIC 文本，按记录流式读取并缓存解析索引。
- AZW3/KF8 支持 FDST 主内容流与 SKEL/FRAG 章节重建；不支持 KFX、Print Replica 和 DRM 内容。
- 书架按最近打开顺序展示书籍，并支持 EPUB/MOBI/FB2 封面缩略图缓存。
- 共用分页与排版框架：
  - 字号与行距
  - 首行缩进字符数
  - 段间距
  - 左右页边距
  - CJK 标点避头尾与两端排版
  - 图片显示开关
- TTF、OTF 和 EDCBook `.bin` 字体切换。
- `fastest`、`fast`、`text`、`hd`、`quality` 五种墨水屏刷新模式；阅读器支持下一页排版和图片预取。

### 音频

- 从 SD 卡递归扫描并播放 MP3、FLAC、WAV、M4A、AAC、Ogg Vorbis 和 Opus。
- 顺序播放、随机播放、单曲循环、上一首、下一首、暂停与音量调节。
- 雪花省电模式可冻结歌词与进度条的周期刷新，不影响播放控制和界面交互。
- 播放进度、已播放时间和总时长显示。
- 封面、滚动歌词和歌曲列表三视图，可在内容区左右滑动切换。
- 支持 FLAC PICTURE、ID3 APIC/PIC、M4A `covr` 内嵌封面，以及同目录封面图片；封面会生成适合硬解显示的方形缩略图。
- 支持 ES8311 扬声器、UAC 1.0 与 UAC 2.0 USB 耳机/DAC 输出，以及 Speaker、USB、Auto 动态切换。
- 扬声器输出最高 48 kHz；USB 输出按 DAC 描述符动态协商，设备支持时可保持高于 48 kHz 的原始采样率及 16/24/32-bit 位深。
- 相邻歌曲采样率相同时避免重复配置 I²S/Codec。
- 支持 ID3 `SYLT`、`USLT`/`TXXX`、Vorbis Comment 和 M4A `©lyr` 中的同步歌词。
- 没有可用内嵌歌词时，自动查找音频同目录下的同名 `.lrc` 文件。
- 外置 LRC 支持 UTF-8、UTF-16 和 GBK。
- 内置 CNR HLS 网络电台，支持顺序或随机切台。

### 网络与内容

- ESP32-C6 作为 ESP-Hosted Wi-Fi 协处理器，通过 SDIO 与 ESP32-P4 通信。
- 可在设置中选择 Wi-Fi 或 ML307A 4G 数据网络；模组通过 USB RNDIS 接入，AT 通道负责控制。
- 离开网络功能后可停止 Wi-Fi、关闭 C6 电源，降低待机功耗。
- RSS 源列表、新闻列表、正文分页和文章图片缓存。
- Bing 每日壁纸浏览与保存。
- 微信读书书架与正文阅读、知乎热榜、DeepSeek 对话及图片提问、高德在线地图和 SD 卡离线分块地图（自动选包）。
- 在线地图支持 32 MiB 多视口 LRU 缓存，重访优先读缓存，点击页眉地图标题可强制更新；地图缓存清理不影响离线地图包。
- 地图支持联网地点搜索、结果列表和选中定位标记；离线地图使用坐标转换后定位，搜索本身仍需要网络和高德 Key。
- 地图支持 IP 城市粗定位（确认后跳转）和中心点地址查询，离线底图的地址查询会转换坐标后联网获取。
- 地图支持手动选择起终点的驾车、步行、骑行路线规划，显示距离、预计时长、分步说明及在线/离线底图路线；规划仍需网络和高德 Key。
- 地图来源支持纯在线、纯离线和混合模式；混合模式 Z16 及以下使用离线包，Z17–Z19 使用高德在线地图（优先在线缓存），缩回后恢复离线底图；在线请求层级按 PC 实测倍率校正。
- 浏览器上传与管理 SD 卡文件；USB MSC 模式可由电脑直接访问 SD 卡，进入该模式前需确保设备与电脑不会同时挂载文件系统。

### 短信与电话

- ML307A 短信收发，按号码归并为对话；中文短信使用 UCS-2 PDU，发件记录保存在 SD 卡。
- 电话拨打、来电接听、挂断、通话中 DTMF 按键，以及通话计时和本地通话记录。
- 可从 SD 卡选择来电铃声，短铃声循环播放；通话音频使用已连接的模组模拟音频引脚。
- 设置中的时间基准默认为北京时间（UTC+8）；可调整显示时区和模组时钟解释方式。此 ML307A 的 `AT+CCLK?` 数字实测为 UTC，虽附带 `+32` 标记，默认按 UTC 数字处理；短信接收时间仍按短信自身的时区字段解析。

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
| WeRead / ZhiHu / DeepSeek / Map | 在线阅读、热榜、对话和在线/离线地图 |
| Messages / Phone | ML307A 短信对话与电话 |
| Transfer | Wi-Fi 网页传输与手动进入 USB U 盘模式 |
| Setting | 网络、前光、API 密钥、EPD 诊断和时间基准等设置 |
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
| 4G / 通话 | ML307A，经 USB RNDIS 与 AT 通道连接；通话音频使用模组模拟音频 |
| 音频 | ES8311 + I²S + PA |
| USB | HS 端口按功能在 Host 与 Device 模式间切换 |
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
/books/                 TXT、Markdown、EPUB、MOBI、PRC、AZW、AZW3、FB2
/images/                JPG、JPEG、PNG、BMP
/fonts/                 TTF、OTF、EDCBook BIN 字体
/music/                 MP3、FLAC、WAV、M4A、AAC、OGG、OPUS
/ringtone/              可选的来电铃声音频
/rss.txt                自定义 RSS 源
/.reader/               自动生成的应用数据与缓存
```

歌词文件与音频保持同目录、同文件名：

```text
/music/林俊杰 - 江南.flac
/music/林俊杰 - 江南.lrc
```

### Wi-Fi 配置

在主页进入 `Setting > WiFi`，设备会扫描附近网络。选择网络后可通过屏幕虚拟键盘输入密码；只有连接验证成功，SSID 和密码才会写入 NVS。`Setting > Network` 可选择在线功能使用 Wi-Fi 或 ML307A 4G 流量。

扫描、验证以及网络功能退出后会停止 Wi-Fi 并关闭 ESP32-C6，避免设置页面或阅读场景持续耗电。Wi-Fi 密码存放在 NVS；如产品需要更高安全等级，应同时启用 Flash Encryption 与 NVS Encryption。

### RSS 配置

`/sdcard/rss.txt` 每行一个源，可使用 `标题|URL`：

```text
IT之家|https://www.ithome.com/rss
Bing Wallpaper|https://rsshub.app/bing
```

空行和以 `#` 开头的行会被忽略。没有配置文件或没有有效条目时，应用使用内置 RSS 源。

## 文件传输

Transfer 提供 Wi-Fi 网页传输和独立的 USB MSC 传输模式。USB 模式会重启并将 SD 卡交给电脑独占；退出前请先在电脑上安全弹出磁盘，再返回正常模式。使用 USB Device 时还需确保电脑的 VBUS 与板载 5 V 电源不会对灌。

### Wi-Fi 网页传输

打开主页的 **Transfer**：

1. 设备优先尝试连接已配置的路由器。
2. 连接成功后，屏幕显示设备 IP 地址。
3. 若 STA 连接失败，设备回退到 AP 模式；默认 AP 密码为 `12345678`。
4. 手机或电脑访问屏幕显示的 HTTP 地址。
5. 选择一个或多个支持的文件并上传。

上传文件会自动分类：

| 文件类型 | 目标目录 |
| --- | --- |
| TXT、Markdown、EPUB、MOBI、PRC、AZW、AZW3、FB2 | `/sdcard/books` |
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
fb2progress/             FB2 阅读进度
fb2_tmp/                 FB2 内嵌图片按需解码缓存
rss_img/                 RSS 图片缓存
music_cover/             音频封面与方形缩略图缓存
sms/sent.tsv             本机发送的短信记录
calls.tsv                本机通话记录
```

缓存文件可以删除，应用会在下次使用时重新生成；删除进度文件会丢失对应阅读位置。

## 项目结构

```text
MyPaperP4/
├─ main/
│  ├─ activities/        页面与交互逻辑
│  ├─ audio/             本地音乐、收音机、Codec 与歌词
│  ├─ input/             触摸与 ADC 按键
│  ├─ modem/             ML307A AT 短信与通话控制
│  ├─ time/              NVS 时间基准与时区换算
│  ├─ usb/               USB Host、模组识别及 MSC 模式
│  ├─ parser/            TXT、EPUB、MOBI、FB2、HTML 与编码解析
│  ├─ renderer/          分页、文字、图片与封面缩略图
│  ├─ storage/           SD 卡和应用路径
│  ├─ ui/                字体、样式和位图图标
│  └─ wifi/              ESP-Hosted Wi-Fi 生命周期
├─ components/           定制 M5GFX、M5Unified、FreeType、USB Host 与 UAC2 Host
├─ managed_components/   项目使用并做过适配的 Espressif 组件
├─ Assets/               SVG 设计与图标源文件
├─ tools/                图标转换等开发工具
├─ sdkconfig.defaults
└─ partitions.csv
```

应用内部采用 `Activity`/`ActivityManager` 管理页面。更详细的源码模块说明见 [`main/README.md`](main/README.md)。
USB Audio Host 的架构、实时性和验证基线见 [`docs/USB_AUDIO_OUTPUT_DESIGN.md`](docs/USB_AUDIO_OUTPUT_DESIGN.md)。

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
