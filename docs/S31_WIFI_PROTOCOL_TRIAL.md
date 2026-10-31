# S31 传输页协议对比

> 历史调试记录：2026-10-31 已移除主固件中的协议/STBC/AMPDU/功率对照选项。当前固定采用已验证的 b/g/n/ax、AMPDU 开启、5 dBm 功率上限和 STA 传输禁用省电；以下步骤不再对应当前页面。

测试集成在日常固件的 Wi-Fi 传输、热点传输页面，不需要单独固件或串口指令。

顶部有三档：`b/g`、`b/g/n`、`b/g/n/ax`。默认使用 b/g/n/ax 自动协商；选择保存在 RAM，退出再进入传输仍使用上次选择，重启恢复 b/g/n/ax。协议对照选项不修改普通应用的 Wi-Fi 配置，也不修改 RX/TX 缓冲区、TCP 窗口或编译优化等级。STA 上传期间继续使用 WIFI_PS_NONE。

切换时停止原 HTTP/TCP 测试服务，释放传输持有的 Wi-Fi 连接，再使用新协议重建相同入口的连接。重启过程中不启动普通应用的常开连接，避免与传输争用 Wi-Fi。STA 仍沿用原有的“连接失败则回退热点”行为，实际模式会在页面上显示。热点名称及密码保持原配置；切换后电脑可能需要重新连接，浏览器重新打开设备显示的网址。失败时点击任意档位可重试，包括当前档位。

有 HTTP 上传、下载或文件操作进行时拒绝协议切换，提示完成后重试。切换期间到达的新请求返回 HTTP 503。服务停止会等待请求处理结束后才释放 activity 资源。`TransferSessionGate` 的计数及切换状态用原子操作同步；上传结果通过互斥锁交给 UI。

测试方法：

1. 进入传输应用，使用相同的路由器或同一种热点模式，保持位置不变。
2. 依次选择三个档位，每档上传同一个文件，上传完成后查看页面的平均速度。
3. 三档结果保留在当前传输页面内，退出页面后清除。失败显示上传错误；结果以最近一次上传为准。平均速度按原传输计时起点到文件提交结束计算，单位 KiB/s，包含接收、写入和提交耗时。
4. 路由器模式显示实际协商的 11b/11g/HT20/HT40/HE20、RSSI、信道。允许 ax 不代表一定协商为 ax。热点模式显示已连接设备数。空闲时每两秒检查连接状态，状态变化才刷新，上传过程中不插入这种刷新。
5. 若某档从路由器回退热点，需先统一测试模式，再比较速度，不能把不同连接模式的结果直接当作协议差异。

页面遵循系统语言，新字符串需执行字体子集检查。P4 保留原来的传输协议和页面。原有端口 5001 临时 TCP 测试服务仍随传输服务启停；本次测试直接用浏览器传书即可。

验证包含 S31 完整构建、P4 传输源文件编译检查，以及原生 `tools/test_transfer_session_gate.cpp` 的并发请求/切换互斥测试。实际协议兼容性与速度仍需在设备上对比。

## HE STBC and AMPDU trials

HE STBC uses the IDF 6.1 private esp_wifi_enable_rx_stbc API. It controls HE capabilities; it does not establish that HT STBC is disabled on HT20 links. AMPDU controls both RX and TX aggregation through wifi_init_config_t. Changing these settings restarts the transfer-owned Wi-Fi driver. Selections remain in RAM and are restored to production defaults on normal app connections.

## 2026-10-26 默认功率策略（替代早期测试默认值）

普通 STA 联网、路由器传输及热点传输统一使用 5 dBm 发射功率上限，允许 b/g/n/ax 协商。每次 Wi-Fi 启动后重新设置功率，保留驱动的停止／重启也不例外。共享定义位于 `main/wifi/S31WifiPolicy.h`，仅用于 S31。

传输页功率选项为默认（5 dBm）→14 dBm→20 dBm；对照选项只保存在 RAM，退出传输恢复 5 dBm。诊断 JSON 记录明确请求值，默认档为 raw 20（0.25 dBm 单位），不再使用 SDK 默认 20 dBm。协议重启默认 b/g/n/ax，HE STBC 和 AMPDU 默认开启。
