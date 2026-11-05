"""Generate a native concurrency regression using real 2D transaction/snapshot code."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else '.codex-build-cache/epd-draw-snapshot')
out.mkdir(parents=True, exist_ok=True)
source = (root / 'components/M5GFX/src/lgfx/v1/platforms/esp32/Panel_EPD_2D.cpp').read_text(encoding='utf-8')
hooks = source[source.index('  void Panel_EPD_2D::beginTransaction('):source.index('  bool Panel_EPD_2D::displayBusy(')]
start = source.index('      const int64_t draw_wait_started =')
end = source.index('      const selected_waveform_t selected =', start)
snapshot = source[start:end]
lock_start = source.index('  struct StatsLock {')
lock = source[lock_start:source.index('  };', lock_start) + len('  };')]
panel = (root / 'components/M5GFX/src/lgfx/v1/Panel.hpp').read_text(encoding='utf-8')
transactions = '\n'.join(line for line in panel.splitlines()
                         if line.strip().startswith(('void startWrite(', 'void endWrite(')))
assert len(transactions.splitlines()) == 2
(out / 'test.cpp').write_text(r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#define ESP_LOGW(...) ((void)0)
struct Semaphore { std::mutex mutex; std::atomic<int> owner{0}; };
using SemaphoreHandle_t = Semaphore*;
constexpr unsigned portMAX_DELAY = ~0u;
constexpr int pdTRUE = 1;
const std::thread::id ui_thread = std::this_thread::get_id();
std::atomic<bool> worker_waiting{false};
int xTaskGetCurrentTaskHandle() { return std::this_thread::get_id()==ui_thread ? 1 : 2; }
int xSemaphoreGetMutexHolder(SemaphoreHandle_t lock) { return lock->owner.load(); }
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned) {
    if (std::this_thread::get_id() != ui_thread) worker_waiting.store(true);
    lock->mutex.lock(); lock->owner.store(xTaskGetCurrentTaskHandle()); return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t lock) { lock->owner.store(0); lock->mutex.unlock(); }
void vTaskDelay(unsigned) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void cache_writeback_2d(void*, size_t) {}
enum class epd_mode_t { epd_quality=1, epd_text=2, epd_fast=3, epd_hd=5 };
struct Update {
    uint16_t x=0, y=0, w=4, h=2;
    epd_mode_t mode=epd_mode_t::epd_fast;
    int animation_direction=0;
    bool changed_pixels_only=false;
};
int xQueueReceive(std::vector<Update>* queue, Update* update, int) {
    if (queue->empty()) return 0;
    *update=queue->front(); queue->erase(queue->begin()); return pdTRUE;
}
struct IPanel {
    unsigned _start_count=0;
    bool _auto_display=true;
    virtual void beginTransaction()=0;
    virtual void endTransaction()=0;
    virtual void display(int,int,int,int)=0;
''' + transactions + r'''
};
struct Panel_EPD_2D : IPanel {
    void beginTransaction() override;
    void endTransaction() override;
    void waitDisplay();
    bool displayBusy() { return busy.load(); }
    void display(int,int,int,int) override { ++submissions; }
    void snapshot();
    Semaphore mutex;
    SemaphoreHandle_t _draw_mutex_2d=&mutex;
    bool _draw_locked_2d=false, _physical_state_known=true;
    uint8_t draw[16]{}, target[16]{};
    uint8_t *_buf=draw, *_target_buf=target;
    std::vector<Update> queue;
    std::vector<Update>* _update_queue_2d=&queue;
    unsigned submissions=0, last_merged=0;
    Update captured;
    std::atomic<bool> busy{false};
};
''' + lock + '\n' + hooks + r'''
void Panel_EPD_2D::snapshot() {
    auto* me=this;
    const size_t panel_w=8, panel_h=4, stride=4, frame_bytes=16;
    Update update, pending;
    unsigned merged=0;
    int64_t snapshot_us=0;
''' + snapshot + r'''
    captured=update; last_merged=merged;
}
int main() {
    Panel_EPD_2D panel;
    panel.startWrite();
    panel.startWrite(); // Nested glyph/key drawing must not release protection.
    memset(panel.draw, 0xff, sizeof(panel.draw));
    std::promise<void> copied, finish_scan;
    auto copied_done=copied.get_future(), scan_release=finish_scan.get_future();
    std::atomic<bool> did_copy{false};
    std::thread worker([&] {
        panel.snapshot();
        did_copy.store(true); copied.set_value();
        scan_release.wait(); // The scan must not retain the draw-buffer lock.
    });
    while (!worker_waiting.load()) std::this_thread::yield();
    assert(!did_copy.load());
    memset(panel.draw, 0x3c, sizeof(panel.draw)/2);
    panel.endWrite();
    assert(!did_copy.load() && panel.submissions==0);
    memset(panel.draw+sizeof(panel.draw)/2, 0x3c, sizeof(panel.draw)/2);
    // A completed pending update queued during the wait must be coalesced.
    panel.queue.push_back({4,2,4,2,epd_mode_t::epd_text});
    panel.endWrite();
    assert(copied_done.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    assert(panel.submissions==1 && panel.last_merged==1);
    for (auto value:panel.target) assert(value==0x3c);
    assert(panel.captured.x==0 && panel.captured.y==0 && panel.captured.w==8 && panel.captured.h==4);
    assert(panel.captured.mode==epd_mode_t::epd_text); // No forced Quality.
    panel.startWrite();
    memset(panel.draw,0x7e,sizeof(panel.draw));
    panel.endWrite();
    for (auto value:panel.target) assert(value==0x3c); // Active scan stays immutable.
    finish_scan.set_value(); worker.join();
    // A synchronous flush within an outer transaction must neither deadlock
    // nor lose protection when the caller resumes drawing afterward.
    panel.startWrite();
    panel.busy.store(true);
    std::atomic<bool> sync_copied{false};
    std::thread synchronous([&] {
        panel.snapshot(); sync_copied.store(true); panel.busy.store(false);
    });
    std::promise<void> stop_guard;
    auto guard_done=stop_guard.get_future();
    std::thread guard([&] {
        if (guard_done.wait_for(std::chrono::seconds(2))!=std::future_status::ready)
            panel.busy.store(false);
    });
    panel.waitDisplay();
    assert(sync_copied.load() && panel._draw_locked_2d);
    assert(xSemaphoreGetMutexHolder(panel._draw_mutex_2d)==xTaskGetCurrentTaskHandle());
    panel.endWrite();
    stop_guard.set_value(); guard.join(); synchronous.join();
    panel._physical_state_known=false;
    panel.snapshot();
    assert(panel.captured.mode==epd_mode_t::epd_quality); // Existing recovery preserved.
    assert(panel.captured.w==8 && panel.captured.h==4);
    std::puts("EPD snapshot: no intermediate clear/redraw, pending merge, UI during scan, nested synchronous flush and Quality recovery PASS");
}
''', encoding='utf-8')
print(out / 'test.cpp')
