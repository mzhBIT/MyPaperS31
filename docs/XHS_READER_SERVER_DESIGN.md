# MyPaper 小红书应用服务器设计与接入说明

MyPaper 的小红书功能采用独立应用入口、推荐列表和笔记详情。群晖运行已登录的小红书网页版，根据阅读器发送的指令读取推荐、滚动和查询详情。阅读器通过 HTTPS 取得文字和互动数量，并可在群晖 4 位灰度转发和小红书 CDN 直连之间切换。小红书页面使用独立的布局。

阅读服务接口版本为 `/v1`。群晖已验证 `mypaper/xhs-reader:1.0.1-amd64` 的外网访问和笔记评论数。`1.0.2` 增加压缩 4 位灰度和 CDN 直连地址，`1.0.3` 增加每批六条、推荐列表点赞数和详情 IP 属地。`1.0.4` 补齐推荐封面的多种地址来源，兼容 `1.0.3` 的设备端协议。`1.0.5` 在 XHS-Downloader 返回空详情或笔记 ID 不一致时，尝试读取登录浏览器里可正常打开的同一篇笔记。

## 运行架构

| 组件 | 职责 |
| --- | --- |
| MyPaper 小红书应用 | 保存服务地址和设备令牌，展示推荐和笔记详情，按用户操作发起请求 |
| SakuraFrp 群晖套件 | 将外网 HTTPS 入口转发到阅读服务的 `8091` 端口 |
| 小红书阅读服务 | 用 Python、FastAPI、Uvicorn 提供 JSON、JPEG 和压缩 4 位灰度接口，管理任务、推荐会话和图片缓存 |
| Chromium 和 Playwright | 使用保存在群晖上的个人登录状态读取小红书网页版推荐，必要时滚动已打开的页面 |
| XHS-Downloader 2.8 | 在群晖内网提供 `POST /xhs/detail`，返回笔记正文、静态图片来源和四项互动数量 |
| Xvfb、x11vnc 和 noVNC | 提供群晖中 Chromium 的可视化控制台，用于首次扫码和处理失效登录 |

主要调用路径为 `MyPaper → SakuraFrp → 阅读服务`。阅读服务在同一台群晖上访问 Chromium 和 XHS-Downloader，并从小红书图片 CDN 获取要转发的图片。群晖持续运行生产服务。Windows 测试用于验证交互与接口，群晖上的 Chromium 由同一局域网的电脑、平板或手机浏览器通过 noVNC 操作。

推荐排序来自当前账号的小红书网页版。服务优先读取当前页中已呈现且尚未发送的卡片。可用卡片不足六条时，服务可滚动一次，尝试补齐本批。笔记详情在选中卡片时请求，图片在读到媒体地址后请求。整个流程由应用操作驱动。

## 群晖部署与访问

### 服务地址和凭据

把以下示例中的 `NAS_IP` 替换成群晖实际的局域网 IP 地址。

| 用途 | 示例或配置项 |
| --- | --- |
| 局域网阅读测试页和 API | `http://NAS_IP:8091/` |
| Chromium 控制台 | `http://NAS_IP:6080/vnc.html`，供局域网扫码 |
| XHS-Downloader API | `http://NAS_IP:15557`，`/docs` 是该容器的 API 测试页 |
| 外网阅读测试页和 API | `https://实际子域名.nyat.app:远程端口/`，域名和远程端口取自 SakuraFrp 启动日志 |
| 阅读器设备令牌 | 项目环境变量 `XHS_API_TOKEN`，实时模式必填，至少 16 个字符 |
| 浏览器控制台密码 | 项目环境变量 `XHS_VNC_PASSWORD`，在 noVNC 连接时使用 |
| XHS-Downloader 令牌 | 项目环境变量 `XHS_DOWNLOADER_TOKEN`，仅在上游启用独立鉴权时配置 |

MyPaper 应用配置只需要阅读服务的完整基地址和 `XHS_API_TOKEN`。SakuraFrp 接入密钥和套件管理密码用于隧道管理。群晖的浏览器用户数据目录是 `/data/chromium-profile`，位于 Compose 的 `reader-data:/data` 命名卷中。容器重建会使用该卷中的浏览器状态，登录有效期由小红书决定。图片缓存也在该卷下，实时模式使用 `/data/live/media`。

### 上线和升级

1. 在群晖保留现有的 XHS-Downloader 2.8 容器。阅读项目使用 `XHS_DOWNLOADER_URL=http://NAS_IP:15557` 访问它。
2. 可以从仓库源码构建镜像。已有离线包时，也可导入包含 Chromium 和依赖的 Linux amd64 镜像。在 Container Manager 的「映像 → 操作 → 导入」选 `xhs-reader-1.0.5-linux-amd64.tar`。现有项目在 YAML 中引用 `image: mypaper/xhs-reader:1.0.5-amd64` 和 `pull_policy: never`。用同一项目和命名卷重新部署。
3. 阅读服务在容器中监听 `0.0.0.0:8091`。需要在局域网扫码时，将阅读端口和控制台端口绑定到群晖的局域网 IP。
4. 打开局域网测试页，填写设备令牌并点「打开推荐」，以启动容器内的 Chromium。在同一局域网打开 `6080/vnc.html`，连接后输入控制台密码，用小红书手机 App 扫码。完成后回到 `8091` 再点「打开推荐」。
5. 在现有 SakuraFrp 群晖套件启动小红书专用 TCP 隧道。本地地址为 `NAS_IP:8091`，「自动 HTTPS」选「自动」，绑定 `nyat.app` 子域名。使用隧道日志给出的完整 HTTPS 地址。`6080` 控制台和 `15557` 详情容器都供内网访问。
6. 用移动数据打开外网测试页，输入同一个设备令牌，验证推荐、详情和图片。设备端配置保留 HTTPS 域名和日志上的远程端口。

从源码部署时，按服务 README 配置环境变量，并使用仓库中的 Dockerfile 和 Compose 模板构建。仓库提供源码和部署配置，离线镜像通过独立文件分发。[服务器部署 README](../tools/xhs_reader_server/README.md) · [Dockerfile](../tools/xhs_reader_server/Dockerfile) · [Compose 模板](../tools/xhs_reader_server/compose.yaml)

## 阅读器 API

### 基地址、鉴权和请求格式

| 请求 | 成功响应 | 用途 |
| --- | --- | --- |
| `GET /v1/health` | `200`，JSON | 检查阅读服务连通性、运行模式和令牌要求 |
| `POST /v1/jobs` | `202`，JSON | 提交 `open`、`more`、`refresh` 或 `detail` |
| `GET /v1/jobs/{job_id}` | `200`，JSON | 查询任务状态和结果 |
| `GET /v1/media/{image_id}.jpg` | `200`，`image/jpeg` | 获取列表封面或笔记静态图片 |
| `GET /v1/media/{image_id}.gray4` | `200`，`application/octet-stream` | 获取压缩的 4 位灰度图，可以指定显示尺寸 |

健康检查、首页和首页静态资源可直接访问。任务提交、轮询和群晖媒体请求都使用 `Authorization: Bearer <设备令牌>`。群晖媒体路径拼接到同一个服务地址。直连 CDN 使用图片对象返回的完整 `cdn_url`，该请求的鉴权由 CDN 自身决定。

POST 使用 UTF-8 JSON、`Content-Type: application/json` 和有效的 `Content-Length`。请求体上限为 4096 字节。使用会发送明确长度的 HTTP 客户端模式，服务会拒绝缺少 `Content-Length` 的 POST。健康检查成功代表阅读服务已响应，浏览器登录及 XHS-Downloader 状态需要用对应操作验证。

健康检查示例

```json
{"status":"ok","mode":"live","token_required":true}
```

### 提交任务和轮询

| 操作 | 请求字段 | 服务行为 |
| --- | --- | --- |
| 打开推荐 `open` | `request_id`、`action` | 打开当前账户的推荐页，成功时返回新会话和首批卡片 |
| 加载更多 `more` | `request_id`、`action`、`session_id` | 沿用当前会话，取得尚未发送的卡片，必要时滚动页面 |
| 刷新 `refresh` | `request_id`、`action` | 重新读取推荐页，成功时返回新会话和首批卡片 |
| 打开详情 `detail` | `request_id`、`action`、`session_id`、`note_id` | 用当前会话中已发送的笔记查询详情 |

使用 `open` 打开列表，然后按顺序提交 `more` 或 `detail`。`request_id`、`session_id`、`note_id` 和 `job_id` 按字符串处理。请求标识符允许 1 至 64 个 ASCII 字母、数字、下划线或连字符。`note_id` 只用于 `detail`，取自服务返回的列表项。请求体使用表中定义的字段。

首次进入应用时发送

```json
{"request_id":"req-open-001","action":"open"}
```

服务接受请求后返回 HTTP `202`

```json
{"job_id":"4fd6922235c940389607051edf2fc734"}
```

客户端轮询对应的 `GET /v1/jobs/4fd6922235c940389607051edf2fc734`。`pending` 表示排队，`running` 表示执行中，`done` 带 `result`，`error` 带 `error`。网页验证客户端使用约 400 毫秒轮询间隔和 35 秒本地等待期限。阅读器可根据网络时延设置自己的轮询间隔和等待期限，页面等待在后台执行。

一项新操作使用新的 `request_id`。发送 POST 后遇到网络错误时，用原请求体和原 `request_id` 重发，服务会在任务记录有效时返回同一个 `job_id`。同一 `request_id` 配上不同请求内容会返回 HTTP `409`。服务串行执行浏览器任务，运行中加排队最多 8 项。已完成的任务结果保存在进程内存，提交或查询时按约 15 分钟保留期和 128 项容量限制清理。重新打开笔记会提交新的详情任务。

### 推荐列表与会话

`open`、`more` 和 `refresh` 的 `result` 结构相同。每批最多 6 条，便于双列各排三张卡片。上游可用卡片不足时，会返回实际读到的数量。`likes` 是推荐卡片的点赞数，格式为字符串或 `null`，例如 `"0"` 或 `"1.2万+"`。封面 `cover` 和 `cover_media` 也可为 `null`。

```json
{
  "job_id": "4fd6922235c940389607051edf2fc734",
  "status": "done",
  "result": {
    "session_id": "edb05ea4e8e64ad7bc07d99f9886d959",
    "items": [
      {
        "id": "123456789abcdef012345678",
        "title": "周末的街角书店",
        "author": "示例作者",
        "likes": "26",
        "cover": "/v1/media/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.jpg",
        "cover_media": {
          "gray4": "/v1/media/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.gray4",
          "cdn_url": "https://ci.xiaohongshu.com/example-cover?imageView2/format/jpeg"
        }
      }
    ]
  }
}
```

加载更多

```json
{
  "request_id": "req-more-002",
  "action": "more",
  "session_id": "edb05ea4e8e64ad7bc07d99f9886d959"
}
```

当前容器只维护一个推荐会话。任意客户端成功执行 `open` 或 `refresh` 后，其他客户端手里的旧 `session_id` 也会失效。服务重启后用 `open` 恢复列表。阅读器返回已有列表时可继续使用已有会话，主动刷新时用新结果替换列表，并保存新的 `session_id`。

`more` 返回空 `items` 时，含义是这一次没有读到新卡片。当前协议没有页数、总数或永久结束标记。设备端按笔记 ID 去重。服务保留最近 300 个已发送笔记的详情定位信息，过早的列表项可能返回 `note_not_found`。

### 笔记详情与图片

请求详情

```json
{
  "request_id": "req-detail-003",
  "action": "detail",
  "session_id": "edb05ea4e8e64ad7bc07d99f9886d959",
  "note_id": "123456789abcdef012345678"
}
```

成功结果

```json
{
  "job_id": "a52ad03d9a484e7c85964ffb57d04847",
  "status": "done",
  "result": {
    "id": "123456789abcdef012345678",
    "title": "周末的街角书店",
    "author": "示例作者",
    "body": "第一段正文\n第二段正文",
    "published_at": "2026-11-27T05:35:41+00:00",
    "ip_location": "上海",
    "type": "image_text",
    "counts": {
      "comments": null,
      "favorites": "5",
      "likes": "1.2万+",
      "shares": "0"
    },
    "images": [
      "/v1/media/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.jpg"
    ],
    "image_media": [
      {
        "gray4": "/v1/media/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.gray4",
        "cdn_url": "https://ci.xiaohongshu.com/example-photo?imageView2/format/jpeg"
      }
    ]
  }
}
```

| 字段 | 类型和显示方式 |
| --- | --- |
| `body` | 字符串，可能为空。正文换行应保留 |
| `published_at` | ISO 8601 UTC 时间或 `null` |
| `ip_location` | 笔记公开展示的 IP 属地或 `null`。详情末尾有属地时显示「时间 · 属地」，缺少属地时显示时间 |
| `type` | 当前成功详情使用 `image_text` |
| `counts.comments`、`favorites`、`likes`、`shares` | 依次为评论、收藏、点赞、分享数量，类型为字符串或 `null`。显示服务返回的字符串，例如 `"0"`、`"26"`、`"1.2万+"`。`null` 表示数量未知，测试页显示 `—` |
| `images` | 依原顺序返回的群晖 JPEG 路径数组，可能为空，最多 32 个 |
| `image_media` | 图片的灰度路径与直连地址。顺序和 `images` 相同，按下标匹配 |

阅读服务调用群晖的 XHS-Downloader `POST /xhs/detail` 查询单篇笔记，使用 `download: false` 和 `check_record: false`。该操作读取详情并登记静态图片地址。`cover` 和 `images` 对应的 JPEG 供网页测试和兼容模式使用，缩放至最长边 1400 像素。群晖第一次收到某一图片规格的请求时会获取并转码源图，上游请求有 10 秒超时与 16 MiB、4000 万像素限制。JPEG 和灰度分别缓存约 400 个文件，超过上限时清理较早生成的文件。灰度缓存按图片 ID 和请求尺寸区分。

若 XHS-Downloader 未提供 IP 属地，阅读服务用已登录的 Chromium 在独立标签页读取该笔记网页数据，导航超时为 4 秒。读不到属地时，`ip_location` 返回 `null`，详情仍可显示。作者在笔记上主动标记的餐厅、景点等地点标签属于另一项数据。

XHS-Downloader 返回空详情或笔记 ID 不匹配时，服务尝试从已登录浏览器打开同一篇笔记，核对笔记 ID 后读取可见详情。若网页要求登录或验证，返回 `login_required`。若网页返回 HTTP 429，返回 `detail_rate_limited`。

图片请求与详情任务独立。单张图失败时可以继续显示正文和其他图片。应用可优先加载当前可见图片。未缓存的媒体来源随服务重启丢失，收到 `image_missing` 时可重新获取当前笔记或列表。

### 群晖压缩 4 位灰度

`cover_media.gray4` 和 `image_media[].gray4` 是群晖相对路径。设备可加 `?width=320&height=240` 请求小封面，或按详情图片实际显示框传入宽高。两个参数分别允许 `1–1400`，默认宽 `684`、高 `1216`。服务按 EXIF 方向校正，保持长宽比缩入边界，较小的源图按原尺寸返回。设备按响应中的实际宽高显示。

服务先处理缩放，再把 8 位亮度的高 4 位用作 16 级灰度值。响应为 `application/octet-stream`，文件结构如下。

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 4 字节 | ASCII 魔数 `G4Z1` |
| 4 | 2 字节 | 实际图像宽度，无符号大端整数 |
| 6 | 2 字节 | 实际图像高度，无符号大端整数 |
| 8 | 剩余字节 | zlib 数据流，解压后是从上到下排列的 4 位灰度行 |

每行字节数为 `(width + 1) / 2` 向下取整。前一个像素在高半字节，后一个在低半字节。`0` 为黑、`15` 为白。宽度为奇数时行末低半字节填白。设备按实际尺寸校验分配上限，解压结果应恰好为 `stride × height` 字节，然后交给 4 位灰度缓冲的渲染路径。[MyPaper 灰度缓冲布局](../main/renderer/TurboJpegDecoder.cpp)

### CDN 直连与效率对比

`cdn_url` 由服务从图片来源提取，并保留上游路径和查询参数。XHS-Downloader 提供 `?imageView2/format/jpeg` 时，直连会使用这个 JPEG 版本。上游提供裸地址时，直连使用该地址。演示模式的 `cdn_url` 为 `null`。设备直连时用 CDN 的 HTTPS 地址，设备令牌只用于群晖 API。图片响应可随来源返回 JPEG、PNG 或 HEIC 等格式。设备依据媒体类型和文件头选择解码器。当前后端支持的 CDN 主机为 `ci.xiaohongshu.com` 和 `*.xhscdn.com`。

MyPaper 的图片加载设置可提供「群晖灰度」和「CDN 直连」。日常浏览可让直连失败或格式无法解码的图片回退到群晖媒体；对比效率时记录实际命中路径，避免把回退样本算作直连。两种模式使用同一张图、同一显示框和同一网络，分别记录 HTTP 下载字节、首图显示时间及失败，群晖灰度区分首次转码和缓存命中。

测试样本为此前同组四张 CDN 图。缩入 `684 × 1216` 后，8 位灰度 JPEG 共约 326 KiB，4 位灰度打包后用 zlib 压缩共约 167 KiB。CDN 提供的原尺寸 JPEG 共约 530 KiB。上述数据量来自这组样本，真机联调可继续比较清晰度和耗时。

### 错误和恢复

阅读服务在 HTTP 层用 JSON 的 `detail` 字段返回错误信息。任务执行失败时，轮询仍返回 HTTP `200`，机器可读错误位于 `status: "error"` 对应的 `error` 字段。

例如登录失效时，轮询结果为

```json
{"job_id":"4fd6922235c940389607051edf2fc734","status":"error","error":"login_required"}
```

| 错误 | 来源 | 阅读器处理 |
| --- | --- | --- |
| `invalid_token` | HTTP `401` | 提示检查设备令牌 |
| `request_id_conflict` | HTTP `409` | 分配新的请求 ID 再发起新操作 |
| `queue_full` | HTTP `429` | 短暂退避，避免连续重复提交 |
| `job_missing` | 轮询 HTTP `404` | 按当前界面重新发起操作，服务重启时重新打开推荐 |
| `login_required` | 任务错误 | 提示在群晖的浏览器控制台扫码或完成验证，然后重新打开推荐 |
| `session_expired` | 任务错误 | 重新执行 `open`，以新列表和会话恢复 |
| `note_not_found` | 任务错误 | 回列表并从当前会话选笔记，必要时重新打开推荐 |
| `feed_unavailable`、`browser_start_failed`、`browser_navigation_failed` | 任务错误 | 显示推荐读取失败，允许用户重试 |
| `downloader_unavailable`、`downloader_timeout`、`downloader_auth_required`、`downloader_route_missing`、`downloader_http_error`、`downloader_response_invalid` | 任务错误 | 保留推荐列表和当前选中项，提示详情来源异常并允许重试 |
| `detail_unavailable`、`unsupported_note_type` | 任务错误 | 提示当前笔记详情不可用或类型暂不支持 |
| `detail_rate_limited` | 任务错误 | 提示网页详情暂时受限，请稍后再试 |
| `image_missing`、`image_unavailable` | 媒体 HTTP `404` 或 `502` | 对单张图片显示占位，可重新获取对应详情或重试图片 |

其他 HTTP 或任务错误可用通用的失败提示处理。`413` 代表提交的内容长度缺失或超限，`422` 表示请求字段未通过验证。连接、DNS、TLS 和客户端等待超时也可能发生在收到服务响应之前。应用日志适合记录接口路径、状态码和错误名，设备令牌应留在配置和请求头里。[任务和 HTTP 接口实现](../tools/xhs_reader_server/app.py)

## 内容适配规则

- 图文笔记支持标题、作者、正文、按顺序读取的静态图片和四项互动数量。
- 视频卡片在推荐提取阶段按网页类型过滤。页面回退提取也会跳过带视频或播放标记的卡片。详情阶段识别出 `视频` 或 `图集` 时返回 `unsupported_note_type`。
- 按图文发布的动态照片会取 XHS-Downloader `下载地址` 中的静态图。`动图地址` 用于标识上游动态内容，以上图片路径均面向静态画面。
- 设备在推荐卡片和详情作者行显示只读点赞数。服务接口与电脑测试页也提供评论、收藏和分享的数量。
- 小红书网页版的登录状态或页面结构变化会影响推荐读取。服务识别到登录或验证提示时返回 `login_required`，由用户在群晖控制台处理。[推荐卡片提取](../tools/xhs_reader_server/cards.js) · [推荐、详情和媒体实现](../tools/xhs_reader_server/reader.py)

## MyPaperS31 设备端使用

1. 在「设置 → 应用设置 → 小红书」填写完整 HTTPS 服务地址和设备令牌。可先测试连通性，再保存配置。图片方式默认为「群晖灰度」，也可选「CDN 直连」。
2. 从主页第二页打开小红书。推荐每屏双列三行，显示封面、标题、作者和点赞数。首屏下拉刷新，向后滑到末尾时加载更多。若本批为空或只有已显示的笔记，客户端自动刷新一次推荐，成功后回到新列表首屏。
3. 点卡片进入详情。页面先绘制作者、点赞、标题和正文，后台逐张加载相册图片。正文之后显示发布时间和可选 IP 属地。左右滑动可切图，双击进入全屏，相册在全屏也可切换。再次双击恢复正文并保留当前图片序号。
4. 正文相册按 636 × 424 像素的显示框加载。全屏先放大已有图片，再按 684 × 1216 像素的屏幕尺寸请求清晰图。当前推文的解压图片共用 4 MiB PSRAM 缓存，含正在显示的图片。缓存满时释放最久未看的图，退出详情时释放全部。
5. 返回推荐列表时恢复当前页。服务端只维护一个推荐会话，若 PC 测试页重新打开推荐，设备可通过提示重新加载。设备请求验证 HTTPS 证书和主机名，设备令牌用于阅读服务的任务和媒体接口。

[设备应用源码](../main/activities/XhsReaderActivity.cpp) · [设备网络与存储](../main/xhs/XhsReader.cpp)

### 联调与验收

| 检查 | 当前情况或通过条件 |
| --- | --- |
| 个人推荐来源 | Windows 实时测试中推荐与已登录小红书网页一致，按需滚动可读到新笔记 |
| 真实图文详情 | 在群晖的 XHS-Downloader 验证了带正文和图片的笔记详情，本机容器验证了阅读接口和页面图片加载 |
| 局域网及外网服务 | 群晖阅读服务已通过外网测试，外网入口使用现有 SakuraFrp 隧道 |
| 四项互动数量 | 本机更新版验证了数量转换、零和未知的显示，群晖上实测确认新笔记返回评论数 |
| MyPaper 真机连通 | 在目标设备上输入外网 HTTPS 地址和令牌，从非家庭网络打开推荐 |
| MyPaper 连续阅读 | 验证加载更多去重、空批次自动刷新一次、返回列表恢复位置，以及全屏切图和回看图片时命中缓存 |
| MyPaper 图片路径对比 | 切换群晖灰度与 CDN 直连，通过同图实测对比传输体积、首图时间和显示效果 |
| MyPaper 异常恢复 | 验证错误令牌、群晖重新登录、服务重启导致的会话失效，以及单张图片加载失败 |

服务源码和测试网页位于 `tools/xhs_reader_server/`。离开群晖环境时可在项目根目录用 PowerShell 运行 `.\tools\xhs_reader_server\start-windows.ps1`，使用本机样例推荐、正文、图片及互动数量做开发联调。演示模式和实时模式共享同一客户端协议。[本机启动脚本](../tools/xhs_reader_server/start-windows.ps1) · [浏览器测试客户端](../tools/xhs_reader_server/ui/app.js)
