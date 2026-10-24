# 双 Board 构建说明

日期：2026-10-01

## 当前默认目标（2026-10-24）

当前工作区默认调试 MyPaperS31：CMake 的未指定 Board 默认值、
`build-board.ps1` 的默认 Board、VS Code ESP-IDF 环境/目标/构建目录和
默认构建任务均已切换到 S31。VS Code 使用 ESP-IDF 6.1、`esp32s31`、
`build-s31`；修改工作区配置后建议重新加载 VS Code 窗口并新建 IDF 终端。
既有 CMake 缓存中的 Board 不会被默认值覆盖，P4 构建目录及配置不作迁移。

S31 当前配置：16 MiB Flash，DIO / 80 MHz；Octal PSRAM / 200 MHz。
PSRAM 时钟不属于分区表参数，由 S31 sdkconfig.defaults 和实际生成的
`build-s31/sdkconfig` 控制；是否稳定仍需实机启动与内存测试确认。

| 区域 | 起始地址 | 大小 | 结束地址（不含） |
|---|---|---|---|
| Bootloader | 0x2000 | 由构建检查 | 不得进入分区表 |
| 分区表 | 0x8000 | 0x1000 保留区 | 0x9000 |
| NVS | 0x9000 | 24 KiB | 0xF000 |
| PHY | 0xF000 | 4 KiB | 0x10000 |
| Factory 应用 | 0x10000 | 8 MiB | 0x810000 |
| 未分配 | 0x810000 | 7.9375 MiB | 0x1000000 |

上述布局符合 16 MiB Flash 范围，应用起点满足 64 KiB 对齐要求。
本次只将原先自动计算的分区偏移写明，不移动已有分区；没有 OTA 或固件内
资源文件系统分区。SD 卡资源无需占用 Flash 分区。后续完整 S31 应用如超过
8 MiB，或加入 OTA，再重新设计布局，不预先分配未知用途的数据分区。

默认构建（先激活 6.1）：`./scripts/build-board.ps1`。
切回 P4 时激活 5.3.5，并显式执行 `./scripts/build-board.ps1 -Board MyPaperP4`；
VS Code 中仍保留 P4 专用构建任务。不要将 S31 固件烧录到 P4。

## 当前范围

MyPaperP4 保持完整应用，使用 ESP-IDF 5.3.5。板级硬件初始化已抽取到 `main/boards/MyPaperP4/BoardP4.cpp`，顺序仍为 SD 电压域初始化 → M5 显示初始化 → 前光初始化。

MyPaperS31 使用 ESP-IDF 6.1。启动、8 位屏幕、FT6336U 触摸和灰阶测试已通过用户实机验证。
阶段 3 的 SD / FreeType / TXT 验证切片也已通过用户实机验证。
当前默认固件已切到共用 P4 的完整主页、应用界面、阅读器和原生 Wi-Fi 后端，
详见 [S31 完整应用接入边界](S31_FULL_APPLICATION.md)。
音频、USB、基带、前光和 RTC 等专用硬件后端尚未适配，不等同于所有外设功能已完成。

`main/boards/MyPaperS31/Pins.h` 保存参考硬件引脚，仍需核对实际板版本。S31 当前仅开启显示和触摸的编译能力位，实际初始化成功与否另外报告。阶段 2 默认启用 Octal PSRAM / 200 MHz；无 PSRAM 不启动屏幕。

波形数据与 P4 扫描参数保持不变。S31 使用独立扫描后端，8 位 / 20 MHz 暂定 17 μs 行周期，等待实机时序验证。详见 [S31 显示验证](S31_DISPLAY_BRINGUP.md)。

## 构建入口

在仓库根目录执行。建议两个型号分别使用新 PowerShell 窗口，避免混用 Python 与工具链环境。

P4：

```powershell
& 'C:\Espressif\tools\Microsoft.v5.3.5.PowerShell_profile.ps1'
& ./scripts/build-board.ps1 -Board MyPaperP4
```

S31：

```powershell
& 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
& ./scripts/build-board.ps1 -Board MyPaperS31
```

可追加 `-Action reconfigure` 仅重新生成构建配置。其他电脑先激活对应的 IDF 环境，再运行同一个脚本，无需使用以上本机 profile 路径。

| 型号 | 目标 | 构建目录 | 配置 | 固件 |
|---|---|---|---|---|
| MyPaperP4 | esp32p4 | build-p4 | build-p4/sdkconfig | build-p4/MyPaperP4.bin |
| MyPaperS31 | esp32s31 | build-s31 | build-s31/sdkconfig | build-s31/MyPaperS31.bin |

P4 新配置首次由根目录 `sdkconfig.defaults` 加 `configs/mypaperp4/sdkconfig.defaults` 生成，不是复制根目录 sdkconfig 的临时 menuconfig 改动。需要保留的自定义项应核对后写入 defaults 或在新构建目录配置。

原 P4 的根目录 `sdkconfig` 与 `build` 保留；当前默认 Board 已切到 S31，
P4 请使用上述显式 Board 脚本入口。不要在原 `build` 执行 S31 的 `set-target`。

脚本和 CMake 检查 IDF 版本、目标与 Board 是否匹配。S31 禁止使用原 `build` 或覆盖根目录 `sdkconfig`。

## 依赖隔离边界

S31 仅构建专用 application、M5GFX 与 IDF 自带组件，禁用该构建进程中的 Component Manager；不读取 P4 的 main 组件清单，不改共享 `dependencies.lock` 和 `managed_components`。P4 依赖路径保持不变。

这不代表完整 S31 应用的第三方依赖已经适配。后续接入字体、图片和网络组件时，须另行验证 IDF 6.1 兼容性与依赖隔离，不能直接打开当前 P4 全部组件。

## 检查结果与下一步

- P4 原构建目录完整编译通过；新的 build-p4 配置生成通过。
- S31 最小工程完整编译通过，产物为 MyPaperS31.bin。
- 用户已验证 S31 启动骨架；阶段 2 显示与触摸尚未烧录验证。

阶段 2 已接入独立扫描、电源、M5GFX 和触摸测试页；P4 与 S31 均已编译通过。下一步先实机验证灰阶、方向、触摸坐标和稳定性，再接入 SD、完整主页与阅读应用。
