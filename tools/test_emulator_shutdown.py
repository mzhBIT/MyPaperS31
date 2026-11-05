"""Exercise the production shutdown method with completed and stalled audio workers."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else '.codex-build-cache/emulator-shutdown')
out.mkdir(parents=True, exist_ok=True)
source = (root / 'firmware/emulator/GameAudioOutput.cpp').read_text(encoding='utf-8')
start = source.index('bool GameAudioOutput::shutdown() {')
end = source.index('\n} // namespace emulator', start)
method = source[start:end]
cleanup = source[source.index('void GameAudioOutput::cleanup() {'):source.index('bool GameAudioOutput::pause() {')]
(out / 'shutdown.cpp').write_text(r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
using SemaphoreHandle_t = void*;
constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGE(...) ((void)0)
static bool completed;
static unsigned waited, freed, deleted, channels_disabled, channels_deleted, uart_deleted;
int xSemaphoreTake(void*, unsigned ticks) { waited = ticks; return completed; }
void vSemaphoreDelete(void*) { ++deleted; }
void tracked_free(void*) { ++freed; }
void vTaskDelay(unsigned) {}
void i2s_channel_disable(void*) { ++channels_disabled; }
void i2s_del_channel(void*) { ++channels_deleted; }
constexpr int UART_NUM_1 = 1;
void uart_driver_delete(int) { ++uart_deleted; }
struct Hardware {
    unsigned muted = 0;
    bool amplifier(bool on) { assert(!on); ++muted; return true; }
};
struct GameAudioOutput {
    bool shutdown();
    void cleanup();
    Hardware* hardware_ = nullptr;
    void *task_ = nullptr, *ring_ = nullptr, *slots_ = nullptr, *zeros_ = nullptr;
    SemaphoreHandle_t queue_lock_ = nullptr, idle_ = nullptr, exited_ = nullptr;
    void *tx_ = nullptr, *rx_ = nullptr;
    bool pa_on_ = false, tx_enabled_ = false, rx_enabled_ = false, uart_ = false;
    std::atomic<bool> playing_{true}, stopping_{false}, ready_{true}, failed_{false};
};
#define free tracked_free
''' + cleanup + method + r'''
#undef free
int main() {
    Hardware hw;
    GameAudioOutput a;
    a.hardware_ = &hw;
    a.task_ = a.ring_ = a.slots_ = a.zeros_ = reinterpret_cast<void*>(1);
    a.queue_lock_ = a.idle_ = a.exited_ = reinterpret_cast<void*>(2);
    // A blocked SDK teardown must not block forever or free live worker memory.
    completed = false;
    assert(!a.shutdown());
    assert(waited == 2000 && a.stopping_ && !a.playing_ && a.failed_);
    assert(hw.muted == 1 && freed == 0 && deleted == 0 && channels_deleted == 0);
    assert(a.task_ && a.ring_ && a.exited_);
    // Completion after the timeout can still be joined and cleaned up safely.
    completed = true;
    assert(a.shutdown());
    assert(freed == 3 && deleted == 3 && !a.task_ && !a.ring_ && !a.exited_);
    assert(!a.ready_ && hw.muted == 1);
    // Repeated shutdown does not wait for an already consumed completion signal.
    waited = 0;
    assert(a.shutdown());
    assert(waited == 0 && deleted == 3 && hw.muted == 2);
    // Partial initialization has no worker and needs no completion semaphore.
    GameAudioOutput partial;
    assert(partial.shutdown() && channels_deleted == 0);
    // Driver teardown mutes PA and releases I2S/UART. Hardware deliberately has
    // no audio_power(false) API: that would cut the shared TPS/VOUT rail.
    GameAudioOutput drivers;
    drivers.hardware_ = &hw;
    drivers.tx_ = drivers.rx_ = reinterpret_cast<void*>(3);
    drivers.tx_enabled_ = drivers.rx_enabled_ = drivers.uart_ = true;
    assert(drivers.shutdown());
    assert(hw.muted == 3 && channels_disabled == 2 && channels_deleted == 2 && uart_deleted == 1);
    assert(!drivers.tx_ && !drivers.rx_ && !drivers.uart_);
    std::puts("Emulator audio shutdown: timeout/late completion/repeated/partial, driver cleanup without cutting shared VOUT PASS");
}
''', encoding='utf-8')
print(out / 'shutdown.cpp')
