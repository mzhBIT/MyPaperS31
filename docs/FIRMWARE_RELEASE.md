# S31 固件发布与离线刷机

发布目录：`firmware-release/MyPaperS31/`；分享用 ZIP：`firmware-release/MyPaperS31-offline.zip`。

使用者解压后用 Chrome / Edge 电脑版打开 `刷机工具.html`，即可选择串口并烧录。固件、官方 esptool-js 0.7.1、S31 stub 和校验库都嵌入同一 HTML，不访问 CDN、不加载本地 ESM、不依赖 Python/ESP-IDF/服务器。详细操作见发布包的 `使用说明.md`。

## 更新发布包

先激活 ESP-IDF 6.1，并重新构建两份固件：

```powershell
./scripts/build-board.ps1 -Board MyPaperS31 -Action build
./scripts/build-emulator.ps1 -Action build
python tools/package_firmware.py
```

默认打包当前 `build-s31` 和 `build-s31-emulator`，不会自动修改日志级别或配置。若需要 Release 构建，可显式指定 `--system-build build-s31-release`。打包不打开串口，不擦除或烧录设备。

打包脚本使用构建产物而非猜测地址，核对：

- 两份构建的芯片型号及烧录清单。
- 镜像校验、ESP32-S31 chip ID、主系统与模拟器的应用描述身份。
- 系统和模拟器的分区表逐字节一致，分区表 MD5 和完整布局符合合同。
- 每份镜像及其按扇区擦除的范围都在对应槽位内。
- 初始 otadata 为擦除状态，安装模式会选择 factory 主系统。

发布包只含公开构建产物和相关说明/许可证，不复制设备 NVS、sdkconfig、主机环境路径或 SD 卡内容。`manifest.json` 的版本和时间来自实际应用描述和打包时间；未提交源码构建的 `-dirty` 标记予以保留，不伪装为已打标签版本。

HTML 是固件快照。替换目录中的 `.bin` 不会更新 HTML 内的数据，必须重新打包。ZIP 只包含脚本明确生成的文件，目录中的其他个人文件不会被顺带打包。

## 烧录策略

默认更新主系统，仅写 0x10000。仅更新模拟器仅写 0x810000。二者都要求设备已有完整匹配的分区表，每次开始局部更新前再读取复核一次。首次安装写引导、分区表、主系统、可选模拟器和初始 otadata，页面展示目标清单并要求确认；不整片擦除。

连接时通过 GET_SECURITY_INFO 识别 S31 chip ID 32，不把 S31 的零 magic 误认为 P4。确认芯片、非加密/非受限模式后才加载官方 S31 stub，并确认 16 MB Flash。固件写入采用官方 API、压缩传输和 Flash MD5 核对；失败不主动复位。写入校验通过后的 USB 自动复位失败单独提示手动重启，保持烧录成功状态。

静态公开固件不适用于加密或安全启动设备，本工具不更改 eFuse。升级保留 NVS 和模拟器启动选择；首次安装写入初始 otadata 会恢复从主系统启动。各模式均不操作 SD 卡。

## 验证

```powershell
node --test tools/web-flasher/test_core.cjs
# 需已安装 Playwright；可通过 PLAYWRIGHT_MODULE 指定其模块目录。
node tools/web-flasher/test_browser.cjs
```

核心测试使用实际发布镜像/分区表，覆盖芯片、身份、散列、槽位、NVS 保护和错误分区拦截。浏览器测试使用真实 Chrome 和 file:// HTML，覆盖离线加载、窄屏布局、三种写入计划、错误芯片/容量/安全模式、文件损坏、写入/校验失败不复位及成功后的复位异常。串口和底层 loader 在浏览器流程测试中模拟，**不会打开真实 USB 口，也不能替代实机烧录验收**。

官方来源和固定版本许可证见 `tools/web-flasher/vendor/THIRD_PARTY.md`。发布目录中的 `.bin` 是有意纳入的可分享产物，生成 ZIP 忽略于 Git；未执行自动提交或发布。
