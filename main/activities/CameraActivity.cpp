#include "activities/CameraActivity.h"
#include "activities/ActivityManager.h"
#include "activities/StatusBarActivity.h"
#include "boards/MyPaperS31/camera/CameraS31.h"
#include "lgfx/v1/platforms/esp32/EpdPeripheralGuard.hpp"
#include "lgfx/v1/platforms/esp32/Panel_EPD_2D.hpp"
#include "ui/ListFocus.h"
#include "ui/UiLocale.h"
#include <freertos/task.h>
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <atomic>
#include <new>

namespace {
namespace camera = mypaper::board::s31::camera;
using ui_locale::TextId;
constexpr uint32_t preview_interval_ms = 1500;
struct Frame {
    uint8_t *packed = nullptr;
    esp_err_t error = ESP_OK;
    esp_err_t save_error = ESP_OK;
    bool photo = false;
    char saved_path[camera::photo_path_capacity]{};
    ~Frame() { heap_caps_free(packed); }
};
struct WorkerState {
    std::atomic<bool> stop{false}, requested{false}, busy{false}, running{true}, save_requested{false};
    std::atomic<esp_err_t> error{ESP_OK};
    std::atomic<Frame*> result{nullptr};
    TaskHandle_t task = nullptr;
    ~WorkerState() { delete result.exchange(nullptr); }
};
bool cancelled(void *arg) { return static_cast<WorkerState *>(arg)->stop.load(); }
void worker(void *arg) {
    auto *state = static_cast<WorkerState *>(arg);
    {
        camera::Session session(cancelled, state);
        for (;;) {
            if (state->stop.load()) break;
            if (!state->requested.exchange(false)) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
            auto *frame = new (std::nothrow) Frame;
            if (!frame) { state->error.store(ESP_ERR_NO_MEM); state->busy.store(false); break; }
            frame->photo = state->save_requested.exchange(false);
            // The scan task takes this same gate before any waveform work.
            // Time-limited acquisition keeps Back/power cancellation responsive.
            for (;;) {
                if (state->stop.load()) { frame->error = ESP_ERR_INVALID_STATE; break; }
                lgfx::EpdPeripheralGuard hardware(50);
                if (!hardware) continue;
                frame->error = session.open();
                if (frame->error == ESP_OK) {
                    frame->packed = static_cast<uint8_t *>(heap_caps_malloc(camera::packed_bytes,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    frame->error = frame->packed ? session.capture(frame->packed, camera::packed_bytes) : ESP_ERR_NO_MEM;
                }
                if (frame->error != ESP_OK) session.close();
                break;
            }
            // The owning worker keeps the captured RGB buffer stable. JPEG
            // encoding and SD writes need no LCD_CAM gate and do not block UI.
            if (frame->error == ESP_OK && frame->photo && !state->stop.load())
                frame->save_error = session.save_photo(frame->saved_path, sizeof(frame->saved_path));
            const auto error = frame->error;
            state->error.store(error);
            if (state->stop.load()) delete frame;
            else delete state->result.exchange(frame); // One completed result, never a frame queue.
            state->busy.store(false);
            if (error != ESP_OK) break;
        }
        // The sensor/clock cleanup is also serialized with panel refreshes.
        lgfx::EpdPeripheralGuard hardware;
        session.close();
    }
    state->running.store(false);
    // The UI owns task deletion after this cleanup acknowledgement. Keeping
    // the handle valid avoids notifying an already deleted task on Back.
    vTaskSuspend(nullptr);
}

class CameraActivity final : public StatusBarActivity {
public:
    ~CameraActivity() override { stop_worker(); delete _frame; }
    void on_create() override { draw(true); start_worker(); }
    void on_show() override { if (!_worker) start_worker(); }
    void on_hide() override { stop_worker(); }
    void on_overlay_dismiss(bool restore) override { if (restore) _dirty = true; }
    bool prepare_standby() override { stop_worker(); return true; }
    void on_standby_resume() override { _dirty = true; if (_automatic) start_worker(); }
    void loop() override {
        if (_worker) {
            if (auto *result = _worker->result.exchange(nullptr)) {
                _error = result->error;
                if (result->photo) {
                    _saving = false;
                    _save_error = result->save_error;
                    _saved_path = result->saved_path;
                }
                if (_error == ESP_OK) { delete _frame; _frame = result; }
                else { delete result; _automatic = false; }
                _dirty = true;
            }
            if (!_worker->running.load() && _worker->error.load() != ESP_OK && _error == ESP_OK) {
                _error = _worker->error.load(); _automatic = false; _dirty = true;
                if (!_capture_pending) _saving = false;
            }
        }
        if (M5.Display.displayBusy() || touch_input_needs_fast_poll()) return;
        if (_dirty) { draw(false); return; }
        StatusBarActivity::loop();
        if (M5.Display.displayBusy()) return;
        if (_capture_pending && (!_worker || !_worker->busy.load())) {
            _capture_pending = false;
            if (!request_frame(true)) { _saving = false; _dirty = true; }
            return;
        }
        if (_automatic && uint32_t(lgfx::millis()-_last_submit) >= preview_interval_ms)
            request_frame();
    }
    bool on_touch(const TouchEvent &event) override {
        if (event.type == TouchEventType::Back || (event.type == TouchEventType::Tap &&
            app_status_bar_layout::back_hit(event.x,event.y))) {
            ActivityManager::instance().pop(); return true;
        }
        if (status_bar_touch(event)) return true;
        if (event.type != TouchEventType::Tap) return false;
        if (event.y >= controls_top() && event.y < controls_top()+96) {
            _focus = event.x < M5.Display.width()/2 ? 0 : 1;
            select(); return true;
        }
        return true;
    }
    bool on_key(int key) override {
        if (key == APP_KEY_BACK) { ActivityManager::instance().pop(); return true; }
        if (key == APP_KEY_UP || key == APP_KEY_DOWN) { _focus ^= 1; _dirty = true; return true; }
        if (key == APP_KEY_OK) { select(); return true; }
        return false;
    }
private:
    WorkerState *_worker = nullptr;
    Frame *_frame = nullptr;
    bool _automatic = true, _dirty = false;
    bool _capture_pending = false, _saving = false;
    std::string _saved_path;
    int _focus = 0;
    uint32_t _last_submit = 0;
    esp_err_t _error = ESP_OK;
    esp_err_t _save_error = ESP_OK;
    static int image_top() { return 230; }
    static int controls_top() { return image_top()+camera::height+60; }
    void start_worker() {
        if (_worker) return;
        _worker = new (std::nothrow) WorkerState;
        if (!_worker) { _error = ESP_ERR_NO_MEM; _automatic = false; _dirty = true; return; }
        if (xTaskCreate(worker,"camera_s31",8192,_worker,4,&_worker->task) != pdPASS) {
            delete _worker; _worker = nullptr;
            _error = ESP_ERR_NO_MEM; _automatic = false; _dirty = true;
        }
    }
    void stop_worker() {
        _capture_pending = false;
        _saving = false;
        _dirty = true;
        if (!_worker) return;
        _worker->stop.store(true);
        if (_worker->running.load() && _worker->task) xTaskNotifyGive(_worker->task);
        // No force-deleting a task that still owns DMA or the camera session.
        while (_worker->running.load()) vTaskDelay(pdMS_TO_TICKS(10));
        if (_worker->task) vTaskDelete(_worker->task);
        delete _worker; _worker = nullptr;
    }
    bool request_frame(bool photo = false) {
        if (_worker && _worker->busy.load()) return false;
        if (_worker && !_worker->running.load()) stop_worker();
        if (!_worker) start_worker();
        if (!_worker) return false;
        _error = ESP_OK;
        if (photo) _saving = true;
        _worker->busy.store(true);
        _worker->save_requested.store(photo);
        _worker->requested.store(true);
        _last_submit = lgfx::millis();
        xTaskNotifyGive(_worker->task);
        return true;
    }
    void select() {
        if (_saving) return; // One requested photo at a time; never queue saves.
        if (_focus == 0) {
            _automatic = false;
            _capture_pending = true;
            _saving = true;
            _save_error = ESP_OK;
            _error = ESP_OK;
            _saved_path.clear();
        } else {
            _automatic = !_automatic;
            if (_automatic) { _saved_path.clear(); _save_error = ESP_OK; request_frame(); }
        }
        _dirty = true;
    }
    void draw(bool quality) {
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_hd);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(TextId::Camera));
        system_font::draw_left(M5.Display,"<",24,94,TFT_BLACK);
        const auto message = _saving ? TextId::CameraSavingPhoto :
            _error == ESP_ERR_NOT_FOUND ? TextId::CameraMissing :
            _error != ESP_OK ? TextId::CameraFailed :
            _save_error != ESP_OK ? TextId::CameraPhotoSaveFailed :
            !_saved_path.empty() ? TextId::CameraPhotoSaved : !_frame ? TextId::CameraPreparing :
            _automatic ? TextId::CameraPreview : TextId::CameraSnapshot;
        system_font::draw_left(M5.Display,system_font::ellipsize(ui_locale::text(message),M5.Display.width()-40),20,170,TFT_BLACK);
        const int x = (M5.Display.width()-camera::width)/2;
        if (_frame && _frame->packed) {
            auto *panel = static_cast<lgfx::Panel_EPD_2D *>(M5.Display.panel());
            panel->writeNativeGrayscale4Rect(_frame->packed,camera::width,camera::height,x,image_top());
        }
        M5.Display.drawRect(x-1,image_top()-1,camera::width+2,camera::height+2,TFT_BLACK);
        const TextId options[] = {TextId::CameraCapture,TextId::CameraAutoPreview};
        for (int i=0;i<2;++i) {
            const int bx = 20+i*(M5.Display.width()/2);
            const int bw = M5.Display.width()/2-40;
            M5.Display.drawRect(bx,controls_top(),bw,96,TFT_BLACK);
            list_focus::draw(M5.Display,bx+8,controls_top(),96,_focus==i);
            std::string label = ui_locale::text(options[i]);
            if (i==1) label += std::string(" ")+ui_locale::text(_automatic ? TextId::On : TextId::Off);
            system_font::draw_left(M5.Display,system_font::ellipsize(label,bw-40),bx+list_focus::text_inset()+10,controls_top()+30,TFT_BLACK);
        }
        system_font::draw_left(M5.Display,system_font::ellipsize(ui_locale::text(
            _automatic ? TextId::CameraPreviewHint : TextId::CameraPhotoFolder),M5.Display.width()-40),20,controls_top()+155,TFT_DARKGRAY);
        if (_error != ESP_OK && _error != ESP_ERR_NOT_FOUND)
            system_font::draw_left(M5.Display,esp_err_to_name(_error),20,controls_top()+225,TFT_DARKGRAY);
        else if (_save_error != ESP_OK)
            system_font::draw_left(M5.Display,_save_error == ESP_ERR_NOT_FOUND ?
                ui_locale::text(TextId::SdCardNotMounted) : esp_err_to_name(_save_error),20,controls_top()+225,TFT_DARKGRAY);
        else if (!_saved_path.empty()) {
            const size_t slash = _saved_path.find_last_of('/');
            system_font::draw_left(M5.Display,system_font::ellipsize(_saved_path.substr(slash+1),M5.Display.width()-40),
                20,controls_top()+225,TFT_DARKGRAY);
        }
        M5.Display.display();
        M5.Display.endWrite();
        _dirty = false;
    }
};
}
void launch_camera() { ActivityManager::instance().push(new CameraActivity()); }
