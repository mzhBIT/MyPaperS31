# ZIP / CBZ 漫画阅读

## 方案

`ComicZip` → `ReflowBook::Format::ComicZip` → 现有 `EpubReaderActivity`。
只扫描 ZIP 目录索引，图片按需读入，每张图片生成一个仅含 img 的虚拟 XHTML 章节，复用现有图文阅读器的居中缩放、单资源预解码、图片页 Quality 刷新、目录/进度跳转、累计阅读计时。

没有新建异步任务，也没有改动 EPD、DMA、刷新波形或预取的并发策略。

## 排序与资源

- 扩展名 `.zip` 和 `.cbz` 均可从文件管理器打开；Wi-Fi 默认将这些文件放入 `/sdcard/books`。
- 支持 JPG/JPEG、PNG、BMP（扩展名不区分大小写），递归包含包内子目录。
- 对完整路径使用自然排序：数字段按数值排列，`1、2、10`；数值相同则前导零较少者优先。目录数字也按此规则排序。
- 比较任意长度数字串，不转换整数，避免大编号溢出；大小写相同时用原始字节序稳定打破平局。
- 跳过非图片、空图片、点号开头的隐藏路径、`__MACOSX` 资源路径。
- 每张图片用 `comic-image-N.ext` 别名映射到原 ZIP 项名称，避免名称中的 `#`、`?`、`&` 等字符被 XHTML 链接处理误截断；原始名称只用于 ZIP 读取和目录显示。
- 第一个有效图片用作封面。只按原有封面/资源逻辑缓存必要内容，不整包解压或保存生成页面。

## 持久化

- 书架类型为 `comic`，格式占位标识为 CBZ；实际打开 ZIP 和 CBZ 使用相同解析路径。
- 阅读进度复用 epubprogress/EPB1 和进度摘要，阅读时长使用现有独立时间记录，只在退出时保存。
- ZIP 索引增加 general-purpose flags，缓存版本从 1 升到 2。旧索引下次打开会自动重建一次，正文阅读进度不变。

## 限制

- 普通单盘 ZIP，支持存储（method 0）和 Deflate（method 8）。
- 不支持加密 ZIP、ZIP64、分卷 ZIP、RAR/CBR、BZIP2/LZMA 等其他 ZIP 压缩方式。
- 单张图片解压后上限 12 MiB，沿用现有图片资源预算。超限、加密或不支持压缩的图片明确报错，不静默漏页。
- 对内容不是图片漫画的 ZIP 显示“没有 JPG、PNG 或 BMP 图片”。
- 文件名目录显示按原始编码交给现有字体模块；推荐压缩包使用 UTF-8 文件名。
- Show images、背景、页眉等沿用全局阅读设置；不新增漫画专用阅读方向或双页模式。

## 验证

集成测试使用实际 `EpubZip`、miniz、`ComicZip`、XHTML 解析器，仅替换平台计时/内存接口，不 mock ZIP 读取。

```bat
python tools/tests/generate_comic_fixtures.py
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /EHsc /std:c++17 /utf-8 /TP /I main /I tools/tests/docx_stubs /I components/M5GFX/src tools/tests/comic_zip_test.cpp main/parser/comic_zip.cpp main/parser/epub_zip.cpp main/parser/epub_html.cpp components/M5GFX/src/lgfx/utility/lgfx_miniz.c /Fo:tmp/ /Fe:tmp/comic_zip_test.exe
tmp\comic_zip_test.exe
```

测试覆盖混合图片扩展名、子目录、隐藏文件、超长数字自然排序、前导零、特殊字符文件名、存储/Deflate 原始数据读取、前缀读取、索引缓存重开、空包、加密、错误压缩类型、ZIP64、超大图片和未知资源访问。

实机待验证：正常 JPG/渐进式 JPG/PNG/BMP 包连续前后翻页、目录和进度跳转、退出重开、封面和累计阅读时间、图片 Quality 刷新与预解码命中。
