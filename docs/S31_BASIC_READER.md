# MyPaperS31 SD / 字体 / TXT 基础阅读验证

日期：2026-10-24。阶段 2 的灰阶、显示方向和触摸切换已通过实机验证。
更新：本页记录历史验证切片；当前默认构建已改为
[共用 P4 完整应用](S31_FULL_APPLICATION.md)，下面的 BasicReader 不再参与默认编译。
本阶段先建立可测试的小闭环，不直接编译依赖 P4 外设的整个 main 组件。

## 本次范围

- S31 独立 SD 后端，复用 `storage_sdcard.h` 的挂载、卸载、卡信息接口。
  USB raw-medium 等接口暂未实现；当前应用不调用这些接口。
- SDMMC Slot0，4 位，20MHz；CLK24、CMD25、D0..3=20/21/22/23，来自已有 Board 引脚表。
- 不自动格式化，不创建/覆盖书籍、P4 阅读进度和设置文件。不支持热拔卡；换卡先关机。
- FAT 长文件名 / UTF-8；挂载允许 24 个文件描述符，挂载失败显示 Retry。
- 复用现有 FreeType `TextRenderer` 和字形缓存，不复制字体光栅化实现。
  TextRenderer 公共头在 S31 上只依赖 M5GFX，P4 保留原 M5Unified 包含关系。
- IDF 6.1 的 libc 名称和 SD DMA 分配 API 单独兼容。S31 字体缓存走对齐的
  PSRAM 缓冲及 POSIX read，DMA 可用性不作虚假声明，由 FAT/SD 驱动按需中转。
  P4 原来的 DMA 分配逻辑不变。
- 独立基础 TXT 流式解码器，支持 UTF-8（可带 BOM）、GBK、带 BOM 的 UTF-16LE/BE。
  无 BOM 编码根据前 4KiB 判断，自动判断并非绝对可靠。最大文件为 INT32_MAX 字节。
- 只按当前可用宽高分页，不预载整本；上一页使用本次打开时的内存偏移历史。
  尚未接入 P4 的段落两端对齐、断词、简繁转换、竖排、持久化进度和阅读时长。
- 第一次打开 / 退出书籍用 Quality，普通翻页和目录切换用 Text。
  保留已验证的显示时序、VCOM 和波形数据，仍全部准备驱动帧再扫描。

## 操作

1. 将 TXT 放在 SD 卡 `books` 中，TTF / OTF 放在 `fonts` 中；路径即 `/sdcard/books`、`/sdcard/fonts`。
2. 开机显示基础目录浏览器；没有 books 目录时回退到 SD 根目录。
3. 点击 `+` 开头的行进入目录，点击 TXT 打开。顶部 Back 返回上级或退出阅读。
4. 目录底部 Previous / Next 分页；单目录验证列表最多显示 512 项，超过会提示。
5. 点击 Font 行循环切换字体（最多列出 127 个外部字体和一个固件字体）。
   默认尝试排序后的第一个 SD 字体，失败回退固件子集；字体选择暂不持久化。
6. 阅读时点击左半屏上一页、右半屏下一页，顶部左侧 Back 退出；到末页不会继续前进。

固件字体为现有 `system-ui.ttf` 子集，不能保证任意书籍中的汉字均覆盖。
完整中文阅读请选 SD 卡上的完整 TTF / OTF。使用 36px 正文、384KiB 字形池，
字体流缓存 16 个窗口。切换字体复用同一个渲染器并释放旧字体资源。
本次验证 UI 新提示均为基本 ASCII，未新增子集外的 UI 汉字，无需重生成 TTF 子集。
这不是最终本地化系统界面。

## 构建与回退

默认执行 `scripts/build-board.ps1`，输出 `build-s31/MyPaperS31.bin`。
S31 main 栈设为 32KiB：FreeType `gray_convert_glyph` 的局部光栅池本身就需要
约 16KiB，不能把整个任务栈也设为 16KiB。其余空间供字体解析、UI 和编码探测调用链使用。
验证 reader 增加了 32KiB 最小配置编译检查，首次绘制后打印 main 栈最低剩余字节数。
已有 build-s31/sdkconfig 也已同步 main 栈与 FAT UTF-8；其他旧构建目录如 Debug/Release
需自行核对有效配置，defaults 不会覆盖其已有 menuconfig 值。

menuconfig 的 `MyPaperS31 bring-up` 下关闭 `MYPAPER_S31_BASIC_READER`，
可回到原有四幅显示/触摸测试图（板级仍会尝试挂载 SD，但不会格式化）。
S31 构建继续禁用 Component Manager，只增加本地 FreeType 与 IDF FAT/SD 组件。
P4 的 main 源文件清单、外设引脚和 sdkconfig 不改动。

## 验收

- 启动应有 `S31_SD: mounted`、卡容量信息、`TEXT_RENDER: begin`、`S31_READER: browse`。
- 目录中文名称正确；无 SD 时有错误提示，启动不重启循环；插入卡后点击 Retry。
- 用完整字体打开 UTF-8、GBK、UTF-16 测试书，检查中文、标点、连续前翻与后翻。
- 检查页末/页首文字衔接，不应漏字、重复字或无法越过末页。
- 字体切换前后多次打开/返回，检查文件描述符、PSRAM 和屏幕灰阶是否稳定。
- 日志 `TXT page offset/next/size/lines/encoding/prepare` 可用于定位 SD、分页和字体问题。

PC 测试：`tools/test_s31_txt_stream.cpp` 覆盖 BOM、GBK、UTF-16 代理对、
非法/截断输入、字节偏移回读和 EOF；实机验证仍需用户烧录完成。
下一步在此闭环通过后接入 EPUB / 完整阅读应用，不在本次承诺全部格式已经可用。
