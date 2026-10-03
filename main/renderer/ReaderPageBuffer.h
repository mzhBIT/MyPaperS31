#pragma once

#include <M5GFX.h>
#include "lgfx/v1/platforms/esp32/MyPaper_EPD_Config.hpp"
#include <memory>

#if MYPAPER_EPD_USE_2D_ENGINE
#include "lgfx/v1/platforms/esp32/Panel_EPD.hpp"
#include <esp_heap_caps.h>
#include <cstring>

// Offscreen destination using exactly the same gray quantizer and drawing
// implementation as Panel_EPD_2D. No bus, task, waveform, or display submission.
class ReaderPageBuffer : public lgfx::LGFX_Device {
    class MemoryPanel : public lgfx::Panel_EPD {
    public:
        ~MemoryPanel() override { heap_caps_free(_buf); _buf = nullptr; }
        bool allocate(int w, int h, uint8_t offset_rotation = 0) {
            auto cfg = config();
            cfg.panel_width = cfg.memory_width = w;
            cfg.panel_height = cfg.memory_height = h;
            cfg.offset_rotation = offset_rotation;
            config(cfg);
            _auto_display = false;
            _buf = static_cast<uint8_t *>(heap_caps_malloc(
                size_t((w + 1) / 2) * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            setColorDepth(lgfx::color_depth_t::grayscale_8bit);
            if (_buf) std::memset(_buf, 0xff, size_t((w + 1) / 2) * h);
            return _buf != nullptr;
        }
        const uint8_t *buffer() const { return _buf; }
        bool writeNativeGrayscale4Rect(const uint8_t *src, uint16_t w, uint16_t h,
                                       int16_t x, int16_t y) override {
            if (!src || !_buf || !w || !h || x < 0 || y < 0 ||
                x + w > width() || y + h > height()) return false;
            // Input has already been quantized by the image decoder. Copy raw
            // nibbles, avoiding a second gray quantization / any hardware path.
            const size_t src_stride = (size_t(w) + 1) / 2;
            const size_t dst_stride = size_t(_cfg.panel_width) / 2;
            for (uint16_t sy = 0; sy < h; ++sy) {
                for (uint16_t sx = 0; sx < w; ++sx) {
                    uint_fast16_t px = x + sx, py = y + sy;
                    _rotate_pos(px, py);
                    const uint8_t packed = src[size_t(sy) * src_stride + sx / 2];
                    const uint8_t gray = (sx & 1) ? packed & 15 : packed >> 4;
                    uint8_t &dst = _buf[size_t(py) * dst_stride + px / 2];
                    dst = (px & 1) ? (dst & 0xf0) | gray : (dst & 0x0f) | (gray << 4);
                }
            }
            return true;
        }
        bool init(bool) override { return _buf != nullptr; }
        void beginTransaction() override {}
        void endTransaction() override {}
        void waitDisplay() override {}
        bool displayBusy() override { return false; }
        void display(uint_fast16_t, uint_fast16_t, uint_fast16_t, uint_fast16_t) override {}
    } _memory;
public:
    ReaderPageBuffer() { setPanel(&_memory); }
    void setPsram(bool) {}
    const void *getBuffer() const { return _memory.buffer(); }
    bool createForPanel(const lgfx::Panel_Device &panel, uint8_t rotation) {
        const auto &cfg = panel.config();
        if (cfg.offset_x || cfg.offset_y || cfg.memory_width != cfg.panel_width ||
            (cfg.panel_width & 1)) return false;
        if (!_memory.allocate(cfg.panel_width, cfg.panel_height, cfg.offset_rotation)) return false;
        setRotation(rotation);
        setColorDepth(lgfx::color_depth_t::grayscale_8bit);
        setEpdMode(lgfx::epd_mode_t::epd_text);
        return true;
    }
    uint16_t physicalWidth() const { return _memory.config().panel_width; }
    uint16_t physicalHeight() const { return _memory.config().panel_height; }
    uint8_t offsetRotation() const { return _memory.config().offset_rotation; }
    bool createSprite(int w, int h) {
        if (!_memory.allocate(w, h)) return false;
        setPanel(&_memory);
        setRotation(0);
        setColorDepth(lgfx::color_depth_t::grayscale_8bit);
        setEpdMode(lgfx::epd_mode_t::epd_text);
        return true;
    }
};
#else
// Cache feature is disabled on the legacy backend.
using ReaderPageBuffer = lgfx::LGFX_Sprite;
#endif

// Shared by TXT and RSS. Ownership never escapes the activity/main thread.
// A ready buffer is immutable until import; all work is gated on EPD idle.
class ReaderFrameCache {
public:
    std::unique_ptr<ReaderPageBuffer> buffer;
    uint64_t key = 0;
    size_t item = 0;
    int cursor = 0;
    bool ready = false;
    mutable bool consumed = false;
    void reset() { buffer.reset(); key = 0; item = 0; cursor = 0; ready = false; consumed = false; }
    bool begin(uint64_t identity, int initial_cursor) {
#if MYPAPER_EPD_USE_2D_ENGINE
        key = identity;
        cursor = initial_cursor;
        item = 0;
        ready = false;
        consumed = false;
        buffer.reset(new ReaderPageBuffer());
        return true;
#else
        (void)identity; (void)initial_cursor;
        return false;
#endif
    }
    bool import(lgfx::LGFX_Device &display, uint64_t identity) const {
#if MYPAPER_EPD_USE_2D_ENGINE
        if (!ready || !buffer || key != identity || display.displayBusy() ||
            display.getPanel()->getDither() || buffer->width() != display.width() ||
            buffer->height() != display.height() ||
            buffer->getRotation() != display.getRotation() ||
            buffer->offsetRotation() != display.getPanel()->config().offset_rotation) return false;
        const bool ok = display.getPanel()->writePhysicalGrayscale4(
            static_cast<const uint8_t *>(buffer->getBuffer()),
            buffer->physicalWidth(), buffer->physicalHeight(), display.getRotation());
        if (ok) consumed = true;
        return ok;
#else
        (void)display; (void)identity;
        return false;
#endif
    }
};

// Keep the last two completely prepared pages, without copying their pixels.
// This covers back/forward navigation and jumps to recently visited positions.
class ReaderFrameHistory {
    ReaderFrameCache recent[2];
public:
    void clear() { for (auto &entry : recent) entry.reset(); }
    void remember(ReaderFrameCache &source) {
        if (!source.ready || !source.buffer || !source.consumed) return;
        recent[1] = std::move(recent[0]);
        recent[0] = std::move(source);
        source.reset();
    }
    bool import(lgfx::LGFX_Device &display, uint64_t identity) const {
        for (const auto &entry : recent) if (entry.import(display, identity)) return true;
        return false;
    }
};
