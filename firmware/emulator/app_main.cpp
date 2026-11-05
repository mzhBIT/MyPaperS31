#include "GameAudioOutput.h"
#include "PaperDisplay.h"
#include "RomStorage.h"
#include "boards/MyPaperS31/Pins.h"
#include "boot/firmware_switch.h"
#include "frontlight/frontlight.h"
#include "gb_core.h"
#include "input/BoardKeyState.h"
#include <algorithm>
#include <cstdlib>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lgfx/v1/LGFX_Sprite.hpp>
#include <lgfx/v1/lgfx_fonts.hpp>
#include <nvs_flash.h>

namespace emulator {
namespace {
constexpr char TAG[] = "EMULATOR";
constexpr uint32_t Black = 0x000000, White = 0xffffff;
struct Rect {
    int x, y, w, h;
    bool hit(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};
constexpr Rect PauseButton{508, 25, 150, 65};
constexpr Rect SelectButton{305, 1055, 150, 75}, StartButton{490, 1055, 150, 75};
constexpr int ListTop = 185, ListRow = 92, ListRows = 9;
constexpr Rect SoundButton{72, 1050, 190, 64}, VolumeDown{300, 1050, 120, 64},
    VolumeUp{460, 1050, 120, 64};
enum class View { Browser, Game, Menu };
enum Action {
    None,
    Previous,
    Next,
    Up,
    Return,
    Reload,
    Mute,
    Quieter,
    Louder,
    PickBase = 100,
    MenuBase = 200
};

class Application {
  public:
    bool init() {
        if (!hardware_.init() || !display_.init(hardware_))
            return false;
        canvas_.setPsram(true);
        canvas_.setColorDepth(1);
        if (!canvas_.createSprite(PanelWidth, PanelHeight))
            return false;
        canvas_.setRotation(3);
        canvas_.setPaletteColor(0, Black);
        canvas_.setPaletteColor(1, White);
        canvas_.setFont(&lgfx::fonts::efontCN_24);
        canvas_.setTextColor(Black, White);
        gpio_config_t config{};
        config.pin_bit_mask = 1ULL << mypaper::board::s31::boot_button;
        config.mode = GPIO_MODE_INPUT;
        config.pull_up_en = GPIO_PULLUP_ENABLE;
        if (gpio_config(&config) != ESP_OK)
            return false;
        frontlight_init();
        if (!audio_.begin(hardware_))
            ESP_LOGW(TAG, "audio initialization unavailable; game stays usable");
        mounted_ = storage_.mount();
        if (!mounted_)
            message_ = storage_.error();
        else
            read_directory();
        draw_browser();
        return display_.healthy();
    }
    void run() {
        board_keys::Button boot, power;
        boot.reset(gpio_get_level(gpio_num_t(mypaper::board::s31::boot_button)) == 0,
                   lgfx::millis());
        bool power_initialized = false, touch_down = false;
        int touch_action = None;
        int64_t last_input = 0, last_rtc = esp_timer_get_time(), last_log = last_rtc;
        uint32_t frames = 0, presented = 0;
        for (;;) {
            if (!display_.healthy())
                return_system(false, true);
            Contact contacts[2];
            unsigned count = 0;
            uint8_t port = 0xff;
            const int64_t now = esp_timer_get_time();
            const bool input_ok = hardware_.input(contacts, count, port);
            if (input_ok)
                last_input = now;
            const uint32_t ms = now / 1000;
            const auto boot_action = boot.sample(
                gpio_get_level(gpio_num_t(mypaper::board::s31::boot_button)) == 0, ms, 3000);
            board_keys::Gesture power_action = board_keys::Gesture::None;
            if (input_ok) {
                const bool down = !(port & 0x80);
                if (!power_initialized) {
                    power.reset(down, ms);
                    power_initialized = true;
                } else
                    power_action = power.sample(down, ms, 3000);
            }
            if (boot_action == board_keys::Gesture::Click ||
                power_action == board_keys::Gesture::Click) {
                if (view_ == View::Game)
                    pause();
                else if (view_ == View::Menu)
                    resume();
                else
                    return_system();
                touch_action = None;
                touch_down = false;
            }
            if (power_action == board_keys::Gesture::LongPress) {
                if (view_ == View::Game)
                    pause();
                return_system();
            }
            if (input_ok) {
                const bool down = count > 0;
                if (suppress_touch_) {
                    if (!down)
                        suppress_touch_ = false;
                } else {
                    if (down && !touch_down)
                        touch_action = action_at(contacts[0].x, contacts[0].y);
                    if (down && action_at(contacts[0].x, contacts[0].y) != touch_action)
                        touch_action = None;
                    if (!down && touch_down && touch_action != None) {
                        dispatch(touch_action);
                        touch_action = None;
                    }
                }
                touch_down = down;
                buttons_ = 0;
                if (view_ == View::Game && !suppress_touch_) {
                    for (unsigned i = 0; i < count; ++i)
                        buttons_ |= game_buttons(contacts[i]);
                    if (!(port & 1))
                        buttons_ |= EMU_A;
                    if (!(port & 2))
                        buttons_ |= EMU_B;
                }
            } else if (now - last_input > 100000) {
                buttons_ = 0;
                touch_action = None;
                touch_down = false;
            }
            const uint32_t seconds = (now - last_rtc) / 1000000;
            if (seconds) {
                if (rom_)
                    emu_gb_tick(seconds);
                last_rtc += int64_t(seconds) * 1000000;
            }
            if (view_ == View::Game) {
                unsigned catchup = 0;
                while (now >= next_frame_ && catchup++ < 3) {
                    if (!emu_gb_frame(buttons_)) {
                        core_failed_ = true;
                        message_ = emu_gb_error();
                        audio_.pause();
                        display_.stop();
                        view_ = View::Menu;
                        draw_menu();
                        break;
                    }
                    next_frame_ += 16742;
                    audio_.submit(emu_gb_audio(), emu_gb_audio_frames());
                    frame_dirty_ = true;
                    ++frames;
                }
                if (view_ == View::Game) {
                    if (catchup > 3 && now > next_frame_ + 4 * 16742)
                        next_frame_ = now;
                    if (frame_dirty_ && display_.present(emu_gb_lcd())) {
                        frame_dirty_ = false;
                        ++presented;
                    }
                }
            }
            if (now - last_log >= 5000000) {
                ESP_LOGI(TAG, "core frames=%lu presented=%lu window=%lu ms SRAM=%u PSRAM=%u",
                         (unsigned long)frames, (unsigned long)presented,
                         (unsigned long)((now - last_log) / 1000),
                         unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
                last_log = now;
                frames = presented = 0;
            }
            vTaskDelay(1);
        }
    }
    void shutdown_audio() {
        audio_.shutdown();
    }

  private:
    void text(const std::string &value, int x, int y, int max_width = 620) {
        std::string fit = value;
        while (!fit.empty() && canvas_.textWidth(fit.c_str()) > max_width) {
            size_t last = fit.size() - 1;
            while (last > 0 && (uint8_t(fit[last]) & 0xc0) == 0x80)
                --last;
            fit.resize(last);
        }
        canvas_.drawString(fit.c_str(), x, y);
    }
    void base(const char *title) {
        canvas_.fillScreen(White);
        canvas_.setTextColor(Black, White);
        canvas_.setTextSize(1.4f);
        canvas_.drawString(title, 26, 35);
        canvas_.drawFastHLine(24, 110, 636, Black);
        canvas_.setTextSize(1);
    }
    void button(Rect r, const char *title) {
        canvas_.drawRect(r.x, r.y, r.w, r.h, Black);
        canvas_.drawCenterString(title, r.x + r.w / 2, r.y + (r.h - 24) / 2);
    }
    void show() {
        display_.image(static_cast<const uint8_t *>(canvas_.getBuffer()));
        suppress_touch_ = true;
    }
    void read_directory() {
        message_.clear();
        if (!storage_.list(directory_, entries_))
            message_ = storage_.error();
        page_ = std::min(page_, entries_.empty() ? size_t(0) : (entries_.size() - 1) / ListRows);
    }
    void draw_browser() {
        view_ = View::Browser;
        base("S31 Game Boy");
        button({490, 25, 170, 65}, "MyPaper");
        text(directory_, 26, 136);
        for (int i = 0; i < ListRows; ++i) {
            const size_t index = page_ * ListRows + i;
            if (index >= entries_.size())
                break;
            const auto &entry = entries_[index];
            const int y = ListTop + i * ListRow;
            text((entry.directory ? "[+] " : "") + entry.name, 32, y + 24, 610);
            canvas_.drawFastHLine(26, y + ListRow - 1, 632, Black);
        }
        if (entries_.empty() && message_.empty())
            text("Copy .gb ROMs to /sdcard/roms", 32, 250);
        if (!message_.empty())
            text(message_, 26, 1028);
        const size_t pages = std::max(size_t(1), (entries_.size() + ListRows - 1) / ListRows);
        text(std::to_string(page_ + 1) + " / " + std::to_string(pages), 290, 1100, 160);
        button({26, 1070, 150, 72}, "Previous");
        button({508, 1070, 150, 72}, "Next");
        button({26, 1156, 190, 48}, "Up");
        button({468, 1156, 190, 48}, "Reload SD");
        show();
    }
    void draw_game() {
        base("Game Boy");
        button(PauseButton, "Menu");
        text(emu_gb_title(), 26, 125, 460);
        canvas_.drawRect(GameX - 2, GameY - 2, GameWidth * 3 + 4, GameHeight * 3 + 4, Black);
        button({120, 705, 90, 90}, "UP");
        button({30, 795, 90, 90}, "LEFT");
        button({210, 795, 90, 90}, "RIGHT");
        button({120, 885, 90, 90}, "DOWN");
        canvas_.drawCircle(555, 800, 64, Black);
        canvas_.drawCenterString("A", 555, 788);
        canvas_.drawCircle(425, 900, 64, Black);
        canvas_.drawCenterString("B", 425, 888);
        button(SelectButton, "SELECT");
        button(StartButton, "START");
        text("VOL+ = A   VOL- = B   BOOT = Menu", 26, 1173);
        show();
    }
    static Rect menu_rect(int i) {
        return {72, 200 + i * 118, 540, 88};
    }
    void draw_menu() {
        view_ = View::Menu;
        base("Paused");
        text(emu_gb_title(), 26, 140);
        static const char *labels[] = {"Resume",          "Save state", "Load state",
                                       "Restart game",    "ROM list",   "Return to MyPaper",
                                       "Discard & return"};
        for (int i = 0; i < (save_failed_ ? 7 : 6); ++i)
            button(menu_rect(i), labels[i]);
        if (!message_.empty())
            text(message_, 26, save_failed_ ? 1010 : 990);
        button(SoundButton, audio_.muted() ? "Sound: OFF" : "Sound: ON");
        button(VolumeDown, "Volume -");
        button(VolumeUp, "Volume +");
        text("Volume: " + std::to_string(audio_.volume()) + "%", 72, 1140);
        if (audio_.failed())
            text("Sound unavailable", 320, 1140, 340);
        else if (!audio_.ready())
            text("Sound starting...", 320, 1140, 340);
        show();
    }
    int action_at(int x, int y) const {
        if (y >= 1570 && y <= 1630)
            return x >= 70 && x <= 130 ? Return : None;
        if (view_ == View::Game)
            return PauseButton.hit(x, y) ? Return : None;
        if (view_ == View::Menu) {
            if (SoundButton.hit(x, y))
                return Mute;
            if (VolumeDown.hit(x, y))
                return Quieter;
            if (VolumeUp.hit(x, y))
                return Louder;
            for (int i = 0; i < (save_failed_ ? 7 : 6); ++i)
                if (menu_rect(i).hit(x, y))
                    return MenuBase + i;
            return None;
        }
        if (Rect{490, 25, 170, 65}.hit(x, y))
            return Return;
        if (Rect{26, 1070, 150, 72}.hit(x, y))
            return Previous;
        if (Rect{508, 1070, 150, 72}.hit(x, y))
            return Next;
        if (Rect{26, 1156, 190, 48}.hit(x, y))
            return Up;
        if (Rect{468, 1156, 190, 48}.hit(x, y))
            return Reload;
        if (x >= 26 && x < 658 && y >= ListTop && y < ListTop + ListRows * ListRow)
            return PickBase + (y - ListTop) / ListRow;
        return None;
    }
    uint8_t game_buttons(Contact p) const {
        uint8_t mask = 0;
        if (Rect{120, 705, 90, 90}.hit(p.x, p.y))
            mask |= EMU_UP;
        if (Rect{30, 795, 90, 90}.hit(p.x, p.y))
            mask |= EMU_LEFT;
        if (Rect{210, 795, 90, 90}.hit(p.x, p.y))
            mask |= EMU_RIGHT;
        if (Rect{120, 885, 90, 90}.hit(p.x, p.y))
            mask |= EMU_DOWN;
        if ((p.x - 555) * (p.x - 555) + (p.y - 800) * (p.y - 800) < 64 * 64)
            mask |= EMU_A;
        if ((p.x - 425) * (p.x - 425) + (p.y - 900) * (p.y - 900) < 64 * 64)
            mask |= EMU_B;
        if (SelectButton.hit(p.x, p.y))
            mask |= EMU_SELECT;
        if (StartButton.hit(p.x, p.y))
            mask |= EMU_START;
        if (p.y >= 1570 && p.y <= 1630) {
            if (p.x >= 170 && p.x <= 230)
                mask |= EMU_LEFT;
            if (p.x >= 270 && p.x <= 330)
                mask |= EMU_RIGHT;
        }
        return mask;
    }
    uint8_t *buffer(size_t size) {
        return static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    bool save_battery() {
        if (!rom_)
            return true;
        const size_t size = emu_gb_battery_size();
        if (!size)
            return true;
        uint8_t *data = buffer(size);
        const bool allocated = data != nullptr;
        const bool ok = data && emu_gb_battery_export(data, size) &&
                        storage_.write_save(rom_path_, rom_crc_, 1, data, size);
        free(data);
        if (!ok) {
            message_ = allocated ? storage_.error() : "Save buffer allocation failed";
            save_failed_ = true;
            ESP_LOGE(TAG, "save failed: %s", message_.c_str());
        } else
            save_failed_ = false;
        return ok;
    }
    void load_battery() {
        const size_t size = emu_gb_battery_size();
        if (!size)
            return;
        uint8_t *data = buffer(size);
        bool found = false;
        if (!data) {
            message_ = "Battery buffer allocation failed";
            return;
        }
        if (!storage_.read_save(rom_path_, rom_crc_, 1, data, size, found))
            message_ = storage_.error();
        else if (found && !emu_gb_battery_import(data, size))
            message_ = emu_gb_error();
        free(data);
        if (!message_.empty())
            ESP_LOGW(TAG, "battery load: %s", message_.c_str());
    }
    void state(bool load) {
        const size_t size = emu_gb_state_size();
        uint8_t *data = buffer(size);
        bool found = false, ok = false;
        if (data) {
            if (load) {
                size_t loaded_size = size;
                ok = storage_.read_save(rom_path_, rom_crc_, 2, data, loaded_size, found);
                if (!ok) {
                    loaded_size = emu_gb_legacy_state_size();
                    ok = storage_.read_save(rom_path_, rom_crc_, 2, data, loaded_size, found);
                }
                if (ok && !found) {
                    message_ = "No saved state";
                    ok = false;
                } else if (ok && !emu_gb_state_import(data, loaded_size)) {
                    message_ = emu_gb_error();
                    ok = false;
                } else if (!ok)
                    message_ = storage_.error();
            } else {
                ok = emu_gb_state_export(data, size) &&
                     storage_.write_save(rom_path_, rom_crc_, 2, data, size);
                if (!ok)
                    message_ = storage_.error();
            }
        } else
            message_ = "State buffer allocation failed";
        free(data);
        if (ok)
            message_ = load ? "State loaded" : "State saved";
        if (ok && load)
            core_failed_ = false;
        ESP_LOGI(TAG, "%s state ok=%d bytes=%u", load ? "load" : "save", ok, unsigned(size));
        draw_menu();
    }
    void release_rom() {
        emu_gb_close();
        free(rom_);
        rom_ = nullptr;
        rom_size_ = 0;
        rom_path_.clear();
    }
    void open(const std::string &path) {
        message_.clear();
        uint8_t *rom = nullptr;
        size_t size = 0;
        uint32_t crc = 0;
        if (!storage_.read_rom(path, rom, size, crc)) {
            message_ = storage_.error();
            draw_browser();
            return;
        }
        if (!emu_gb_init(rom, size)) {
            message_ = emu_gb_error();
            free(rom);
            draw_browser();
            return;
        }
        rom_ = rom;
        core_failed_ = false;
        rom_size_ = size;
        rom_crc_ = crc;
        rom_path_ = path;
        load_battery();
        ESP_LOGI(TAG, "ROM loaded path=%s bytes=%u title=%s", path.c_str(), unsigned(size),
                 emu_gb_title());
        if (!message_.empty()) {
            draw_menu();
            return;
        }
        resume();
    }
    void resume() {
        if (!rom_)
            return;
        if (core_failed_) {
            draw_menu();
            return;
        }
        draw_game();
        (void)audio_.play();
        if (display_.video()) {
            view_ = View::Game;
            buttons_ = 0;
            frame_dirty_ = true;
            next_frame_ = esp_timer_get_time();
        }
    }
    void pause() {
        audio_.pause();
        display_.stop();
        message_.clear();
        save_battery();
        draw_menu();
        buttons_ = 0;
    }
    void return_system(bool discard = false, bool scan_failed = false) {
        ESP_LOGI(TAG, "return: saving game");
        audio_.pause();
        if (!discard && !save_battery()) {
            if (!scan_failed) {
                draw_menu();
                return;
            }
        }
        const esp_err_t result = firmware_switch::select_system();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "return: system selection failed: %s", esp_err_to_name(result));
            message_ = "System image validation failed";
            if (!scan_failed)
                draw_menu();
            return;
        }
        // Check the strap before blanking and dismantling the UI. A held or
        // electrically low BOOT must not leave the user staring at white forever.
        const auto boot_pin = gpio_num_t(mypaper::board::s31::boot_button);
        if (gpio_get_level(boot_pin) == 0) {
            ESP_LOGW(TAG, "return: BOOT GPIO%d low; release the key", int(boot_pin));
            if (!scan_failed) {
                canvas_.fillScreen(White);
                text("Release BOOT to return", 72, 550);
                show();
            }
            const int64_t deadline = esp_timer_get_time() + 3000000;
            while (gpio_get_level(boot_pin) == 0 && esp_timer_get_time() < deadline)
                vTaskDelay(pdMS_TO_TICKS(20));
            if (gpio_get_level(boot_pin) == 0) {
                ESP_LOGW(TAG, "return: BOOT still low; restart cancelled");
                message_ = "BOOT held; release and retry";
                if (!scan_failed) {
                    if (rom_)
                        draw_menu();
                    else
                        draw_browser();
                }
                return;
            }
        }
        if (!scan_failed) {
            ESP_LOGI(TAG, "return: clearing display");
            canvas_.fillScreen(White);
            show();
            display_.stop();
        }
        ESP_LOGI(TAG, "return: stopping audio");
        if (!audio_.shutdown())
            ESP_LOGW(TAG, "return: audio worker did not exit; continuing to system restart");
        ESP_LOGI(TAG, "return: unmounting SD");
        storage_.unmount();
        frontlight_set_suspended(true);
        // Recheck in case BOOT was pressed during teardown. Never restart with
        // the strap low, but provide a visible explanation instead of white.
        if (gpio_get_level(boot_pin) == 0) {
            ESP_LOGW(TAG, "return: BOOT pressed during teardown; waiting for release");
            if (!scan_failed) {
                canvas_.fillScreen(White);
                text("Release BOOT to return", 72, 550);
                show();
            }
            while (gpio_get_level(boot_pin) == 0)
                vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGI(TAG, "returning to MyPaper");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
    void dispatch(int action) {
        if (view_ == View::Game) {
            if (action == Return)
                pause();
            return;
        }
        if (view_ == View::Menu) {
            if (action == Mute || action == Quieter || action == Louder) {
                if (action == Mute)
                    audio_.mute(!audio_.muted());
                else
                    audio_.volume(audio_.volume() + (action == Louder ? 5 : -5));
                draw_menu();
                return;
            }
            if (action == MenuBase)
                resume();
            else if (action == MenuBase + 1)
                state(false);
            else if (action == MenuBase + 2)
                state(true);
            else if (action == MenuBase + 3) {
                if (!save_battery()) {
                    draw_menu();
                    return;
                }
                if (emu_gb_init(rom_, rom_size_)) {
                    core_failed_ = false;
                    message_.clear();
                    load_battery();
                    resume();
                } else {
                    message_ = emu_gb_error();
                    draw_menu();
                }
            } else if (action == MenuBase + 4) {
                if (!save_battery()) {
                    draw_menu();
                    return;
                }
                release_rom();
                message_.clear();
                draw_browser();
            } else if (action == MenuBase + 5 || action == Return)
                return_system();
            else if (action == MenuBase + 6 && save_failed_)
                return_system(true);
            return;
        }
        if (action == Return) {
            return_system();
            return;
        }
        if (action == Previous) {
            if (page_ > 0)
                --page_;
            draw_browser();
        } else if (action == Next) {
            if ((page_ + 1) * ListRows < entries_.size())
                ++page_;
            draw_browser();
        } else if (action == Up) {
            if (directory_ != "/sdcard/roms") {
                directory_ = directory_.substr(0, directory_.find_last_of('/'));
                page_ = 0;
                read_directory();
            }
            draw_browser();
        } else if (action == Reload) {
            if (!mounted_)
                mounted_ = storage_.mount();
            if (mounted_)
                read_directory();
            else
                message_ = storage_.error();
            draw_browser();
        } else if (action >= PickBase && action < PickBase + ListRows) {
            const size_t i = page_ * ListRows + action - PickBase;
            if (i >= entries_.size())
                return;
            const RomEntry entry = entries_[i];
            if (entry.directory) {
                directory_ = entry.path;
                page_ = 0;
                read_directory();
                draw_browser();
            } else
                open(entry.path);
        }
    }
    S31Hardware hardware_;
    PaperDisplay display_;
    GameAudioOutput audio_;
    RomStorage storage_;
    lgfx::LGFX_Sprite canvas_;
    View view_ = View::Browser;
    std::vector<RomEntry> entries_;
    std::string directory_ = "/sdcard/roms", message_, rom_path_;
    size_t page_ = 0, rom_size_ = 0;
    uint8_t *rom_ = nullptr;
    uint32_t rom_crc_ = 0;
    uint8_t buttons_ = 0;
    int64_t next_frame_ = 0;
    bool mounted_ = false, suppress_touch_ = true, save_failed_ = false;
    bool frame_dirty_ = false, core_failed_ = false;
};
} // namespace
} // namespace emulator
extern "C" void app_main() {
    const esp_err_t armed = firmware_switch::select_system();
    ESP_LOGI("EMULATOR", "S31-only independent firmware; return armed=%s", esp_err_to_name(armed));
    const esp_err_t nvs = nvs_flash_init(); // Never erase shared system settings.
    if (nvs != ESP_OK)
        ESP_LOGW("EMULATOR", "NVS unavailable: %s", esp_err_to_name(nvs));
    const auto usb = gpio_num_t(mypaper::board::s31::usb_analog_switch);
    gpio_set_level(usb, 1);
    gpio_set_direction(usb, GPIO_MODE_OUTPUT);
    static emulator::Application app;
    if (app.init())
        app.run();
    app.shutdown_audio();
    ESP_LOGE("EMULATOR", "initialization failed; returning to system in 3 seconds");
    gpio_set_level(gpio_num_t(mypaper::board::s31::epd_xoe), 0);
    frontlight_set_suspended(true);
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (armed == ESP_OK)
        esp_restart();
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
