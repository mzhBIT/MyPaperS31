# Reader Page Prefetch Plan

## Goal

Reduce perceived page-turn latency for EPUB and MOBI reading by preparing the
next page after the current page is drawn. The first implementation stays above
the EPD waveform layer: it warms text glyphs, extracts/probes page images, and
prepares the next spine's page layout when the current page is near the section
boundary.

## Current Page-Turn Path

The reflow reader currently does most of the work only after the user turns a
page:

1. Update page/spine index.
2. Save progress.
3. Prepare glyphs for the page text.
4. Extract/probe/draw images.
5. Draw page into the display framebuffer.
6. Queue the EPD refresh.
7. The 2D EPD engine prepares and scans waveform phases.

Only the actual EPD scan must happen after the user action. Glyph preparation,
image extraction/probing, and the next spine layout can be prepared earlier.

## Stage 1: Single-resource pipeline

- Schedule prefetch after every reader redraw.
- Never prefetch while `displayBusy()` is true. The EPD scan owns the PSRAM
  bandwidth until the physical refresh is complete.
- Prefer the next page in the current spine.
- If the current page is the last page in the spine, parse and paginate the next
  non-empty spine into a side cache without changing the visible page.
- Advance only one resource stage per activity-loop pass: layout, glyphs, then
  one image read/decode. There is no concurrent document/image worker.
- Build the candidate page text and call `TextRenderer::preparePage()` in the
  glyph stage before the user turns the page.
- Keep at most one decoded JPEG/PNG/BMP resource in PSRAM as a target-sized native
  packed 4bpp grayscale buffer. EPUB/MOBI/FB2/Markdown and cached WeRead resources are
  read directly into memory; body images are not extracted into the temporary
  cache directory. The encoded vector is released after a successful decode.
- On a page turn, copy the decoded rectangle directly into the 2D EPD draw
  packed buffer with `writeNativeGrayscale4Rect()`. This bypasses `pushSprite()` color
  conversion and never writes the EPD target or physical-state buffers.
- When `next_page()` reaches a prefetched spine, swap the prefetched pages into
  the active reader state instead of calling `load_spine()`.
- Invalidate prefetch on font/style changes, chapter jumps, and manual spine
  reloads.

This stage avoids background threads and runs from the reader activity loop, so
it does not introduce concurrent access to the global text renderer, document
parser, PSRAM framebuffer, or EPD DMA source. A very fast page turn can still
reach a resource before its stage is ready; that resource then falls back to a
synchronous in-memory read/decode, never a temporary-file extraction.

## Stage 2: Native 4bpp Framebuffer Import

Render the prefetched page into an off-screen framebuffer. The estimated memory
cost for a full 4bpp screen is about 416 KB. This removes most of the foreground
draw cost by letting the foreground page turn import an already-rendered
`LGFX_Sprite` into the EPD framebuffer.

The first `pushSprite()` implementation was rejected because it copied through
LGFX's generic pixel conversion path. That path is slow for a full-screen EPD
framebuffer, and a paletted 4bpp sprite can visibly degrade grayscale quality by
adding an extra quantization/conversion step.

The current implementation avoids `pushSprite()` and uses
`Panel_EPD_2D::writeNativeGrayscale4()` to copy a raw grayscale 4bpp buffer into
the panel's native 4bpp framebuffer with the correct rotation and packing. This
preserves the same 16-level grayscale representation used by normal EPD drawing.

## Stage 3: Deferred

Precompute 2D waveform phase frames for the prefetched framebuffer. This can
remove the `waveform prepared before power-on` time, but it is only valid while
the old framebuffer state remains unchanged. Any popup, overlay, settings
change, or unrelated refresh must invalidate it.

## Invalidation Rules

Prefetch must be discarded when:

- Reader style, font, margins, line spacing, or refresh mode changes.
- The user jumps to a chapter/spine.
- The document changes.
- The cached target no longer matches the next visible page.
- A future waveform prefetch is added and the current framebuffer changes.
