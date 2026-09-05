#include "weread/WereadQr.h"

#include "weread/qrcodegen.hpp"

#include <algorithm>

namespace weread {

bool draw_qr(lgfx::LGFXBase &display, const std::string &text, int x, int y, int size)
{
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
            text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        constexpr int border = 4;
        const int modules = qr.getSize() + border * 2;
        const int scale = std::max(1, size / modules);
        const int drawn = modules * scale;
        const int left = x + (size - drawn) / 2;
        const int top = y + (size - drawn) / 2;
        display.fillRect(x, y, size, size, TFT_WHITE);
        for (int module_y = -border; module_y < qr.getSize() + border; ++module_y) {
            for (int module_x = -border; module_x < qr.getSize() + border; ++module_x) {
                if (qr.getModule(module_x, module_y)) {
                    display.fillRect(left + (module_x + border) * scale,
                                     top + (module_y + border) * scale,
                                     scale, scale, TFT_BLACK);
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace weread
