#pragma once
#include "weread/WereadStore.h"
#include <memory>
#include <mutex>

namespace weread {
// Serialize progress transactions (not reader rendering). UI reconciliation
// uses try_lock, so it cannot steal a live checkpoint before its report POST.
std::mutex &reading_transaction_mutex();

class ReadingSession {
public:
    struct State;
    ~ReadingSession();
    bool start(const store::PendingProgress &initial);
    // Mailbox contains owned copies only; no SD/HTTP operations on the caller.
    bool publish(const store::PendingProgress &position, uint64_t active_us,
                 uint64_t live_us, bool finish = false);
    void allow_io(bool allowed);
    int64_t ready_since_us() const;
private:
    std::shared_ptr<State> _state;
};
} // namespace weread
