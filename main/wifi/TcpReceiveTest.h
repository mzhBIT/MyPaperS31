#pragma once

#include <sdkconfig.h>

#if CONFIG_IDF_TARGET_ESP32S31
#include <atomic>
#include <cerrno>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>

// Temporary raw TCP sink. No HTTP parsing, file writes, or application replies.
// Only the worker owns/closes sockets; stop joins it before Wi-Fi teardown.
class TcpReceiveTest {
public:
    ~TcpReceiveTest() { stop(); }
    void start() {
        if (done_) return;
        buffer_ = static_cast<char*>(heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        done_ = xSemaphoreCreateBinary();
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(5001);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (!buffer_ || !done_ || listener_ < 0 ||
            bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 1) != 0) {
            ESP_LOGW("TCP_RX_TEST", "start failed errno=%d", errno);
            cleanup();
            return;
        }
        stopping_.store(false);
        if (xTaskCreate(run, "tcp_rx_test", 4096, this, 5, nullptr) != pdPASS) {
            ESP_LOGW("TCP_RX_TEST", "task allocation failed");
            cleanup();
            return;
        }
        ESP_LOGI("TCP_RX_TEST", "listening port=5001 buffer=32768 priority=5 affinity=any; use tools/wifi_tcp_test.py");
    }
    void stop() {
        if (!done_) return;
        stopping_.store(true);
        xSemaphoreTake(done_, portMAX_DELAY);
        cleanup();
    }
private:
    std::atomic<bool> stopping_{false};
    SemaphoreHandle_t done_ = nullptr;
    char* buffer_ = nullptr;
    int listener_ = -1;
    void cleanup() {
        if (listener_ >= 0) { close(listener_); listener_ = -1; }
        if (done_) { vSemaphoreDelete(done_); done_ = nullptr; }
        heap_caps_free(buffer_);
        buffer_ = nullptr;
    }
    static void run(void* context) {
        auto* self = static_cast<TcpReceiveTest*>(context);
        self->receive();
        // No further access to self after signalling the owner.
        SemaphoreHandle_t done = self->done_;
        xSemaphoreGive(done);
        vTaskDelete(nullptr);
    }
    static void report(const char* stage, uint64_t bytes, int64_t elapsed, uint64_t delta, int64_t interval) {
        ESP_LOGI("TCP_RX_TEST", "%s bytes=%llu elapsed=%lld ms avg=%.1f KiB/s inst=%.1f KiB/s",
                 stage, static_cast<unsigned long long>(bytes), static_cast<long long>(elapsed / 1000),
                 elapsed > 0 ? double(bytes) * 1000000 / elapsed / 1024 : 0,
                 interval > 0 ? double(delta) * 1000000 / interval / 1024 : 0);
    }
    void receive() {
        int client = -1;
        uint64_t bytes = 0, previous_bytes = 0;
        int64_t begin = 0, previous = 0, yielded = esp_timer_get_time();
        while (!stopping_.load()) {
            const int fd = client >= 0 ? client : listener_;
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(fd, &read_set);
            timeval wait{0, 200000};
            const int ready = select(fd + 1, &read_set, nullptr, nullptr, &wait);
            if (ready < 0) {
                if (errno == EINTR) continue;
                ESP_LOGW("TCP_RX_TEST", "select failed errno=%d", errno);
                break;
            }
            if (ready == 0) continue;
            if (client < 0) {
                client = accept(listener_, nullptr, nullptr);
                if (client < 0) continue;
                bytes = previous_bytes = 0;
                begin = previous = esp_timer_get_time();
                ESP_LOGI("TCP_RX_TEST", "connected internal=%u largest=%u",
                         unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
                continue;
            }
            const int got = recv(client, buffer_, 32768, 0);
            const int64_t now = esp_timer_get_time();
            if (got <= 0) {
                report(got == 0 ? "complete" : "receive-error", bytes, now - begin, bytes - previous_bytes, now - previous);
                if (got < 0) ESP_LOGW("TCP_RX_TEST", "recv errno=%d", errno);
                close(client);
                client = -1;
                continue;
            }
            bytes += got;
            if (now - previous >= 2000000) {
                report("progress", bytes, now - begin, bytes - previous_bytes, now - previous);
                previous = now;
                previous_bytes = bytes;
            }
            // Bound CPU starvation on fast links; no delay per packet/read.
            if (now - yielded >= 250000) {
                vTaskDelay(1);
                yielded = esp_timer_get_time();
            }
        }
        if (client >= 0) {
            const int64_t now = esp_timer_get_time();
            report("stopped", bytes, now - begin, bytes - previous_bytes, now - previous);
            close(client);
        }
    }
};
#else
class TcpReceiveTest {
public:
    void start() {}
    void stop() {}
};
#endif
