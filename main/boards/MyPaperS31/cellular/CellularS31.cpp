#include "modem/cellular_network.h"
#include "uart_eth_modem.h"
#include "boards/MyPaperS31/Pins.h"
#include "wifi/wifi_setting.h"
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <atomic>
#include <memory>
#include <mutex>
#if CONFIG_ESP_CONSOLE_UART
#error "NT26 uses GPIO58/59: select CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG, disable UART console"
#endif

namespace {
constexpr const char *TAG = "CELLULAR_S31";
constexpr const char *NVS_NAME = "cellular_s31";
std::mutex command_mutex;
TaskHandle_t worker = nullptr;
std::atomic<bool> wanted{false}, suspended{false}, stopped{true};
std::atomic<bool> hardware_missing{false};
std::atomic<bool> boot_modem_present{false};
std::atomic<uint32_t> request{0}, revision{0};
std::atomic<cellular_state_t> state{CELLULAR_OFF};
std::atomic<int> confirmed_slot{-1}, preferred_slot{-1}, csq{99};
std::atomic<esp_err_t> last_error{ESP_OK};

void publish(cellular_state_t next, esp_err_t error = ESP_OK) {
    const auto old = state.exchange(next);
    const auto old_error = last_error.exchange(error);
    if (old != next || old_error != error) {
        ++revision;
        ESP_LOGI(TAG, "state=%d error=%s", int(next), esp_err_to_name(error));
    }
}
void wake() { if (worker) xTaskNotifyGive(worker); }
esp_err_t save_slot(int slot) {
    nvs_handle_t handle;
    auto result = nvs_open(NVS_NAME, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(handle, "sim_slot", uint8_t(slot));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}
esp_err_t ensure_rails() {
    using namespace mypaper::board::s31;
    // P12 is shared with touch; P14 supplies NT26 + BH1098G + PA. Never pulse
    // either rail for a network switch, or power down somebody else's audio.
    uint8_t output = 0, config = 0;
    if (!m5::In_I2C.readRegister(io_address, 3, &output, 1, 100000) ||
        !m5::In_I2C.readRegister(io_address, 7, &config, 1, 100000)) return ESP_FAIL;
    constexpr uint8_t mask = (1u << 2) | (1u << 4);
    if ((output & mask) != mask &&
        !m5::In_I2C.bitOn(io_address, 3, mask, 100000)) return ESP_FAIL;
    if ((config & mask) &&
        !m5::In_I2C.bitOff(io_address, 7, mask, 100000)) return ESP_FAIL;
    // Match the official modem boot settling period. Only a newly raised rail
    // needs this wait; switching between already powered interfaces does not.
    if ((output & mask) != mask) vTaskDelay(pdMS_TO_TICKS(1000));
    return ESP_OK;
}
UartEthModem::Config modem_config() {
    UartEthModem::Config config;
    using namespace mypaper::board::s31;
    config.uart_num = UART_NUM_2;
    config.baud_rate = 2000000;
    config.tx_pin = static_cast<gpio_num_t>(modem_tx);
    config.rx_pin = static_cast<gpio_num_t>(modem_rx);
    config.mrdy_pin = static_cast<gpio_num_t>(modem_mrdy);
    config.srdy_pin = static_cast<gpio_num_t>(modem_srdy);
    config.sim_slot = preferred_slot.load();
    return config;
}
void run(void *) {
    std::unique_ptr<UartEthModem> modem;
    uint32_t active_request = 0;
    int persisted_slot = preferred_slot.load();
    int64_t last_signal = 0;
    for (;;) {
        const uint32_t current = request.load();
        if (modem && (!wanted.load() || suspended.load() || active_request != current)) {
            publish(CELLULAR_STOPPING);
            modem->Stop(); // Joins tasks before releasing GPIO, DMA and netif.
            modem.reset();
            csq.store(99);
            publish(hardware_missing.load() ? CELLULAR_NOT_PRESENT : CELLULAR_OFF,
                hardware_missing.load() ? ESP_ERR_NOT_FOUND : ESP_OK);
            stopped.store(true);
        }
        if (!modem && wanted.load() && !suspended.load()) {
            active_request = request.load();
            stopped.store(false);
            publish(CELLULAR_STARTING);
            auto result = wifi_setting_network_stack_init();
            if (result == ESP_OK) result = ensure_rails();
            if (active_request != request.load() || !wanted.load() || suspended.load()) {
                stopped.store(true);
                continue;
            }
            if (result == ESP_OK) {
                // GPIO service is shared with touch and key drivers.
                result = gpio_install_isr_service(0);
                if (result == ESP_ERR_INVALID_STATE) result = ESP_OK;
            }
            if (result == ESP_OK) {
                const auto config = modem_config();
                modem.reset(new (std::nothrow) UartEthModem(config));
                if (!modem) result = ESP_ERR_NO_MEM;
                else {
                    modem->SetNetworkEventCallback([epoch = active_request](auto event, const std::string &) {
                        if (epoch != request.load() || !wanted.load() || suspended.load()) return;
                        using Event = UartEthModem::UartEthModemEvent;
                        switch (event) {
                            case Event::Connected: publish(CELLULAR_ONLINE); break;
                            case Event::Connecting: publish(CELLULAR_SEARCHING); break;
                            case Event::Disconnected: publish(CELLULAR_SEARCHING); break;
                            case Event::ErrorNoSim: publish(CELLULAR_NO_SIM, ESP_ERR_NOT_FOUND); break;
                            case Event::ErrorRegistrationDenied: publish(CELLULAR_REGISTRATION_DENIED, ESP_ERR_INVALID_STATE); break;
                            case Event::ErrorInitFailed: publish(CELLULAR_ERROR, ESP_FAIL); break;
                            case Event::ErrorNoModem:
                                hardware_missing.store(true);
                                publish(CELLULAR_NOT_PRESENT, ESP_ERR_NOT_FOUND);
                                wanted.store(false);
                                break;
                        }
                    });
                    result = modem->Start();
                }
            }
            if (result != ESP_OK) {
                if (modem) { modem->Stop(); modem.reset(); }
                if (active_request == request.load()) {
                    wanted.store(false);
                    stopped.store(true);
                    publish(CELLULAR_ERROR, result);
                }
            } else {
                last_signal = 0;
                ESP_LOGI(TAG, "NT26 UART2 2000000, TX=58 RX=59 MRDY=60 SRDY=3; internal=%u PSRAM=%u",
                    unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                    unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            }
        }
        if (modem && active_request == request.load()) {
            // Slot confirmation is atomic and arrives before registration. Do
            // not keep showing the old card while a new SIM searches for service.
            const int slot = modem->GetSimSlot();
            if (slot >= 0 && confirmed_slot.exchange(slot) != slot) ++revision;
            // Persist only a modem-confirmed choice, on this internal stack.
            if (slot >= 0 && preferred_slot.load() == slot && persisted_slot != slot) {
                const auto result = save_slot(slot);
                if (result == ESP_OK) persisted_slot = slot;
                else { publish(CELLULAR_ERROR, result); persisted_slot = slot; }
            }
            if (!modem->IsInitializing()) {
                if (modem->HasDataLink()) {
                    if (state.load() == CELLULAR_SEARCHING) publish(CELLULAR_ONLINE);
                } else if (state.load() == CELLULAR_ONLINE) publish(CELLULAR_SEARCHING);
            }
            const auto now = esp_timer_get_time();
            if (!modem->IsInitializing() && modem->IsInitialized() && now - last_signal >= 5000000) {
                const int signal = modem->GetSignalStrength();
                if (csq.exchange(signal) != signal) ++revision;
                last_signal = now;
            }
        }
        if (!modem && !wanted.load()) {
            if (state.load() != CELLULAR_ERROR && state.load() != CELLULAR_NOT_PRESENT) publish(CELLULAR_OFF);
            stopped.store(true);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    }
}
void shutdown() { suspended.store(true); (void)cellular_network_stop(15000); }
esp_err_t ensure_worker() {
    if (worker) return ESP_OK;
    const auto result = esp_register_shutdown_handler(shutdown);
    if (result != ESP_OK) return result;
    if (xTaskCreate(run, "cellular_s31", 6144, nullptr, 4, &worker) != pdPASS) {
        esp_unregister_shutdown_handler(shutdown);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
}

extern "C" esp_err_t cellular_network_probe_at_boot() {
    std::lock_guard<std::mutex> lock(command_mutex);
    if (worker || !stopped.load()) return ESP_ERR_INVALID_STATE;
    const int64_t begin = esp_timer_get_time();
    auto result = ensure_rails();
    if (result == ESP_OK) {
        // The shared supply may already be high although the modem is still
        // cold-booting. Allow it to settle without pulsing touch/audio reset.
        vTaskDelay(pdMS_TO_TICKS(1000));
        result = gpio_install_isr_service(0);
        if (result == ESP_ERR_INVALID_STATE) result = ESP_OK;
    }
    if (result == ESP_OK) {
        std::unique_ptr<UartEthModem> modem(new (std::nothrow) UartEthModem(modem_config()));
        result = modem ? modem->Probe() : ESP_ERR_NO_MEM;
        // Probe has joined its tasks and released UART/UHCI before destruction.
    }
    boot_modem_present.store(result == ESP_OK);
    hardware_missing.store(result != ESP_OK);
    publish(result == ESP_OK ? CELLULAR_OFF : CELLULAR_NOT_PRESENT, result);
    ESP_LOGI(TAG, "boot probe: result=%s present=%u time=%lld ms (AT only; no SIM or network setup)",
        esp_err_to_name(result), unsigned(result == ESP_OK), (esp_timer_get_time()-begin)/1000);
    return result;
}
extern "C" bool cellular_network_available() { return boot_modem_present.load(); }

extern "C" esp_err_t cellular_network_load_config() {
    nvs_handle_t handle;
    const auto result = nvs_open(NVS_NAME, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    uint8_t slot = 255;
    const auto read = nvs_get_u8(handle, "sim_slot", &slot);
    nvs_close(handle);
    if (read != ESP_OK && read != ESP_ERR_NVS_NOT_FOUND) return read;
    preferred_slot.store(slot <= 1 ? int(slot) : -1);
    return ESP_OK;
}
extern "C" esp_err_t cellular_network_start() {
    std::lock_guard<std::mutex> lock(command_mutex);
    if (!boot_modem_present.load()) return ESP_ERR_NOT_FOUND;
    if (suspended.load() || wifi_setting_get_network_mode() != WIFI_SETTING_NETWORK_CELLULAR)
        return ESP_ERR_INVALID_STATE;
    if (hardware_missing.load()) return ESP_ERR_NOT_FOUND;
    const auto result = ensure_worker();
    if (result != ESP_OK) return result;
    if (!wanted.load() || state.load() == CELLULAR_ERROR || state.load() == CELLULAR_NO_SIM ||
        state.load() == CELLULAR_REGISTRATION_DENIED) {
        ++request;
        stopped.store(false);
        wanted.store(true);
        publish(CELLULAR_STARTING);
    }
    wake();
    return ESP_OK;
}
extern "C" esp_err_t cellular_network_stop(int timeout_ms) {
    // Worker never takes command_mutex. Keep starts/SIM changes out until the
    // stopped acknowledgement arrives, including concurrent application jobs.
    std::lock_guard<std::mutex> lock(command_mutex);
    wanted.store(false);
    ++request; // Invalidates late IP/registration callbacks immediately.
    wake();
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    while (!stopped.load() && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(10));
    return stopped.load() ? ESP_OK : ESP_ERR_TIMEOUT;
}
extern "C" esp_err_t cellular_network_retry() {
    if (!boot_modem_present.load()) return ESP_ERR_NOT_FOUND;
    hardware_missing.store(false); // Explicit user action, not an app's reconnect loop.
    return cellular_network_start();
}
extern "C" bool cellular_network_is_online() {
    return wanted.load() && !suspended.load() && state.load() == CELLULAR_ONLINE;
}
extern "C" esp_err_t cellular_network_wait_online(int timeout_ms) {
    const auto result = cellular_network_start();
    if (result != ESP_OK) return result;
    const auto epoch = request.load();
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms > 0 ? timeout_ms : 60000) * 1000;
    while (esp_timer_get_time() < deadline) {
        if (epoch != request.load() || suspended.load()) return ESP_ERR_INVALID_STATE;
        if (cellular_network_is_online()) return ESP_OK;
        const auto current = state.load();
        if (current == CELLULAR_NO_SIM || current == CELLULAR_REGISTRATION_DENIED || current == CELLULAR_ERROR ||
            current == CELLULAR_NOT_PRESENT)
            return last_error.load();
        if (!wanted.load()) return ESP_ERR_INVALID_STATE;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_ERR_TIMEOUT;
}
extern "C" void cellular_network_get_status(cellular_status_t *out) {
    if (out) *out = {state.load(), confirmed_slot.load(), preferred_slot.load(),
                    csq.load(), last_error.load(), revision.load()};
}
extern "C" esp_err_t cellular_network_set_sim_slot(int slot) {
    if (slot != 0 && slot != 1) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(command_mutex);
    if (!boot_modem_present.load()) return ESP_ERR_NOT_FOUND;
    if (suspended.load() || wifi_setting_get_network_mode() != WIFI_SETTING_NETWORK_CELLULAR)
        return ESP_ERR_INVALID_STATE;
    hardware_missing.store(false);
    const auto result = ensure_worker();
    if (result != ESP_OK) return result;
    preferred_slot.store(slot);
    ++request;
    stopped.store(false);
    wanted.store(true);
    publish(CELLULAR_STARTING);
    wake();
    return ESP_OK;
}
extern "C" esp_err_t cellular_network_suspend() {
    suspended.store(true);
    return cellular_network_stop(15000);
}
extern "C" void cellular_network_resume() {
    suspended.store(false);
    if (wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR) (void)cellular_network_start();
}
