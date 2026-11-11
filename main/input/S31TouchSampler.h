#pragma once
#include <cstdint>

namespace s31_touch {
struct Sample {
    uint32_t at = 0, epoch = 0;
    int16_t x = 0, y = 0;
    bool down = false, valid = true;
};
bool init();
bool running();
bool pop(Sample* sample);
bool pending();
bool take_overflow();
void discard();
void wait(bool fast);
bool pause();
void resume();
}
