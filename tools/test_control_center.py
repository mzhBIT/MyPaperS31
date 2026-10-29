"""Native regression: actual modal manager lifecycle and global edge gesture.

Generate a C++ test/stub include directory; compile with main on the include path.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
(out / 'M5GFX.h').write_text('#pragma once\nnamespace lgfx { inline unsigned millis() { return 1000; } }\n')
(out / 'esp_log.h').write_text('#pragma once\n#define ESP_LOGI(...) ((void)0)\n')
source = (root / 'main/ui/ControlCenter.cpp').read_text(encoding='utf-8')
start = source.index('bool control_center_handle_touch(')
end = source.index('\nvoid control_center_service()', start)
program = r'''
#include "activities/ActivityManager.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
bool pending = false;
unsigned pending_generation = 0, cancelled = 0;
void touch_input_cancel_until_release() { ++cancelled; }
struct Counts { int create=0,show=0,hide=0,destroy=0,restore=0,discard=0; };
struct Page : Activity {
    Counts& c;
    Page(Counts& c) : c(c) {}
    ~Page() override { ++c.destroy; }
    void on_create() override { ++c.create; }
    void on_show() override { ++c.show; }
    void on_hide() override { ++c.hide; }
    void on_overlay_dismiss(bool restore) override { restore ? ++c.restore : ++c.discard; }
};
'''
program += source[start:end]
shutdown_start = source.index('        if (_shutdown) {')
shutdown_end = source.index('\n#endif', shutdown_start)
program += r'''
namespace mypaper::board::s31::standby {
unsigned calls=0, requested_generation=0;
void request_shutdown() {
    auto& manager=ActivityManager::instance();
    assert(!manager.has_overlay() && manager.can_standby());
    requested_generation=manager.generation();
    ++calls;
}
}
struct ShutdownModal : Page {
    bool _shutdown=true;
    using Page::Page;
    void loop() override {
'''
program += source[shutdown_start:shutdown_end]
program += '\n    }\n};\n'
program += r'''
int main() {
    auto& am = ActivityManager::instance();
    Counts root, modal, replacement;
    auto* page = new Page(root);
    am.push(page);
    const auto generation = am.generation();
    assert(am.show_overlay(new Page(modal)));
    assert(root.hide == 0 && root.show == 1 && am.depth() == 1);
    assert(am.has_overlay() && am.can_pop() && !am.can_standby());
    assert(am.generation() > generation);
    TouchEvent e; e.type=TouchEventType::SwipeDown; e.start_y=10; e.dy=100;
    assert(!control_center_handle_touch(e)); // Never nest overlays.
    am.pop();
    assert(am.current()==page && !am.has_overlay() && !am.can_pop());
    assert(modal.restore==1 && modal.destroy==1 && root.show==1 && root.hide==0);
    e.type=TouchEventType::Tap; assert(!control_center_handle_touch(e));
    e.type=TouchEventType::SwipeUp; assert(!control_center_handle_touch(e));
    e.type=TouchEventType::SwipeDown; e.start_y=49; assert(!control_center_handle_touch(e));
    e.start_y=20; e.dy=30; assert(!control_center_handle_touch(e));
    e.dy=100; e.dx=150; assert(!control_center_handle_touch(e));
    e.dx=0; assert(control_center_handle_touch(e));
    assert(pending && cancelled==1 && pending_generation==am.generation());
    assert(control_center_handle_touch(e) && cancelled==1);
    pending=false;
    assert(am.show_overlay(new Page(modal)));
    am.push(new Page(replacement)); // Incoming call/application supersedes modal.
    assert(modal.discard==1 && modal.destroy==2 && root.hide==1 && replacement.create==1);
    am.pop();
    assert(am.current()==page && root.show==2);
    assert(am.show_overlay(new Page(modal)));
    am.pop_to_root(); // Root-only overlay still restores the screen.
    assert(modal.restore==2 && am.current()==page && root.show==2);
    Counts tabs;
    for(int i=0;i<50;++i) {
        am.show_above_root(new Page(tabs));
        assert(am.depth()==2 && am.current()!=page);
        assert(tabs.destroy==i && root.show==2);
    }
    am.pop_to_root();
    assert(tabs.destroy==50 && root.show==3 && am.depth()==1);
    Counts shutdown;
    assert(am.show_overlay(new ShutdownModal(shutdown)));
    const auto before_shutdown=am.generation();
    am.current()->loop(); // Request must survive overlay destruction/generation change.
    assert(!am.has_overlay() && am.current()==page);
    assert(shutdown.restore==1 && shutdown.destroy==1 && root.show==3);
    assert(mypaper::board::s31::standby::calls==1);
    assert(mypaper::board::s31::standby::requested_generation==am.generation());
    assert(am.generation()>before_shutdown);
    am.pop();
    assert(root.destroy==1);
    std::puts("Control center: modal lifecycle, restore/discard, root, edge gesture, shutdown order PASS");
}
'''
(out / 'test.cpp').write_text(program, encoding='utf-8')
