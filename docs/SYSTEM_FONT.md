# 系统 UI 字体

## 使用

设置 → 系统字体（设置列表向上滑动可见）。

- 默认：内置 UI TTF 子集，28 px。参考 pico reader 的约 27～28 px 常规 UI 文字，不是照搬其全部书架布局。
- 字号：24～48 px，每次增减 2 px，立即生效。
- 字体：内置字体，或 SD 卡 `/fonts` 目录下的 TTF / OTF 文件。
- 字体路径与字号存入 NVS `system_font` 命名空间的 `path` / `size` 键；重启保留，恢复默认设置时清除。
- 系统字号与阅读器正文字号独立。位图图标、地图署名小字及触摸校准数字不随系统字号变化。

当前嵌入字体来自 `Assets/pingxianzhensong.ttf`，生成产物约 497 KiB。无需把此源 TTF 放到 SD 卡才能显示常规界面。

## 生成与构建

```powershell
python -m pip install fonttools
python tools/subset_system_font.py
python tools/subset_system_font.py --check
```

换内置字体时指定 `--src path/to/font.ttf`。脚本扫描 `main` 下 C/C++ 字符串，包括英文、简体中文、繁体中文语言表、键盘和运算符；另外保留可打印 ASCII 与常用符号。字符覆盖取决于源字体本身，缺字写入 manifest，不会凭空生成字形。

提交 `main/assets/system-ui.ttf` 和 `system-ui.json`。普通 ESP-IDF 构建直接嵌入已生成文件，不需要 Python fonttools，也不会在每次构建时重裁剪。更新 UI 文案后需重新生成；`--check` 检查是否新增未包含字符。manifest 记录源文件名、SHA-256、字符数及缺字。

源字体保留原授权，发布固件前应确认该字体允许子集化及嵌入分发；本次不改变字体授权。

## 运行时

- 字体轮廓直接从 Flash 映射数据交给 FreeType，字号变化后重新栅格化，支持 4bpp 灰度字形。
- 系统 UI 使用独立 TextRenderer，不借用正文或下一页预绘制的缓存。最多缓存三个字号，每个按需打开的字体实例使用 128 KiB 字形池；SD 字体流缓存为 64 KiB。优先分配 PSRAM。
- UI 子集无法预知任意书名、文件名、短信、搜索结果。优先使用所选字体，缺字回退内置字体，再按需加载一份 SD 完整字体；优先 `pingxianzhensong.ttf`、`Hei.ttf`，然后文件名排序后的首个可用字体。没有覆盖该字的字体时显示缺字框，不能保证任意 Unicode 全覆盖。
- 所选 SD 字体缺失时使用内置字体显示 UI，原选择保留。重新插卡后重启或重新选择字体以重新加载。
- 原 `lv_font_SourceHanSansNormalCN_36.c` 不再参与 M5GFX 构建，包装符号与应用调用已移除；源文件留存但不会占用固件空间。

## 阅读器兼容

阅读字体仍使用原有独立设置与完整 SD 字体，不与系统字号联动。旧 `builtin:source_han_sans_cn_36` 阅读配置在启动时迁移到可用的 SD 完整字体：优先上述两个文件，否则按文件名选择第一份 TTF / OTF。用户原先选择的有效 SD 字体保持不变。没有 SD 完整字体时只能使用内置 UI 子集，普通书籍会缺字，需安装完整字体。

## 实机验证

1. 三种系统语言下打开主页、设置、键盘、文件管理器与计算器。
2. 调整 24 / 28 / 48 px，检查页眉、设置行、输入框及较长文案；布局仍为固定尺寸，超长标题按原逻辑省略。
3. 切换 SD TTF / OTF，重启检查持久化；移除所选字体检查内置回退。
4. 用不在 UI 文案中的中文书名/短信检查 SD 字体回退。
5. 正文阅读、预绘制翻页、字号切换及 RSS 阅读保持正常。
6. 恢复默认设置后回到内置 28 px。
