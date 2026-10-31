#include "activities/EmulatorActivity.h"
#include "activities/StatusBarActivity.h"
#include "activities/ActivityManager.h"
#include "boot/firmware_switch.h"
#include "audio/audio_radio.h"
#include "audio/usb_audio_output.h"
#include "time/network_time.h"
#include "wifi/wifi_setting.h"
#include "weread/WereadReadingSession.h"
#include "ui/UiLocale.h"
#include "ui/SystemFont.h"
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <mutex>

namespace {
using ui_locale::TextId;
class EmulatorActivity final:public StatusBarActivity {
    bool pending=false,available=false;
    int64_t requested=0;
    std::string message;
    void draw() {
        auto& gfx=M5.Display;
        gfx.startWrite();gfx.setEpdMode(lgfx::epd_mode_t::epd_text);gfx.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(TextId::Emulator));
        system_font::draw_center(gfx,ui_locale::text(TextId::EmulatorRestartHint),gfx.width()/2,160);
        system_font::draw_center(gfx,system_font::ellipsize(message,gfx.width()-40),gfx.width()/2,250);
        const int y=gfx.height()-240;
        gfx.drawFastHLine(20,y,gfx.width()-40,TFT_BLACK);
        system_font::draw_center(gfx,ui_locale::text(available ? TextId::EmulatorStart : TextId::Cancel),gfx.width()/2,y+28);
        gfx.display();gfx.endWrite();
    }
    void confirm() {
        if(pending)return;
        if(!available){ActivityManager::instance().pop();return;}
        pending=true;requested=esp_timer_get_time();
        // Leave a known white panel for the other firmware's fresh framebuffer.
        auto& gfx=M5.Display;
        gfx.startWrite();gfx.setEpdMode(lgfx::epd_mode_t::epd_quality);
        gfx.fillScreen(TFT_WHITE);gfx.display();gfx.endWrite();
    }
    void fail(esp_err_t error) {
        pending=false;
        ESP_LOGE("FIRMWARE_SWITCH","emulator entry failed: %s",esp_err_to_name(error));
        message=std::string(ui_locale::text(TextId::EmulatorSwitchFailed))+": "+esp_err_to_name(error);
        draw();
    }
public:
    void on_create() override {
        available=firmware_switch::check_emulator()==ESP_OK;
        message=ui_locale::text(available ? TextId::EmulatorReady : TextId::EmulatorMissing);
        draw();
    }
    bool can_standby()const override{return !pending;}
    void loop() override {
        if(!pending){StatusBarActivity::loop();return;}
        if(esp_timer_get_time()-requested>15000000){fail(ESP_ERR_TIMEOUT);return;}
        if(M5.Display.displayBusy()||wifi_setting_background_busy()||network_time_busy()||
            !weread::reading_sessions_idle())return;
        std::unique_lock<std::mutex> reports(weread::reading_transaction_mutex(),std::try_to_lock);
        if(!reports.owns_lock()||!network_time_try_pause())return;
        struct Resume{~Resume(){network_time_resume();}} resume;
        if(audio_radio_stop()!=ESP_OK){fail(ESP_FAIL);return;}
        audio_radio_deinit();
        if(!usb_audio_output_is_quiescent()){fail(ESP_ERR_INVALID_STATE);return;}
        const auto error=firmware_switch::select_emulator();
        if(error!=ESP_OK){fail(error);return;}
        esp_restart();
    }
    bool on_key(int key) override {
        if(key==APP_KEY_BACK){pending=false;ActivityManager::instance().pop();return true;}
        if(key==APP_KEY_OK){confirm();return true;}
        return pending;
    }
    bool on_touch(const TouchEvent& event) override {
        if(event.type==TouchEventType::Back)return on_key(APP_KEY_BACK);
        if(event.type==TouchEventType::Tap&&event.y>=M5.Display.height()-240){confirm();return true;}
        return pending;
    }
};
}
void launch_emulator(){ActivityManager::instance().push(new EmulatorActivity);}
