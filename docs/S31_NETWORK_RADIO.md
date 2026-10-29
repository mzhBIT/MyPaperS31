# S31 网络收音机外放

网络收音机继续使用共享 `RadioActivity` 的电台列表和交互。
S31 的 `audio_radio_play()` 接入与 P4 相同的 GMF simple player 和 HLS IO：

```text
HTTP/HTTPS MP3 或 HLS(m3u8/TS/AAC)
  → GMF 网络读取、分段处理、解码
  → 固定采样率转换 + 双声道 + 16 bit
  → 已验证的 S31 write_pcm（音量、32 bit slot 展开）
  → I2S Slave → BH1098G → NS4150B
```

BH1098G 提供 BCLK/WS，输出采样率沿用当前测得的硬件速率（通常 16 kHz）。
网络解码器不能按电台源采样率重配 I2S，也不能使用 P4 的“只限制最高输出采样率”策略。
低于硬件速率的源也必须升采样到相同输出速率。

## 生命周期和资源

- 网络播放器按需创建；本地音乐仍使用已有 S31 文件解码路径。
- 切台、切换本地文件、退出时关闭 PCM 输出，再停止并销毁网络播放器及 HLS IO。
- 停止失败时保留播放器，避免仍有网络任务引用已释放的输出设备；下次停止可重试。
- 网络与本地解码器的全局注册表在停止后移交所有权，避免 ASP 销毁时注销本地音乐所需的解码器。
- GMF 解码任务优先级 5、8 KiB PSRAM 栈、CPU0；HLS 任务优先级 9、20 KiB PSRAM 栈、
  CPU0、128 KiB 缓冲。沿用共享收音机页面的 UI 调度策略。
- `radio_play` 启动任务使用 8 KiB 内部 SRAM 栈，调用音频初始化/NVS 读取时不能使用
  PSRAM 栈。任务用 `xTaskCreatePinnedToCoreWithCaps` 创建，并以 `vTaskDeleteWithCaps` 退出。
  GMF/HTTP/HLS 流任务不承担音频设置的 NVS 读取。
- HTTP IO 显式绑定系统证书包以支持 HTTPS；直播不写 SD，不查询文件总时长。
- S31 网络播放期间保存当前 Wi-Fi 省电模式并临时设为 `WIFI_PS_NONE`，停止并销毁网络
  播放器后恢复。只改变收音机播放期间的策略，不写 NVS。
- `radio link start/error` 记录 STA 连接结果、RSSI、信道、实际协议掩码、省电状态、IP 和网关。
  HTTP 地址的连接失败也会由 ESP-TLS 传输层报错，不能仅凭日志标签判断为证书失败。
- `radio default route`/`radio DNS server` 记录默认接口和 DNS；`radio resolve`/`radio endpoint`
  在每次选台的首个 HTTP 请求前记录一次域名解析结果及 SDK 默认首选地址。
  解析使用与 ESP-TLS 相同的 `AF_UNSPEC + SOCK_STREAM`，不修改 URL、Host 或 SNI。
- 暂停/继续通过 GMF 操作，音量仍走 S31 已有软件音量。
- 播放就绪需已向 I2S 送出 PCM，不能仅凭成功创建播放器就清除加载提示。

S31 禁用 Component Manager，因此根 CMake 显式加入本地已有的 GMF/HLS/extractor/SAL
组件，并补齐通常来自组件清单的依赖关系。没有修改 managed_components 源码。
没有新增用户可见字符串，不需要更新 UI 字体子集。

## 实机验收

1. 选“中国之声”检查 HLS 播放，日志应出现 `radio HLS audio format`、
   `radio PCM target: 16000 Hz stereo 16 bit -> BH1098G`。
2. 选“上海流行音乐”检查 MP3 直播。
3. 验证暂停/继续、音量、连续切台和退出；退出后打开本地 MP3/FLAC，验证解码器注册表恢复。
4. 检查首次网络连接失败时的错误提示，以及再次选台的恢复。

构建通过不能代替电台可用性和设备实际出声验证；当前联网速度也可能影响直播缓冲。

## 按需省电模式的 Wi-Fi 生命周期

实机对照出现“始终开启能播放，切回省电模式不能建立 TCP”的现象。设置中的省电模式
控制应用退出后的 Wi-Fi 启停，与 `WIFI_PS_NONE` 这种无线休眠配置不同。

S31 日常应用释放连接及设置切回省电时，改为沿用原项目 `WifiStation::PauseForLp()` 的
生命周期：先抑制自动重连，停止射频并等待 STA_STOP，保留 Wi-Fi driver、STA netif 和
默认网络处理器，下次使用时 start/connect。停止事件未完成时拒绝提前重新启动。
这仍会关闭射频，不会把用户选择的省电模式强制改为常开。

`wifi_setting_deinit()` 仍完整释放资源，用于 USB/热点传输接管、待机等显式调用；
P4 原有释放行为保持。状态逻辑由 `tools/test_s31_wifi_lifecycle.py` 提取生产函数并用
SDK stub 验证暂停/恢复、资源复用、常开和停止失败/超时恢复。是否解决实机播放异常需复测。
