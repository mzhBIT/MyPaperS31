# 微信读书墨水屏接口接入设计

## 目标与当前结论

S31 实验版支持原生扫码登录、令牌续期、只读书架和章节目录。原生扫码登录已通过真机验证。PC 脚本已验证登录、书架和目录，单章试读返回受保护的数据，原生正文尚未接入。

S31 默认走网页版书架、目录和正文。升级时沿用各自的登录会话，并回到网页版入口。微信读书页面左下角可切换「网页登录」和「墨水屏登录」，升级后重新选择的模式会在重启后保留。

## APK 接口

APK 的生产 API 主机为 `i.weread.qq.com`，来自 `feature/network/Official.java`。

| 接口 | 用途 | APK 证据 |
| --- | --- | --- |
| `GET /wxticket` | 向服务端取得扫码签名及时间戳 | `WRLoginService.wxTicket`、`TicketResult` |
| `POST /login` | 交换用户确认扫码后返回的授权码 | `QRLoginDialogFragment`、`LoginService.getLoginInfoObservable` |
| `POST /login` | 使用独立的续期请求获取更新后的令牌 | `WRLoginService.refreshToken`、`LoginService.refreshToken` |
| `GET /shelf/sync` | 读取书架，接受书架和有声内容两个同步游标 | `BaseShelfService.Sync` |
| `POST /book/chapterInfos` | 读取章节目录及更新信息 | `BaseChapterService` |
| `GET /book/chapterdownload` | 流式读取获授权章节，响应可能需要原生加密处理 | `BaseBookDownloadService`、`ChapterResp`、`EncryptUtils` |
| `GET /book/getProgress` | 读取云端进度 | `BaseReportService` |

## 登录与续期

1. 从微信读书页面切换至墨水屏后端后启动后台任务。已有该后端的会话时先读取书架，无会话时请求授权票据并显示二维码。
2. APK 的 `QRLoginDialogFragment` 通过微信开放 SDK 的 `DiffDevOAuth` 显示二维码、接收已扫码状态及微信确认后返回的一次性授权码。S31 使用票据请求原生二维码图片，在后台轮询微信的扫码与确认状态。扫码和确认分别显示，超时后用户可刷新二维码、退出或打开已有离线书架。
3. 使用该授权码调用原生登录。APK 请求包含设备 ID、时间戳、随机数、防重放签名及二维码来源标记。设备使用自身稳定标识。设备时钟无效时先校时。授权码交换遇到结果不明确的传输失败时，由用户开启新一轮扫码。
4. 仅在业务成功且 `vid`、`accessToken`、`refreshToken` 有效时提交新会话。原生会话使用独立 NVS 数据，更新整份记录。令牌、授权码和票据仅用于授权及认证，日志只记录阶段、耗时和错误码。
5. 书架请求携带当前原生会话的 `vid` 和 `accessToken`。出现明确的令牌失效应答时，同一账号只发起一次续期，成功后重试一次原读取。DNS、TCP 和 TLS 故障走网络错误路径。续期明确被拒绝时显示二维码及离线入口。
6. 切换后端或退出时先取消并等待网络任务结束。同一后端同一时间仅有一个登录或书架任务。恢复默认设置清理原生会话及后端选择，网页会话沿用原设置行为。

## 书架与阅读

- 复用 [WereadActivity](../main/activities/WereadActivity.cpp) 的工作任务、状态栏和列表。原生与网页分别鉴权，结果转换为现有的 [Book 和 Chapter 模型](../main/weread/WereadApi.h)。首版实验后端显式启用，关闭后使用现有入口。
- 首次请求两个游标均从零开始。先用授权账号确认完整快照、空书架、有声内容和业务错误的响应结构，再开放缓存写入。缓存记录后端和 `vid`，通过结构及账号校验的完整结果才可原子替换上一份快照。以后启用增量同步时，同一事务提交书架和对应游标。
- 更新失败时沿用现有离线提示和缓存优先加载。若确认同一 `vid` 且书籍、章节标识兼容，可以复用已缓存正文。账号归属未知的历史缓存留在原来源。
- 只读阶段展示书架和目录。点击书架条目后，使用原生令牌请求 `/book/chapterInfos`，目录按 `chapterIdx` 排序，打开时定位并标记书架记录的 `chapterUid`。在线失败时读取同一账号与书籍的目录缓存。后续验证获得授权的单章下载，在 PSRAM 和 SD 文件之间分块处理，产出当前阅读器支持的章节数据，再接图片和下一章预取。
- 最后接入进度写回。先比对 APK 返回的章节 UID、位置坐标和云端进度，再以真实阅读测试回读。现有网页端待核验时长按原会话管理。

## Wi-Fi 与 4G

原生 API 和 4G 下的网页版 API 在系统 DNS 解析失败后，通过阿里公共 DNS 的 HTTPS 接口查询 IPv4 地址。解析请求只携带待解析域名。成功重连后按 A 记录 TTL 缓存，最长 5 分钟。连接微信读书时保留原域名的 HTTP Host、TLS SNI 和证书校验。

S31 用 Wi-Fi 拉取原生书架或目录时临时关闭驱动的射频省电，请求结束后恢复。S31 用 Wi-Fi 拉取网页版书架或目录时，首次建连超时为 6 秒，响应读取和重试建连使用 15 秒超时。

## 实验入口验收

1. 在 S31 用微信扫码。先验证 `WEREAD_UI: native QR ready` 和 `native QR scanned`，再查看 `WEREAD_NATIVE` 的 `login`、`shelf` 状态与错误码。登录令牌、二维码会话 ID 和授权码均无需贴出。
2. 主机回归覆盖扫码取消和超时、重复续期、响应格式异常、账号切换时旧结果返回、缓存写失败。确认网络失败时旧书架仍可打开。
3. S31 在 Wi-Fi 和 4G 下分别拉取书架，重启后确认恢复会话，断开网络后确认可以打开已缓存书架。
4. 加入目录和单章后，对照 APK 检查正文、图片、章节位置及 SD 写入结果。最后验证进度回读和中断后的重复提交处理。

腾讯公开的 Agent Gateway 可以单独提供书架、目录和元数据，具体能力见 [官方 API 说明](https://github.com/Tencent/WeChatReading/blob/main/skills/SKILL.md)。它使用独立的 API Key 鉴权。
