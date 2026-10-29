# MyPaperS31 USB 大容量存储（模拟 U 盘）设计方案

日期：2026-10-25

状态：一期已实现（重启隔离式），实机已验证 HS 枚举与 PC 拷贝。

> 实现更新（2026-10-25）：第 1、2 节记录的是改造前的缺口，现已全部关闭；
> S31 改为与 P4 共用 `main/storage/storage_sdcard.c`（含双槽 32 KiB 写回缓存）。
> 具体改动见第 12 节“一期实现记录”。UI 未改动。

## 1. 结论

当前 MyPaperS31 **没有 USB 模拟 U 盘功能**，而且是有意关闭的：

- S31 的构建脚本把 `usb/usb_msc_mode.cpp` 和 `usb/usb_transfer_boot.c` 从源文件列表移除；
- `UnavailableHardware.cpp` 把 `usb_msc_mode_run()`、`usb_transfer_boot_*()` 实现为
  `ESP_ERR_NOT_SUPPORTED`；
- `StorageS31.cpp` 把 raw SD 导出接口全部实现为不支持（一期已删除该文件，
  改由共享的 `main/storage/storage_sdcard.c` 提供）；
- `BoardS31.cpp` 在 `usb_transfer_mode=true` 时直接返回失败；
- S31 的 `sdkconfig` 没有启用 TinyUSB MSC；
- `BoardS31` 的能力位里没有 `Capability::UsbMsc`。

MyPaperP4 已经有一套完整、可复用的“重启隔离式” USB MSC 实现。**一期建议复用这套
架构，为 S31 补齐板级与 TinyUSB 后端**，而不是直接照搬 Metalio 的运行时开关。

Metalio 的运行时方案在 S31 上技术上可行，且 S31 的 HS UTMI 与 USB Serial/JTAG
使用不同 PHY，没有 P4 那种 USB Host/Device 争用问题。但本项目没有集中的
`SdCardManager`、storage lease 或全量文件句柄关闭机制，运行时切换需要先把这些
基础设施补齐，风险和工作量都明显高于重启隔离。

因此推荐：

1. **一期**：复用 P4 的“NVS 启动标志 + 重启进入专用 MSC 模式”，补齐 S31 的
   HS UTMI、ANA_SW、raw SD 后端和构建依赖。
2. **二期（可选）**：在具备 SD 所有权管理后，再演进为 Metalio 式的运行时开关。

## 2. 当前工程现状

### 2.1 MyPaperP4 已有实现

| 模块 | 实现 | 说明 |
| --- | --- | --- |
| `main/usb/usb_transfer_boot.c` | NVS `usb_transfer/boot_msc` 标志 | 进入前写 1，退出/异常时清除 |
| `main/main.cpp:78-92` | 启动分支 | 标志存在时调用 `initialize_hardware(true)`，再进入 `usb_msc_mode_run()` |
| `main/boards/MyPaperP4/BoardP4.cpp` | USB 启动模式的硬件初始化 | 不挂载 `/sdcard`，改调 `storage_sdcard_init_usb_medium()` |
| `main/usb/usb_msc_mode.cpp` | 专用 MSC 页面与 TinyUSB 生命周期 | PC 安全弹出、二次确认退出、flush、清除标志并重启 |
| `main/storage/storage_sdcard.c` | raw SD 介质 + 异步写缓存 | 同一时刻只允许 APP 或 USB 一方拥有 SD |
| `main/activities/TransferActivity.cpp` | 入口 UI | 选择 USB 后写标志并重启 |

P4 的关键约束是 USB Host/Device 互斥和 SD 文件系统独占。它通过“重启”一次性
关闭所有 Activity、字体、阅读器、图片缓存和网络任务的文件句柄，再用最小系统
启动 MSC 模式，避免 PC 与固件同时修改 FAT。

### 2.2 MyPaperS31 改造前的缺口

| 位置 | 改造前行为 | 需要补齐 |
| --- | --- | --- |
| `main/boards/MyPaperS31/application/CMakeLists.txt` | `REMOVE_ITEM` 移除 USB MSC 源文件 | 恢复 `usb_msc_mode.cpp`、`usb_transfer_boot.c` |
| `main/boards/MyPaperS31/UnavailableHardware.cpp` | USB MSC/boot 标志全部返回不支持 | 删除对应 stub，链接真实实现 |
| `main/boards/MyPaperS31/StorageS31.cpp` | raw SD 接口返回 `ESP_ERR_NOT_SUPPORTED` | 实现 raw card 初始化/释放/flush（一期改为直接共享 `storage_sdcard.c`，该文件已删除） |
| `main/boards/MyPaperS31/BoardS31.cpp` | 拒绝 `usb_transfer_mode` | 增加 USB 启动分支、ANA_SW 控制 |
| `main/boards/MyPaperS31/Pins.h` | 已定义 `usb_analog_switch = 36` | 增加输出控制函数 |
| `BoardS31::profile()` | 无 `Capability::UsbMsc` | 实机验证后加入能力位 |
| `configs/mypapers31/sdkconfig.defaults` | 无 TinyUSB 配置 | 启用 MSC、FIFO、DMA |
| 根 `CMakeLists.txt`（S31 分支） | `COMPONENT_DIRS` 无 TinyUSB | 增加 esp_tinyusb 与 tinyusb 组件目录 |

## 3. Metalio 参考实现分析

参考工程：`E:\opensource\Metalio-E-INK4-Plus-main`

核心文件：

- `main/boards/common/storage/usb_virtual_disk.cc/.h`
- `main/boards/common/storage/SdCardManager.cc/.hpp`
- `main/display/screen/settings_screen/settings_storage_tab.cc`

### 3.1 架构

Metalio 采用运行时开关，不重启：

```text
UI 开关
  -> UsbVirtualDisk::Toggle()
  -> 单槽请求队列 + usb_vd_worker
  -> UsbGadgetStart()/UsbGadgetStop()
       -> TinyUSB Device 安装/卸载
       -> ANA_SW(GPIO36) 低 = 虚拟 U 盘，高 = 普通模式
       -> SD 所有者在 APP/PC 之间切换
```

关键机制：

1. **单例 + worker**：UI 只投递请求，TinyUSB 安装/卸载、SD 切换都在 worker
   任务中完成，避免阻塞 UI。
2. **SD 所有权切换**：`SdCardManager::InitRawCardForMsc()` 先卸载 VFS，再
   `sdmmc_host_init` + `sdmmc_host_init_slot` + `sdmmc_card_init` 得到 raw card；
   `NotifyExportedToHost()`、`NotifyExternalAppMount()`、`RemountVfsFromCard()`
   同步“本机可用/电脑占用”状态。
3. **自动挂载控制**：`tinyusb_msc_driver_config_t.user_flags.auto_mount_off = 1`，
   由 worker 在 USB attach/detach 事件中显式调用
   `tinyusb_msc_set_storage_mount_point()`。
4. **安全弹出**：监听 `TINYUSB_MSC_EVENT_MOUNT_COMPLETE`、
   `TINYUSB_MSC_EVENT_MOUNT_FAILED`、`FORMAT_REQUIRED`，UI 显示
   `EnabledHost/EnabledLocal/NoSdCard/FormatRequired/HostBusy` 等状态。
5. **S31 专用细节**：
   - `TINYUSB_CONFIG_HIGH_SPEED(...)`，HS UTMI，内置 PHY；
   - 不需要 `usb_new_phy()`，也没有 P4 的 PHY 选路；
   - `TINYUSB_TASK_CUSTOM(size, prio, 0)`，TinyUSB 任务固定在 CPU0；
   - ANA_SW 低电平选出虚拟 U 盘通路。

### 3.2 Metalio 与 MyPaper 的差异

| 项目 | Metalio | 本项目 |
| --- | --- | --- |
| SD 访问入口 | 集中到 `SdCardManager` | 大量 Activity/解析器直接使用 VFS |
| 文件句柄关闭 | 有统一管理器与缓存失效 | 无统一 lease，重启是唯一可靠屏障 |
| USB Host | 有蓝牙/音频等扩展 | S31 当前没有 USB Host 使用 |
| UI 入口 | 设置→存储开关 | Transfer 页面已有“重启进入”确认 |
| 参考价值 | 运行时切换、S31 TinyUSB 细节 | 可直接复用的重启隔离流程 |

## 4. S31 硬件与平台约束

### 4.1 USB

- ESP32-S31 的 HS USB 使用 **UTMI 内置 PHY**，没有 P4/S3 那样需要切换的
  GPIO D+/D- 或 `USB_WRAP`。
- S31 的 **USB Serial/JTAG（USJ）与 HS UTMI 使用不同 PHY**，TinyUSB 安装时
  不需要像 S3 那样把 USJ PHY 让给 OTG。
- **ANA_SW（GPIO36）**：参考工程语义为“低 = 虚拟 U 盘，高 = 普通模式”。
  本工程 `Pins.h` 已保留该引脚，但目前没有驱动。该开关很可能决定 USB-C
  数据线接到 S31 HS UTMI 还是 USJ，必须按实板原理图确认。
- 板子自供电，一期不使用 VBUS 检测：手动进入/退出，USB 线只连接
  `D-、D+、GND`，不要与板载 5V 输出硬并联。
- 如果 ANA_SW 确实在 USJ 与 HS 之间选路，那么 U 盘模式下 USB-C 上的串口
  控制台会消失，这是预期行为；退出 U 盘模式后自动恢复。

### 4.2 需要在实板确认的项目

1. ANA_SW=GPIO36 是否存在、极性是否低=U 盘。
2. USB-C 的 CC/角色电阻是否允许设备身份；PC 能否枚举为 HS。
3. Device 模式下是否停止向接口输出 5V，避免与 PC VBUS 对灌。
4. USJ 与 HS 是否共用同一个 USB-C，以及切换后是否需要重新插拔。
5. SD 卡 4-bit 走线在高频（40 MHz）下是否稳定。

### 4.3 资源与实时性

- EPD 扫描任务固定在 **CPU1**（`Panel_EPD_2D.cpp`），TinyUSB 任务应固定在
  **CPU0**，避免显示刷新阻塞 EP0 控制传输。
- S31 使用 PSRAM-first malloc，内部 RAM 保留仅 64 KiB。TinyUSB 的 FIFO、
  任务栈和 DMA 缓冲必须使用内部 RAM，需实测余量。
- 进入 MSC 前已 `M5.Display.waitDisplay()` 等待首帧完成，避免 Windows
  在 EP0 超时前拿不到描述符。

## 5. 一期方案：重启隔离的专用 MSC 模式

### 5.1 启动流程

```text
正常模式
  -> Transfer 页面选择 USB
  -> 写 NVS boot_msc=1
  -> 等待 EPD 空闲后重启

USB MSC 模式
  -> 初始化 NVS / 显示 / 触摸
  -> 不挂载 /sdcard
  -> storage_sdcard_init_usb_medium() 初始化 SDMMC raw card
  -> ANA_SW(GPIO36) 置低
  -> 安装 TinyUSB Device + MSC（HS UTMI，CPU0）
  -> 电脑可读写整张 SD 卡
  -> PC 安全弹出 / 用户二次确认退出
  -> 清除 boot_msc
  -> 复位回正常模式
```

与 P4 的差异：

- 不需要处理 UAC/UAC2 Host 销毁；
- 不需要 P4 的自定义 HCD/PHY 选路；
- 但 SD 所有权切换、退出确认、禁止自动格式化、FAT 独占规则完全保留。

### 5.2 复用 `usb_msc_mode.cpp`

现有 `main/usb/usb_msc_mode.cpp` 的主体逻辑（页面、状态机、TinyUSB MSC 安装、
退出重启）是目标无关的，可以直接给 S31 编译，只需增加少量条件分支：

```cpp
#if CONFIG_IDF_TARGET_ESP32S31
  // 1. 产品名使用 MyPaperS31
  // 2. 使用 TINYUSB_CONFIG_HIGH_SPEED()，HS UTMI
  // 3. tinyusb_driver_install() 前 ANA_SW=0，退出/失败时 ANA_SW=1
#else
  // 保持 P4 现有 TINYUSB_DEFAULT_CONFIG() 与产品名
#endif
```

建议把 ANA_SW 操作封装在板级：

```cpp
// main/boards/MyPaperS31/BoardS31.cpp 或 Pins.h 配套实现
namespace mypaper::board::s31 {
void usb_analog_switch_set(bool usb_device); // true: GPIO36=0, false: GPIO36=1
}
```

不要在每个调用点直接写 GPIO 电平，便于后续按实板版本调整极性。

### 5.3 S31 raw SD 后端

一期最终没有在 `StorageS31.cpp` 里另写一份实现，而是让 S31 直接编译
P4 的 `main/storage/storage_sdcard.c`（按目标选择引脚），从而得到与 P4 完全同名、
同语义的 raw medium 接口：

```c
esp_err_t storage_sdcard_init_usb_medium(void);
void      storage_sdcard_release_usb_medium(void);
esp_err_t storage_sdcard_flush_usb_write_cache(void);
```

实现要点（参考 Metalio `SdCardManager::InitRawCardForMsc()`）：

1. 调用前对所有 SD 引脚执行 `gpio_hold_dis()`，与当前
   `storage_sdcard_mount()` 一致。
2. 使用 `SDMMC_HOST_DEFAULT()` 和 4-bit slot（CLK=24、CMD=25、D0..D3=20..23）。
3. 依次执行 `host.init()`、`sdmmc_host_init_slot()`、`sdmmc_card_init()`；
   失败时按相反顺序 deinit，不能留下半初始化状态。
4. 使用 `calloc(sizeof(sdmmc_card_t))` 分配 card，释放时调用 host deinit
   并 `free()`。
5. 该模式下 **不调用** `esp_vfs_fat_sdmmc_mount()`，也不执行
   `app_paths::ensure_data_dir()`。
6. 一期直接沿用 P4 的双槽 32 KiB 写回缓存（含 CPU1 writer 任务与
   `do_transaction` 拦截），退出前由 `storage_sdcard_flush_usb_write_cache()`
   强制落盘；缓存分配失败时自动降级为直写，不影响正确性。
7. S31 原先使用 `SDMMC_FREQ_DEFAULT`（20 MHz）；一期已统一改为
   `SDMMC_FREQ_HIGHSPEED`（40 MHz，见共享文件中的
   `host->max_freq_khz = SDMMC_FREQ_HIGHSPEED`），
   普通挂载与 USB raw medium 共用同一时钟设置。

注意 host deinit 要兼容 `SDMMC_HOST_FLAG_DEINIT_ARG`：

```c
if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
    host.deinit_p(host.slot);
} else if (host.deinit) {
    host.deinit();
}
```

### 5.4 BoardS31 启动分支

将 `initialize_hardware(bool usb_transfer_mode)` 改为：

```text
display init
  -> if usb_transfer_mode:
         storage_sdcard_init_usb_medium()
         （不 ensure_data_dir、不挂载 /sdcard）
     else:
         storage_sdcard_mount()
         ensure_data_dir()
  -> rotation / frontlight / power
```

因为 S31 没有 P4 的 `VDD_IO_5` 电源域依赖，SD 初始化不必强制排在显示之前。
显示先初始化可以在 SD 失败时显示错误页。USB 模式下 ANA_SW 默认保持高，
等 TinyUSB 安装前再拉低。

### 5.5 TinyUSB 配置

```cpp
tinyusb_config_t cfg = TINYUSB_CONFIG_HIGH_SPEED(device_event, nullptr);
cfg.phy.self_powered = false;
cfg.phy.vbus_monitor_io = -1;       // 一期手动模式
cfg.task = TINYUSB_TASK_CUSTOM(4096, 5, 0); // CPU0，EPD 在 CPU1
cfg.descriptor.string = strings;
cfg.descriptor.string_count = ...;
```

MSC storage：

```cpp
tinyusb_msc_storage_config_t storage = {};
storage.medium.card = storage_sdcard_get_card();
storage.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
storage.fat_fs.base_path = (char *)"/usbdata";
storage.fat_fs.config.max_files = 2;
storage.fat_fs.do_not_format = true;   // 绝不自动格式化
```

默认描述符由 `esp_tinyusb` 生成；如果 Windows 在 HS 下枚举异常，再按
Metalio 的方式显式提供 FS/HS 配置描述符：

- FS：MSC bulk 端点 64 字节；
- HS：MSC bulk 端点 512 字节。

产品字符串建议：

```text
Manufacturer: MyPaperS31
Product:      MyPaperS31 SD Card
Serial:       芯片 MAC 派生，保持稳定
```

### 5.6 UI 与用户流程

复用现有 Transfer 页面与 USB 模式页面：

- Transfer 页：USB / Wi-Fi / FileHotspot 三个入口保持不变；
- USB 确认页：保留“SD 卡将不可用”；
- **S31 没有 USB 音频**，应通过 `supports(Capability::UsbAudio)` 隐藏
  “USB headphones will be disconnected.”，不要显示误导信息；
- USB 模式页：显示 `Waiting / Connected / Ejected / Error`，不收文件级进度；
- 退出规则与 P4 一致：
  - 已安全弹出：直接退出；
  - 仍连接：二次确认；
  - 未安全弹出：允许强制退出，但提示电脑缓存可能未写完。

### 5.7 数据安全规则

```text
APP 模式   : /sdcard 已挂载，TinyUSB 不存在
USB 模式   : /sdcard 未挂载，PC 独占 raw FAT 卷
EJECTED    : PC 已安全弹出，等待退出/重启
ERROR      : 允许退出，不允许自动格式化
```

- 任何情况下都保持 `format_if_mount_failed=false`、`do_not_format=true`。
- 正常模式挂载失败只提示用户检查/修复 SD 卡。
- 掉电时 NVS 标志仍在，下次启动会重新进入 USB 模式；这比回到 APP 模式
  更安全，因为不会出现 PC 写过一半、固件又挂载同一张卡的情况。
- 建议增加启动失败计数或“长按 Back 跳过 MSC 模式”的逃生路径，避免
  描述符/SD 故障导致设备只能停在 USB 页面。

## 6. 依赖与构建方案

### 6.1 esp_tinyusb 版本是关键前提

当前工程 `managed_components/espressif__esp_tinyusb` 是 **2.0.1~1**：

- 它的 `idf_component.yml` target 列表只有 esp32s2/s3/p4/h4，**没有 esp32s31**；
- `tinyusb_default_config.h` 只把 ESP32P4 映射为 High-Speed；
- Kconfig 的 FIFO 默认值和范围也没有 S31 分支；
- `tinyusb.c` 的 UTMI 分支只处理 P4。

ESP32-S31 支持是在 **esp_tinyusb 2.2.0** 加入的
（[changelog](https://components.espressif.com/components/espressif/esp_tinyusb/versions/2.2.0/changelog)）。
因此 S31 必须使用 **>= 2.2.0**，不能复用 P4 当前的 2.0.1~1。

底层的 `espressif/tinyusb 0.21.0~1` 已经包含 `OPT_MCU_ESP32S31` 和
S31 的 DWC2 分支，可以继续使用。

### 6.2 隔离 P4 与 S31 的依赖

P4 使用 IDF 5.3.5 且已锁定 esp_tinyusb 2.0.1~1，不要把 P4 一起升级。
S31 构建当前关闭了 Component Manager（`IDF_COMPONENT_MANAGER=0`），推荐：

```text
third_party/s31/esp_tinyusb/            # 固定 2.2.x 源码，纳入版本管理
managed_components/espressif__tinyusb/  # 0.21.0~1，已支持 S31
```

在根 `CMakeLists.txt` 的 S31 分支把两者加入 `COMPONENT_DIRS`，不要让 P4
发现它们。由于 S31 不走 Component Manager，vendor 后的
`esp_tinyusb/CMakeLists.txt` 需要确认：

- `tinyusb` 组件已在构建中；
- `esp_tinyusb` 对 `tinyusb` 的链接依赖通过 `REQUIRES`/`PRIV_REQUIRES`
  显式声明，而不是依赖 `idf_component.yml` 自动解析。

也可以在独立的小工程里用 Component Manager 下载 2.2.x 后拷贝源码，
但构建过程本身应保持离线可重复。

### 6.3 sdkconfig.defaults（S31）

```ini
CONFIG_TINYUSB_MSC_ENABLED=y
# 参考工程使用 512 作为基线；稳定后按内部 RAM 余量提升到 8K/16K/32K
CONFIG_TINYUSB_MSC_BUFSIZE=512
CONFIG_TINYUSB_MSC_MOUNT_PATH="/usbdata"
CONFIG_TINYUSB_MODE_DMA=y
CONFIG_TINYUSB_DESC_USE_ESPRESSIF_VID=y
CONFIG_TINYUSB_DESC_PRODUCT_STRING="MyPaperS31 SD Card"
CONFIG_TINYUSB_DESC_MSC_STRING="MyPaperS31 MSC"
```

如果使用 `TINYUSB_CONFIG_HIGH_SPEED()` 显式配置，描述符字符串也可以像 P4
一样在代码里传入，不依赖 Kconfig 默认值。

### 6.4 应用构建

`main/boards/MyPaperS31/application/CMakeLists.txt`：

- 从 `REMOVE_ITEM` 中只保留 `usb/usb_modem_probe.c`；
- 恢复 `usb/usb_transfer_boot.c`、`usb/usb_msc_mode.cpp`；
- 在 `REQUIRES` 中加入 `esp_tinyusb`（以及实际使用的 tinyusb 组件名）。

`UnavailableHardware.cpp` 删除：

```cpp
esp_err_t usb_msc_mode_run() { ... }
bool usb_transfer_boot_requested() { ... }
esp_err_t usb_transfer_boot_set(bool) { ... }
```

## 7. 性能与内存

参考 Metalio 的 S31 配置（`CONFIG_TINYUSB_MSC_BUFSIZE=512`、DMA 模式）可以
先建立正确性基线。目标：

- HS 枚举（480 Mbit/s），不是 Full-Speed；
- 大文件连续读 ≥ 4 MB/s，写 ≥ 3 MB/s（最终以实板为准）；
- 4 GiB 连续传输无 I/O error、无断连；
- 安全弹出后重新挂载，抽检 SHA-256 一致。

调优顺序：

1. 确认 `TUD_OPT_HIGH_SPEED` 生效、bulk 端点 512 字节；
2. 用 512 字节 FIFO 建立基线；
3. 依次测试 8K、16K、32K FIFO，记录内部 RAM 和吞吐；
4. 评估 SDMMC 40 MHz；
5. 如果写入吞吐不足，再考虑移植 P4 的异步写缓存（必须实现退出前 flush）。

内存注意事项：

- TinyUSB FIFO、DMA 缓冲和任务栈优先内部 RAM；
- 不要为了吞吐把 EPD 或系统关键缓冲挤掉；
- USB 任务优先级不要高于 EPD 扫描/网络关键任务。

## 8. 实施阶段与验收

### 阶段 0：板级验证

1. 按原理图确认 ANA_SW、USB-C CC、VBUS 和 USJ/HS 的接法。
2. 编译一个只枚举 MSC 的最小固件，确认 PC 显示 High-Speed。
3. 确认 U 盘模式下不会向 PC VBUS 反灌。

### 阶段 1：只读 MSC

1. 接入 esp_tinyusb 2.2.x 与 S31 构建配置。
2. 实现 raw SD medium。
3. TinyUSB 只读 LUN，验证容量、目录、大文件。
4. 验证 U 盘模式期间设备不挂载 `/sdcard`。

### 阶段 2：可写与安全弹出

1. 打开 WRITE10。
2. 接入 attach/detach、mount/eject 事件。
3. 完成退出确认、boot flag、异常重启。
4. 做删除、重命名、小文件、断线测试。

### 阶段 3：UI 与集成

1. Transfer 页面按能力位显示 USB 入口。
2. **不隐藏 USB 耳机提示**：S31 后续也会实现 USB 耳机功能，本阶段不改动
   任何 UI 文案或流程。
3. 加入 `Capability::UsbMsc`。
4. 回归 Wi-Fi 传输、阅读、文件浏览。

### 验收清单

- PC 识别为 `MyPaperS31 SD Card`，速度 High-Speed；
- U 盘模式中设备侧无法访问 `/sdcard`；
- 1 GiB 文件复制后两端哈希一致；
- 数千个小文件、重命名、删除、新建目录正常；
- Windows/Linux/macOS 安全弹出后重启可正常挂载；
- 未安全拔线不会自动格式化；
- 传输期间触摸可响应，EPD 刷新不影响 USB；
- 多次进入/退出无 TinyUSB/SDMMC/heap 泄漏；
- P4 的 USB Audio、Wi-Fi Transfer、阅读器无回归。

## 9. 二期方案：Metalio 式运行时切换（可选）

只有在确实需要“免重启、设置页开关”时再做。前置条件：

1. 增加全局 storage ownership/lease：
   - `APP_OWNED / TRANSITION / USB_OWNED / EJECTED / ERROR`；
   - 所有 `/sdcard` 访问必须经过 lease，切换时等待归零。
2. 为阅读器、字体、封面、ZIP、Wi-Fi、RSS、微信读书等建立统一关闭/失效机制。
3. 增加与 `SdCardManager` 等价的 raw card 管理：
   - `unmount VFS -> sdmmc_card_init -> MSC`；
   - MSC 释放后 `RemountVfsFromCard()`；
   - PC 修改 SD 后使书架、字体、图片缓存失效。
4. TinyUSB 运行时 install/uninstall，worker 串行化所有切换请求；
5. ANA_SW 与 PC attach/detach 事件联动；
6. 若未来接入 USB Host（ML307/UAC），还要加 Host/Device 独占仲裁。

在这些条件满足前，运行时切换会出现“Activity 仍持有文件句柄时 SD 被卸载”的
数据损坏风险，不建议一期直接采用。

## 10. 风险与待确认

| 风险 | 影响 | 处理 |
| --- | --- | --- |
| esp_tinyusb 版本不足 | S31 无法 HS 枚举/编译 | 固定 >= 2.2.0，版本纳入仓库 |
| ANA_SW 极性或实板版本不符 | PC 看不到设备 | 实板确认后再封装板级 API |
| USB-C 与 USJ 共用 | U 盘模式下串口日志消失 | 以触摸 UI 退出；保留 UART 调试路径 |
| VBUS 反灌 | 硬件损坏风险 | 只接 D-/D+/GND，或增加反向截止 |
| SD 40 MHz 不稳定 | 读写错误 | 已切到 40 MHz；大文件哈希/小文件复测，异常则回退 `SDMMC_FREQ_DEFAULT` |
| 内部 RAM 不足 | TinyUSB 分配失败/系统不稳定 | FIFO 从 512 起，实测后调整 |
| 未安全弹出 | PC 缓存未写回 | 禁止自动格式化，退出时提示风险 |
| 掉电后仍进入 U 盘模式 | 用户以为设备异常 | 二次启动失败计数/按键逃生 |

## 11. 推荐结论

对当前代码库，**一期最稳妥、改动最小的路径是：把 P4 已验证的重启隔离式 MSC
流程开放给 S31，并补上 S31 的 HS UTMI、ANA_SW、raw SD 和 esp_tinyusb 2.2.x 依赖。**
这样不会引入新的文件系统所有权模型，也不会改变 P4 的 USB 行为。

Metalio 的运行时方案作为二期目标保留；等本项目具备统一的 SD lease 和文件句柄
关闭机制后，再按它的 worker + `SdCardManager` 模式演进。

## 12. 一期实现记录（2026-10-25）

### 12.1 依赖

| 位置 | 内容 |
| --- | --- |
| `third_party/s31/esp_tinyusb/` | esp_tinyusb **2.4.0** 源码内置（许可证 Apache-2.0） |
| `third_party/s31/tinyusb/` | tinyusb **0.21.0~1** 源码内置（许可证 MIT） |
| 根 `CMakeLists.txt` | S31 的 `COMPONENT_DIRS` 加入上述两个目录 |
| `configs/mypapers31/sdkconfig.defaults` | 启用 MSC、32 KiB FIFO（与 P4 一致）、DMA、产品字符串 |

版本原因：S31 需要 esp_tinyusb >= 2.2.0 才支持 `esp32s31` 与 `TINYUSB_CONFIG_HIGH_SPEED`，
而 P4 侧 `dependencies.lock` 固定 2.0.1~1。S31 关闭了 Component Manager
（`IDF_COMPONENT_MANAGER=0`），因此用 `third_party/s31` 内置依赖，不影响 P4 构建。

### 12.2 代码改动

| 文件 | 改动 |
| --- | --- |
| `main/boards/MyPaperS31/UsbSwitch.h` | 新增 ANA_SW 控制接口声明 |
| `main/boards/MyPaperS31/BoardS31.cpp` | 实现 `usb_analog_switch_set()`；普通启动强制复位到 USJ 通路；USB 启动分支改用 raw medium，且不创建 `/data`；能力位加入 `UsbMsc` |
| `main/boards/MyPaperS31/StoragePins.h` | 新增 SD 引脚宏（CLK=24、CMD=25、D0..D3=20..23），`Pins.h` 改为引用它 |
| `main/storage/storage_sdcard.c` | 由 P4 独占改为 P4/S31 共享：按 `CONFIG_IDF_TARGET_ESP32S31` 选择引脚并在挂载/raw 初始化前 `gpio_hold_dis()`；raw medium 与双槽 32 KiB 写回缓存原样用于 S31；“列举根目录”日志函数加 `__attribute__((unused))` |
| `main/boards/MyPaperS31/StorageS31.cpp` | **已删除**；S31 不再单独实现 raw SD |
| `main/usb/usb_msc_mode.cpp` | S31 使用 `TINYUSB_CONFIG_HIGH_SPEED` 与 S31 产品字符串；安装 TinyUSB 前拉低 ANA_SW，失败/退出时拉高 |
| `main/boards/MyPaperS31/UnavailableHardware.cpp` | 删除 `usb_msc_mode_run`、`usb_transfer_boot_*` 桩 |
| `main/boards/MyPaperS31/application/CMakeLists.txt` | 恢复 `usb/usb_transfer_boot.c`、`usb/usb_msc_mode.cpp`；不再从源列表移除 `storage/storage_sdcard.c`；`REQUIRES` 增加 `esp_tinyusb` |

UI 未改动：Transfer 页面、USB 耳机提示与进入/退出流程保持原样。

### 12.3 与 P4 的实现关系

一期最终采用“同一份代码”策略：S31 与 P4 编译相同的 `main/storage/storage_sdcard.c`，
包括 raw medium、释放路径和 **2 × 32 KiB 双槽写回缓存**（CPU1 writer 任务、
`do_transaction` 拦截、CMD24 缓存、读/CMD25 前 flush、10 ms 空闲 flush）。
两侧仅有两处差异：

1. **引脚来源**：S31 使用 `StoragePins.h` 的 CLK=24、CMD=25、D0..D3=20..23
   （P4 保持 43/44/39/40/41/42），且在挂载/raw 初始化前对 6 根 SD 线执行
   `gpio_hold_dis()`。
2. **电源控制**：S31 没有 `SOC_SDMMC_IO_POWER_EXTERNAL`，不经过 LDO 电源控制路径。

其余实现差异：

3. **ANA_SW 时序**：只在 `tinyusb_driver_install()` 前拉低，避免启动早期
   （显示初始化、SD 初始化、日志输出）失去 USB Serial/JTAG。先锁存输出电平再
   使能输出，避免切换瞬间的毛刺。
4. **raw card 描述符**：从内部 RAM `heap_caps_calloc`，不经过 VFS。
5. **SD 时钟 40 MHz**：普通挂载与 USB raw medium 共用 `SDMMC_FREQ_HIGHSPEED`，
   实机协商结果为 38.46 MHz、4-bit。

关于双槽写缓存对 U 盘吞吐的实际作用（重要，避免预期偏差）：
esp_tinyusb 2.4.0 的 WRITE10 会先把数据 memcpy 进 32 KiB DMA FIFO，再在
TinyUSB 任务里调用 `medium->write`，最终以一次 `sdmmc_write_sectors()` 提交。
当 PC 以 32 KiB 为单位写时，这天然就是 CMD25 多块传输，**不经过** 512 字节
缓存路径；只有单块写（CMD24，典型来源是 FAT 表、目录项和小文件尾部）才会
命中双槽缓存。因此：

- 大文件顺序拷贝的速度仍由 CMD25 和 SD 卡自身的编程速度决定，
  40 MHz 4-bit 下实测 7–9 MB/s，已是该系统方案的实际上限；
- 小文件、目录密集操作和 FAT 元数据更新会因写合并/异步化而更快、更稳；
- 缓存分配失败时自动降级为直写，日志打印
  `USB MSC double write cache unavailable; using direct writes`。

### 12.4 验证状态

- `build-s31` 使用 ESP-IDF 6.1 完整编译通过，无警告；
  `MyPaperS31.bin` = 0x3e8c70 字节（32 KiB FIFO + 缓存版本）。
- 实机已确认 HS 枚举与 PC 拷贝可用：20 MHz 下写入约 5 MB/s；
  40 MHz 下大文件写入实测 7–9 MB/s，满足一期写入 ≥3 MB/s 的验收线。
- 待复测：启动日志中应出现
  `USB MSC double write-back cache ready: slots=2 slot_bytes=32768 writer=CPU1`；
  大文件哈希、小文件、连续多次拷贝的稳定性；内部 DMA 堆余量。
- P4 构建未在本次改动后重跑；共享的 `storage_sdcard.c` 对 P4 仅有目标宏分支与
  `__attribute__((unused))` 两处非行为性改动，`usb_msc_mode.cpp` 的 P4 分支
  保持原逻辑。
