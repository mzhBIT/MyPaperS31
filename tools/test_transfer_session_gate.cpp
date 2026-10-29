#include "wifi/TransferSessionGate.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
    TransferSessionGate gate;
    {
        TransferSessionGate::Request upload(gate);
        assert(upload && gate.busy());
        assert(!gate.try_begin_switch());
    }
    assert(!gate.busy() && gate.try_begin_switch());
    assert(!gate.try_begin_switch()); // Does not reopen an existing switch.
    {
        TransferSessionGate::Request late_request(gate);
        assert(!late_request);
    }
    gate.open();
    {
        TransferSessionGate::Request retry(gate);
        assert(retry);
    }
    gate.stop_accepting();
    {
        TransferSessionGate::Request closing(gate);
        assert(!closing);
    }
    gate.open();

    // Accepted HTTP payload work must never overlap a successful UI switch.
    std::atomic<bool> switching{false}, done{false};
    std::atomic<unsigned> payloads{0};
    auto client = [&] {
        while (!done.load()) {
            TransferSessionGate::Request request(gate);
            if (!request) { std::this_thread::yield(); continue; }
            payloads.fetch_add(1);
            assert(!switching.load());
            std::this_thread::yield();
            assert(!switching.load());
            payloads.fetch_sub(1);
        }
    };
    std::thread one(client), two(client);
    for (unsigned i=0; i<10000; ++i) {
        if (!gate.try_begin_switch()) { std::this_thread::yield(); continue; }
        switching.store(true);
        assert(payloads.load() == 0);
        std::this_thread::yield();
        assert(payloads.load() == 0);
        switching.store(false);
        gate.open();
    }
    done.store(true);
    one.join(); two.join();
    assert(!gate.busy());
    std::puts("Transfer session gate: active requests, restart, shutdown, concurrent switch PASS");
}
