#pragma once
#include <cstdint>

namespace mypaper::board::s31::keys {
void init();
// Main thread only: invalidate old-screen events and held-key repeats.
void sync_context(uint32_t generation);
// Feed exactly the SAME raw FT sample used by the screen gesture recognizer.
// Returns true for a bezel-key contact (including its final release sample).
bool cover_sample(bool pressed, int raw_x, int raw_y, uint32_t now);
bool cover_active();
// Main thread only. At most one event per call; never draws from the worker.
bool dispatch_one();
bool pause_for_sleep();
void resume_after_sleep();
}
