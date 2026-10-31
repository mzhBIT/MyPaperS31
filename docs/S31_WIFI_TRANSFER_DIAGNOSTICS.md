# S31 传输诊断报告

> 历史调试记录：2026-10-31 已移除设备及网页诊断入口、JSON 接口、收发统计采集和相关测试代码。普通上传、下载、文件管理与已验证的 Wi-Fi 策略保留。以下说明仅记录此前的诊断方法，不再是当前操作入口。

诊断直接随主固件传输应用工作，不需要串口命令或单独测试固件。进入 Wi-Fi／热点传输，电脑打开设备网址，完成一次普通上传或下载后，在网页“Wi-Fi 传输诊断”中查看结果；“保存诊断报告”下载 JSON 文件。上传结束自动刷新报告，下载结束点击刷新。每次测量覆盖上一份报告，连续上传多个文件后保存的是最后一份。

诊断不改变选中的协议、STBC、AMPDU、省电策略及缓冲配置。开启 S31 的小型 RX/TX 统计支持，不开启约 10KB 的 MU 详细统计。运行时只在文件数据路径启用 RX 统计和 BE 类 TX 统计，结束读取后关闭。统计自身存在少量开销，应在同一诊断固件中对比。

## 报告内容

- `direction`、`allowed_protocol`、`negotiated_phy`、`mode`、`he_stbc_requested`、`ampdu`、`power_save_mode`：测量环境。热点模式不能使用 STA 查询结果冒充客户端协商模式。
- `elapsed_us`、`network_wait_us`、`network_call_max_us`：数据路径总耗时和阻塞接收／发送调用耗时。上传从缓冲分配完成、接收 body 前开始，到文件提交成功结束，不包含成功响应的发送；下载包含数据发送，成功 send 仅表示交给网络栈，不等同于浏览器已收到。失败报告包含退出清理期间耗时。
- `storage_us`、`storage_call_max_us`、`flush_us`：fwrite/fread 及上传 fclose 的耗时。低速发生于接收等待且写卡很短时，不能据此优化写卡。网络调用包含等待对端发送、无线传输及协议栈交付，不能单独作为无线耗时。
- `tcpip_probe_samples`、`tcpip_queue_average_us`、`tcpip_queue_max_us`、`tcpip_probe_rejected`、`tcpip_probe_pending_at_end`：异步投递到 lwIP 线程的回调排队延迟。没有新增常驻任务，每次最多一个未完成回调，通常随半秒无线采样提交。它能发现线程长时间无法运行的迹象，但不是 TCP RTT，也不直接测量 Wi-Fi 驱动到 lwIP 收包的时间。
- `tx_be_attempts/success/retries_*`、`tx_be_wait_ack_count`、`tx_be_ack_timeout`、`tx_be_cts_timeout`：BE 访问类别的驱动发送计数及等待无线 ACK/BA、CTS 失败原因，不是 TCP 重传次数。`tx_be_rtt_max_raw` 保留 SDK 原值，未校验单位，不与抓包 RTT 直接相减。
- `rx_ht/stbc`、`rx_no_buffer`、`rx_base_null/reset`：接收模式、动态接收缓冲不足及接收队列异常的线索。
- `rx_fcs_errors`、`rx_abort`、`rx_same_bitmap_errors`、`rx_buffer_full/fifo_overflow`、`rx_abort_good_fcs`、HT/HE 信令错误、AGC 错误、PHY 重启：无线硬件计数的增量。
- `rx_cfo_hz_last` 是 SDK 定义的有符号频偏观测值，不是 CFO 错误次数，不能累加成丢包数。
- `internal_*_min_sampled`：测量中采样到的内部空闲内存及最大连续块最小值，不保证捕获瞬时最低点。

启用、清零、读取接口的返回状态一并保存。软件统计不可用时相关字段为 null，不能把它当作零错误。没有 probe 样本时延迟为 null。硬件部分有采样失败时，结合 `hw_read_failures` 判断完整性。

## 边界与解释

无线计数包含本机收到的其他无线流量，部分统计还包括管理帧或失败发送尝试，不与上传 TCP 包一一对应，不计算“丢包百分比”。硬件计数通常为 16 位，按模 65536 累加相邻样本增量；通常半秒取样，但阻塞收发期间不能保证周期。查看 `hw_sample_gap_max_us`，若期间多次回绕会低估。驱动也可能让计数饱和，数据只作为诊断线索。

数据进入 HTTP handler 前可能已被缓冲，统计清零无法覆盖 TCP 建连及所有先到的 body 包。当前报告不能独立定位电脑网卡排队、空中 MAC 重试及 S31 调度各自的精确耗时，应与已有抓包联合判断，不保证直接得出硬件或驱动故障结论。

未完成的 lwIP 回调只引用静态探针，退出页面不会引用已释放 activity。epoch 阻止上一文件的迟到回调污染下一文件。HTTP 操作仍受 `TransferSessionGate` 保护，协议切换等待传输结束。

`tools/test_transfer_diagnostics.py` 生成原生 SDK stub 测试，直接编译生产头文件，覆盖 16 位回绕、计时、JSON 转义、失败报告、旧回调跨生命周期／epoch、队列拒绝和不可用接口。构建与测试通过后仍需实际上传生成报告，才能分析本机低速原因。

## Power and link comparison (schema 2)

Power choices are Default (5 dBm), 14 dBm and 20 dBm. Requests are explicit quarter-dBm values: 20, 56 and 80. Settings remain in RAM; exiting transfer restores the 5 dBm production limit. The selected protocol, HE STBC, AMPDU, power and actual STA/AP mode must match when comparing trials.

Reports include tx_power_requested_qdbm, tx_power_profile and link_start/link_end. The SDK readback is a configured upper limit, not measured per-packet RF output. STA records include AP RSSI, BSSID and channel; AP records describe associated peers. Unavailable values are null, not fabricated zero-RSSI measurements.

tx_be_failure_details preserves the SDK SUCCESS/FAIL_RTS/WAIT_CTS/FAIL_CTS/FAIL_DATA/WAIT_ACK stages and MATCH/NOT_TO_SELF/MISMATCH/TIMEOUT classifications. Hardware error details apply to MATCH results only. TIMEOUT cannot independently locate a fault inside the S31.

Compare throughput and retries/ACK/CTS timeouts together. Counters may include unrelated wireless traffic and are not packet-loss percentages. See S31_WIFI_POWER_RESULTS.md for the measured power trials.
