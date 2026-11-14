# MyPaperS31 共用 P4 主页与应用

日期：2026-10-24。用户已验证 S31 显示、灰阶、触摸、SD、FreeType/TXT 基础切片。
当前默认固件不再运行 BasicReader，而是直接进入 P4 的完整系统主页和 Activity 栈。
本次为**完整应用层接入**，不是所有 S31 外设驱动均完成。

## 共用范围

`main/ApplicationSources.cmake` 是 P4 / S31 共用源码清单，P4 main 组件直接包含，
S31 application 包含后仅替换板级和不兼容硬件后端；不复制 Home、Bookshelf 或 Reader。

已纳入 S31 编译、链接：

- 系统图标主页、阅读首页、书架、文件管理、设置和三语 UI / 字体设置。
- 原 TXT、EPUB、AZW3/MOBI、FB2、Markdown、DOCX、ZIP/CBZ 漫画解析与阅读 Activity。
- 原图片渲染、字体预览、简繁转换、横竖排、批注/脚注、预绘制、翻页动画、阅读进度/时长。
- 地图（在线/离线、搜索/路线）、DeepSeek、RSS、股票、计算器、微信读书。
- Wi-Fi 传输与热点传输；音乐/电台/短信/电话的原 Activity 界面。

这些是编译接入清单，不能替代实机逐项验收。特别是 S31 只有 16MiB PSRAM，
大图片、封面、缓存和网络同时工作时，可能早于 P4 触发内存预算限制。

## 刷新模式

所有 Activity 沿用 P4 原代码的刷新决策，不加 S31 强制 Text 或统一 Quality 覆盖。
例如主页普通刷新为 Text、书架为 HD、首次进入书籍为 Quality，图片页和周期清理规则同 P4。
Fastest / Fast / Text / HD / Quality 仍使用已有波形表；S31 保留8位RGB扫描，
像素时钟请求21.25MHz、340时钟/行，对齐P4的16us行周期及13.4us CKV高电平
（低电平2.6us）。由同一组宏生成RGB行长度和RMT周期，并校验脉宽不超过周期。
这是从20MHz/17us/16.3us调整的新测试配置，尚待实机验证，不代表其他总线时序
或VCOM与P4完全相同；VCOM保持现有配置，不在本次时序调整中修改。
S31 的 GC16/HD 已切到 16 槽环形流水线，首版固定预热 16 槽以保留余量。
每张 207936 字节，环形帧缓存共 3326976 字节（约 3.17MiB）；不再随
39 张等去重帧数量增长到约 8MiB。去重帧数不超过 16 时仍完整预生成。
DU4 暂保留完整预生成。S31 的并发生产吞吐需实机测试，重点检查 pipeline
underrun/incomplete；波形、扫描相数及总线时序未改变，欠载时仍中止而不延长波形。

驱动帧内存由 S31 显示初始化时一次预留 3326976 字节（16 槽）并常驻，
Quality/HD 环形缓存和 Text/DU4 完整缓存顺序共用，避免运行后 PSRAM 碎片化导致
总空闲大于 3.17MiB 却申请不到连续块。不另存一份 9 帧 DU4 工作区。
这会增加空闲时的常驻内存，但不增加正常 Quality/HD 刷新时的帧缓存峰值。
启动日志 `S31 shared phase arena ready`；失败日志同时输出总空闲和最大连续块。
超出 arena 容量的特殊路径仍保留原临时分配/安全中止逻辑，动画输出缓存不在此区。
此区属于面板/刷新任务生命周期，不借给读者预绘制或其他后台任务，不能在一次刷新后释放。

触摸映射：FT6336U 范围按竖屏 X=0..683、Y=0..1215 配置，
touch offset_rotation=1 抵消 panel offset_rotation=3，避免再交换/拉伸坐标轴。
menuconfig 的 `MyPaperS31 touch mapping` 提供 mounting transform 和原始 X/Y
独立镜像开关；默认两轴不镜像。此修正基于现有竖屏原始坐标配置，仍需实机四角确认。
这些选项只改变触摸，不改变显示扫描或屏幕方向。

## 板级与 IDF 6.1 边界

- S31 仍从专用 application 组件进入，不读取 P4 main/idf_component.yml，
  Component Manager 保持关闭，不升级或重解析共享 managed_components。
- 添加本地 FreeType、M5Unified S31 facade、已安装的 libjpeg-turbo；S31 与 P4 使用各自构建目录。
- S31 的 `M5.Display` 指向同一个真实 M5GFX/2D 面板实例；没有第二套显示缓冲或重复初始化。
  S31 facade 只提供应用实际用到的 Display、Button 类型和系统时间戳。
  不编译其他 M5 板卡的 PMIC、I2S 和 GPIO 初始化；`M5.Rtc` 在此 facade 中明确使用系统时钟，
  尚不读取 PCF8563。网络校时仍使用原网络时间服务。
- 原生 esp_wifi 复用既有连接、设置、常开/按需生命周期；C6 电源接口在 S31 上不操作 GPIO54，
  不初始化 ESP-Hosted。S31 使用 NT26 UART 以太网接入 4G，支持运行时切换 Wi-Fi / 4G 和内置 / 外置 SIM；见 [4G 数据通信](S31_CELLULAR_DATA.md)。
- IDF 6.1 / Mbed TLS 4 的 MD5、SHA256 改用 PSA；P4 保留原 Mbed TLS 调用。
- 字体 / Wi-Fi 上传缓冲在 S31 上使用对齐 PSRAM + VFS，SD 驱动负责必要的 DMA 中转。
  上传入口只要求该 stdio 缓冲有效且容量足够，不将 `dma_ok=0` 误判为失败；
  日志标记 `path=sdmmc-bounce`。P4 仍校验直接 DMA 兼容性（`path=direct-dma`）。
  S31 独立 Wi-Fi 传输 STA 启动后设为 WIFI_PS_NONE（驱动由该 Activity 独占并在退出时
  释放，不修改全局 Wi-Fi 常开/按需设置或 P4 hosted 策略）。上传打印首包、约 2 秒/
  每 MiB 进度、超时/失败时的字节数和网络/写卡耗时，连续无数据约 30 秒中止，
  不再无限重试接收超时。链路日志的 ps=0 表示省电关闭；phy 是协商模式而非实时速率。
  S31 Wi-Fi 上传跳过 SDK 当前未启用的 f_expand 连续预分配，使用原流式分配回退。
- S31 接收吞吐试验：静态RX=16、动态RX=64、RX BA窗口=32，TCP接收窗口=65535字节，
  TCP接收邮箱和TCP/IP邮箱均为64。普通联网与传输应用都使用这组sdkconfig参数；
  传输应用不再将S31覆盖为P4的8/32/6。P4保留原有运行时设置。
  保持TCP/IP优先级18、不绑核、100Hz tick及现有IRAM设置；启动日志打印实际RX参数和
  Wi-Fi初始化前后堆余量。已有 `build-s31/sdkconfig` 同步更新，不能只改defaults后假定生效。
- JPEG 编码配置改用零初始化再赋值，兼容 IDF 6.1 新增字段；不改变 P4 已指定参数。
- 默认 main 栈 32KiB，保留 FreeType 光栅池的栈预算；开启长文件名与 UTF-8 FAT API。
- S31 普通 malloc/new 的内部 RAM 优先阈值为 0，包含小字符串/HTML 节点在内
  均优先 PSRAM；1KiB 仍会使上千个 ZIP/目录节点积累占满内部 RAM。P4 不变。
  字体索引/字形元数据向量和 ZIP 索引节点、桶使用 AppAllocator 显式分配 PSRAM，
  不回退内部 SRAM；字符串内容遵循普通堆策略。分配失败遵循 STL 分配失败行为，
  并不保证 PSRAM 耗尽时还能打开大书。Wi-Fi/LwIP 启用
  SDK 的 PSRAM 优先分配，保留 64KiB 内部专用池给任务栈和 DMA 等用途。
  专用池不是绝对可用容量保证，显式 INTERNAL/DMA 申请仍会消耗它。
  BOARD 启动日志应输出 malloc_internal_max=0 / reserve_internal=65536。
  已有 build 目录须同步 sdkconfig 后 reconfigure，defaults 不覆盖已保存值。
  EPUB 在 ZIP/TOC 阶段记录 SRAM/DMA/PSRAM 余量，Wi-Fi 记录驱动初始化前后余量。
- SD 挂载允许 32 个文件描述符，与 P4 相同：最多 24 个 UI 主/回退字体句柄，
  另留 8 个给书籍、ZIP、封面等 I/O，避免沿用基础验证版的 24 个上限。
- 固件仍是 16MiB Flash / 200MHz Octal PSRAM，8MiB factory 应用分区，无 OTA。

## 尚未接入的硬件后端（不能当作已成功移植）

`main/boards/MyPaperS31/UnavailableHardware.cpp` 是显式边界，错误返回
ESP_ERR_NOT_SUPPORTED，日志前缀 `S31_HW`，不假装播放、发短信或拨号成功：

| 功能 | 当前状态 |
|---|---|
| BH1098G 扬声器音频 | 已接入本地音乐播放（MP3/FLAC/WAV/M4A/AAC/OGG/Opus），见下节；尚未实机验证 |
| 电台流 / 录音 / USB 耳机 | 网络收音机已接入（见 `S31_RADIO_PLAYBACK.md`）；USB UAC1/UAC2 已接入、待实机验证（见 `S31_USB_AUDIO.md`）；录音仍返回 NOT_SUPPORTED |
| 4G 数据 / 电话 / 短信 | NT26 UART 数据已接入；电话和短信未适配、入口隐藏 |
| 可选 GC2145 摄像头 | 已接入低频预览和手动拍摄定格，采集与 EPD 互斥；待实机验证，见 [摄像头与可选硬件](S31_CAMERA.md) |
| USB MSC / raw SD 导出 | 已实现重启隔离式虚拟 U 盘（TinyUSB 2.4.0 HS UTMI + ANA_SW GPIO36），详见 `S31_USB_MASS_STORAGE_DESIGN.md`；尚未实机验证 |
| 冷暖前光 | 已接入 GPIO6 冷光 / GPIO7 暖光，高电平有效 20kHz、10bit PWM；需实机确认 |
| PCF8563 / 电池计 | 已接入，见下文 RTC 与 BQ27220 说明 |
| 实体键 / 盖板触摸键 | 已接入采样、UI 操作、电源短按待机及长按关机；音量键保留不执行 |
| 充电控制 / 振动 | CX25601N 启动恢复已接入，见下文；振动仍待适配 |

S31 / IDF 6.1 使用 Picolibc。M5GFX 的 `DataWrapper.hpp` 必须自行包含 `stdio.h`，
并在 FILE 特化条件中识别 `__PICOLIBC__`；不能只依赖 Newlib 的 `_FSTDIO` 等宏。
否则默认文件封装会落到空实现：`open()` 返回成功而 `read()` 固定返回0，导致
普通 `fread` 能探测图片尺寸，但 `drawPngFile` 报头部读取不足，BMP/JPG文件路径也受影响。
`tools/test_lgfx_file_wrapper.cpp` 应使用两块板的实际 SDK 编译参数检查，避免仅用裸编译器
（默认 Newlib）误判通过；文件 `seek()` 以 `fseek(...) == 0` 为成功。

因此，这一版可测试完整界面、本地阅读、本地音乐外放、原生 Wi-Fi 网络应用和
重启隔离式 USB 虚拟 U 盘；基带等硬件功能仍需按 S31 接线单独适配，
不应靠测试 P4 驱动碰运气。扬声器播放尚未实机验证，见下节。

USB 虚拟 U 盘沿用现有 Transfer 页面与文案；与现已接入的 USB 耳机采用重启隔离的 Device / Host 互斥模式，因此“USB 耳机将断开”等提示保持原样。

### S31 扬声器音频（BH1098G）

`main/audio/audio_radio_s31.cpp` 是 S31 专用后端，S31 构建显式剔除 P4 的
`audio_radio.c`，但对上层保持 `audio_radio.h` 接口不变，音乐界面无需改动：

- 硬件路径：BH1098G 兼 DAC，I2S 采用 32bit 槽宽、立体声、左对齐，16bit PCM 放在
  高 16 位。PA_EN 为 PCA9535 P0.4，BT/PA 电源为 P1.4；PA 关闭状态下先上电，
  进入模式 1 后再配置 I2S，停止时先关 PA 并延时 120ms 排空。
- 控制：UART1 115200（GPIO39 TX / GPIO38 RX），上电等待 1200ms 让模组启动后发
  `AT+RX=2`（无回应则重发一次），700ms 后发 `AT+MODE=1`（模式 1 = 播放/对讲），
  响应打印为 `BT RX`。
- I2S 主从自动判定：参考工程假定 BH1098G 提供 WS/BCLK、ESP 作从机，但实机日志
  显示模组并未给出可用时钟（持续 `I2S write timeout`）。后端在模组进入模式 1 后，
  用内部弱上/下拉实测 BCLK(42)/WS(43) 的驱动方向：两条线都呈高阻时由 ESP 作
  **主机**输出 16kHz 时钟；只要任一条被模组驱动就保持从机，避免双主机争用。
  日志分别为 `I2S BCLK GPIO42 high-Z/driven by BH1098G` 与
  `ESP is I2S master at 16000 Hz`。
- 从机模式下的采样率：用 I2S RX 统计输入字节数并吸附到 8k/11.025k/12k/16k/
  22.05k/24k/32k/44.1k/48k 之一（误差超过 5% 或测不到时钟时回退 16kHz），再用
  `esp_ae_rate_cvt` 把音源重采样到该值。启动日志：
  `BH1098G master rate measured=... snapped=...`。主机模式下采样率固定 16kHz，
  不再测量。
- 解码：本地文件用 `esp_audio_simple_dec`（MP3/AAC/WAV/FLAC/OGG/Opus/M4A），
  经 `esp_ae_bit_cvt` / `esp_ae_ch_cvt` / `esp_ae_rate_cvt` 统一成 16bit 立体声，
  再扩成 32bit 槽写入 I2S；音量按 `(vol/100)^2` 衰减，NVS 键与 P4 相同
  （`audio_out/mode`、`spk_volume`，默认音量 30）。
- 后续状态：网络电台流已接入，USB 输出也已适配，分别见 `S31_RADIO_PLAYBACK.md` 和 `S31_USB_AUDIO.md`；录音（`audio_radio_record_wav`）仍返回 `ESP_ERR_NOT_SUPPORTED`。
- 依赖：新增托管组件 `espressif__esp_audio_codec`、`espressif__esp_audio_effects`、
  `espressif__gmf_fft`。gmf_fft 是 audio-effects 预编译库的链接依赖，在
  `set(COMPONENTS application)` 下必须显式写进 application 的 `REQUIRES`。
- 构建结果：`MyPaperS31.bin` 0x4946f0 字节，8MiB 应用分区剩余约 43%。

实机验证时先看 `AUDIO_S31` 的 BCLK/WS 方向日志：显示 high-Z 即 ESP 作主机；
显示 driven 则仍作从机，再看 `BT RX` 是否有模组回应、速率日志是否接近 16000。
若完全无声，检查 P0.4/P1.4 是否被其他 PCA9535 使用者（前光、按键）以非读-改-写
方式覆盖，并用示波器确认 BCLK(42)/WS(43)/DOUT(44) 是否有波形。

### S31 机械键与盖板键

本次只接入现有应用能力，不把未实现的功能映射成无关动作：

| 按键 | 短按 | 长按 |
|---|---|---|
| BOOT / GPIO61 | TXT、重排阅读器（含EPUB/MOBI/漫画/DOCX/微信正文）和RSS正文切换阅读设置 | 500ms识别但不执行AI动作，松手不补短按 |
| 音量+ / TCA P10（索引8） | 音量增加10%，最高100% | 500ms设为最大音量100%，松手不补短按 |
| 音量- / TCA P11（索引9） | 音量降低10%，最低0% | 500ms静音至0%，松手不补短按 |
| 电源 / TCA P17（索引15） | 切换待机壁纸后 light sleep，再按唤醒 | 长按3秒，保存阅读记录后输出P13硬件关机脉冲 |

音量键在主线程分发。音乐／收音机通过独立 `on_volume_key()` 响应，复用页面原有的音量设置和显示；其他页面及控制中心浮层使用系统音量接口，不能转成列表焦点或翻页动作。音量保存在 `audio_out/spk_volume`，范围0–100，0为静音。S31 设置读取采用一次加载，首次未播放时调整音量不会启动BT/I2S，也不会被随后初始化播放器覆盖。浮层遮挡期间调整音量不会绘制到底层页面，恢复后音乐提示值及收音机音量行根据实际音量更新。保留既有按键去抖、长按抑制松手短按、页面切换丢弃旧事件和待机后吞掉唤醒按键的规则。

待机壁纸在系统设置中选择，也可在文件管理的图片长按菜单中设置；选择“无”时保留当前画面。
阅读正文时待机优先使用阅读设置→美化中的“书封”；未设置、图片缺失、解码或缓存生成失败时回退到系统待机壁纸，两者均不可用则保留当前画面。TXT、本地重排格式和微信读书使用共享书封，RSS 正文使用独立的 RSS 书封；RSS 列表、Bing 壁纸及其他应用仍使用系统待机壁纸，不读取后台书籍的配置。阅读设置浮层仍属于阅读页面；独立的字体/图片选择页按普通页面处理。关机图片沿用系统待机壁纸逻辑。
书封沿用背景图片解码、裁切缩放和 4bpp 灰度缓存，使用独立的 `/sdcard/.reader/reader_book_cover.g4`，不改写阅读背景或系统壁纸配置；缓存校验包含图片路径、大小、修改时间和屏幕尺寸。解码后临时图像和灰度缓冲在待机前释放，仅保留用于唤醒恢复的原页面快照；唤醒后通过 Quality 刷新恢复原阅读页面。
PNG 书封在每次待机时先复制当前阅读画面到独立的 8 位灰度画布，再按原 PNG 的 alpha 进行融合，最终转为 4bpp 待机图：完全透明处保留正文，半透明处混合，完全不透明处覆盖。不清白底、不改变当前阅读 framebuffer；融合成功才提交 Quality 刷新。PNG 融合结果只存在本次操作的 PSRAM 中，不写入或复用 `.g4` 文件，因此不会显示上一次待机的正文；非 PNG 书封仍使用上述独立图片缓存。系统待机壁纸仍铺白底显示。
壁纸使用独立的 `standby_bg` NVS 配置及 `/sdcard/.reader/standby_wallpaper.g4` 缓存，
不改变阅读背景。支持 JPG/PNG/BMP，按屏幕裁切缩放，设置后释放临时图像缓存。

短按电源先等待显示和后台操作完成，再暂停按键采样及阅读计时、关闭 Wi-Fi 和前光，进入
保留 RAM 的 light sleep。传输服务运行、网络请求或校准进行中会推迟待机；15 秒仍未就绪则取消。
使用 TCA9555 的 GPIO2 中断唤醒并检查 P17，仅电源键唤醒界面，忽略其他共享中断。
有壁纸时保存原页面并在唤醒后用 Quality 恢复；无壁纸时不额外刷新。
前光关闭不修改 NVS 亮度，唤醒后恢复原亮度和色温，Wi-Fi 按原连接状态恢复。
SD 在待机时保持供电及挂载，避免破坏已打开的书籍和字体文件；待机不使用深睡眠，
实际待机电流、壁纸刷新及电源唤醒需在 S31 实机验证。

长按电源3秒触发硬件关机，松开后不再补发短按待机。先等待安全的显示/后台边界，
关闭网络、前光，关闭应用并执行现有析构保存流程（阅读位置和时长）；微信读书的
最终记录交给后台离线存盘，最多等10秒，仍未结束则取消断电并返回主页。
保存完成后，沿用原项目P13高/低各100ms、15次一组的脉冲；最多3组，每组间隔200ms。
日志仅显式刷新 `stdout` / `stderr`，不调用 `fflush(nullptr)`：当前Picolibc实现会
直接访问空FILE指针偏移0x14并触发Load access fault。阅读记录使用各自保存/关闭流程落盘。
只修改P13的输出锁存位和方向位，不覆盖同端口的触摸复位、蓝牙供电等引脚。
正常情况下设备在脉冲期间断电；若外部USB供电/硬件原因使其仍运行，或I2C操作失败，
停止重试、恢复前光和网络并返回主页，在 `S31_STANDBY` 日志中报告失败。
| 盖板Home | 正文优先返回/关闭阅读浮层；其他页面回系统主页 | 500ms返回系统主页（受忙状态/校准页保护） |
| 盖板Previous | 阅读上一页；首页上一屏；书架/文件/RSS列表上一项；其他页面按原有返回逻辑处理 | 500ms开始、350ms间隔连翻/导航；通用返回最多一次 |
| 盖板Next | 阅读下一页；首页下一屏；书架/文件/RSS列表下一项；无对应功能的页面不操作 | 与Previous相同，屏幕忙时不积压连翻 |

按原项目屏外FT坐标识别：Y=1600±30，Home X=100±30、Previous X=200±30、
Next X=300±30。在同一次FT采样中先识别盖板键，再调用现有屏内坐标转换；不更改
FT校准、镜像或旋转。盖板接触不会进入屏内触摸队列；跨键/滑入屏内取消本次接触。
Home长按、翻页长按结束后不再补短按。BOOT短按单独使用阅读设置入口，不复用
重排阅读器原来用于目录的EXTRA键，避免打开错误标签页。

机械键采用单独3072字节栈、优先级2、CPU0的采样任务，每20ms读TCA两个输入端口，
30ms消抖。BOOT低有效，启动时已按住的键吞到松开；TCA设置只将P10/P11/P17改为
输入；采样任务不写P13，不改变P12触摸复位、P14供电、P16 RTC及端口0的电源控制。
复用现有LGFX I2C锁，不安装第二个I2C驱动、不占用与RTC/加速度计共用的GPIO2中断。
读失败保留按下状态，不合成“松开/短按”；恢复后继续采样。

事件进入16项FreeRTOS队列，仅主线程调用Activity；屏幕忙时等待，盖板连翻最多
一个待处理事件。切换Activity递增generation，丢弃旧页面事件和当前长按；过期1.5s
事件不回放到新界面。阅读退出仍走原Activity析构和进度/时长保存，不调用重启、深睡
或硬断电。P4不编译S31按键驱动，原P4触摸路径不变。

纯状态机测试见 `tools/test_board_keys.cpp`（消抖、长按不补短按、跨键取消、屏外坐标、
连翻节流、millis回绕）；需实机验证键位和页面操作。没有新增UI提示字符串，字体子集
覆盖检查仍需执行。

### S31 冷暖前光

复用设置 → 前光页面，亮度 0..10（每档 5%，最高50%，0 为关闭），色温 0..10
（0 纯暖、5 中性、10 纯冷；其它值显示暖:冷比例），两项均即时生效。
色温是 PWM 混合比例而非标定的 Kelvin 数值；两个通道总占空比固定为设定亮度，
因此中间色温不像参考项目 Both 模式那样两路均以全亮度输出。
实际光通量仍会受冷暖灯珠效率影响，不能保证肉眼亮度绝对一致。

使用 LEDC 低速 timer0 / channel0（冷）和 channel1（暖），20kHz、10bit，
对应原项目 `periph/frontlight.cc` / `config.h` 的有效电平和引脚。
不使用 P4 GPIO24，也不写 P4 的 PCA9535 前光使能位。
输出为持续 PWM，无新增后台任务、渐变定时器或 SRAM 栈。
首次启动关闭，默认色温中性；NVS `frontlight_s31` 的 `level` / `cct` 恢复上次设置，
恢复默认设置会清除该命名空间。两板统一为5%一档、最高50%；S31冷暖两路
占空比之和最高为50%，保持色温混合比例。已有NVS档位无需迁移，输出减半。
P4保持原有单通道，不显示色温调节。

### PCF8563 日期与时间

启动时在已配置的 I2C1（SDA0/SCL1）初始化 PCF8563 @0x51，复用 M5Unified
PCF8563 驱动及现有 LGFX I2C 锁，不安装另一套 I2C 驱动。检查 VL、BCD 和完整
日历；不可信或早于2024年的时间不会覆盖系统时钟。RTC离线/无效不阻止系统启动。

RTC日历固定保存北京时间（UTC+8），系统保存UTC；读取时减8小时、写入时加8小时，
不依赖进程TZ设置。显示仍由“时间基准”和12/24小时选项控制，切换显示时区不会
改写RTC。网络校时成功后自动回写并回读验证；失败只警告，不撤销已成功的网络校时。

S31“设置 → 日期与时间 → 手动设置日期与时间”可离线输入 `YYYY/MM/DD HH:MM`，
按当前时间基准解释，秒归零。允许2024..2099（UTC不能早于2024-01-01），检查闰年、
月份天数及小时分钟，拒绝自动滚动无效日期。先写RTC并验证，成功后更新系统时间。
与网络校时互斥，忙或RTC写失败会明确提示；不会调用Wi-Fi或4G连接。
无需每秒读I2C，启动载入后的系统时钟持续计时，沿用主页/书架每分钟状态栏刷新。
本次不启用RTC闹钟、中断或唤醒，避免改变TCA P16及其他外设行为。

## 烧录与验证

### 共用 P4 连续扫描后端（新测试配置）

S31 的 Bus_EPD_S31 现在与 P4 一样调用 EpdScan2DContinuous 的初始化和逐相
扫描函数。不再创建 esp_lcd_rgb_panel，不再复制驱动帧到独立RGB缓冲。
两者共用GDMA挂载、LCD FIFO复位、STV/CKV启动顺序、VSYNC结束中断停止LCD、
等待RMT完成及逐相调用流程。P4关闭消隐区HSYNC、HSYNC位置为1；
S31保留原正常RGB驱动的消隐区HSYNC开启、HSYNC位置为0，并在每相启动前
复位LCD TX控制器，再复位FIFO。不能把P4芯片的寄存器配置原样套到S31：
DMA EOF只证明数据已传输，不证明LE锁存和行选通信号正确对齐。
S31的数据阶段按SDK的RGB实现启用DMA长度控制，P4保留原设置；这不是自动连续刷屏，
自动下一帧仍关闭，VSYNC中断仍停止单帧扫描。
DMA策略两板都启用owner_check/auto_update_desc，S31另外等待整帧DMA EOF。
仅有VSYNC而没有DMA EOF时拒绝本次扫描，不更新物理帧状态；此前每相52条描述符
保留DMA所有权的告警不能当作成功传输依据，也不能简单删掉告警掩盖问题。

S31仅保留必要适配：8位总线（304次传输/行）、IDF6 DMA接口、LCD传输缓冲供电，
以及160MHz源时钟分数分频得到精确21.25MHz。每行340个时钟，对齐16us/13.4us
CKV配置。初始化若计算出的像素时钟不精确则报错停止，不带误差继续驱动。
P4仍为16位总线（152次传输/行），原时钟分频及扫描设置保持不变。

S31的64字节对齐PSRAM驱动帧由LCD GDMA直接读取；普通PSRAM没有MALLOC_CAP_DMA
标志不代表外设不能访问。启动日志应包含 `EPD_S31: shared P4 HAL/LL scan ready`。
本改动不重调DU4/GC16波形、VCOM及板级PMIC配置；需验证灰阶、四灰阶残影、翻页
动画及测量后刷新，不把编译成功视为显示效果已通过验证。

### S31 OE 行带门控 A/B 诊断

入口：设置 → 墨水屏诊断 → S31 OE 门控对比（仅S31可见）。默认A组OE常开，
反复点中央切换中间黑白矩形，观察左右黑/灰参考块是否变淡。点击底部切换B组
（再次点击返回A组）；每次切换都先关闭门控，用Quality重建相同参考底图，计数归零。
推荐两组各点10～20次，比较相同次数后的左右参考块；中央是否完整变黑/变白也要检查。
上、下短黑条与刷新区处于相同物理行，作为OE不能单独保护物理列的对照。

B组将预留的第二个RMT通道接管GPIO18，与现有CKV通过IDF6同步管理器同时启动，
每相重置同步状态；不用逐行中断或任务定时翻转GPIO。固定竖屏684×1216、中间x=228..456，
对应对称的物理扫描行区间228..456。采用粗窗口，按CKV预留3行启动偏移并向两侧各扩8行，
这是保守隔离实验，不表示已完成OE/CKV/LE精确边界标定，更不能用于任意矩形局刷。
门控结束OE自动归低；传输/同步失败中止扫描，禁止将不完整结果认作有效物理帧。
出现OE test aborted需要重启后继续，不应靠切回A组恢复失败的扫描器。

退出测试等待当前刷新结束、释放临时RMT同步管理器、恢复GPIO及日常自动门控策略，并用
Quality重画诊断菜单。诊断A/B期间暂停日常自动策略，避免A组被自动改成门控。

实屏对比后，按要求将同一粗窗口方案默认接入S31日常局刷，以便观察真实UI效果。
开关为 `MYPAPER_EPD_S31_PARTIAL_OE_DEFAULT=1`。使用最终物理更新区域（含恢复全刷、
动画提升到全屏后的范围）选择行带；整行范围覆盖全屏或保护余量覆盖全屏时保持普通OE。
竖屏顶部状态栏等覆盖全部物理行的更新因此仍为原路径，不声称能保护同一物理行的其他列。
每批上电前申请OE同步资源，关电后释放并恢复GPIO所有权，避免影响VCOM测量。
OE通道和编码器与CKV在启动初始化的同一CPU上创建并长期保留，不逐批创建/销毁。
IDF6按RMT模块名绑定中断组：CKV在CPU0创建后，CPU1的扫描任务再创建OE通道会被
esp_intr_alloc_info拒绝（ESP_ERR_INVALID_STATE）。因此逐批只建立/删除同步管理器，
切换GPIO矩阵路由；不在扫描核上申请中断。启动日志显示OE与CKV初始化所在CPU。
资源申请失败回退普通空驱动扫描；实际传输失败仍拒绝提交物理状态，不以回退掩盖丢帧。
这一默认仍属实屏评估配置，未完成精确边界标定；保护余量和启动偏移与诊断B组一致。
P4不增加OE资源或改变控制行为；
DU4/GC16波形、VCOM与既有扫描时序不变。需实屏验证效果，不能以编译通过代替。

### BQ27220 只读检测细节

复用 M5Unified 的 BQ27220_Class 和 I2C_Class，新增 beginReadOnly /
readMeasurement 接口。S31 使用 I2C1 SDA0/SCL1、0x55、100kHz，复用 LGFX
总线锁；读 Voltage(0x08)、瞬时 Current(0x0C)，不调用原 begin() 的复位/解封流程，
电量计采样不写 CX25601N、设计容量或电量计数据存储；充电启动恢复单独说明如下。

M5.Power 提供电量、电压(mV)、电流(mA)及充电方向接口。按原项目的 3300..4330mV
线性估算电量，60次滑动平均、30秒单向1%步进，4330mV持续30秒后才允许满电，
满电保持阈值4280mV；电流 >5mA 为充电、<-5mA 为放电。此百分比是原项目的
电压估算，不是读取芯片 0x2C 的库仑计 SOC，也不能视为适配所有电池的精确标定。

主循环在触摸/EPD空闲时最多每秒采样一次，不新建任务、不主动刷新；失败后30秒
重试，缓存超过120秒显示未知。系统主页、书架、阅读首页的原有电池图标随状态栏
绘制填充电量，未知时显示短横线，仍不显示百分比文字，不增加独立屏幕刷新。
启动和每30秒日志标签 S31_BATTERY；读取失败不会伪装为0%或100%。

充电标记共用 `BatteryStatus.h` 的实心闪电绘制：状态栏在电池图标右侧显示，控制中心
在电量百分比左侧显示。不依赖字体字符，不新增 TTF 子集。以电量计有效净电流 >5mA
判定充电；即使估算百分比为100%，仍有净充电流时也显示。未知、充满待机或净放电
不显示，避免仅插 USB 就误报充电。状态栏保留每分钟同步刷新，同时把充电方向变化
加入刷新条件，不再等时间跳分钟才更新闪电标记。主页、阅读首页、书架、文件及公共
AppStatusBar 均覆盖；文件页补上原先缺失的状态栏更新。使用既有主循环与电量计缓存，
无闪烁动画或新增任务/定时器，显示忙碌/触摸交互时延后；控制中心仍每秒检查电量行。

### CX25601N 正常启动恢复

此前只有 BQ27220 电量检测及 USB 耳机 OTG 所有权代码，没有正常启动的充电初始化。
MCU 重启不会复位充电芯片；遗留 EN_OTG / EN_CHG=0 / HIZ 或 watchdog 配置
会跨固件切换保留，不能把插着 USB 等同于正在充电。

主系统在板级 I2C 就绪后、创建音频 Host 之前调用 `s31_charger_restore_at_boot()`；
普通应用和 MSC Device 启动均执行。模拟器使用同一 `ChargerState.h` 策略，自己的
原生 I2C 实现，不依赖主系统显示或 M5Unified。最多重试三次：先关闭并读回确认
REG18.EN_OTG=0，再将 REG16.EN_CHG=1、HIZ=0、WDT=0，读回验证。
不复位芯片、不解锁私有寄存器、不改电压/电流、温控、终止和安全定时器等参数。

启动日志 `S31_CHARGER` 打印恢复前后控制寄存器、VBUS/充电状态及保留的
VREG/ICHG/IINDPM；模拟器对应 `EMU_HW: charger recovered`。启用充电只是必要
条件，实际有无输入、是否截止/限流、温度保护及电池净充电需结合日志和
`S31_BATTERY` 电流判断，不能仅凭使能读回宣称已经充入电池。
正常运行不周期性强制写充电寄存器，避免与活动中的 USB Host OTG 所有权竞争。

原生测试 `tools/test_s31_charger.py` 覆盖全部 65536 种控制寄存器组合、其他限制字段
保持、重复恢复、I2C 失败及 OTG 关闭不生效时禁止启用充电；USB 音频测试仍验证
正常退出、失败回滚、外部 VBUS 拦截和重启遗留状态。还需实机验证净充电电流。

### Wi-Fi 批量传输 TCP 配置

S31 的默认配置和现有 build-s31 配置均对齐 P4：TCP 接收窗口 32768 字节、
接收邮箱 32 项、发送缓冲 16384 字节。替换此前 SDK 默认的 5760/6/5760。
只改变网络缓冲，不改变上传文件提交、SD 写入或 Wi-Fi 用户省电策略。
传输应用继续关闭 STA 省电；增大的接收窗口允许更多未确认数据在途，但是否
解决低速必须用相同路由器、发送端和文件实测，不预先承诺吞吐量。

### VCOM 回弹测量

S31 的 `Bus_EPD_S31::measureVCOM` 使用 I2C1 / TPS65185 地址 0x68，
TCA9555 Port0 的 P00/P01/P02 控制 VCOM_CTRL/PWRUP/WAKEUP。
入口由 Panel 等待刷新结束；关闭输出后唤醒 PMIC，保存 VCOM1/VCOM2/ENABLE，
测量面板控制时序对齐 P4：MODE/VCOM_CTRL 低、OE/WAKEUP 高，SPV 低时拉高
PWRUP，等待 1ms 后拉高 SPV 和 XSTL/SPH，再等待 20ms；写 ENABLE=0x3F，
等待 PG 有效，清 INT1，写 VCOM2=0xA0 同时开启 HiZ/ACQ，轮询 INT1.ACQC。
不再发送额外的 RGB/RMT 全零帧。XSTL/SPH 原由 RGB DE 驱动；在刷新完全结束后
保存完整 GPIO 输出矩阵路由，临时接管为高电平，结束时关闭 OE 后原样恢复路由。
测量结果按 VCOM[8:0] × 10mV 返回幅值；这是 kick-back 诊断值，不是万用表实测
工作电压，也不会自动将该结果设为新的 VCOM。

所有已开始的测量出口共用清理路径：PWRUP 拉低，恢复原 VCOM/ENABLE，恢复此前
供电状态及 MODE/OE/SPV、XSTL/SPH 外设路由。恢复通信失败时安全关断，不宣称成功。
绝不置位 PROG，不写 PMIC NVM。测量期间不改动正常波形及面板物理状态缓存。
日志包含 stage、INT1、PG、kickback、restored、was_on；需实机验证回弹测量值
及测量后刷新。阶段超时独立计时，总过程可能超过 UI 传入的 800ms。

流程依据 TI TPS65185x 数据手册 §8.3.7.1 / §8.6.5：
https://www.ti.com/lit/gpn/TPS65185

激活 IDF 6.1，执行 `scripts/build-board.ps1`，输出仍是 `build-s31/MyPaperS31.bin`。
默认烧录速度 460800。启动应看到：

```text
MyPaper: S31 full application: shared P4 home/activities/refresh policies
BOARD: MyPaperS31 ...
S31_SD: mounted /sdcard ...
```

之后进入原系统图标主页，而不是灰阶图或 S31 TXT 列表。

推荐验收顺序：主页/设置 → 文件管理/书架 → TXT/EPUB → 图片/漫画/竖排 →
预绘制/动画 → Wi-Fi 配网 → RSS/地图/DeepSeek/股票/微信读书 → Wi-Fi/热点传输。
遇到问题时保留从进入该应用开始的日志，尤其是内存、栈、图像和 EPD 状态。
不要在未验证前声称上述所有格式和网络服务均通过实机测试。

没有新增用户可见文案，复用现有本地化字符串与字体子集，不需要再生成 UI TTF。

## 本次构建验证

- S31 / IDF 6.1：完整构建退出码 0，固件 0x3BFDC0（3,931,584 字节），
  8MiB 应用分区剩余约 53%。
- P4 / IDF 5.3.5：回归构建退出码 0，固件 0x4C0A00，应用分区剩余约 41%。
- ELF 已检查包含主页、书架、文件管理、TXT/EPUB、地图、计算器、股票、
  DeepSeek、微信读书、RSS、音乐、电台、短信、电话、传输的入口。
- 未执行烧录或账号网络操作，实机结果待用户验证。

### Continuous frontlight controls

Brightness and colour temperature use continuous 0..1000 controls. The existing physical brightness cap remains 50% PWM; brightness is saved once after releasing the slider, rather than writing NVS on every movement. The global control centre shares these settings and displays battery percentage and available PSRAM. See CONTROL_CENTER.md.
