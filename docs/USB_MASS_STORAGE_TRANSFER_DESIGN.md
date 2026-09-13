# ESP32-P4 USB 大容量存储传输方案

## 1. 结论

在 MyPaperP4 上使用现有 High-Speed USB 口，把 SD 卡作为 USB Mass Storage Class（MSC）设备暴露给电脑是可行的。电脑端会把设备识别为普通可移动磁盘，可以直接复制、重命名、删除和新建文件夹，不再受当前 Wi-Fi 上传页面支持格式和自动分类规则的限制。

但该功能有两个不能绕过的前提：

1. 同一个 HS USB OTG 控制器不能同时充当 USB Audio Host 和 USB MSC Device。进入 U 盘模式前必须完全退出 UAC1/UAC2 Host，退出 U 盘模式后才能恢复 USB 耳机 Host。
2. PC 与设备固件不能同时挂载并修改同一个 FAT 文件系统。SD 卡交给 PC 后，固件必须停止一切 SD 访问并卸载 `/sdcard`；只有 PC 安全弹出后才能重新交还给固件。

因此，一期推荐使用“独立 USB 传输启动模式”：用户确认进入后保存启动标志并重启，系统以最小功能启动为 MSC Device；退出时先要求 PC 安全弹出，再清除标志并重启回正常模式。ESP32-P4 硬件支持运行时切换 Host/Device，但结合本项目已定制的 USB Host/HCD、UAC2 和大量 SD 后台任务，直接热切换的状态复杂度和数据损坏风险明显更高，应放到二期。

官方资料表明 ESP32-P4 的 USB OTG HS 控制器支持 Device 模式及 480 Mbit/s 链路，`esp_tinyusb` 已提供以 SD/MMC 为介质的 MSC 封装：

- [ESP32-P4 USB Device Stack](https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_device.html)
- [ESP32-P4 USB OTG 说明](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_otg.html)
- [esp_tinyusb 组件与 MSC 性能数据](https://components.espressif.com/components/espressif/esp_tinyusb/versions/2.0.1~1/readme?language=en)

## 2. 当前项目条件

| 项目 | 当前实现 | 对 MSC 的影响 |
| --- | --- | --- |
| USB 端口 | ESP32-P4 HS USB Host，支持 UAC1/UAC2 | 进入 MSC 前必须销毁音频流、UAC client 和 Host library |
| SD 卡 | SDMMC Slot 0，4-bit，High Speed，挂载到 `/sdcard` | 很适合作为 MSC 块设备，但必须做所有权切换 |
| 文件系统 | FAT，应用层直接使用 POSIX/FatFs 文件 API | PC 占用期间所有文件 API 都必须被禁止 |
| SD 内容 | 书籍、音乐、图片、字体、输入法和 `.reader` 缓存 | MSC 会暴露整个卷，包括隐藏缓存目录 |
| Wi-Fi Transfer | HTTP 上传、下载和远程文件管理 | 保留，作为无需数据线且可自动分类的传输方式 |
| 启动顺序 | SD 初始化早于显示初始化，用于配置共享 VDD_IO_5 电源域 | MSC 启动模式仍需先初始化 SD 电源和卡，但不挂载 VFS |

`main/storage/storage_sdcard.c` 目前使用 `esp_vfs_fat_sdmmc_mount()` 一次完成 SDMMC 初始化与 FAT 挂载。MSC 模式需要把它拆成“介质初始化”和“应用文件系统挂载”两个层次，确保同一个 `sdmmc_card_t` 在任意时刻只有一个所有者。

## 3. 硬件前置检查

芯片支持 Device 模式不等于板级 USB 口必然可以作为 Device 使用。正式编码前必须核对原理图或实测以下项目：

- HS 口的数据线确实连接 ESP32-P4 的 HS OTG 控制器与 HS PHY。
- USB-C CC 电阻/角色控制允许设备端呈现 UFP/Device 身份；若接口被硬件固定为 Host，单靠软件不能让 PC 枚举它。
- Device 模式下不会继续向 VBUS 输出 5 V，避免与 PC 的 VBUS 对灌。
- 若需要自动识别插拔，应能检测 PC VBUS；一期采用用户手动进入/退出模式时，可不增加 VBUS 检测 GPIO。
- 使用支持数据的 USB 线，且接口接到电脑时无需 OTG Host 转接头。

这是整个方案唯一可能造成“芯片可行但现有 PCB 不可行”的硬门槛。应先做一个不接 SD 卡、只枚举 MSC 描述符的最小固件验证。如果电脑能以 High-Speed 枚举，再进入完整实现。

### 3.1 当前开发板四线 HS 接口的建议接法

当前开发板由独立 Type-C 口供电，HS USB 接口的 5 V 又由该电源经 AO3401 与 MMDT3906 输出，因此整机属于自供电 USB Device。外部接口仍然只需要 `VBUS、D-、D+、GND` 四根线。若未来需要自动识别插拔，VBUS 在板内应分成“Host 供电输出”和“Device VBUS 检测”两条路径：

```text
                         Host 模式才使能
板载 Type-C 5 V ---- 反向截止电源开关 ----+---- HS 口 VBUS ---- PC/USB 外设
                                          |
                                          +---- 分压器/比较器 ---- 空闲 GPIO（VBUS valid）

ESP32-P4 HS D- -------------------------------- HS 口 D-
ESP32-P4 HS D+ -------------------------------- HS 口 D+
开发板 GND ------------------------------------ HS 口 GND
```

Device 模式下：

- `D+`、`D-`、`GND` 正常连接电脑。
- 电脑提供的 VBUS 只能进入高阻分压器或比较器，再送入一个 3.3 V GPIO。
- 板载 Type-C 继续给整机供电。
- AO3401/MMDT3906 所在的 Host 5 V 输出路径必须关闭并具备反向截止，不能把电脑 VBUS 与板载 5 V 硬并联。

Host 模式下：

- AO3401/MMDT3906 电路向接口 VBUS 提供 5 V。
- USB Host 栈运行，连接 USB 耳机。
- Device TinyUSB 栈与 VBUS attach 不启用。

一期确定使用手动模式，因此可以将原来四根引线中的“板载 VCC 5V 输出线”悬空，只连接板端 `D-、D+、GND` 到电脑。用户先连接数据线，再从 Transfer 页面选择 USB 传输，固件主动安装 TinyUSB Device；传输结束后在 PC 安全弹出，再由用户点击退出。不要把电脑 VBUS 接回原来的板载 5 V 输出线。

在这个手动方案中，不设置 `phy.self_powered` 和 `phy.vbus_monitor_io`，也不依赖 VBUS 自动触发 attach/detach。TinyUSB 的 mount、unmount、SCSI eject 和传输事件仅用于更新状态与确认 PC 行为，不用于自动切换 Host/Device。代价是拔线事件与 USB suspend 不一定能可靠区分，用户必须遵守“安全弹出后手动退出”的流程；再次连接异常时通过退出并重启 USB 传输模式恢复。

以下 VBUS 分压电路作为未来自动热插拔的可选升级，不列入一期硬件要求。

推荐的简单分压器为：

```text
PC VBUS ---- 100 kΩ ----+---- GPIO_VBUS_SENSE
                        |
                       150 kΩ
                        |
                       GND
```

该比例在 VBUS=4.4 V 时输出约 2.64 V，在 5.25 V 时输出约 3.15 V，符合 3.3 V GPIO 的电压范围以及官方对自供电设备 VBUS 高电平检测的建议。使用 1% 电阻；布线较长或环境干扰大时优先换成带迟滞的比较器。ESP32-P4 HS Device 使用该 GPIO 时，需要在 TinyUSB 配置中设置 `phy.self_powered=true` 和 `phy.vbus_monitor_io=<GPIO>`，并按 `esp_tinyusb` 要求在安装驱动前安装 GPIO ISR service。

AO3401 是 P 沟道 MOS 管，单管关断不等于双向隔离。能否复用现有 AO3401+MMDT3906 电路，取决于 AO3401 的源漏方向及 MMDT3906 的实际连接。正式连接 PC 前必须用原理图确认；若没有反向截止能力，应增加带 reverse-current blocking 的 USB load switch，或采用背靠背 MOS，而不是仅修改控制 GPIO。

## 4. 推荐架构

### 4.1 一期：重启隔离的 MSC 模式

正常模式与 USB 传输模式使用同一固件，通过 NVS 启动标志选择启动路径：

```text
正常模式
  -> Transfer 页面选择 USB
  -> 提示“USB 耳机将断开，进入后设备不能读取 SD 卡”
  -> 停止音频/网络/文件任务并保存状态
  -> 写入 boot_mode=usb_msc
  -> 重启

USB MSC 模式
  -> 初始化 NVS
  -> 初始化 SD 电源、SDMMC Host 和 sdmmc_card_t，不挂载 /sdcard
  -> 初始化最小显示与触摸界面
  -> 安装 TinyUSB Device + MSC，SD 所有权交给 PC
  -> 显示连接、读写、已弹出或错误状态
  -> PC 安全弹出后允许用户退出
  -> 清除 boot_mode
  -> 重启回正常模式并重新挂载 /sdcard
```

选择重启隔离的原因：

- 当前 USB Audio Host 的销毁涉及播放器、UAC stream、device、client、Host library 多层状态，历史上已有卸载超时和残留 Host 状态问题。
- 阅读器、音乐、RSS、微信读书、DeepSeek、缩略图和预取任务都可能保留 SD 文件句柄；重启是最可靠的统一关闭屏障。
- MSC 模式不需要启动 Wi-Fi、GMF、UAC、字体缓存、图片缓存和完整 Activity 栈，内部 RAM 更充足，故障面更小。
- PC 对 FAT 的独占天然跨越整个 MSC 会话，不会因 Activity 切换意外恢复本地访问。

切换时间会多出一次启动，但 USB 传输通常是低频、长会话操作，可靠性比节省一两秒更重要。

### 4.2 二期：不重启的动态角色切换

ESP32-P4 官方支持通过卸载 Host/Device 驱动动态切换 OTG 角色，但只有一期稳定后才建议实现。运行时切换必须增加全局资源协调器，并满足：

- USB Audio Host 确认完全卸载，而不是只停止播放。
- 所有 SD 使用者注册 shutdown hook，并在超时内关闭文件和停止任务。
- 全局 storage lease 归零后才能卸载 FAT。
- TinyUSB 卸载后确认端点、PHY 和中断资源全部释放，才能重新安装 Host。
- 任一步失败都不能继续强行切换，应提示重启恢复。

二期可以缩短切换时间，但不改变 Host/Device 互斥和 SD 独占这两个基本约束。

## 5. 模块划分

建议新增以下模块，避免把 USB Device 逻辑塞进现有 Wi-Fi Activity 或 USB Audio 文件：

| 模块 | 职责 |
| --- | --- |
| `main/usb/usb_msc_mode.c/.h` | 启停 TinyUSB Device、MSC 描述符、连接/弹出状态、错误恢复 |
| `main/storage/storage_sdcard.c/.h` | 拆分 SD 介质初始化、APP 挂载、APP 卸载和介质释放 |
| `main/storage/storage_ownership.cpp/.h` | 一期先记录 APP/USB 所有权并拒绝非法访问；二期扩展为 lease/barrier |
| `main/activities/UsbTransferActivity.cpp/.h` | 正常模式中的确认入口，以及 MSC 启动模式的最小状态页面 |
| `main/boot_mode.c/.h` | NVS 启动标志、失败次数和强制恢复逻辑 |

建议的存储接口：

```c
esp_err_t storage_sdcard_init_medium(sdmmc_card_t **card);
esp_err_t storage_sdcard_mount_app(void);
esp_err_t storage_sdcard_unmount_app(void);
esp_err_t storage_sdcard_release_medium(void);
bool storage_sdcard_app_accessible(void);
```

正常模式继续对外保留现有 `storage_sdcard_mount()`，其内部组合 `init_medium + mount_app`，避免一次性修改所有调用者。

## 6. USB MSC 实现方式

### 6.1 组件与配置

在 `main/idf_component.yml` 增加与当前 ESP-IDF 5.3.5 兼容的固定版本 `espressif/esp_tinyusb`，不使用无上限的浮动版本。启用：

- `CONFIG_TINYUSB_MSC_ENABLED=y`
- ESP32-P4 High-Speed Device 端口
- MSC 缓冲区先设为 32 KiB
- TinyUSB 服务任务使用内部 RAM 栈；大块、非 DMA 元数据优先放 PSRAM

现有 `components/usb/` 是项目定制的 Host/HCD 组件，加入 `esp_tinyusb` 后必须先做依赖和符号冲突检查。Host 与 Device 不会在运行时同时安装，但构建期仍可能引用同名底层 USB 组件；若冲突，应保留项目已验证的 Host 修改并把 Device 依赖所需源文件合并进去，不能直接用组件管理器覆盖现有 `components/usb/`。

### 6.2 描述符

一期只暴露一个 MSC interface 和一个 LUN：

- Product：`MyPaperP4 SD Card`
- Manufacturer：`MyPaperP4`
- Serial：从芯片 MAC 派生并保持稳定，避免 Windows 每次建立新设备记录
- Interface：MSC / SCSI transparent / Bulk-Only Transport
- Full-Speed 与 High-Speed 配置描述符都提供，实际应枚举为 High-Speed

不要一期就加入 CDC、MTP 或厂商自定义接口。复合设备会增加端点、驱动和兼容性变量，不利于先验证 SD 可靠性和速度。

### 6.3 SD 块设备后端

优先使用官方封装：

```c
tinyusb_msc_storage_config_t cfg = {
    .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    .medium.card = card,
};
tinyusb_msc_new_storage_sdmmc(&cfg, &storage_handle);
```

它直接通过 `sdmmc_card_t` 提供扇区读写，避免自行实现整套 SCSI 命令。只有在性能或安全弹出回调无法满足需求时，才下沉到 `tud_msc_read10_cb()`、`tud_msc_write10_cb()` 等 TinyUSB 原始回调。

所有 SD 读写缓冲必须满足 SDMMC DMA 对齐要求。可以复用当前 Wi-Fi Transfer 已使用的 `esp_dma_utils` 分配策略，但不要让 EPD 或其他任务共享这块缓冲。

## 7. 文件系统所有权与数据安全

### 7.1 单一所有者规则

```text
APP_OWNED  : /sdcard 已挂载；PC 必须看到介质未就绪
TRANSITION : 禁止新文件访问；等待现有访问退出
USB_OWNED  : /sdcard 未挂载；PC 可读写原始 FAT 卷
EJECTED    : PC 已安全弹出；等待交还 APP 或重启
ERROR      : 两端都不挂载；只允许重试或重启
```

绝对禁止 APP 与 PC 同时访问。PC 写 FAT 表和目录项时，设备端的 VFS 缓存完全不知情；即使双方修改不同目录，也可能造成簇链、空闲空间计数和目录项损坏。

### 7.2 进入前处理

一期通过重启自然关闭绝大多数文件句柄，但进入前仍应主动完成：

1. 保存阅读进度、音量和设置。
2. 停止 Music/Radio，并退出 USB Audio Host。
3. 停止 Wi-Fi 下载、RSS 图片、地图、微信读书和 DeepSeek 写盘。
4. 结束封面、图片、字形和页面预取任务。
5. 对需要持久化的文件调用 `fflush`/`fsync` 或完成原子 rename。
6. 设置启动标志并重启。

MSC 启动分支不能调用 `app_paths::ensure_data_dir()`、`reader_font_init()` 或任何读取 `/sdcard` 的初始化逻辑。

### 7.3 安全弹出

设备页面必须明确显示：

> 传输完成后，请先在电脑上安全弹出 MyPaperP4 SD Card，再点击退出。

USB 拔线、TinyUSB unmount 和“操作系统已完成所有缓存写入”不是完全等价的事件。优先监听 SCSI eject/START STOP UNIT 或 `esp_tinyusb` 的 storage mount change 事件；收到安全弹出后把状态改为 `EJECTED`。如果用户未安全弹出就拔线，只能提示“上次可能未安全移除”，下次挂载失败时不得自动格式化。

退出按钮的规则：

- 已收到安全弹出：直接允许退出并重启。
- 仍连接且未弹出：弹出二次确认，不直接重新挂载。
- 已意外断线：允许重启，但记录 dirty-removal 标志并在正常启动时报告风险。

项目必须继续保持 `format_if_mount_failed=false`。任何挂载失败都不能自动格式化用户 SD 卡。

### 7.4 PC 端格式限制

MSC 暴露的是整张卡，不是经过过滤的虚拟目录。用户能看到并修改 `.reader` 缓存，也能删除所有书籍或格式化磁盘。界面和 README 应提示：

- 保持当前 FAT32 文件系统；除非固件明确增加支持，不要格式化为 NTFS、APFS 或其他文件系统。
- PC 生成的 `System Volume Information`、`.Spotlight-V100` 等目录可以忽略。
- U 盘模式不执行 Wi-Fi Transfer 的自动分类；用户应自行放入 `books`、`music`、`images`、`fonts`，输入法词库放入 `.reader/system/ime`。

## 8. UI 设计

主页 `Transfer` 保持不变，进入后先选择：

```text
Transfer

USB Transfer
Connect to a computer as an SD card

Wi-Fi Transfer
Upload and manage files in a browser
```

进入 USB 模式前显示确认页：

```text
USB Transfer

The SD card will be unavailable to the device.
USB headphones will be disconnected.

[Cancel]                 [Restart & Enter]
```

MSC 模式使用内置字体绘制静态页面，不依赖 SD 字体：

```text
USB Transfer

Connected / Transferring / Safely ejected
MyPaperP4 SD Card

Safely eject the drive on your computer
before leaving this mode.

[Exit USB Transfer]
```

为避免读写期间误导和 EPD/USB/SD 并发压力，状态更新只在连接状态发生变化时刷新，不显示每个扇区或每个文件的实时进度。

## 9. 性能预期与优化顺序

USB 2.0 HS 的 480 Mbit/s 是总线信号速率，不是最终文件复制速度。实际瓶颈包括 TinyUSB MSC 单缓冲、SDMMC、FAT/操作系统缓存和小文件元数据操作。

Espressif 公布的 ESP32-P4 MSC + SD 卡参考数据中，32 KiB 缓冲约为 6.0 MB/s 读取、4.5 MB/s 写入。因此一期目标应设为：

- 正确枚举为 High-Speed，而不是退化为 Full-Speed。
- 32 KiB 单缓冲下，大文件连续读取不低于 4 MB/s，连续写入不低于 3 MB/s。
- 连续传输 4 GiB 或卡容量允许的更大测试文件无断连、无 I/O error。
- 安全弹出后 FAT 能被设备重新挂载，抽检文件哈希一致。

优化顺序：

1. 确认 High-Speed 枚举、512-byte sector 和 SDMMC 4-bit/40 MHz 生效。
2. 使用 32 KiB MSC buffer 建立基线，同时记录内部 RAM、PSRAM 和吞吐。
3. 测试 8/16/32/64 KiB，选择收益与内部 RAM 占用的平衡点。
4. 增加顺序扇区合并、DMA 对齐和减少复制；不要先改 SCSI 层。
5. 分别测试大文件与大量小文件，避免只看缓存后的瞬时速度。

MSC 是 Bulk 传输，不像 UAC2 等时传输那样要求每 125 us 供数，因此不需要复制现有 UAC2 的高优先级/IRAM 策略。TinyUSB/HCD 的必要中断和 DMA 缓冲保留内部 RAM即可，管理任务和非实时状态可放 PSRAM。避免把 MSC 任务提到 EPD 或系统关键任务之上。

## 10. 异常恢复

| 场景 | 处理 |
| --- | --- |
| SD 卡不存在 | 不安装 MSC LUN，显示 `SD card not found`，允许退出 |
| TinyUSB 初始化失败 | 清除启动标志并提示重启，不能回退为本地挂载后继续假装传输 |
| PC 未连接 | 保持 `Waiting for computer`，允许安全退出 |
| PC 安全弹出 | 状态切换为 `Ejected`，允许退出；一期仍通过重启恢复 APP |
| 直接拔线 | 标记 dirty removal，禁止在同一时刻立即本地挂载，建议重启恢复 |
| 启动循环 | NVS 记录 MSC 启动失败次数；连续失败后自动清除标志进入正常模式 |
| USB Host 未释放 | 一期不会发生跨模式残留；若进入前清理失败，重启仍会清除 RAM 状态 |
| 正常模式 FAT 挂载失败 | 只读提示检查/修复 SD 卡，绝不自动格式化 |

建议保留一个物理按键逃生路径：启动时长按 Back/指定按键可忽略 `boot_mode=usb_msc`，防止描述符、线缆或 SD 故障导致设备一直停留在传输模式。

## 11. 分阶段实施

### 阶段 0：板级验证

1. 核对 HS 口的 CC、VBUS 和角色控制电路。
2. 编译最小 TinyUSB HS Device 固件，只验证 PC 枚举。
3. 确认不会向 PC VBUS 反向供电。

### 阶段 1：只读 MSC 原型

1. 引入固定版本 `esp_tinyusb`。
2. 拆分 SD 介质初始化与 VFS 挂载。
3. 在独立启动模式将 SD 暴露为只读 LUN。
4. 验证 Windows、Linux 和 macOS 能识别容量、目录和大文件。

只读原型可以先证明描述符、HS 链路和 SD 块映射正确，避免第一轮测试就损坏 FAT。

### 阶段 2：可写 MSC 与安全弹出

1. 开启 SCSI WRITE10。
2. 接入 storage mount/eject 事件和 dirty-removal 状态。
3. 完成进入/退出 UI、NVS boot mode 和失败恢复。
4. 做大文件、小文件、删除、重命名和断线测试。

### 阶段 3：集成 Transfer 页面

1. Transfer 首页增加 USB/Wi-Fi 入口。
2. 保留 Wi-Fi 智能分类与 Web 文件管理。
3. USB 模式退出后使最近书架、音乐列表、输入法字典和图片缓存失效并重新扫描，避免继续使用 PC 修改前的内存缓存。

### 阶段 4：可选动态切换

在一期长期稳定且确有切换速度需求后，再增加无重启 Host/Device 动态切换。该阶段不是 USB 文件传输上线的必要条件。

## 12. 验收清单

- PC 将设备识别为 `MyPaperP4 SD Card`，USB 速度显示为 High-Speed。
- UAC1/UAC2 耳机连接时进入 USB Transfer，不会留下 Host client 或发生角色冲突。
- PC 占用期间设备侧所有 `/sdcard` 打开操作都被拒绝。
- 复制 1 GiB 文件后两端 SHA-256 一致。
- 复制数千个小文件、重命名、删除、新建目录均正常。
- Windows/Linux/macOS 安全弹出后，设备重启可正常挂载并浏览 SD 卡。
- 未安全拔线后不会自动格式化，能提示风险并允许用户在 PC 修复。
- 传输期间触摸可响应，状态页不频繁刷新，EPD 不影响 USB 稳定性。
- 多次进入/退出 USB Transfer 后无 USB Host、TinyUSB、SDMMC 或 heap 资源泄漏。
- 正常模式中的 Music/Radio USB Audio、Wi-Fi Transfer 和阅读器功能没有回归。

## 13. 最终建议

该功能值得实现，且会比当前 Wi-Fi 传输更接近普通存储设备的使用体验。推荐立即按“板级 Device 验证 → 只读 MSC → 可写与安全弹出 → Transfer UI 集成”的顺序推进。

一期不要追求运行时无重启切换，也不要允许 PC 与固件共享挂载。把 USB 角色和 SD 文件系统所有权彻底隔离，是这项功能能否长期稳定、是否会损坏用户书库的决定性条件。

## 14. 当前落地状态

一期可写原型已经接入主工程：

- 主页 `Transfer` 先进入 USB/Wi-Fi 方式选择页，原 Wi-Fi 传输功能保留。
- 选择 USB 后写入 NVS 启动标志并重启；专用启动分支只初始化显示、触摸、SDMMC 裸介质和 TinyUSB MSC，不挂载 `/sdcard`，也不启动 Wi-Fi、阅读器及 USB Audio Host。
- 使用 `esp_tinyusb 2.0.1~1` 的 ESP32-P4 High-Speed Device 默认端口，MSC 缓冲区为 32 KiB。
- SD 卡以单 LUN、可读写块设备暴露给 PC，自动格式化被禁止。
- PC 安全弹出后显示 Ejected；仍处于连接状态时点击退出会先显示二次确认，再次点击才强制重启。
- 一期不使用 VBUS 检测 GPIO。板端 5 V 输出与 PC VBUS 由用户手动隔离，只连接 HS `D-`、`D+` 和公共 `GND`；USB 模式由 Transfer 页面手动进入和退出。

代码已通过 ESP-IDF 5.3.5 全量编译。PC 端 High-Speed 枚举、实际吞吐量、跨平台安全弹出和异常断线后的 FAT 完整性仍需在目标硬件上验证。
