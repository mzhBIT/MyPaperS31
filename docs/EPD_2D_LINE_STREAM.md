# ESP32-P4 2D line-stream backend

## Scope

> Status: disabled by default. Hardware testing showed that the current P4
> circular-GDMA path can consume the software queues without reliably showing
> the refilled line data on the panel. Fast therefore uses validated
> whole-phase precomputation.

`MYPAPER_EPD_2D_LINE_STREAM=1` enables an experimental epdiy-style producer/consumer path
for DU (`epd_fast` and `epd_fastest`). GC16/Quality deliberately continues to
use whole-sequence precomputation until the stream timing has been validated on
the panel. The legacy `Panel_EPD` implementation is unaffected.

Set `MYPAPER_EPD_2D_LINE_STREAM=0` to restore the previous whole-phase path.

## Architecture

- Two high-priority tasks are pinned to core 0 and core 1.
- Core 0 renders even rows and core 1 renders odd rows.
- Each producer owns a 96-row internal-RAM circular queue.
- A phase starts after 184 rows have been prepared.
- AXI GDMA cycles over two internal-RAM bounce buffers, each containing four
  rows. The DMA EOF callback refills the buffer that has just been consumed.
- Only the current phase is rendered. The DU sequence and phase count are not
  deduplicated or modified.

At 1216 pixels, the line queues consume about 57 KiB and the two bounce buffers
about 2.4 KiB. This replaces the 4.16 MiB DU phase cache. Quality still uses
the existing temporary GC16 phase cache.

## Failure semantics

An empty producer queue is a waveform error, not a normal idle line. The ISR
counts the underrun, the phase returns failure, and `Panel_EPD_2D` invalidates
its tracked physical state. The next display request is therefore promoted to
Quality. No failed streamed phase is committed to the physical-state buffer.

## Expected logs

Initialization:

```text
EPD_2D_SCAN: line stream ready: producers=2 queue=96 lines/core bounce=4 lines ...
```

A successful Fast batch:

```text
Panel_EPD_2D: phase sequence complete: ... prefetch_total=... prefetch_max=... backend=continuous-line-stream
```

`prefetch_max` measures click-to-ready latency for the slowest DU phase.
`sequence - scan_total` is the accumulated preparation and phase-switching
overhead. Any `line stream underrun` means the current queue depth or row
renderer throughput is insufficient and the stream macro should be disabled
until corrected.

Quality should continue to log `waveform prepared before power-on` followed by
`backend=continuous-precomputed`.
