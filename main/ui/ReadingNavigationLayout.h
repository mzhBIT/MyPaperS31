#pragma once
// Portrait coordinates adapted from read-pico-reader.
// Copyright 2026 mindreset. SPDX-License-Identifier: Apache-2.0

// Shared portrait navigation strip for reading home, bookshelf and files.
namespace reading_navigation_layout {
constexpr int width = 684, screen_height = 1216;
constexpr int top = 1096, height = screen_height - top;
constexpr int count = 4, icon_y = 1141, icon_size = 44;
constexpr int indicator_y = top + 1, indicator_width = 56, indicator_height = 5;
constexpr int label_width = 159;
constexpr int center_x(int tab) { return (2 * tab + 1) * width / (2 * count); }
constexpr int tab_at(int x, int y) {
    return x >= 0 && x < width && y >= top && y < screen_height
        ? x * count / width : -1;
}
}
