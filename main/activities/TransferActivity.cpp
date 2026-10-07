#include "activities/TransferActivity.h"
#include "ui/ListFocus.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/WifiTransferActivity.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/UiLocale.h"
#include "usb/usb_transfer_boot.h"

#include <M5Unified.h>
#include <esp_log.h>
#include <esp_system.h>

namespace {

constexpr int HEADER_H = header_layout::height;
constexpr int PAD = 24;
constexpr int ROW_H = 170;
constexpr int ROW_GAP = 24;
constexpr int FIRST_ROW_Y = HEADER_H;

class TransferActivity final : public Activity {
public:
    void on_create() override { draw(); }
    void on_show() override
    {
        if (_shown) draw();
        _shown = true;
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back ||
            (event.type == TouchEventType::Tap && header_layout::left_hit(event.x, event.y, 120))) {
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

        if (event.y >= FIRST_ROW_Y && event.y < FIRST_ROW_Y + ROW_H) {
            _confirm_usb = true;
            draw();
            return true;
        }
        const int wifi_y = FIRST_ROW_Y + ROW_H + ROW_GAP;
        if (event.y >= wifi_y && event.y < wifi_y + ROW_H) {
            launch_wifi_transfer();
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
            _focus = 1 - _focus;
            draw();
            return true;
        }
        if (key == APP_KEY_OK) {
            if (_confirm_usb) enter_usb_mode();
            else if (_focus == 0) {
                _confirm_usb = true;
                draw();
            } else launch_wifi_transfer();
            return true;
        }
        return false;
    }

private:
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
        list_focus::draw(M5.Display, PAD, y, ROW_H, focused);
        system_font::draw_left(M5.Display, title, PAD + list_focus::text_inset(), y + 24, TFT_BLACK);
        system_font::draw_left(M5.Display, detail, PAD + list_focus::text_inset(), y + 88, TFT_DARKGRAY);
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
        system_font::draw_center(M5.Display, ui_locale::text(_confirm_usb ? ui_locale::TextId::EnterUsbTransfer
                                                                          : ui_locale::TextId::Transfer),
                                 w / 2, header_layout::text_y(), TFT_BLACK);
        header_layout::draw_separator(M5.Display);

        if (!_confirm_usb) {
            draw_row(FIRST_ROW_Y, ui_locale::text(ui_locale::TextId::UsbTransfer),
                     ui_locale::text(ui_locale::TextId::ConnectAsSdCard), _focus == 0);
            draw_row(FIRST_ROW_Y + ROW_H + ROW_GAP, ui_locale::text(ui_locale::TextId::WifiTransfer),
                     ui_locale::text(ui_locale::TextId::UploadManageBrowser), _focus == 1);
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
