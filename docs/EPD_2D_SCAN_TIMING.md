# EPD 二维引擎连续扫描时序

## 目标

二维波形引擎直接执行官方 GC16 的各个相位，不在相位前插入旧驱动的
LUT eraser 或额外黑白预处理。旧 `Panel_EPD` 和原 I80 扫描路径保持不变。

ESP32-P4 的二维引擎使用 LCD_CAM RGB 时序发生器、AXI GDMA 整帧传输和
RMT CKV。每个波形相位先生成完整的 source 数据，再连续扫描 684 行，避免
逐行提交 I80 事务造成相位被拉长。

## 当前官方时序基线

| 参数 | 宏 | 当前值 |
| --- | --- | ---: |
| 连续扫描开关 | `MYPAPER_EPD_2D_CONTINUOUS_SCAN` | `1` |
| 相位序列预生成 | `MYPAPER_EPD_2D_PRECOMPUTE_WAVEFORM` | `1` |
| source 时钟 | `MYPAPER_EPD_2D_PIXEL_CLOCK_HZ` | 20 MHz |
| 行周期 | `MYPAPER_EPD_2D_LINE_PERIOD_US` | 16 us |
| CKV 高电平 | `MYPAPER_EPD_2D_CKV_HIGH_DUS` | 6.0 us |
| LE 高电平 | `MYPAPER_EPD_2D_LE_HIGH_PCLKS` | 4 PCLK |
| 行前沿 | `MYPAPER_EPD_2D_LINE_FRONT_PCLKS` | 4 PCLK |

1216 像素在 16-bit source bus 上按每个时钟 8 个 2-bit 像素输出，因此有效区
为 152 PCLK。整行固定为 320 PCLK：

```text
320 / 20 MHz = 16 us/line
16 us * 684 = 10.944 ms/phase
```

剩余的行消隐时钟由引擎根据上述宏自动计算。若有效区加固定前沿超过总行
周期，引擎会拒绝初始化并打印 `invalid line timing`。

## 宏隔离与回退

- `MYPAPER_EPD_USE_2D_ENGINE=0`：使用原 `Panel_EPD`，完全不进入二维引擎。
- `MYPAPER_EPD_USE_2D_ENGINE=1` 且
  `MYPAPER_EPD_2D_CONTINUOUS_SCAN=0`：二维查表仍保留，但回退到原逐行 I80
  扫描，便于对照测试。
- 两个宏均为 `1`：使用新的连续整相位扫描。

所有相位和扫描周期实验只修改 `MyPaper_EPD_Config.hpp` 中的二维专用宏，
不要修改 `Bus_EPD` 原逐行 CKV/LE 延时，以免影响旧引擎。

连续扫描启用相位序列预生成时，48个1216×684驱动帧会在面板上电前生成，
临时占用约9.52 MiB PSRAM。面板上电后不再执行逐相位像素转换，也不在相位
循环内打印日志；整个缓存会在本批次断电后立即释放。这样可以避免CPU转换和
串口输出在官方相位之间插入数十毫秒的空档。分配失败时本批次会直接终止，
不会静默回退到时序不正确的边生成边扫描方式。

## 实屏验证

启动时应出现：

```text
EPD_2D_SCAN: continuous scan ready: ... pclk=20000000 Hz line=16 us CKV=6.0 us
Bus_EPD_V7: 2D continuous LCD_CAM scan selected; legacy I80 not initialized
```

Quality 完成后会汇总输出 `backend=continuous-precomputed`。重点检查：

1. `scan_min` 和 `scan_max` 应接近 11 ms；它们包含帧启动的少量 STV/CKV
   前导时间，因此允许略高。
2. `sequence` 应接近所有相位的 `scan_total`，差值是缓存同步和帧提交开销；
   `submit_overhead_max` 用于定位最差的相位间提交开销。
3. 先分别执行诊断页的 `White -> 16 gray` 和 `Black -> 16 gray`，对比灰阶
   塌陷是否解除。
4. 若灰阶仍塌陷，保持波形数据和 VCOM 不变，优先小步调整行周期和 CKV 高电平，
   每次只改变一个宏并记录结果。
5. 若出现 `continuous frame timeout`，立即回退连续扫描宏，检查 LCD VSYNC、
   GDMA 长度和 CKV/STV 示波器波形，不应继续调整 GC16 表内容。

## 物理灰阶状态提交

差分波形完成后不能无条件把逻辑目标复制为物理状态。引擎会检查本次波形中
每个 `old -> new` 转换是否至少包含一个非零驱动相位：有实际驱动才把该像素
提交为新灰阶；全程为零则保留旧灰阶。

这对DU尤其重要：当前官方DU表只驱动目标0和15，目标1～14的行全为零。
Fast/Fastest画面中的中间灰阶实际上保持旧状态。日志字段
`undriven_retained`记录因此未提交的像素数，防止后续GC16使用虚假的旧灰阶并
留下上一张画面的轮廓。
