# `main/` 应用代码

此目录包含运行在 ESP32-P4 上的 MyPaperP4 应用。页面由 `ActivityManager` 管理，阅读器、音频、网络、解析、排版和持久化功能拆分为独立模块。

用户侧功能、硬件要求、构建烧录和 SD 卡说明请先阅读根目录 [`README.md`](../README.md)。

## 入口

- `main.cpp`
  - 初始化 M5Unified、自定义 EPD、SD 卡、触摸、ADC 按键、字体和 Activity 管理器。
  - 启动 `HomeActivity`。
- `CMakeLists.txt`
  - 注册应用源文件与 ESP-IDF 组件依赖。
- `Kconfig.projbuild`
  - Wi-Fi、音频、Codec 和引脚等构建配置。

## Activities

位于 `activities/`：

- `HomeActivity`
  - 主页面和最近阅读书架。
  - EPUB 封面缩略图显示。
- `FileBrowserActivity`
  - 浏览 SD 卡中的书籍、图片、字体和音频文件。
- `ReaderActivity`
  - TXT 阅读、分页、进度保存和最近阅读更新。
- `EpubReaderActivity`
  - EPUB spine/TOC 导航、正文分页、图片按需提取、封面缓存和进度保存。
- `ReaderSettingsActivity`
  - 字体、刷新模式、字号、行距、首行缩进、段间距、边距和图片开关。
- `MusicActivity`
  - 本地音乐扫描、列表、播放控制、进度、音量和同步歌词。
- `RadioActivity`
  - HLS 网络电台播放、切台、顺序和随机模式。
- `RssReaderActivity`
  - RSS 源、文章列表、正文分页、图片缓存和 Bing 壁纸。
- `WifiTransferActivity`
  - STA/AP 网络初始化、HTTP 上传和文件自动分类。
- `ImageViewerActivity`
  - JPG、PNG、BMP 浏览和同目录图片切换。

## Audio

位于 `audio/`：

- `audio_radio.*`
  - Espressif GMF/simple player 管线。
  - 本地文件与 HLS 播放。
  - I²S/ES8311 输出格式切换、音量、播放状态和时间统计。
- `audio_file_formats.*`
  - 应用统一支持的音频扩展名。
- `embedded_lyrics.*`
  - ID3、Vorbis Comment、MP4 歌词和外置 LRC 解析。
- `m5_i2c_codec_ctrl.*`
  - 将 ES8311 寄存器访问统一接入 M5Unified 的内部 I²C 所有权模型。

## Parser

位于 `parser/`：

- `txt_parser.*`
  - TXT 编码检测、分页读取与进度。
- `gbk_decode.*`
  - TXT 与 LRC 共用的 GBK 到 Unicode 映射。
- `epub_zip.*`
  - ZIP central directory、条目读取/提取和持久化索引缓存。
- `epub_parser.*`
  - container/OPF、manifest、spine、TOC、标题与封面解析。
- `epub_html.*`
  - XHTML 文本块、标题、换行、分隔线、分页符和图片块解析。

## Renderer

位于 `renderer/`：

- `DocumentLayout.*`
  - TXT、EPUB 和 RSS 共用的文档分页结构。
- `TextLayout.*`
  - CJK 断行、段落缩进、段间距和页面规则。
- `TextRenderer.*`
  - FreeType/位图字体测量、glyph 缓存、排版与绘制。
- `CoverThumbnail.*`
  - 最近阅读封面缩略图缓存。
- `P4JpegDecoder.*`
  - ESP32-P4 JPEG 硬件解码路径。
- `TurboJpegDecoder.*`
  - Baseline/Progressive JPEG 软件解码和内存预算控制。

## 状态与存储

- `storage/app_paths.*`
  - `/sdcard/.reader` 下的统一数据路径。
- `activities/recent_books.*`
  - 最近阅读列表、标题、类型和封面路径。
- `ui/reader_style.*`
  - 阅读排版设置的加载、保存与布局签名。
- `ui/reader_font.*`
  - 当前阅读字体持久化。

## 输入

- `input/touch_input.*`
  - 点击、长按、滑动、GT911 硬件手势和边缘返回。
- `input/adc_keys.*`
  - GPIO1/ADC1_CH0 上的五键电阻梯形键盘。

## 构建

当前目标为 ESP32-P4，验证环境为 ESP-IDF 5.3.5：

```bash
idf.py set-target esp32p4
idf.py build
```

项目依赖和编译源列表以 `main/CMakeLists.txt`、`main/idf_component.yml`、根目录 `dependencies.lock` 为准。
