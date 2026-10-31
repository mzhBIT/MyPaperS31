#pragma once
#include <atomic>

// Reject new HTTP operations during shutdown and avoid status-bar refreshes
// during a transfer. The owner joins the server before releasing the activity.
class TransferSessionGate {
public:
    class Request {
    public:
        explicit Request(TransferSessionGate& gate) : gate_(gate) {
            gate_.active_.fetch_add(1);
            accepted_ = !gate_.switching_.load();
        }
        ~Request() { gate_.active_.fetch_sub(1); }
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;
        explicit operator bool() const { return accepted_; }
    private:
        TransferSessionGate& gate_;
        bool accepted_ = false;
    };
    bool try_begin_switch() {
        if (switching_.exchange(true)) return false;
        if (active_.load() == 0) return true;
        switching_.store(false);
        return false;
    }
    void stop_accepting() { switching_.store(true); }
    void open() { switching_.store(false); }
    bool busy() const { return active_.load() != 0; }
private:
    std::atomic<unsigned> active_{0};
    std::atomic<bool> switching_{false};
};
