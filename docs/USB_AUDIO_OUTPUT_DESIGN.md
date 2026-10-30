# ESP32-P4 USB Audio Host 技术记录

状态：UAC 1.0 与 UAC 2.0 播放均已完成真机验证（2026-09-13）。

本文记录 MyPaperP4 在 ESP32-P4 High-Speed USB Host 端口上驱动 USB 耳机、USB DAC 的最终实现。它既是当前实现说明，也是后续排查音频卡顿、热插拔和 EPD 并发问题的基线。

## 1. 功能范围

- 保留 ES8311 + I2S + PA 扬声器输出。
- 支持 UAC 1.0 PCM Playback。
- 支持 UAC 2.0 PCM Type-I Playback。
- Music 与 Radio 共用 Speaker、USB、Auto 三种输出模式。
- 支持运行时格式协商、音量调节、热插拔和播放中的输出切换。
- UAC2 优先于 UAC1 claim 同一物理设备。
- 当前只实现播放输出，不把录音输入纳入产品功能。
- UAC2 当前支持同步 OUT 端点；需要显式 feedback 的异步设备仍应返回不支持，不能假装启动成功。

首个 UAC2 验证设备：CX31993+97220HIFI，VID:PID `06CB:1594`，USB 2.0 High-Speed。

## 2. 代码结构

| 文件 | 作用 |
| --- | --- |
| `main/audio/audio_radio.c` | GMF 解码管线、统一 PCM 出口、扬声器/USB 路由、格式重定向与热插拔策略 |
| `main/audio/usb_audio_output.c` | UAC1/UAC2 统一门面、设备状态、格式选择、音量和生命周期 |
| `main/audio/usb_audio_output.h` | USB Audio 对应用层暴露的接口 |
| `components/uac2_host/` | 项目内 UAC2 Host 类驱动、描述符解析、Clock/Feature Unit 控制和等时传输 |
| `components/usb/` | 项目内 USB Host/HCD 实现及 IRAM 链接配置 |
| `main/activities/MusicActivity.cpp` | 本地音乐 UI 与输出切换入口 |
| `main/activities/RadioActivity.cpp` | 网络收音机异步切台、状态机和输出切换入口 |

数据通路如下：

```text
本地文件 / HLS
    -> GMF decoder
    -> rate converter
    -> channel converter
    -> bit converter
    -> audio_radio_write_cb()
         -> ES8311 / I2S
         -> UsbAudioOutput
              -> UAC1 Host
              -> UAC2 Host
```

`audio_radio_write_cb()` 是唯一 PCM 出口。播放业务不直接操作 UAC 驱动，避免 Music、Radio 各自维护一套生命周期。

## 3. 设备枚举与后端选择

USB Host Library 只安装一次，UAC1 和 UAC2 client 共用它。

1. UAC1 使用 Espressif `usb_host_uac` 组件。
2. UAC2 使用项目内 `components/uac2_host`。
3. 设备枚举后分别解析 AudioStreaming playback interface。
4. UAC2 能正确打开时优先选择 UAC2；否则保留可用的 UAC1 后端。
5. `usb_audio_output_is_connected()` 使用原子快照，UI 查询连接状态时不等待 PCM 写路径持有的互斥锁。

UAC2 描述符解析保留以下信息：

- AudioControl 与 AudioStreaming interface 编号；
- alternate setting；
- PCM channel 数；
- `valid bits` 与 `subslot bytes`；
- OUT endpoint、MPS 和 `bInterval`；
- Clock Source entity；
- Feature Unit mute/volume 能力。

CX31993 实测提供双声道 16、24、32-bit 三个播放 alternate setting，并声明 8 kHz 到 384 kHz 的离散采样率。HS `bInterval=1` 表示每 125 µs 服务一次等时端点。

## 4. 格式协商

协商输入来自解码器实际输出的：

```text
{sample_rate, channels, valid_bits}
```

选择顺序：

1. 优先保持音源采样率、声道数和位深完全不变。
2. 不支持源位深时，在 32、24、16-bit 中选择设备支持的格式。
3. 不支持源采样率时，选择设备声明范围内最接近的采样率。
4. 最后才进行声道转换。

扬声器路径仍以 48 kHz 为上限；USB 路径没有人为设置 48 kHz 上限。只要外置 DAC 声明支持，高于 48 kHz 的音源可以按原采样率送出。

`valid_bits` 和 `subslot_bytes` 必须区分：例如 packed 24-bit 是每个采样三个小端字节。只有 GMF 输出容器与 USB alternate setting 不一致时才需要符号扩展或重新封装；格式一致时直接传输，不能无条件扩成 32-bit。

相邻曲目或切台后的目标格式完全相同时，不重复配置 USB alternate setting 和采样率。

## 5. UAC2 等时传输

44.1 kHz 无法在 8000 次/秒的 HS service interval 中使用固定帧数，因此使用分数累加器生成 5/6 个 sample frame 交替的数据包：

```text
accumulator += sample_rate
frames = accumulator / 8000
accumulator %= 8000
packet_bytes = frames * channels * subslot_bytes
```

实现要点：

- 多个 URB 预先分配并循环提交。
- PCM 首先进入 ring buffer，达到预充阈值后才启动等时输出。
- 回调只完成取数、静音填充、状态更新和快速重提交，不执行 UI、日志风暴或设备销毁。
- PCM 不足时填静音，保持同步端点连续运行。
- stop 等待在途 URB 有界回收；disconnect 的 close/release 放到非回调任务执行。
- 所有控制传输、停止和卸载均有超时或可退出路径，避免离开 Music/Radio 时永久卡死。

当前缓冲参数：

| 后端 | PCM 缓冲 | 启动阈值 |
| --- | ---: | ---: |
| UAC1 | 32 KiB | 8 KiB |
| UAC2 | 64 KiB | 32 KiB |

UAC2 缓冲位于内部 RAM，目的是避免 EPD 大量读取 PSRAM 时与 125 µs USB 周期争用外部存储器总线。

## 6. 实时性、IRAM 与任务优先级

EPD 2D 刷新会连续访问 PSRAM，并进行 cache writeback。早期实现因此出现 `TX URB packets skipped` 和可闻卡顿。最终处理包括：

- USB Host 中断使用 `ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM`。
- 项目内 `components/usb/linker.lf` 将 `libusb.a` 和 DWC HAL 的关键执行代码放入 IRAM，Flash/PSRAM cache 暂停时仍能继续服务 USB。
- HCD 等时提交、描述符处理关键路径标记为 `IRAM_ATTR`。
- EPD DU4 波形生成分块访问 PSRAM，周期性释放共享总线。
- 高频传输路径不打印逐包日志；异常日志采用聚合或限频方式。

当前 Core 0 的关键任务优先级：

| 任务 | 优先级 | 说明 |
| --- | ---: | --- |
| UAC2 client/event | 20 | 125 µs 等时传输实时任务 |
| UAC1 class event | 14 | UAC1 设备事件与传输服务 |
| USB Host library | 13 | Host/HCD 事件处理 |
| Radio 切台 worker | 11 | 有限时长地停止旧 HLS 并启动新流 |
| Radio UI / HLS HTTP | 10 | 同级时间片，防止 HTTP 让触摸任务饥饿 |
| HLS 组织与供数 | 9 | 保证解码端持续获得媒体数据 |
| GMF player | 5 | 解码与 PCM 管线 |

Core 1 保留给 EPD 刷新/扫描任务。USB、网络和音频任务不应迁移到 Core 1。

这里有两个已经验证过的反例：

- Radio UI 低于 HLS HTTP 优先级时，声音继续播放但触摸和页面状态完全不响应。
- HLS 供数任务降到播放器以下时，UAC1/UAC2 都可能因 PCM 供数不及时而卡顿。

## 7. 音量

UI 始终使用 0 到 100。

- UAC2 优先解析 Feature Unit，并读取左右声道的 `GET_RANGE(VOLUME_CONTROL)`。
- CX31993 实测 channel 1/2：`min=-18944`、`max=0`、`res=128`，单位为 1/256 dB。
- 硬件音量可用时，将 UI 音量映射到设备范围，按 `res` 对齐后分别写左右声道。
- 设置非零音量时先写衰减再解除 mute，避免连接瞬间全音量爆音。
- 控制请求失败或设备未提供可靠 Feature Unit 时，当前设备降级为 PCM 软件增益，后续不反复发送失败请求。
- UAC1 实测设备的硬件音量请求兼容性不稳定，因此保持软件音量路径。

当前 `CONFIG_MYPAPER_USB_AUDIO_MAX_VOLUME=5`：UI 100 对应 PCM 满幅的 5%。硬件音量与软件音量互斥，避免双重衰减。

## 8. 热插拔与输出切换

输出切换顺序：

```text
暂停播放 / 门控 PCM 写入
    -> 停止旧 sink
    -> 根据新设备重新协商 GMF rate/channel/bit
    -> 启动新 sink
    -> 恢复播放
```

拔出当前 USB 设备时不在驱动回调中销毁播放器。回调只投递事件，业务任务负责暂停、回退或关闭：

- `AUTO`：USB 消失后可回退扬声器，重新接入后可重新认领 USB。
- `USB`：设备消失时保持可恢复状态，等待重新接入。
- `SPEAKER`：USB 热插拔不改变当前扬声器播放。

停止播放前先设置 output write gate，使残余 GMF 回调只消费数据而不再进入已停止的 USB sink。销毁顺序为 player stop/destroy、UAC stream stop、device close、client uninstall、Host library uninstall；Host 卸载前必须继续排空 pending flags。

## 9. Radio 异步切台状态机

HLS 的 stop/destroy/run 不能放在 UI 触摸回调中，否则网络超时会直接冻结页面。Radio 使用独立 PSRAM 栈 worker 执行切台，UI 只轮询结果队列。

每次切台带单调递增的 `request_id`：

- 连续点击时只保留最后一次目标。
- 旧 worker 完成只能更新“物理管线当前实际播放对象”，不能提交最终 UI 状态。
- 只有 worker 已结束、`current == switch_target` 且新流 `playback_ready` 时，才将 `Switching` 改为 `Playing`。
- 标题、电台名称和实际播放流都以最终完成的请求为准。

这避免了旧流的 `playback_ready` 误判为新流就绪，以及标题电台和实际声音不一致的问题。

## 10. 配置项

相关 Kconfig：

- `CONFIG_MYPAPER_USB_AUDIO`：启用 USB Audio Host。
- `CONFIG_MYPAPER_USB_AUDIO_SOFTWARE_VOLUME`：保留软件音量兜底配置。
- `CONFIG_MYPAPER_USB_AUDIO_MAX_VOLUME`：USB 最大有效音量百分比，当前默认 5。

扬声器的采样率上限和初始音量仍由 `CONFIG_AUDIO_RADIO_SAMPLE_RATE`、`CONFIG_AUDIO_RADIO_OUTPUT_VOLUME` 管理。

## 11. 验证基线

已经完成的真机验证：

- UAC1 耳机可以枚举、播放、调音量、切换输出和退出页面。
- CX31993 UAC2 可以枚举并读取 Clock Source、16/24/32-bit alternate setting 与 Feature Unit。
- 44.1 kHz 双声道音源可按 44.1 kHz 输出，不强制转换成 48 kHz。
- UAC2 在 EPD DU4 刷新期间不再出现持续 URB skip 或可闻卡顿。
- UAC1 在 HLS + Radio UI + EPD 并行时不再卡顿。
- Radio 播放期间触摸保持响应。
- 连续切台最终标题、Playing 电台和实际流一致。
- 拔出、重新接入、停止以及退出 Music/Radio 均不会永久阻塞。

后续修改 USB、EPD、HLS 或任务优先级时，必须至少回归以下组合：

1. UAC1 本地 FLAC + 连续 EPD Text 刷新；
2. UAC1 HLS + 切台 + 触摸；
3. UAC2 44.1 kHz + 连续 EPD Text 刷新；
4. UAC2 高采样率/24-bit 文件；
5. UAC1/UAC2 播放中拔出、重插和退出页面；
6. Speaker、USB、Auto 三种模式互相切换。

诊断时重点关注：

- `USB_AUDIO: UAC1/UAC2 stream started` 的最终格式；
- `uac2-host: TX PCM primed`；
- `TX URB ... packets skipped` 是否只为偶发一次还是持续出现；
- Radio worker 的 `request/latest` 是否一致；
- EPD 刷新期间是否同时出现 USB gap、HLS underrun 或 UI 饥饿。


## S31 / IDF 6.1 接入

S31 复用本文件描述的 UAC1/UAC2 facade 与驱动。Type-C 路由、OTG 供电、PCM 末端转换和实机验证范围见 [S31_USB_AUDIO.md](S31_USB_AUDIO.md)。P4 保持原 IDF 5.3.5 Host 覆盖组件和缓冲参数。
