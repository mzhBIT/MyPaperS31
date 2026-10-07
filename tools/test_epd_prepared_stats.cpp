#include "lgfx/v1/platforms/esp32/EpdPreparedStats.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    using namespace lgfx_epd_stats;
    std::vector<uint8_t> saved(415872, 0x55), target = saved;
    const PreparedKey key{42, 0, 7, saved.data()};
    auto matches = [&](bool valid, bool known, PreparedKey current, bool full) {
        return prepared_matches(valid, known, key, current, full,
                                saved.data(), target.data(), saved.size());
    };
    assert(matches(true, true, key, true));
    // Panel offset=3: logical import rotation 0 is physical/internal 3.
    // Cache preparation and update must both store the internal value.
    const PreparedKey physical_key{42, 3, 7, saved.data()};
    assert(prepared_matches(true, true, physical_key, physical_key, true,
                            saved.data(), target.data(), saved.size()));
    assert(!prepared_matches(true, true, key, physical_key, true,
                             saved.data(), target.data(), saved.size()));
    assert(!matches(false, true, key, true));
    assert(!matches(true, false, key, true));
    assert(!matches(true, true, key, false));
    auto changed = key;
    ++changed.generation; assert(!matches(true, true, changed, true));
    changed = key; ++changed.rotation; assert(!matches(true, true, changed, true));
    changed = key; ++changed.type; assert(!matches(true, true, changed, true));
    changed = key; changed.phases = target.data(); assert(!matches(true, true, changed, true));
    // Content changes anywhere, including header/footer or one nibble, fail.
    for (size_t i = 0; i < target.size(); ++i) {
        target[i] ^= 1;
        assert(!matches(true, true, key, true));
        target[i] ^= 1;
    }
    assert(matches(true, true, key, true));
    assert(!prepared_matches(true, true, key, key, true, nullptr,
                             target.data(), target.size()));
    std::puts("Prepared statistics: exact full-page content and invalidation checks passed");
}
