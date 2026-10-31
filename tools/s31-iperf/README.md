# MyPaperS31 官方 iperf 独立基线

来源：本机 ESP-IDF v6.1.0 的 `examples/wifi/iperf`。复制了示例主程序、组件清单、
CMake 和两份 sdkconfig.defaults。官方说明：
https://github.com/espressif/esp-idf/tree/release/v6.1/examples/wifi/iperf

与原示例的差别仅为：独立项目名和 S31 target、追加 `board.defaults` 使用已验证的
DIO/16MB Flash 与原生 USB Serial/JTAG 交互控制台，以及取消 NVS 初始化失败后的自动擦除。
串口输入使用 `simple_console.c` 的普通行输入，跳过 linenoise 的 ANSI 终端探测，
支持 CR/LF 提交、退格、Ctrl+C/Ctrl+U 清空输入，不提供 TAB 补全和上下键历史。
命令解析仍调用原 `esp_console_run`，Wi-Fi 和 iperf 命令注册及实现沿用官方组件。
不修改 Wi-Fi 或 iperf 组件源码。下载版本固定在本目录 `dependencies.lock`。

## 构建、烧录和串口

构建需先激活 **IDF 6.1**；已构建完成后的 flash/monitor 可以直接在普通 PowerShell 执行，
脚本从独立构建的 CMakeCache 中读取对应 Python 工具路径，不重新构建、不访问组件服务器。
在阅读器工程根目录执行：

```powershell
./scripts/test-s31-iperf.ps1 -Action build
./scripts/test-s31-iperf.ps1 -Action flash -Port COM5
./scripts/test-s31-iperf.ps1 -Action monitor -Port COM5
```

COM5 换成实际串口；烧录速率默认 460800。输出在 `tools/s31-iperf/build/s31_iperf.bin`，
与完整阅读固件的 `build-s31` 分离。**烧录必须包含本测试的 bootloader、分区表和 app**，
不要只向原固件写 app；脚本的 flash 会使用本测试的完整构建产物。

运行后不初始化屏幕、SD、前光、阅读任务或 PSRAM，因此屏幕可能仍保留之前画面。
通过原生 USB Serial/JTAG 端口的 `iperf>` 提示符交互，monitor 参数波特率 115200。
原生 USB 必须设为主控制台：把它设为 UART0 的辅助日志输出口，只能看日志，不能向
UART0 REPL 输入命令。如果另接 GPIO59/58 的外部 UART 转接器测试，需要在本独立工程
`-Action menuconfig` 中将 `Component config -> ESP System Settings -> Channel for console output`
改回 UART0，然后重新 build/flash。

## 路由器模式：测 PC → S31

在**设备串口终端**依次输入：

```text
sta_connect "你的SSID" "你的密码"
```

等到打印 `IP_EVENT_STA_GOT_IP`，记下本次 IP；不能沿用上次 .106 的假设。然后输入：

```text
wifi_ps 0
iperf -s -i 2 -t 30
```

在第二个 **PC PowerShell 窗口**执行：

```powershell
./tmp/iperf2/iperf-2.2.1-win64.exe -c 192.168.0.106 -p 5001 -C -i 2 -t 30
```

IP 改成刚获取的地址。服务端 accept 默认只等 5 秒，先准备好 PC 命令，
在串口启动 `iperf -s` 后马上运行 PC 命令；如果等超时，重新在设备执行 `iperf -s`。
若已有测试正在运行，可以先 `iperf --abort`。每轮结束后都重新启动设备端服务。

若难以在 5 秒内切换窗口，可先在 PC 启动带重试的客户端，再在设备执行 `iperf -s`：

```powershell
./tmp/iperf2/iperf-2.2.1-win64.exe -c 设备当前IP -p 5001 -C -i 2 -t 30 --connect-retry-time 60 --connect-retry-timer 1
```

该参数组合已用本机延迟启动接收端验证。若仍不能连接，需查看 PC 的完整错误与
双方 IP/子网；不能只凭设备 accept 超时就认定是操作太慢。手机热点可能限制客户端互通。

必须使用 **iperf 2.x**。`-C` 关闭新版客户端额外协商消息；不使用 iperf3，
也不要与此前应用内的临时 TCP 接收器混用（该功能已移除；官方服务端按自己的计时器停止）。
记录设备端每 2 秒 `Mbits/sec` 和最终统计，以及连接日志/PHY/RSSI；1 Mbit/s = 约 122 KiB/s。

## 热点模式

设备重启后，在串口输入：

```text
wifi_mode ap
ap_set S31-iperf 12345678 -a wpa2 -n 11
```

PC 连接 `S31-iperf` 热点，密码 `12345678`。然后设备执行：

```text
iperf -s -i 2 -t 30
```

PC 立即执行：

```powershell
./tmp/iperf2/iperf-2.2.1-win64.exe -c 192.168.4.1 -p 5001 -C -i 2 -t 30
```

保留相同 PC、距离和路由器作对比。官方 profile 显著提速只说明与完整固件存在差异，
并不能直接证明某个单独参数是根因；再逐项回查配置、外部内存和运行任务。

## TCP 停顿时的 UDP 对比

在同一次路由器连接下，先等 TCP 测试结束（必要时 `iperf --abort`），再在设备输入：

```text
iperf -s -u -i 2 -t 30
```

PC 立即运行（替换本次设备 IP）：

```powershell
./tmp/iperf2/iperf-2.2.1-win64.exe -c 192.168.0.106 -p 5001 -u -C -b 1M -l 1400 -i 2 -t 30
```

以设备端实际接收速率为准，PC 的 UDP 发送速率不代表接收成功。UDP 正常而 TCP 很慢，
优先检查 TCP 确认/处理及协议兼容；两者都慢，则公共接收通路仍需检查。这个对比本身
不能区分驱动和硬件，也不排除 PC/路由器一侧的影响。

### 已观察的接收异常与 b/g 兼容性对比

2026-10-25 实测：PC UDP 发送约 1.05Mbit/s，S31 30 秒仅收到约 0.19MiB，
接收均值约 0.05Mbit/s；官方基线仍复现低速。统计出现 `tot_errors=8985`、
`0xc6=8985`、`SAMEBM_ERRCNT=8594`、`FCS_ERR=172`；`ht=8729` 与 `ht_stbc=8729`。
这些说明需要继续检查底层无线接收及协议兼容。错误百分比的分母是错误总数，
`0xc6 100%` 不是整条链路 100% 丢包。补充核对 SDK 后确认：CFO 对应私有统计结构的
`int16_t rx_cfo_hz`，单位 Hz，是频偏而非错误次数；例如 26840 表示约 26.84kHz。
仅凭这个值不能认定晶振异常。
没有找到公开的 0xc6/SAMEBM 细分解释，不据此认定具体硬件故障。

无需重新烧录即可先排除 HT/STBC/聚合这一组路径；设备串口依次执行：

```text
iperf --abort
sta_disconnect
wifi_protocol b/g
sta_connect "你的SSID" "你的密码"
```

确认连接成功及新 IP，再执行 `wifi_ps 0` 和前面的 UDP 接收测试。
这只改变诊断固件本次连接的协议选择，Wi-Fi NVS 已关闭，不会改变阅读器设置。
若吞吐恢复，再在 HT/STBC/聚合等功能之间做进一步单变量实验；不能仅凭此一步
直接认定 STBC 是根因。此步骤也同时排除了 11ax，尽管前一次统计主要显示 HT 帧。

后续 b/g TCP 实测 30 秒约 2.71MiB、平均 0.72Mbit/s，后两个区间 2.32/2.56Mbit/s，
波动仍明显。TX 统计 enable=3586、complete=3583、succ=932、fail=2651（73.93%），
其中 CTS 类失败 1009（921 次 timeout），ACK/BA 类失败 1642（1521 次 timeout）。
这里的比例是驱动发送尝试的统计口径，不是应用 TCP 丢包率；TCP 接收也依赖反向发送确认。

统计口径需特别注意：当前 wifi-cmd 0.2.12 的 `wifi_cmd_clr_rx_statistics()` 仅在
`CONFIG_ESP_WIFI_ENABLE_WIFI_RX_MU_STATS` 开启时才调用 `esp_test_clr_rx_error_occurs()`，
本 profile 关闭该选项，因此 `tot_errors/0xc6` 可能跨轮累计，不能当作单轮错误数。
另外 `wifi_cmd_get_tx_statistics()` 最后会清空共享硬件统计，所以其后的 RX 输出里
很多硬件计数为零，并不表示整轮没有这些错误。分析 TCP 时应保留 TX 部分附带的第一份硬件统计。

接下来优先保持 b/g、同一 PC、同一客户端及码率，换一个 2.4GHz 路由器或手机热点，
重新连接后用新 IP 重复 TCP。若明显恢复，重点排查原路由器与 S31 的无线兼容；若仍有
大量 CTS/ACK 超时，再检查 SDK/PHY 校准、时钟、射频和供电。基线禁用 PSRAM且未运行阅读应用，
不应继续把正式应用的缓存或渲染作为这次基线低速的主要解释。

## 已验证的官方基线参数

CPU 320MHz，PSRAM 关闭；Flash DIO/40MHz/16MB。官方 S31 参数为静态 RX 48、
动态 RX 72、动态 TX 64、RX/TX BA 64、TCP 收发窗口 65535、接收邮箱 64；
TCP/IP 优先级 23/CPU1，tick 1000Hz，性能编译优化、LwIP IRAM 优化开启。
官方实验配置还关闭了看门狗和 PHY PLL track；这些仅用于独立基线，不直接推广到阅读固件。

## 恢复 PHY PLL 跟踪的单变量版本

手机热点 b/g TCP 对照仍只有约 0.08Mbit/s（0.30MiB/30秒），TX 尝试失败约 92.34%，
CTS timeout=643、ACK timeout=890，说明原路由器不是唯一因素。下一步检查基线中
专为实验关闭的 PLL 跟踪。IDF `components/esp_phy/Kconfig` 说明，跟踪用于适应温度变化，
默认开启。这里不能仅凭频偏数字认定 PLL 或晶振故障。
正式阅读固件原本没有关闭 PLL 跟踪；本对照只是排除官方测速基线附带的实验条件，
不能预先解释正式固件中的全部低速现象。

新增 `PhyTrack` 版本，配置仅把 `CONFIG_ESP_PHY_DISABLE_PLL_TRACK` 由 y 改为未设置，
不同时改变校准方式、协议选择、缓冲、CPU、Flash 或看门狗。构建配置/产物独立放在
`tools/s31-iperf/build-phy-track`，原 `build` 基线不覆盖。

```powershell
# 构建需要 IDF 6.1 环境；flash/monitor 不需要激活。
./scripts/test-s31-iperf.ps1 -Variant PhyTrack -Action build
./scripts/test-s31-iperf.ps1 -Variant PhyTrack -Action flash -Port COM5
./scripts/test-s31-iperf.ps1 -Variant PhyTrack -Action monitor -Port COM5
```

烧录重启后协议回到默认，串口需要重新输入 `wifi_protocol b/g`，再用
`sta_connect "热点名" "密码"` 连同一个手机热点；等待新 IP，执行 `wifi_ps 0`。
PC 使用前面的带重试命令，设备执行 `iperf -s -i 2 -t 30`，保持同一网络/距离/PC。
可用 `-Variant Baseline -Action flash` 刷回原基线作交叉复测。正式阅读固件未因本试验改变。

## FullCal：跳过缓存并执行完整 RF 校准

PhyTrack 手机热点 b/g TCP 实测仍只有约 0.09Mbit/s（0.33MiB/30秒），没有解决吞吐异常。
TX 尝试失败约 69.91%，CTS timeout=89、ACK timeout=119；尝试总数和流量不同，
不能仅凭失败总数比上一轮减少就认定链路恢复。接收错误计数为 0 也不能覆盖
第一份硬件统计中的 FCS/ABORT 等错误。

FullCal 在 PhyTrack 基础上仅关闭 `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE`。
核对 IDF 6.1 `components/esp_phy/src/phy_init.c`：此分支每次 PHY 初始化均调用
`register_chipv7_phy(..., PHY_RF_CAL_FULL)`，不读取或写回已有 RF 校准 NVS。
无需 erase-flash，也不删除旧 RF 校准数据或阅读器设置。虽然菜单校准模式仍可显示
partial，其配置在该无缓存分支不使用；实际走完整校准。

```powershell
./scripts/test-s31-iperf.ps1 -Variant FullCal -Action build
./scripts/test-s31-iperf.ps1 -Variant FullCal -Action flash -Port COM5
./scripts/test-s31-iperf.ps1 -Variant FullCal -Action monitor -Port COM5
```

输出在 `build-full-cal`，原两套产物保留。启动日志显示 `PHY full calibration: ignore cached RF data`。
然后保持相同手机热点、b/g、PC和客户端参数复测 TCP；若恢复，后续针对缓存校准数据处理；
若没有改善，再检查其他共有因素（SDK/射频/供电/PC链路），不把此试验预先当作修复。

工厂分区位于 `0x10000`，大小 1500KiB；app 实际大小见 build 的 check_sizes 输出。
NVS 仍为 `0x9000/24KiB`、PHY 为 `0xf000/4KiB`，Wi-Fi NVS 写入关闭。
未执行 NVS 擦除或设备烧录；只完成构建和 PC 客户端本机收发检查，板端吞吐待实测。

## PC 客户端与恢复完整固件

已下载到工程的 `tmp/iperf2/iperf-2.2.1-win64.exe`，版本查询及本机 TCP 发送验证通过。
来源：https://sourceforge.net/projects/iperf2/files/iperf-2.2.1-win64.exe/download
SHA256：`96ddcf44d769c2a043faaf458b4d8703d968fde9bda68b0b8f540218f7f822fc`。
临时二进制不提交 Git；换 PC 时从该来源下载。

完成测试后关闭串口 monitor（Ctrl+]），在 IDF 6.1 终端执行：

```powershell
./scripts/build-board.ps1 -Board MyPaperS31 -Action flash -Port COM5
```

这会刷回原 `build-s31` 的完整固件、bootloader 和分区表。该目录的构建产物与配置没有被
独立 iperf 工程覆盖。无需 erase-flash。
