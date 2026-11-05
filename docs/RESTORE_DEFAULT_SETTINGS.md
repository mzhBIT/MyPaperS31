# 恢复默认设置

用户确认后清除应用偏好并重启；书籍、阅读记录、股票列表、微信读书登录和硬件标定保留。不整片擦除 NVS。

Wi-Fi 清理包括应用 `wifi_cfg` 下的 SSID、密码、网络选择和常开模式，以及 SDK 自身保存的 Wi-Fi 配置。先停掉当前驱动并等待已有常开连接任务退出，再仅初始化驱动，调用 `esp_wifi_restore()` 清理 SDK 配置，最后关闭驱动并删除应用偏好。清理期间不启动 STA、不连接旧网络、不触发新的联网校时。

DeepSeek 的 `ds_chat/api_key`、默认模型和高德地图的 `amap/api_key`、地图配置也随对应命名空间删除。它们没有 SD 卡密钥回退路径。微信读书扫码登录会话是账户状态，沿用原有保留策略；其中自动取得的会话令牌不属于设置页手动配置的服务 API Key。

应用偏好提交后，重新打开 `wifi_cfg`、`ds_chat` 和 `amap`，逐项确认 Wi-Fi 的 `ssid`、`password` 及两个服务的 `api_key` 均返回 `ESP_ERR_NVS_NOT_FOUND`。读取异常、提交失败、SDK 重置失败或凭据仍然存在，都必须返回失败，设置页显示“恢复未完成，请重试”，不作为成功重启。

成功日志包括 `Wi-Fi driver defaults restored; radio off` 和 `reset verified: saved Wi-Fi credentials and DeepSeek/AMap API keys absent`。失败日志不输出密码和密钥。清除后扫描列表仍可看到周围路由器；它们不应再标记为已保存，也不应依赖原有密码自动连接。

`tools/test_settings_reset.py` 使用生产重置流程生成 PC 回归，覆盖保存凭据清除、SDK 清理和停止顺序、失败回传、清理结果回读、保留登录/用户内容以及未配置 Wi-Fi 时的重复恢复。
