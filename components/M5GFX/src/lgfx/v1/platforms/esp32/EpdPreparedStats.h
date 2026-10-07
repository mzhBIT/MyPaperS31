#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lgfx_epd_stats {
struct PreparedKey {
    uint64_t generation;
    uint8_t rotation, type;
    const void* phases;
};
// Exact comparison, not a probabilistic hash or borrowed-buffer identity.
inline unsigned prepared_miss_reason(bool valid, bool physical_known,
                             const PreparedKey& saved, const PreparedKey& current,
                             bool full_frame, const uint8_t* saved_target,
                             const uint8_t* target, size_t bytes) {
    if (!valid) return 1;
    if (!physical_known) return 2;
    if (!full_frame) return 3;
    if (!saved_target || !target) return 4;
    if (saved.generation != current.generation) return 5;
    if (saved.rotation != current.rotation) return 6;
    if (saved.type != current.type) return 7;
    if (saved.phases != current.phases) return 8;
    return std::memcmp(saved_target, target, bytes) == 0 ? 0 : 9;
}
inline bool prepared_matches(bool valid, bool physical_known,
                             const PreparedKey& saved, const PreparedKey& current,
                             bool full_frame, const uint8_t* saved_target,
                             const uint8_t* target, size_t bytes) {
    return prepared_miss_reason(valid, physical_known, saved, current,
                                full_frame, saved_target, target, bytes) == 0;
}
}
