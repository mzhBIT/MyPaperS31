# 高德地图应用设计方案

## 1. 目标

在 MyPaperP4 上实现一个适合墨水屏交互的地图应用。首版使用高德 Web
服务 API，不引入 Android、iOS 或 JavaScript 地图 SDK，也不依赖浏览器运行时。

首版目标：

- 显示以指定坐标为中心的地图；
- 支持滑动平移和按钮缩放；
- 支持地点输入提示、POI 搜索和结果定位；
- 支持驾车、步行、骑行、电动车和公交路线规划；
- 缓存最近使用的地图和查询结果；
- 地图活动内保持 Wi-Fi 可用，退出地图活动时完整关闭 Wi-Fi/C6；
- 网络、解码和 EPD 刷新不互相争用内部 RAM；
- API Key 不写入固件常量、SD 卡缓存、URL 日志或错误日志。

首版不做：

- 连续动画、惯性滚动和逐帧缩放；
- 离线地图包和完整瓦片地图引擎；
- 实时导航、语音导航和后台持续定位；
- 轨迹记录、轨迹纠偏及公交站点详情；
- 未接入定位硬件时的精确当前位置。

## 2. 当前条件与约束

### 2.1 可复用能力

项目已经具备以下基础设施：

- `Activity`/`ActivityManager` 页面框架；
- ESP-Hosted Wi-Fi 的连接、复用和完整断电；
- NVS 配置持久化；
- SD 卡文件、临时文件和应用缓存目录；
- JPEG、渐进式 JPEG、PNG 解码；
- 中文 26 键和九宫格输入浮层；
- 1216×684 的 4 bpp 显示缓冲；
- `epd_quality`、`epd_text` 等 2D 波形刷新模式。

### 2.2 硬件与交互约束

- 当前设备没有 RTC，HTTPS 前需要沿用现有 SNTP 校时能力；
- 当前设备没有 GNSS，IP 定位只能提供初始城市或粗略位置；
- 墨水屏不适合跟手动画，手指移动期间只画轻量提示，松手后请求并刷新新地图；
- 地图属于灰阶图片，完整地图更新使用 `epd_quality`；
- EPD Quality 帧生产、网络 TLS 和图片解码都可能占用较多内存，不允许三者并行；
- EPD 刷新专用核心不承担 HTTP、JSON、缓存和图片解码任务。
- 个人开发者账号的接口上限为 3 QPS。应用内部采用更保守的全局 2 QPS
  上限，任意两次高德请求的启动时间至少间隔 500 ms。

## 3. 文档与接口选择

本设计基于 `docs/map` 中保存的高德 Web 服务 API 文档。优先使用较新的
v5/2.0 接口；只有尚无 v5 替代或静态地图要求时使用 v3。

| 能力 | 接口 | 用途 |
| --- | --- | --- |
| 静态地图 | `GET /v3/staticmap` | 获取地图底图、标记和简单路线 |
| 地理编码 | `GET /v3/geocode/geo` | 地址转换为坐标 |
| 逆地理编码 | `GET /v3/geocode/regeo` | 坐标转换为地址和附近 POI |
| 输入提示 | `GET /v3/assistant/inputtips` | 搜索框候选项 |
| POI 关键字搜索 | `GET /v5/place/text` | 名称、地址和类型搜索 |
| POI 周边搜索 | `GET /v5/place/around` | 搜索当前地图中心附近地点 |
| POI 详情 | `GET /v5/place/detail` | 查询选中 POI 的详细信息 |
| 路径规划 | `GET /v5/direction/*` | 驾车、步行、骑行、电动车、公交 |
| 普通 IP 定位 | `GET /v3/ip` | 获取城市和城市矩形范围 |
| 高级 IP 定位 | `GET /v5/ip/location` | 在权限允许时获取更具体的位置 |
| 行政区域 | `GET /v3/config/district` | 城市选择和行政区边界 |
| 天气 | `GET /v3/weather/weatherInfo` | 后续可作为地点信息扩展 |

坐标格式统一为 `longitude,latitude`，小数点后最多 6 位。国内高德接口返回和
使用 GCJ-02 坐标。以后接入输出 WGS-84 的 GNSS 时，必须先执行坐标转换。
当前本地文档中没有独立保存坐标转换 API 文档，接入 GNSS 前需要补齐。

## 4. 产品结构

### 4.1 页面层级

```text
Home
└── MapActivity
    ├── Map view
    ├── Search overlay
    │   └── Search result list
    ├── POI detail overlay
    └── RouteActivity
        ├── Start/destination selector
        ├── Travel mode selector
        ├── Route overview
        └── Route step list

Settings
└── Map settings
    ├── AMap Web Service Key
    ├── Default city/location
    └── Clear map cache
```

### 4.2 地图主页面草图

```text
┌──────────────────────────────────────────────────────────────┐
│ <  地图                         [搜索] [路线]                 │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│                       静态地图画面                           │
│                                                              │
│             POI 标记、路线和中心点覆盖物                    │
│                                                              │
│                                                     ┌─────┐  │
│                                                     │  +  │  │
│                                                     ├─────┤  │
│                                                     │  -  │  │
│                                                     └─────┘  │
├──────────────────────────────────────────────────────────────┤
│ 当前地址 / 加载状态                              [定位中心] │
└──────────────────────────────────────────────────────────────┘
```

地图主体尽量占满屏幕。标题栏采用项目统一的白底黑字。加载提示覆盖在底部状态区，
不新建全屏等待页面。

### 4.3 手势

- 单击 POI 标记：显示简要详情；
- 向任意方向滑动：松手后计算新中心点并请求地图；
- 点击 `+`/`-`：每次改变一级 zoom 并请求地图；
- 左上角：退出地图并关闭 Wi-Fi/C6；
- 搜索结果页上下滑动翻页；
- 地图请求进行中仍允许返回，取消后丢弃迟到的响应。

不使用双指缩放作为首版必需交互，以免触摸识别和墨水屏反馈不一致。

## 5. 显示与视口

### 5.1 静态地图请求尺寸

面板物理扫描尺寸为 1216×684，但当前 Activity 使用 684×1216 竖屏逻辑坐标。
高德静态地图 `size` 最大为 1024×1024，且
`scale=2` 返回宽高均放大两倍。因此首选请求：

```text
size=342*608&scale=2
```

目标响应尺寸为 684×1216，可减少设备端缩放。开发阶段必须用实际响应头和图片
尺寸验证 `scale=2` 对 zoom 的具体行为。若高德服务策略变化，则回退为不超过
1024×684 的图片并在设备端等比缩放、居中裁剪。

静态地图参数上限：

- zoom：1～17；
- 标记：最多 10 个；
- 标签：最多 10 个；
- 折线/多边形：最多 4 个。

### 5.2 平移计算

内部保存逻辑视口：

```cpp
struct MapViewport {
    double longitude;
    double latitude;
    uint8_t zoom;
};
```

平移按 Web Mercator 像素空间计算：

```text
world_size = 256 × 2^zoom
x = (longitude + 180) / 360 × world_size
y = (1 - ln(tan(latitude) + sec(latitude)) / π) / 2 × world_size
```

手指滑动 `(dx, dy)` 后，地图内容跟随手指，因此新中心像素为：

```text
center_x' = center_x - dx / response_scale
center_y' = center_y - dy / response_scale
```

再反算经纬度。纬度限制在 Web Mercator 有效范围约 ±85.051129°，经度跨越
±180°时循环。`response_scale` 和高德 `scale=2`/zoom 的对应关系必须通过设备
上的标尺测试确认，不能只依赖文档描述。

为了避免轻微误触，滑动距离低于阈值时按单击处理。连续滑动只保留最后一个目标
视口，中间尚未开始的请求直接合并。

### 5.3 覆盖物

首版优先让静态地图服务绘制：

- 当前选择点 marker；
- 起点、终点 marker；
- 较短的路线 path。

路线坐标过多可能使 URL 超长。路径规划返回的 polyline 应先做抽稀；抽稀后仍然
过长时采用 POST 能力（若该接口支持）或在设备端按 Web Mercator 投影叠加绘制。
设备端绘制覆盖物时必须保留未加覆盖物的底图缓存，避免每次选择 POI 都重新下载。

## 6. 数据流与状态机

### 6.1 状态

```text
Idle
  └── Connecting
        ├── LoadingCache
        ├── Downloading
        │     ├── WritingTempFile
        │     └── Decoding
        ├── ReadyToRefresh
        └── Error

ReadyToRefresh
  └── EpdRefreshing
        └── Idle
```

关键约束：

- 同一时刻最多一个地图 HTTP 请求；
- 所有高德接口共用一个全局请求节流器，连续请求的启动间隔不得小于 500 ms；
- 每个请求携带 generation ID；
- 用户改变视口后，旧 generation 的响应即使成功也不显示；
- HTTP 下载直接流式写入 SD 临时文件，不在内部 RAM 保存整张图片；
- 解码完成并释放网络/TLS 大对象后，才启动 `epd_quality`；
- EPD 刷新期间不启动新的 TLS 握手或图片硬解任务；
- 可以记录下一次请求意图，但延迟执行重资源操作。

### 6.2 首次进入

1. 从 NVS 读取 API Key 和上次视口；
2. API Key 缺失时显示配置引导；
3. 检查是否存在已保存 Wi-Fi；
4. 启动 C6 并连接 Wi-Fi；
5. 优先显示命中的地图缓存；
6. 没有缓存时，使用上次视口、默认城市或 IP 定位建立初始视口；
7. 下载地图、校验响应、解码并 Quality 刷新；
8. 地图活动存续期间复用 Wi-Fi；
9. 退出时取消请求、关闭 HTTP、释放缓存工作区，并调用
   `wifi_setting_deinit(true)` 完整关闭 C6。

### 6.3 搜索

1. 通过共享 `TextInputOverlay` 输入关键字；
2. 用户停止输入一段时间或确认后调用输入提示；
3. 候选项优先限制在当前城市，并携带当前中心点；
4. 选择候选项后，如果已有坐标则直接定位；
5. 没有坐标时调用 POI 2.0 或地理编码；
6. 搜索结果列表每页最多 10 项，避免一次解析过多字段；
7. 选择 POI 后更新地图中心和 marker。

输入提示不应在每次按键后立即发请求。建议 400～600 ms 防抖，同时只保留最新
generation。输入防抖完成后仍须经过全局 500 ms 请求间隔限制，降低 QPS 和无效
刷新。

### 6.4 路径规划

1. 从地图中心、选中 POI 或搜索结果设置起点和终点；
2. 选择出行方式；
3. 调用对应 v5 路径规划接口；
4. 首次只解析总距离、总耗时、主要方案和必要的 polyline；
5. 用户打开详情时再构建 step 列表；
6. 地图总览显示起终点和路线；
7. step 列表使用项目统一的横线分隔和上下滑动翻页。

`show_fields` 只请求实际展示的字段，避免下载和解析无用的大型 JSON。

## 7. 模块划分

```text
main/
├── activities/
│   ├── MapActivity.cpp
│   ├── MapRouteActivity.cpp
│   └── MapSettingsActivity.cpp
└── map/
    ├── AmapConfig.h/.cpp
    ├── AmapHttpClient.h/.cpp
    ├── AmapApi.h/.cpp
    ├── AmapModels.h
    ├── MapViewport.h/.cpp
    ├── MapCache.h/.cpp
    └── MapImage.h/.cpp
```

| 模块 | 职责 |
| --- | --- |
| `AmapConfig` | NVS 中的 Key、默认位置和地图偏好 |
| `AmapHttpClient` | HTTPS、SNTP、超时、取消、流式下载和错误归一化 |
| `AmapApi` | 构建接口参数并解析所需 JSON 字段 |
| `AmapModels` | POI、路线、步骤、地址及错误模型 |
| `MapViewport` | 经纬度、zoom、平移和屏幕投影 |
| `MapCache` | SD 缓存、索引、LRU、校验和原子替换 |
| `MapImage` | 图片探测、解码、裁剪及显示缓冲导入 |
| `MapActivity` | 地图状态、触摸、绘制和请求调度 |

HTTP 层不直接操作屏幕，Activity 不拼接签名和 URL，图片模块不控制 Wi-Fi。
`AmapHttpClient` 负责所有接口共享的请求节流，Activity 和具体 API 层不得绕过。

## 8. 配置与存储

### 8.1 NVS

建议使用 `amap` namespace：

| Key | 内容 |
| --- | --- |
| `api_key` | 高德 Web 服务 Key |
| `last_lon` | 上次地图中心经度 |
| `last_lat` | 上次地图中心纬度 |
| `last_zoom` | 上次 zoom |
| `city_adcode` | 默认城市行政编码 |
| `cache_limit` | 地图缓存上限，后续可配置 |

NVS key 名称必须满足 ESP-IDF 的长度限制。API Key 只能在设置页面以遮罩形式编辑，
日志只允许输出是否已配置和长度，不得输出原值。

### 8.2 SD 卡

```text
/sdcard/.reader/map/
├── index.bin
├── images/
│   ├── <request-hash>.img
│   └── <request-hash>.meta
├── search/
│   └── <query-hash>.json
└── routes/
    └── <route-hash>.json
```

请求哈希由规范化参数生成，必须排除 API Key。`.meta` 保存版本、中心点、zoom、
图片尺寸、内容类型、下载时间和校验值。写入流程为临时文件、校验、rename，掉电后
不把半张图片当作有效缓存。

首版建议地图缓存上限 32 MiB，采用 LRU 清理；搜索结果和路线设置较短 TTL。缓存
清理入口放入 Map settings，也可以后续并入全局清理缓存页面。

## 9. HTTP 与响应校验

所有接口统一使用 HTTPS，并设置连接、首包、总请求和取消超时。普通 JSON 接口
首先检查 HTTP 状态，再检查高德响应中的：

```text
status
info
infocode
```

个人开发者账号按 3 QPS 上限处理。客户端使用单一调度队列，将相邻请求的实际
启动时间强制隔开至少 500 ms，即主动限制为最多 2 QPS，并预留网络重试和服务端
计时窗口误差。静态地图、IP 定位、输入提示、POI、逆地理编码和路径规划共享该
限制，不能按接口分别计时。等待期间如果出现更新的 generation，应删除尚未启动的
旧请求，不能让过期的拖动或输入提示占用后续配额。

静态地图成功时返回图片，失败时可能返回文本或结构化错误。因此不能只依赖扩展名：

1. 检查 HTTP 状态；
2. 检查 `Content-Type`；
3. 检查 JPEG/PNG 文件魔数；
4. 探测实际图片尺寸；
5. 尺寸和格式合法后才写入正式缓存；
6. 错误正文只截取安全的有限长度，不记录包含 Key 的请求 URL。

需要明确处理的错误：

| infocode | 含义 | UI 行为 |
| --- | --- | --- |
| `10001` | Key 错误或过期 | 引导进入 Map settings |
| `10003` | 日调用量超限 | 显示缓存并提示次日恢复 |
| `10004` | 单位时间访问过频 | 延迟重试，不循环刷新 |
| `10021` | 接口 QPS 超限 | 保留当前地图并退避 |

同一个用户动作最多自动重试一次；鉴权错误和配额错误不自动重试。
普通网络错误的重试也必须重新进入全局节流队列，禁止失败后立即发起第二次请求。

## 10. 内存、任务与性能预算

### 10.1 分配原则

- HTTP 响应缓冲、JSON 文本和可放外部内存的任务栈优先 PSRAM；
- TLS/网络驱动必须使用的 DMA/内部对象保留在内部 RAM；
- 图片正文流式写 SD，不分配 1216×684 的压缩文件副本；
- JSON 使用按需字段解析或流式/SAX 解析，不构建完整 DOM；
- 路线 polyline 分批解析，设置坐标点数量上限；
- Quality 刷新开始前释放 HTTP 响应和临时 JSON；
- 复用地图解码和显示工作区，禁止每次平移反复申请大块内存。

### 10.2 调度原则

```text
HTTP/TLS → SD 临时文件 → 关闭响应 → 图片解码 → 释放解码器 → EPD Quality
```

网络工作、解码和 EPD 刷新必须分阶段执行。网络任务不能运行在 EPD 刷新专用核心，
也不能在 Quality 波形生产者创建窗口中抢占内部 RAM。UI 主循环只更新状态和接收
触摸，不执行阻塞式 DNS、TLS、下载或大 JSON 解析。

### 10.3 建议日志

```text
MAP: request begin generation=7 center=... zoom=...
MAP: cache hit key=... age=...
AMAP_HTTP: status=200 type=image/... bytes=... time=...ms
MAP_IMAGE: probe=... decode=... import=... total=...ms
MAP: refresh begin mode=quality internal_free=... largest=...
MAP: ready generation=7 total=...ms
```

请求 URL、API Key 和带 Key 的查询字符串不得进入日志。

## 11. 刷新策略

| 场景 | 刷新模式 |
| --- | --- |
| 首次显示地图 | `epd_quality` |
| 平移或缩放后的新地图 | `epd_quality` |
| 路线总览地图 | `epd_quality` |
| 搜索列表、路线步骤列表 | `epd_text` |
| 纯文字错误/设置页面 | `epd_text` |
| 短暂加载状态 | 合并到下一次完整刷新，必要时 `epd_text` |

不要用 `epd_fast`/`epd_fastest` 显示地图灰阶图。地图下载期间保留上一张图，只在
状态区显示 `Loading...`，新图片完整校验后一次更新，避免白屏和失败响应污染画面。

## 12. 功耗策略

地图是连续网络交互应用。为了避免每次平移都重新启动 C6：

- 进入 MapActivity 时连接 Wi-Fi；
- MapActivity 存续期间保持连接；
- 一段时间无操作后可以进入“缓存浏览”状态，但首版不自动断 Wi-Fi；
- 退出 MapActivity 时取消请求并完整关闭 Wi-Fi/C6；
- 离线且缓存命中时允许只查看缓存，不强制启动 Wi-Fi；
- 后续可增加“地图内自动休眠 Wi-Fi”设置。

## 13. 实施顺序

### 阶段 1：配置和最小地图闭环

- `AmapConfig` 与设置页面；
- API Key 输入、保存、清除和遮罩显示；
- `AmapHttpClient` 基础 HTTPS、校时、取消和安全日志；
- 全局 500 ms 请求间隔、generation 合并和限流日志；
- 固定中心点请求静态地图；
- SD 流式下载、图片校验、解码和 `epd_quality` 显示；
- 退出时关闭 Wi-Fi/C6。

验收：配置 Key 后能显示固定地点地图；无 Key、无 Wi-Fi、HTTP 错误均可返回且不
破坏当前画面。

### 阶段 2：视口和缓存

- 保存上次中心点和 zoom；
- `+`/`-` 缩放；
- 四方向滑动平移；
- generation 合并和取消；
- 图片缓存、LRU 和清理入口。

验收：快速连续滑动最终只显示最后目标；退出再进入优先秒开缓存；缓存键不包含 Key。

### 阶段 3：定位和搜索

- IP 粗定位；
- 搜索输入浮层；
- 输入提示、防抖和 POI 2.0；
- 搜索结果列表、POI marker 和逆地理地址。

验收：没有 GNSS 时仍能从默认城市或搜索进入地图；频繁输入不会触发 QPS 风暴。

### 阶段 4：路径规划

- 起点和终点选择；
- 五种出行方式；
- 总距离、耗时、路线方案和步骤列表；
- polyline 抽稀及地图路线显示；
- 路线结果缓存。

验收：路线总览和步骤一致，长路线不会因 URL、JSON 或坐标数量耗尽内存。

### 阶段 5：稳定性与扩展

- 网络切换、超时、掉电和损坏缓存测试；
- 内部 RAM 水位和碎片监控；
- 行政区选择、天气、路况开关；
- 有 GNSS 硬件后补充坐标转换与当前位置；
- 评估是否需要真正的离线瓦片引擎。

## 14. 设备验收清单

1. 未配置 API Key 时进入地图，只显示设置引导且可以返回；
2. 无 Wi-Fi、DNS 失败、TLS 失败和请求超时均不阻塞触摸返回；
3. 初次下载成功后断网重进，可显示最近缓存；
4. 连续快速滑动 10 次，只显示最后一次目标视口；
5. zoom=1 和 zoom=17 时继续缩放不会越界；
6. 静态地图返回 JSON 错误或损坏图片时，不刷新错误画面；
7. 搜索中文地点，输入提示、POI 坐标和地图 marker 一致；
8. 五种路线接口至少各验证一条短路线；
9. 请求失败后内部 RAM、HTTP handle 和临时文件没有持续增长；
10. Quality 刷新期间没有网络任务占用 EPD 核心或造成生产者创建失败；
11. 退出地图后确认 Wi-Fi/C6 完全断电；
12. 日志和 SD 卡中搜索不到完整 API Key。
13. 连续搜索、拖动和缩放时，日志中任意两次高德请求启动时间间隔均不少于
    500 ms，且没有触发 3 QPS 限制。

## 15. 待确认项

进入代码实施前需要最终确定：

- 主页地图图标及名称；
- 首版默认中心坐标已确定为 `120.10148844719319,30.344269032816978`；
- 高德 Web 服务 API Key 的设置入口名称；
- 是否首版就提供路线规划，还是先完成地图与搜索；
- 实机请求 `size=608*342&scale=2` 后的图片尺寸与有效 zoom 行为；
- 高德账号当前已开通的高级 IP 定位及各 v5 API 权限和日配额。

已确认个人开发者 QPS 上限为 3；实现固定采用至少 500 ms 的全局请求启动间隔。
