# MyPaperP4 二维波形刷新引擎设计与 Quality 验证方案

状态：第一阶段已实现，可编译，等待实屏验证
目标面板：E0470A01-AF-S，1216×684，16-bit source bus
实现入口：`Panel_EPD_2D.cpp`
回退方式：将 `MYPAPER_EPD_USE_2D_ENGINE` 设为 `0`，重新编译即可恢复旧 `Panel_EPD`

## 1. 官方波形检查结论

`main/waveforms` 中的文件采用 EPDiy 兼容格式，不是一维灰阶波形：

- `GC16`：type 2，48 个相位，用于 Quality。
- `GL16`：type 5，当前复用同一份 48 相位 GC16 数据，用于 Text。
- `DU`：type 1，20 个相位，用于 Fast/Fastest。
- 温区：当前只有一个 `0°C <= T < 50°C` 区间。
- `phase_times`：当前均为 `NULL`，每个相位执行一次完整面板扫描。
- GC16 数据体为 `48 × 16 × 4 = 3072 bytes`。

每一相位是完整的 `16 × 16` 状态转换表。数据按目标灰阶分成 16 行，
每行的四个字节按高位到低位打包 16 个源灰阶对应的 2-bit 驱动值：

```text
drive = waveform[phase][new_gray][old_gray]
```

驱动值保持官方定义并直接送往面板 source bus：

- `0`：不驱动
- `1`：黑向驱动
- `2`：白向驱动
- `3`：波形定义的第三/高阻状态，不由引擎重新解释

## 2. 修订后的架构

旧 `Panel_EPD.cpp` 不再作为二维改造载体。新建 `Panel_EPD_2D`，只复用旧面板类
已经稳定的 M5GFX 绘图前端和 4bpp 像素写入能力；以下部分全部由新类独立持有：

- 刷新请求队列及后台任务
- 官方二维波形选择和温区选择
- 稳定目标快照
- 已知物理屏幕状态
- 每相位 `old -> new` 查表与扫描线生成
- 双 DMA 行缓冲
- 电源成功/失败状态管理

运行时缓冲规划：

| 缓冲 | 大小 | 内存 | 用途 |
| --- | ---: | --- | --- |
| draw framebuffer | 415,872 B | PSRAM | M5GFX 当前绘图结果 |
| physical framebuffer | 415,872 B | PSRAM | 上一批成功完成后的屏幕物理灰阶 |
| target snapshot | 415,872 B | PSRAM | 当前波形批次不可变的目标图像 |
| DMA line × 2 | 314 B × 2 | internal DMA | 行生成与 I80 发送流水 |
| transition LUT | 256 B | task stack/internal | 当前相位展开后的单像素转换表 |

三张整屏缓冲合计 1,247,616 B。相比旧引擎的 draw buffer 加 16-bit 双槽状态缓冲，
二维引擎的运行时 PSRAM 占用更低，且状态含义更明确。

## 3. 刷新批次规则

1. `display()` 只提交矩形、模式和当前 draw framebuffer 的可见性信息。
2. 后台任务收到请求后，先合并队列中尚未开始的请求。
3. 开始批次前把 draw framebuffer 复制到 target snapshot。
4. 从第一相位到最后一相位期间，physical 和 target 都保持不变。
5. 新绘图请求在当前批次运行时进入下一批队列，不允许中途重定向当前波形。
6. 全部相位扫描且断电成功后，才把 target 对应区域提交为新的 physical 状态。
7. 上电失败时禁止扫描；断电失败或批次异常时将 physical 状态标记为未知。

这是与旧引擎最关键的行为差异：二维波形必须绑定一个稳定的 `(old, new)` 状态对，
不能在第 N 相位运行期间替换目标像素。

## 4. Quality 第一阶段策略

官方 GC16 是全刷波形，因此第一阶段对 Quality 采用严格策略：

- 无论调用方提交多大的矩形，Quality 都扩展为 1216×684 整屏批次。
- 每个像素均按 physical 灰阶和 target 灰阶查找 48 个相位的驱动值。
- 开机时软件无法读取屏幕粒子状态，暂以全白作为 physical 初值。
- 第一次请求即使是 Fast/Text，也自动提升为整屏 Quality，以建立已知状态。
- Quality 成功后，才允许 DU/GL16 基于已知 physical 状态进行后续刷新。

注意：以全白作为开机初值只适用于设备正常关机后屏幕确实保持白底或出厂测试场景。
若需要覆盖任意掉电画面，后续必须加入官方 INIT/清屏波形，或执行官方定义的恢复序列，
不能用 GC16 自身替代 INIT。

## 5. 模式映射

| M5GFX 模式 | 二维波形 | 当前行为 |
| --- | --- | --- |
| `epd_quality` | GC16/type 2 | 48 相位，强制整屏 |
| `epd_text` | GL16/type 5 | 48 相位，允许矩形；当前数据与 GC16 相同 |
| `epd_fast` | DU/type 1 | 20 相位，允许矩形 |
| `epd_fastest` | DU/type 1 | 暂与 Fast 相同 |

本阶段重点只验证 Quality。其余映射是为了让现有 UI 可运行，并不代表已经完成速度、
残影或局刷边界优化。

## 6. 扫描线生成

draw/physical/target 均为每字节两个像素的 4bpp 格式。每个相位先把官方压缩表展开为
256 项单像素转换表，再为每个 framebuffer 字节做两次 `old -> new` 查询，得到四位的
两个像素驱动结果。16 个像素组合为一个 32-bit source word，并沿用 E0470 当前已经验证的
字节排列 `MYPAPER_EPD_OUTPUT_PERMUTE=1` 和 V7 I80 双行缓冲扫描时序。

二维计算只替代“该像素本相位应输出什么”，不修改以下硬件路径：

- TPS65185/PCA9535 上下电序列
- VCOM 配置
- CKV/STV/LE/source I80 时序
- 26.67 MHz I80 总线速率
- 1216/4 + 10 字节的行传输长度

## 7. 实屏验证顺序

### 7.1 上电前确认

- 确认官方波形对应的面板完整型号确为 E0470A01-AF-S。
- 确认波形设计时使用的 VCOM 与当前 `1050 mV` 设置一致。
- 首轮使用规则的黑白/16 阶灰阶测试图，不先用复杂 UI 判断效果。
- 保留串口日志，特别关注 TPS65185 PG 和每相位扫描时间。

### 7.2 第一轮：基本方向和极性

1. 白屏刷新到全黑。
2. 全黑刷新到全白。
3. 白屏刷新到 0–15 横向灰阶条。
4. 灰阶条反向刷新。

检查项目：

- 黑白方向是否颠倒。
- 灰阶顺序是否正确。
- 是否存在每 2、4、8 或 16 像素重复/错位；这可定位 nibble、字节或总线排列问题。
- 上下/左右方向是否与 framebuffer 一致。
- 48 个相位是否全部完成且最后正常断电。

### 7.3 第二轮：二维转换矩阵

建立 16×16 方块矩阵：列为 old gray，行为 new gray。先生成每列对应的旧灰阶，完成一次
Quality；再刷新为每行目标灰阶。重点检查中间灰阶之间的转换，而不只检查白到灰、黑到灰。
这是区分真正二维波形和一维目标灰阶波形的核心测试。

### 7.4 第三轮：重复与残影

- 同一灰阶图重复 Quality 10 次。
- 黑白棋盘与灰阶渐变交替 20 次。
- 记录温度、VCOM、每次完成后的亮度和残影趋势。

## 8. 首轮日志预期

```text
Panel_EPD_2D: init complete: waveform_modes=3 temp_ranges=1 initial_state=assume-white
Panel_EPD_2D: batch begin: mode=1 waveform_type=2 phases=48 ... area=0,0 1216x684
Panel_EPD_2D: phase 1/48 complete: scan=... ms work=... us
...
Panel_EPD_2D: phase 48/48 complete: scan=... ms work=... us
Panel_EPD_2D: batch complete: phases=48 state=known
```

若出现 `power on failed; batch aborted before scan`，引擎不会继续向未上电面板发送扫描数据。

## 9. 已知限制与后续阶段

1. 当前波形仅有一个宽温区，尚未读取 TPS65185/板载温度并动态选表。
2. 未实现 INIT/Recovery 波形，非正常掉电后的物理态无法严格恢复。
3. `phase_times` 当前为空；引擎检测到非空时会告警，但仍按固定完整扫描执行。
4. Text 当前等同 48 相位 GC16，需在 Quality 验证后确认是否有真正 GL16 数据。
5. Fast/Fastest 暂共用 DU，尚未加入 A2、刷新次数限制和残影债务策略。
6. 尚未实现 tile ghost debt、周期性 GC16 提升和局刷边界扩张策略。
7. 当前目标快照在后台任务收到请求时创建；若调用方在不等待刷新时立即改写同一区域，
   队列会合并到较新的画面。后续可增加提交快照池以提供严格逐请求语义。

下一阶段只应在 Quality 的方向、极性、灰阶矩阵和 VCOM 均通过实屏验证后开始。

## 10. 专用实屏诊断页

入口位于 `Settings > EPD Diagnostics`。页面内的测试刷新都会等待整个波形批次完成，
避免上一批扫描尚未结束时修改目标 framebuffer。建议依次执行以下项目：

1. `Direction & pixel order`：显示四角方向标记、不同宽度的纵向和横向条纹。
   用于检查上下/左右方向、黑白极性，以及 1/2/4/8/16 像素周期是否发生重复、错位或字节交换。
2. `16-level grayscale`：关闭抖动后，以各4bpp量化区间的中心值显示0～15原生灰阶条，
   保证测试标签与 framebuffer 灰阶一一对应。应观察到方向连续且能够区分的16个阶级；
   若顺序反转，先排查灰阶编码极性。
3. `White -> 16 gray`：先用一次完整 Quality 建立全白物理状态，等待完成后自动执行
   第二次 Quality，将全屏转换为精确的16级灰阶条。
4. `Black -> 16 gray`：先用一次完整 Quality 建立全黑物理状态，再自动转换为相同的
   16级灰阶条。将结果与全白起始测试对比，可判断同一目标灰阶是否仍依赖旧状态。
5. `16x16 transitions`：第一步先写入 16 列旧灰阶，待 Quality 完成后点击屏幕或按 OK，
   第二步写入 16 行新灰阶。最终每一行目标灰阶相同，因此同一行各列若出现明显差异，
   说明部分 `old -> new` 转换不正确或波形/像素排列仍有问题。
6. `VCOM measurement`：显示 TPS65185 当前配置值和 kickback 测量值。
   此操作仅测量和记录，不自动改写 VCOM；实际调整仍需依据面板标签或官方规格并小步验证。

诊断页当前以 Quality 为主要验证对象。Fast/Fastest 虽已接入 DU 路径，但在上述项目
通过之前，不应据此判断局刷速度和残影策略是否达到可用状态。
