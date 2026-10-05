#pragma once

#include <atomic>

namespace weread {
// Session-only policy shared with reader/prefetch workers. Never changes Wi-Fi
// policy for other applications or deletes authentication/cache data.
inline std::atomic<bool> offline_session{false};
inline bool offline_mode() { return offline_session.load(); }
inline void set_offline_mode(bool enabled) { offline_session.store(enabled); }
}  // namespace weread
