# 小红书阅读服务

为 MyPaperS31 提供推荐、图文详情和媒体接口。群晖 Chromium 读取已登录网页的推荐，XHS-Downloader 提供单篇详情。需要时服务会尝试从同一浏览器读取可正常打开的笔记。媒体提供带设备令牌的压缩 4 位灰度和 JPEG 接口，以及可用的 CDN 直连地址。

## Windows 浏览器测试

在仓库根目录打开 PowerShell，使用 Python 3.12 运行：

```powershell
& .\tools\xhs_reader_server\start-windows.ps1
```

打开 `http://127.0.0.1:8091/`。点击「打开推荐」，选一篇看正文和两张样例图，再点「加载更多」。演示模式的数据和图片在本机生成。

要看自己账号的推荐，保持群晖上的 XHS-Downloader API 运行。执行下面的命令并粘贴你在 Windows 浏览器里实际打开过的 XHS-Downloader 地址。可以直接复制带 `/docs` 的完整地址。`8092` 可以和 `8091` 的演示服务同时运行。

```powershell
$downloaderAddress = Read-Host 'XHS-Downloader URL from your browser'
& .\tools\xhs_reader_server\start-windows.ps1 -Port 8092 -DownloaderUrl $downloaderAddress
```

1. 打开 `http://127.0.0.1:8092/`，填写启动时终端显示的设备令牌，点「保存」。
2. 点「打开推荐」。服务会打开一个独立的 Edge 窗口。在那个窗口用小红书自己的登录界面扫码，保留窗口，再点一次「打开推荐」。
3. 点「加载更多」或打开一篇图文笔记。网页推荐的内容和排序由当前账号的网页版决定。识别到登录或验证提示时，服务会等待下次操作。

脚本会创建自己的 Python 环境。登录会话和图片存放在 `data`，重启服务会继续使用有效的登录状态。电脑需要能访问群晖的详情 API 和小红书网页。若详情 API 使用独立令牌，启动前设置 `XHS_DOWNLOADER_TOKEN`。

## 群晖独立运行

推荐页所在的 Chromium、阅读 API 和 XHS-Downloader 都在 NAS 上运行。noVNC 把 NAS 上的 Chromium 窗口显示在你手边的浏览器里，扫码后运行状态留在 NAS 上，关闭查看窗口的设备也能继续阅读。

将这个目录完整上传到群晖。在目录中根据 `.env.example` 创建 `.env`。`XHS_API_TOKEN` 填一个至少 16 字符的随机令牌，`XHS_VNC_PASSWORD` 填用于浏览器控制台的密码。保留已验证过的 XHS-Downloader 项目。

在 Container Manager 中以该目录为项目路径，使用 `compose.yaml` 部署。默认复用本机已有的 `joeanamier/xhs-downloader:2.8` 镜像作为构建底座，首次仍需下载 Chromium 和系统依赖。详情默认由 `http://host.docker.internal:15557` 提供，也可以在 `.env` 的 `XHS_DOWNLOADER_URL` 填入已验证的群晖地址。

已有离线镜像包时，可在 Container Manager 中导入，通过镜像标签部署并复用 `reader-data` 卷。仓库保留源码构建和部署模板。

### 在局域网扫码

在 `.env` 中将 `XHS_API_BIND_ADDRESS` 和 `XHS_VNC_BIND_ADDRESS` 都设为群晖内网 IP，然后部署项目：

1. 用局域网中的电脑、平板或另一台手机打开 `http://群晖内网IP:8091/`，输入设备令牌并点击「打开推荐」。这会在 NAS 容器中打开 Chromium。
2. 打开 `http://群晖内网IP:6080/vnc.html`，连接后输入浏览器控制台密码。用手机的小红书 App 扫画面里的登录二维码。
3. 回到 `8091` 的测试页，重新点击「打开推荐」。登录资料保存在 NAS 的 `reader-data` 卷里。

### 用 SSH 查看登录窗口

默认两个端口都只绑定 NAS 本机。可以保留这个设置，然后通过 SSH 把测试页和登录窗口转发到手边的电脑，替换你的用户名和群晖 IP：

```powershell
ssh -N -L 8091:127.0.0.1:8091 -L 6080:127.0.0.1:6080 用户名@群晖内网IP
```

打开 `http://127.0.0.1:8091/`，保存设备令牌并点击「打开推荐」。在 `http://127.0.0.1:6080/vnc.html` 连接浏览器后扫码。登录后回到测试页重新点击「打开推荐」。若 Windows 已有演示占用 `8091`，SSH 命令里的第一个 `8091` 可以换成 `8092`，访问地址使用对应的 Windows 本机端口。

### 从外网阅读

用群晖上的 Sakura Frp 新建 TCP 隧道，转发 `8091`，开启自动 HTTPS 并绑定子域名。默认本地 IP 填 `127.0.0.1`，若 `XHS_API_BIND_ADDRESS` 已改为群晖内网 IP，就填那个 IP。外网只发布阅读服务的 `8091`，登录窗口 `6080` 和详情 API `15557` 供内网使用。

## 电脑与设备对照测试

从局域网测试页用设备令牌打开推荐，再打开要核对的笔记。若详情失败，可在浏览器开发者工具的 Network 中打开 `/v1/jobs/{job_id}` 响应，查看 `error`。然后在群晖 Chromium 控制台打开同一篇笔记，区分详情来源错误、网页登录验证和限流。

电脑与设备共用当前推荐会话。电脑点「打开推荐」后，设备可以刷新推荐取得新会话。

## 接口

除首页、测试页资源和健康检查外，发给阅读服务的请求都带 `Authorization: Bearer <设备令牌>`。群晖媒体接口使用相对地址，客户端用同一个 HTTPS 基础地址拼接并携带令牌。直连图片使用响应中的完整 CDN 地址。

| 请求 | 用途 |
|---|---|
| `GET /v1/health` | 返回运行模式及是否需要令牌 |
| `POST /v1/jobs` | 提交操作，返回 `202` 和 `job_id` |
| `GET /v1/jobs/{job_id}` | 查询一次操作的状态和结果 |
| `GET /v1/media/{id}.jpg` | 读取受令牌保护的 JPEG |
| `GET /v1/media/{id}.gray4?width=684&height=1216` | 读取压缩 4 位灰度媒体 |

```json
{"request_id":"open-1","action":"open"}
```

`open` 和 `refresh` 返回新会话与首批 `items`。`more` 提供 `session_id`，先取尚未返回的卡片，不足六条时尝试滚动补齐。每批最多六条，卡片的 `likes` 是点赞数字符串或 `null`。`detail` 同时提供 `session_id` 和从 `items` 取到的 `note_id`。使用不同的 `request_id` 区分操作，同一操作重试使用原来的编号。

状态包括 `pending`、`running`、`done` 和 `error`。成功详情包含 `id`、`title`、`author`、`body`、`published_at`、`ip_location`、`type`、`counts` 和有序 `images`。`ip_location` 为笔记 IP 属地或 `null`。`counts` 的 `comments`、`favorites`、`likes` 和 `shares` 分别是评论、收藏、点赞和分享数量，返回适合直接显示的字符串（例如 `"0"` 或 `"1.2万+"`），数量未知时返回 `null`。

列表的 `cover_media` 和详情的 `image_media` 提供灰度路径 `gray4` 和直连地址 `cdn_url`，沿用 `cover` 和 `images` 的图片顺序。灰度输出的二进制格式和尺寸约定见 [MyPaper 接入说明](../../docs/XHS_READER_SERVER_DESIGN.md#群晖压缩-4-位灰度)。

服务保留一个连续滚动的推荐会话，重新打开、刷新或重启后，旧会话需要重新打开。登录提示对应 `login_required`，详情服务不可用时会给出独立的错误。
