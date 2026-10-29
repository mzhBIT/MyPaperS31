#include "activities/TransferActivity.h"
#include "ui/ListFocus.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/WifiTransferActivity.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/AppStatusBar.h"
#include "ui/UiLocale.h"
#include "usb/usb_transfer_boot.h"

#include <M5Unified.h>
#include <esp_log.h>
#include <esp_system.h>
#include <algorithm>

namespace {

constexpr int HEADER_H = app_status_bar::height;
constexpr int PAD = 24;
constexpr int ROW_GAP = 24;
constexpr int FIRST_ROW_Y = HEADER_H;
int row_height() { return std::max(170,system_font::line_height()*2+60); }
int row_y(int index) { return FIRST_ROW_Y+index*(row_height()+ROW_GAP); }

class TransferActivity final : public Activity {
public:
    explicit TransferActivity(bool usb = false) : _confirm_usb(usb) {}
    void on_create() override { draw(); }
    void loop() override { _status_bar.refresh(status_title()); }
    void on_show() override
    {
        if (_shown) draw();
        _shown = true;
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;

        if (_confirm_usb) {
            const int h = M5.Display.height();
            if (event.y >= h - 130) {
                if (event.x < M5.Display.width() / 2) {
                    _confirm_usb = false;
                    draw();
                } else {
                    enter_usb_mode();
                }
                return true;
            }
            return true;
        }

        if (event.y >= row_y(0) && event.y < row_y(0) + row_height()) {
            _confirm_usb = true;
            draw();
            return true;
        }
        const int wifi_y = row_y(1);
        if (event.y >= wifi_y && event.y < wifi_y + row_height()) {
            launch_wifi_transfer();
            return true;
        }
        const int hotspot_y = row_y(2);
        if (event.y >= hotspot_y && event.y < hotspot_y + row_height()) {
            launch_hotspot_transfer();
            return true;
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            if (_confirm_usb) {
                _confirm_usb = false;
                draw();
            } else {
                ActivityManager::instance().pop();
            }
            return true;
        }
        if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
            if (_confirm_usb) return true;
            _focus = (_focus + (key == APP_KEY_UP ? 2 : 1)) % 3;
            draw();
            return true;
        }
        if (key == APP_KEY_OK) {
            if (_confirm_usb) enter_usb_mode();
            else if (_focus == 0) {
                _confirm_usb = true;
                draw();
            } else if (_focus == 1) launch_wifi_transfer();
            else launch_hotspot_transfer();
            return true;
        }
        return false;
    }

private:
    app_status_bar::State _status_bar;
    const char* status_title()const {
        return ui_locale::text(_confirm_usb ? ui_locale::TextId::EnterUsbTransfer : ui_locale::TextId::Transfer);
    }
    bool _shown = false;
    bool _confirm_usb = false;
    int _focus = 0;

    void enter_usb_mode()
    {
        esp_err_t ret = usb_transfer_boot_set(true);
        if (ret != ESP_OK) {
            ESP_LOGE("TRANSFER", "Failed to store USB boot mode: %s", esp_err_to_name(ret));
            return;
        }
        M5.Display.waitDisplay();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }

    void draw_row(int y, const char *title, const char *detail, bool focused)
    {
        const int w = M5.Display.width();
        const int ROW_H = row_height();
        list_focus::draw(M5.Display, PAD, y, ROW_H, focused);
        system_font::draw_left(M5.Display, title, PAD + list_focus::text_inset(), y + 24, TFT_BLACK);
        system_font::draw_left(M5.Display, detail, PAD + list_focus::text_inset(),
                               y + 36 + system_font::line_height(), TFT_DARKGRAY);
        M5.Display.drawFastHLine(PAD, y + ROW_H - 1, w - PAD * 2, TFT_BLACK);
    }

    void draw()
    {
        const int w = M5.Display.width();
        const int h = M5.Display.height();
        const int line_h = system_font::line_height();
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        _status_bar.draw(status_title());

        if (!_confirm_usb) {
            draw_row(FIRST_ROW_Y, ui_locale::text(ui_locale::TextId::UsbTransfer),
                     ui_locale::text(ui_locale::TextId::ConnectAsSdCard), _focus == 0);
            draw_row(row_y(1), ui_locale::text(ui_locale::TextId::WifiTransfer),
                     ui_locale::text(ui_locale::TextId::UploadManageBrowser), _focus == 1);
            draw_row(row_y(2), ui_locale::text(ui_locale::TextId::FileHotspotTransfer),
                     ui_locale::text(ui_locale::TextId::FileConnectHotspot), _focus == 2);
        } else {
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::UsbTransferSdUnavailable), w / 2,
                                     220, TFT_BLACK);
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::UsbTransferWhileConnected), w / 2,
                                     220 + line_h + 8, TFT_BLACK);
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::UsbHeadphonesDisconnected), w / 2,
                                     360, TFT_BLACK);
            const int button_y = h - 130;
            M5.Display.drawRect(PAD, button_y, (w - PAD * 3) / 2, 90, TFT_BLACK);
            M5.Display.drawRect(PAD * 2 + (w - PAD * 3) / 2, button_y,
                                (w - PAD * 3) / 2, 90, TFT_BLACK);
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Cancel), PAD + (w - PAD * 3) / 4,
                                     button_y + 24, TFT_BLACK);
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::RestartAndEnter),
                                     PAD * 2 + (w - PAD * 3) / 2 +
                                         (w - PAD * 3) / 4,
                                     button_y + 24, TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

} // namespace

void launch_transfer()
{
    ActivityManager::instance().push(new TransferActivity());
}

void launch_usb_transfer()
{
    ActivityManager::instance().push(new TransferActivity(true));
}
