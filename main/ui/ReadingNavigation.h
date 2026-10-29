#pragma once
// Navigation adapted from read-pico-reader's portrait UI.
// Copyright 2026 mindreset. SPDX-License-Identifier: Apache-2.0
#include "ui/ReadingNavigationLayout.h"
#include "ui/BookshelfLayout.h"
#include "ui/BookshelfFontFit.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include "ui/pico_shelf_icons.h"
#include <algorithm>

namespace reading_navigation {
enum class Tab { Home, Bookshelf, Files, Settings };
inline uint16_t gray(int v) { return ((v>>3)<<11)|((v>>2)<<5)|(v>>3); }
inline void icon(const uint8_t* mask, int cx) {
    namespace n = reading_navigation_layout;
    constexpr int size = n::icon_size;
    constexpr int step = (72 << 16) / size, half = step / 2 - (1 << 15);
    for (int y=0; y<size; ++y) {
        lgfx::rgb565_t row[size];
        for (int x=0; x<size; ++x) {
            const int sx=x*step+half, sy=y*step+half, ix=sx>>16, iy=sy>>16;
            auto alpha = [&](int px,int py) {
                const int at=std::clamp(py,0,71)*72+std::clamp(px,0,71);
                return (at&1) ? mask[at/2]&15 : mask[at/2]>>4;
            };
            const int fx=sx&65535, fy=sy&65535;
            const int upper=(alpha(ix,iy)*(65536-fx)+alpha(ix+1,iy)*fx)>>16;
            const int lower=(alpha(ix,iy+1)*(65536-fx)+alpha(ix+1,iy+1)*fx)>>16;
            const int a=(upper*(65536-fy)+lower*fy)>>16;
            row[x]=gray((((255*(15-a)+7)/15)>>4)*17);
        }
        M5.Display.pushImage(cx-size/2,n::icon_y-size/2+y,size,1,row);
    }
}
inline void draw(Tab selected) {
    namespace n = reading_navigation_layout;
    using ui_locale::TextId;
    M5.Display.fillRect(0,n::top,n::width,n::height,TFT_WHITE);
    M5.Display.drawFastHLine(0,n::top,n::width,gray(160));
    M5.Display.fillRect(n::center_x(int(selected))-n::indicator_width/2,n::indicator_y,
                       n::indicator_width,n::indicator_height,TFT_BLACK);
    const uint8_t* icons[]={pico_shelf_icons::house,pico_shelf_icons::library_big,
        pico_shelf_icons::folder_open,pico_shelf_icons::sliders_horizontal};
    const TextId labels[]={TextId::FileNavHome,TextId::Bookshelf,TextId::Files,TextId::Settings};
    static bookshelf_font_fit::Cache font_cache;
    for (int i=0; i<n::count; ++i) {
        const int cx=n::center_x(i);
        icon(icons[i],cx);
        const std::string label=ui_locale::text(labels[i]);
        const int preferred=bookshelf_layout::ui_font_pixels(17,system_font::pixel_size());
        const int px=font_cache.fit(label,preferred,n::label_width,system_font::generation(),
            [](const std::string& value,int pixels){return system_font::measure(value,pixels);});
        const int cy=std::max(1180,1169+(system_font::ink_height(label,px)+1)/2);
        system_font::draw_on_background(M5.Display,label,cx-system_font::measure(label,px)/2,
            system_font::centered_y(label,cy,px),px,TFT_BLACK,TFT_WHITE);
    }
}
}
