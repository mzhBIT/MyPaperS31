# DOCX 重排阅读

## 目标与接口

DOCX 作为本地书籍接入 `ReflowBook::Format::Docx`，复用 `EpubReaderActivity`，不新增排版引擎，也不生成 EPUB 文件或保存排版结果。

参考：[Microsoft WordprocessingML 结构](https://learn.microsoft.com/en-us/office/open-xml/word/structure-of-a-wordprocessingml-document)、[嵌入图片关系引用](https://learn.microsoft.com/en-us/dotnet/api/documentformat.openxml.drawing.blip)。

流程：EpubZip 读取 OOXML 部件 → DocxBook 将段落/文字运行转成 XHTML 内存片段 → 共用 XHTML 块解析 → 简繁转换、实时分页、字形缓存与图文绘制。

## 首版支持

- 中英文段落及 XML 字符实体。
- 标题样式、outlineLvl、基于父样式的标题继承；按标题拆分片段并提供目录。
- 非标题连续内容超过约 64 KiB 后，在下一段边界拆分为续段，避免整本书一次分页；续段目录名沿用文档标题。
- 段落/运行粗体与斜体，复用当前合成字体策略。格式关闭值可覆盖继承样式。共用 XHTML 解析新增可选样式保留参数，只对 DOCX 开启，不改变其他格式的现有行为。
- 编号列表统一显示项目符号（不复刻 Word 多级编号）。
- DrawingML `blip` 与 VML `imagedata` 的内部图片，使用原有 JPG/PNG/BMP 解码、图片预取及图片页 Quality 策略；图片按固件默认尺寸规则重排，不复刻 Word 浮动位置和裁剪。
- 脚注、尾注转为现有点击查看机制，不占正文分页空间。
- 表格按行、单元格、段落的文档顺序展开为正文，不画网格。
- 修订删除内容不显示，保留普通正文与插入内容。
- 文件管理器、最近阅读书架、Wi-Fi 默认上传到 books 目录。
- 阅读进度、阅读计时、字体、简繁转换和阅读美化复用本地阅读接口。

## 存储与边界

- 主文档 XML 上限 8 MiB，生成正文 XHTML 上限 12 MiB；关系/样式部件各上限 1 MiB，脚注及尾注源各上限 2 MiB。
- 使用扫描式 XML 解析而非 DOM，限制嵌套深度；每约 500 ms 连续处理让出一次 CPU，远低于看门狗上限且避免按每个小块 sleep。
- 主 XML 在 XHTML 分段拼接前释放，移动段落数据以降低内存峰值；此版仍在内存保留全部生成的 XHTML，大文件占用取决于正文大小。
- 图片仍从 DOCX ZIP 资源部件按需读取，复用现有资源缓存；不把 DOCX 全部解压到 SD 卡。
- 进度位置保存到原有 epubprogress 目录，复用 EPB1 结构，书架读取相同的进度摘要；累计阅读时间采用独立 per-book 时间记录。
- 无封面时显示 DOCX 占位封面。原始 ZIP 索引可能使用已有的索引缓存，这是容器索引，不是排版缓存。
- 不支持旧 `.doc`、加密 DOCX、OLE 附件、宏、公式排版、图表/SmartArt 渲染、页眉页脚、原始页码、Word 文字框布局、文档自带字体及复杂 CSS/样式完全还原。
- 外部图片关系不发起网络请求；现有支持范围以 UTF-8 XML、常规 WordprocessingML 文档为准。

## 验证与回归

Host 测试使用实际 DocxBook 和 XHTML 解析代码，模拟 ZIP 部件读取：`tools/tests/docx_parser_test.cpp`。

Windows/MSVC：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /EHsc /std:c++17 /utf-8 /I main /I tools/tests/docx_stubs tools/tests/docx_parser_test.cpp main/parser/docx_parser.cpp main/parser/epub_html.cpp /Fo:tmp/ /Fe:tmp/docx_parser_test.exe
tmp\docx_parser_test.exe
```

覆盖标题继承、粗斜体、格式关闭、中文/数字实体、图片路径/大小限制、脚注/尾注、删除修订、表格顺序、长文分段、损坏/超大/加密文件，以及默认 XHTML 样式兼容保护。ZIP 解压本身复用现有 EPUB 实现，不在本测试中模拟 USB/SD 时序。

设备验证：从文件管理器打开普通 DOCX，测试目录跳转、脚注弹窗、图片页与其后一页的 Quality 清理、简繁切换、退出后恢复进度和累计阅读时间。尚需实际 Word/WPS 文档验证复杂样式差异。
