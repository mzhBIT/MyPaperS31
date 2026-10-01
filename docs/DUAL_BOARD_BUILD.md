# 双 Board 构建说明

日期：2026-10-01

## 当前范围

MyPaperP4 保持完整应用，使用 ESP-IDF 5.3.5。板级硬件初始化已抽取到 `main/boards/MyPaperP4/BoardP4.cpp`，顺序仍为 SD 电压域初始化 → M5 显示初始化 → 前光初始化。

MyPaperS31 使用 ESP-IDF 6.1。启动骨架已通过用户实机验证，现在进入阶段 2：初始化 8 位屏幕、电源、FT6336U 触摸和灰阶测试界面。SD、完整主页、阅读器、网络和音频尚未接入；不要把此固件当作完整阅读器。

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

原 P4 的根目录 `sdkconfig` 与 `build` 保留，激活 5.3.5 后仍可执行原来的 `idf.py build`。不要在原 `build` 执行 S31 的 `set-target`。

脚本和 CMake 检查 IDF 版本、目标与 Board 是否匹配。S31 禁止使用原 `build` 或覆盖根目录 `sdkconfig`。

## 依赖隔离边界

S31 仅构建专用 application、M5GFX 与 IDF 自带组件，禁用该构建进程中的 Component Manager；不读取 P4 的 main 组件清单，不改共享 `dependencies.lock` 和 `managed_components`。P4 依赖路径保持不变。

这不代表完整 S31 应用的第三方依赖已经适配。后续接入字体、图片和网络组件时，须另行验证 IDF 6.1 兼容性与依赖隔离，不能直接打开当前 P4 全部组件。

## 检查结果与下一步

- P4 原构建目录完整编译通过；新的 build-p4 配置生成通过。
- S31 最小工程完整编译通过，产物为 MyPaperS31.bin。
- 用户已验证 S31 启动骨架；阶段 2 显示与触摸尚未烧录验证。

阶段 2 已接入独立扫描、电源、M5GFX 和触摸测试页；P4 与 S31 均已编译通过。下一步先实机验证灰阶、方向、触摸坐标和稳定性，再接入 SD、完整主页与阅读应用。
