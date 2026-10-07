# 统一日志与 Debug / Release 构建

日志等级统一放在 `configs/logging/debug.defaults` 和
`configs/logging/release.defaults`，通过 `scripts/build-board.ps1 -Profile`
选择。现有 ESP_LOG 宏无需逐个更换。

| 配置 | 默认/编译最大等级 | 启动日志 | 输出目录 |
| --- | --- | --- | --- |
| Debug | INFO | INFO | build-p4-debug / build-s31-debug |
| Release | WARN | WARN | build-p4-release / build-s31-release |
| Current（默认） | 原配置 | 原配置 | build-p4 / build-s31 |

Debug 保留现有性能 INFO 日志，不开启第三方组件的全部 DEBUG/VERBOSE。
Release 使用编译最大等级裁剪标准 ESP_LOGI/D/V 调用，通常连格式化参数
计算也不会执行，而非只在运行时过滤输出。WARN/ERROR 保留。
显式覆盖 LOG_LOCAL_LEVEL、自定义日志回调、printf、ROM 日志不受此保证约束。
计时/统计代码本身仍会运行，本次不改变刷新流水线、波形、编译优化等级
或调试符号；这是日志构建配置，不是另一套渲染算法。

## 编译与烧录

VS Code：终端 → 运行生成任务，选择“编译 MyPaperP4 Debug/Release”或
“编译 MyPaperS31 Debug/Release”。原有任务保留。

激活对应 IDF 环境后也可执行：

```powershell
./scripts/build-board.ps1 -Board MyPaperP4 -Profile Debug
./scripts/build-board.ps1 -Board MyPaperP4 -Profile Release
./scripts/build-board.ps1 -Board MyPaperP4 -Profile Release -Action flash -Port COM4
./scripts/build-board.ps1 -Board MyPaperP4 -Profile Release -Action monitor -Port COM4
```

P4 使用 IDF 5.3.5，S31 使用 IDF 6.1。烧录时必须使用同一 Profile，避免
默认扩展烧录按钮仍指向旧 build 目录。COM4 按实际端口替换。

首次建立 Profile 配置时，P4 优先复制根 sdkconfig，否则复制原板型
构建目录的 sdkconfig；S31 复制原 build-s31/sdkconfig。无种子配置时使用
板型默认配置。各 Profile 的 sdkconfig 独立，根配置不会被修改。
脚本在 build/reconfigure 时检查并统一日志字段，仅值变化时写入配置，
避免 menuconfig 留下旧日志等级，也避免重复写入触发无意义的 CMake 配置。
flash 使用构建目录的 flasher_args.json 直接调用 esptool，烧录已编译的
固件，不执行依赖检查或重新编译；修改源码后须先重新 build。烧录默认
921600 波特率，可通过 -Baud 调整。monitor 不重写日志配置。
其他设置初始化后独立维护，不会自动同步根 sdkconfig 后续修改。

新增诊断输出应使用 ESP_LOGI/D，不要用 WARN 伪装普通性能日志，也不要把
必要功能调用放到日志参数中。真正的错误/异常仍使用 ESP_LOGW/E。
