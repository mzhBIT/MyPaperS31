#include "ui/ListFocus.h"
#include <cassert>
#include <cstdio>

struct FakeGraphics {
    int calls = 0;
    list_focus::Rect last{};
    uint16_t color = 0xffff;
    void fillRect(int x, int y, int w, int h, uint16_t c) {
        ++calls; last = {x, y, w, h}; color = c;
    }
};
int main() {
    FakeGraphics gfx;
    for (int h = 1; h <= 300; ++h) {
        const auto r = list_focus::geometry(24, 76, h);
        assert(r.x == 26 && r.w == 5 && r.h > 0);
        assert(r.y >= 76 && r.y + r.h <= 76 + h);
        assert(r.y - 76 == 76 + h - (r.y + r.h));
    }
    list_focus::draw(gfx, 24, 76, 110, false);
    list_focus::draw(gfx, 24, 76, 0, true);
    assert(gfx.calls == 0);
    list_focus::draw(gfx, 24, 76, 110, true);
    assert(gfx.calls == 1 && gfx.last.y == 92 && gfx.last.h == 78 && gfx.color == 0);
    assert(24 + list_focus::text_inset() > gfx.last.x + gfx.last.w);
    std::puts("List focus: 300 row heights and draw/text spacing checks passed");
}
