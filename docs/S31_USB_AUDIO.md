# MyPaperS31 Type-C 数字耳机

## 当前实现

音乐和网络收音机共享 S31 的 USB 输出，复用 P4 的 UAC1 / UAC2 枚举、设备能力查询、流控制和音量接口。UAC2 基本播放和正常退出已由实机日志及听音验证；三字节 24-bit 扩展仍需对应设备实测。这里只支持带 USB DAC 的数字耳机，不支持被动模拟 Type-C 转接头。

音乐/收音机页右上角原有输出按钮保持 `SPK → USB → AUTO`：

- SPK：外放，释放音频 USB Host 与 OTG 供电，恢复电脑/充电使用的数据路由。
- USB：要求有可用耳机；没有耳机时播放请求返回失败。
- AUTO：有可用耳机时切 USB，否则用外放。正在使用的耳机拔出时先暂停；重新插入后恢复。拔出后用户主动点击播放可转外放。

选择 USB 或 AUTO 后，进入音乐列表即可枚举，不必先播放。纯外放模式不接管共享的 Type-C 端口。退出应用释放 Host、UAC 设备和供电。S31 音乐/收音机进入待机前停止音频并释放 USB，释放失败则取消待机，避免 DMA 带入 light sleep。退出后再进入会重新初始化。

## 板级连接与供电

`main/boards/MyPaperS31/UsbAudioPort.cpp` 管理 Host 的板级资源：

1. 读取 CX25601N（I²C 0x6b）0x1e 的 VBUS 状态及 OTG 开关，已有外部电源或状态含糊时拒绝启动。芯片明确报告 VBUS_type=7 且 EN_OTG=1 时，可接管主控重启后遗留的 OTG 状态；正常应用中只有音频模块控制 OTG，USB U盘是独立 Device 启动模式。该恢复会在退出时关闭 OTG 并恢复充电开启。
2. 保存 0x0a～0x0d、0x16、0x18 相关配置；按参考项目的 OTG 流程停充电、退出 HIZ、关闭充电芯片 watchdog，设置 5040 mV / 1000 mA，然后开启 EN_OTG，等待 30 ms。
3. GPIO36 `ANA_SW` 拉低，将 Type-C 数据切到 HS UTMI，安装 Host。
4. 关闭时先完整停止 UAC / Host，再关闭 OTG，恢复改动的充电字段及 GPIO36 高电平。寄存器失败执行回滚，恢复失败保留所有权以便重试，待机检查也会拒绝放行。

配置不覆盖充电电压、充电电流等其他设置。1000 mA 是参考项目的 OTG 限流配置，不表示耳机实际消耗 1000 mA。

USB U盘仍是独立重启模式，运行 TinyUSB Device，不启动音频 Host。测试耳机时先拔掉连接电脑/充电器的 Type-C 线，再连接数字耳机；已有外部 VBUS 会被上述检查拦截。

## USB 组件与 P4 隔离

S31 使用 `third_party/s31/usb` 固定的 Espressif USB Host **1.6.0**，兼容 IDF 6.1。IDF 6.x 已将 Host 从 SDK 移到单独组件，不能使用本仓库针对 IDF 5.3.5 的 `components/usb` 覆盖版本。

- 官方组件：[USB Host 1.6.0](https://components.espressif.com/components/espressif/usb/versions/1.6.0/readme)
- 上游提交：`630f1c4e61f71d62bb57ff1b840931da24c3329f`
- 下载 zip SHA256：`8c79125c1eb7ea94190470922d47f5b741c20223947ec782d9e429e8a750fc41`
- P4 仍使用原来的 IDF 5.3.5 Host 覆盖代码；自定义 HCD 时序诊断仅在 P4 编译。
- S31 Host 配置描述符控制缓冲为 1024 字节，避免默认 256 字节读不完整的复合耳机描述符。
- S31 UAC2 PCM 储备为内部 RAM 32 KiB，预填阈值 16 KiB；P4 仍为 64 / 32 KiB。facade 的 Host/UAC1 服务任务使用 PSRAM 栈，UAC2 驱动事件任务保留内部 RAM 栈以满足等时传输时限。

## PCM 与音量

本地解码和网络 GMF 均接入同一个输出选择器，在完整 PCM 块边界串行切换，转换器和 USB start/stop/write 共用互斥屏障。

- 本地音乐：按耳机支持的位深转换为 stereo，再按耳机声明的采样率输出；优先 48 kHz，最高 48 kHz。识别到 24/32-bit 源时优先选择相同位深，不再固定先降到 16-bit。外放仍使用 16-bit。
- 收音机：GMF 保留原来的 16-bit stereo 输出，末端转换器按 USB 选定的采样率重采样，不在热插拔时修改正在运行的 GMF 图。
- USB 使用独立的小块打包，保留左右声道，不使用外放的单声道复制和 I²S 32-bit 槽格式。支持 mono/stereo 的 16/24/32-bit，其中 24-bit 每采样三个字节；16-bit 音源扩展到 24/32-bit 不会增加原始精度。仅提供 24-bit 三字节格式的耳机也可选择。24-bit 有效位装在四字节容器中的格式仍不选择；能力查询和实际启动选择使用相同的 subslot 约束。
- UAC2 仍沿用现有驱动的传输类型限制，异步反馈 DAC 的完整兼容性不在本次保证范围内。
- 外放与 USB 音量分别保存在 `audio_out/spk_volume` 和 `audio_out/usb_volume`。USB 默认 20，沿用 P4 的 5% 最大幅度配置及 0～100 UI 刻度，优先硬件音量，失败时软件增益兜底。
- 已播放时间按每块实际输出采样率累加，切换到不同速率的输出不会导致进度时间跳变。

## 实机验证

烧录 `build-s31/MyPaperS31.bin` 后：

1. 拔掉 Type-C 电脑/充电连接，进入音乐应用，接入数字耳机，将输出切到 USB 或 AUTO。检查 `S31_USB_AUDIO: Type-C HS Host route selected` 和 `USB_AUDIO` 的设备/格式日志。
2. 将右上角输出切到 USB 或 AUTO，播放同一首歌。应看到 `AUDIO_S31: output USB: ...`，外放关闭，耳机有声、左右声道正常。
3. 调整页面音量和物理音量键，暂停/继续，切歌；检查总时长和已播放时间正常。
4. 播放时拔出耳机，应暂停；重新插入应恢复。拔出后手动继续应走外放；重新插入 AUTO 应切回 USB。
5. 网络收音机重复以上检查，再测试退出、待机/唤醒、重新进入及 USB U盘模式。

若失败，保留从进入音乐应用起的 `S31_USB_AUDIO`、`USB_AUDIO`、`uac2_host` 和 `AUDIO_S31` 日志。电源拒绝、设备未枚举、无支持格式和 PCM 提交失败是不同阶段，不需要先调 BH1098G 的时钟。

原生测试：`tools/test_s31_usb_audio.py` 直接提取实际板级实现和输出选择器，测试充电配置保存/恢复、外部 VBUS 拦截、每一步写失败回滚、重复申请、输出选择以及 stereo/mono 16/24/32-bit 字节布局、符号扩展、增益和低位保留。

## Type-C 被耳机占用时取日志

调试期间用过 PSRAM 环形捕获、退出后保存 `s31-usb-audio.log` 的临时方案。基本播放验证后已按要求移除捕获钩子、128 KiB 缓存及 SD 写入代码；已有日志文件保留，固件不再更新它。普通串口日志仍保留。

需要实时串口时，可使用板上引出的 UART0：GPIO58 为 TX、GPIO59 为 RX、115200 8N1。只收日志需接设备 TX 到 3.3V USB-TTL 的 RX，并共地，不接适配器电源；具体测试点是否引出须按实物确认。

## 首次日志中的启动失败与修正

2026-10-30 的 `tmp/s31-usb-audio.log` 显示，每次失败都发生于：

```text
S31_USB_AUDIO: Type-C HS Host route selected, VBUS OTG 5040 mV / 1000 mA
HCD DWC: Interrupt alloc error: ESP_ERR_INVALID_ARG
USB HOST: Hub driver Install error: ESP_ERR_INVALID_ARG
```

这证明代码已走到路由/OTG 配置之后，尚未开始耳机枚举；配置日志本身不代表已测得 VBUS 实际电压。此时还有约 140～180 KiB 内部空闲内存，返回值也是 INVALID_ARG，不能归因为内存不足或耳机不兼容。

根因是照搬 P4 的 `ESP_INTR_FLAG_IRAM`：P4 有 `components/usb/linker.lf` 将 Host/HCD 映射到 noflash；S31 使用的官方 Host 1.6.0 没有该映射，链接文件中 `intr_hdlr_main` 位于 Flash（本次为 `0x401a1482`）。IDF 6.1 的 `esp_intr_alloc` 会拒绝此类 IRAM 标志与实际入口地址不匹配的请求。S31 改用 LEVEL3，P4 继续使用 LEVEL3 | IRAM，不通过仅给入口加 IRAM_ATTR 的方式掩盖调用链问题。

同时将以 `xTaskCreatePinnedToCoreWithCaps` 创建的两个 facade 任务的三个退出分支改为 `vTaskDeleteWithCaps`，回收栈及 TCB；原日志中每次失败少掉约 5 KiB PSRAM 与 Host 任务栈泄漏吻合。UAC2 内部任务由普通创建接口创建，保留普通删除接口。Host 启动错误也传回原始值，不再统一改写为 ESP_FAIL。

新增 `tools/test_usb_host_config.py`，用生产配置分别编译 S31/P4，检查两种 ISR 放置约束。修正后应先看到 `USB Host installed on default HS peripheral`，再根据耳机的 UAC 枚举及 PCM 日志继续验证；尚不能将编译通过等同于耳机播放成功。
