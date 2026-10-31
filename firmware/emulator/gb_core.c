#include "gb_core.h"
#include "vendor/minigb_apu/minigb_apu.h"
#undef VERTICAL_SYNC // The GB header supplies the same ratio with its own spelling.
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#define PGB_CGB 0
#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define PGB_IMPL
uint8_t audio_read(void *audio, uint16_t addr);
void audio_write(void *audio, uint16_t addr, uint8_t value);
#include "vendor/crankboy/peanut_gb.h"
_Static_assert(AUDIO_SAMPLE_RATE == EMU_AUDIO_RATE, "GB/APU sample rates must match");
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

int preferences_cgb_speed = 0, preferences_ppu_timing = 0, audio_enabled = 1;
static gb_s *core;
static uint8_t *wram, *vram, *lcd, *ram;
static size_t ram_size;
static bool ready, in_frame;
static jmp_buf frame_escape;
static char error_text[128] = "not initialized", title[17];
static struct minigb_apu_ctx apu;
static uint32_t audio_phase;
static size_t pcm_frames;
static int16_t pcm[EMU_AUDIO_FRAME_CAPACITY * 2];
struct AudioSnapshot {
    uint32_t magic, version, rate, phase;
    struct minigb_apu_ctx apu;
};
static void reset_audio(void) {
    memset(&apu, 0, sizeof(apu));
    minigb_apu_audio_init(&apu);
    audio_phase = 0;
    pcm_frames = 0;
}
uint8_t audio_read(void *audio, uint16_t addr) {
    (void)audio;
    return minigb_apu_audio_read(&apu, addr);
}
void audio_write(void *audio, uint16_t addr, uint8_t value) {
    (void)audio;
    minigb_apu_audio_write(&apu, addr, value);
}
static void *allocate(size_t size) {
#ifdef ESP_PLATFORM
    return heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return calloc(1, size);
#endif
}
static bool fail(const char *message) {
    snprintf(error_text, sizeof(error_text), "%s", message);
    return false;
}
static void core_error(gb_s *gb, enum gb_error_e error, uint16_t address) {
    (void)gb;
    snprintf(error_text, sizeof(error_text), "core error %d at %04x", error, address);
    ready = false;
    if (in_frame)
        longjmp(frame_escape, 1);
}
void __gb_on_breakpoint(gb_s *gb, int breakpoint) {
    (void)gb;
    (void)breakpoint;
}
void emu_gb_close(void) {
    ready = false;
    in_frame = false;
    free(ram);
    free(lcd);
    free(vram);
    free(wram);
    free(core);
    ram = lcd = vram = wram = NULL;
    core = NULL;
    ram_size = 0;
    pcm_frames = 0;
}
bool emu_gb_init(uint8_t *rom, size_t size) {
    emu_gb_close();
    // The upstream initializer indexes ROM/RAM tables before validating them.
    // Check them here, including the banked-read bounds, before entering it.
    if (!rom || size < 32768 || size > 4u * 1024 * 1024)
        return fail("ROM must be 32 KiB to 4 MiB");
    if (rom[0x143] == 0xc0)
        return fail("CGB-only games are not supported (DMG core)");
    if (rom[0x148] > 7 || rom[0x149] > 5)
        return fail("unsupported ROM/RAM size header");
    const size_t expected = (size_t)32768 << rom[0x148];
    if (size != expected)
        return fail("ROM length does not match bank count");
    uint8_t checksum = 0;
    for (unsigned i = 0x134; i <= 0x14c; ++i)
        checksum = checksum - rom[i] - 1;
    if (checksum != rom[0x14d])
        return fail("ROM header checksum mismatch");
    core = allocate(sizeof(*core));
    wram = allocate(WRAM_SIZE_CGB);
    vram = allocate(VRAM_SIZE_CGB);
    lcd = allocate(LCD_BUFFER_BYTES);
    if (!core || !wram || !vram || !lcd) {
        emu_gb_close();
        return fail("not enough internal RAM for GB core");
    }
    reset_audio();
    const enum gb_init_error_e result =
        gb_init(core, wram, vram, lcd, rom, size, core_error, NULL, false);
    if (result != GB_INIT_NO_ERROR && result != GB_INIT_NO_ERROR_BUT_REQUIRES_CGB) {
        emu_gb_close();
        return fail("cartridge controller is not supported");
    }
    gb_reset(core, false);
    ram_size = gb_get_save_size(core);
    if (ram_size) {
        // Cartridge RAM is not in the hot instruction path; use PSRAM on S31.
#ifdef ESP_PLATFORM
        ram = heap_caps_calloc(1, ram_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        ram = calloc(1, ram_size);
#endif
        if (!ram) {
            emu_gb_close();
            return fail("cartridge RAM allocation failed");
        }
        core->gb_cart_ram = ram;
        core->gb_cart_ram_size = ram_size;
    }
    gb_init_lcd(core);
    core->direct.joypad = 0xff;
    gb_get_rom_name(rom, title);
    title[16] = 0;
    // Embedded/font APIs must not interpret arbitrary header control bytes.
    for (unsigned i = 0; i < 16 && title[i]; ++i)
        if ((unsigned char)title[i] < 32 || (unsigned char)title[i] > 126)
            title[i] = '?';
    ready = true;
    snprintf(error_text, sizeof(error_text), "ok");
    return true;
}
bool emu_gb_frame(uint8_t buttons) {
    if (!ready)
        return false;
    if (setjmp(frame_escape)) {
        in_frame = false;
        return false;
    }
    in_frame = true;
    core->direct.joypad = (uint8_t)~buttons;
    gb_run_frame__dmg(core);
    in_frame = false;
    // 70224 CPU clocks per GB frame. Carry the fractional audio sample count
    // instead of treating the upstream 704-sample callback as one video frame.
    const uint64_t ticks = (uint64_t)audio_phase + (uint64_t)EMU_AUDIO_RATE * 70224u;
    pcm_frames = (size_t)(ticks >> 22);
    audio_phase = (uint32_t)(ticks & ((1u << 22) - 1));
    minigb_apu_audio_render(&apu, pcm, (unsigned)pcm_frames);
    return ready;
}
const int16_t *emu_gb_audio(void) {
    return pcm;
}
size_t emu_gb_audio_frames(void) {
    return pcm_frames;
}
const uint8_t *emu_gb_lcd(void) {
    return lcd;
}
const char *emu_gb_error(void) {
    return error_text;
}
const char *emu_gb_title(void) {
    return title;
}
void emu_gb_tick(uint32_t seconds) {
    if (ready)
        gb_catch_up_rtc_direct(core, seconds);
}
size_t emu_gb_battery_size(void) {
    return core ? (ram_size ? ram_size + 5 : core->cart_battery ? 5 : 0) : 0;
}
bool emu_gb_battery_export(uint8_t *out, size_t size) {
    if (!core || !out || size != emu_gb_battery_size() || !size)
        return fail("battery size mismatch");
    if (ram_size)
        memcpy(out, ram, ram_size);
    memcpy(out + ram_size, core->cart_rtc, 5);
    return true;
}
bool emu_gb_battery_import(const uint8_t *in, size_t size) {
    if (!core || !in || size != emu_gb_battery_size() || !size)
        return fail("battery size mismatch");
    if (ram_size)
        memcpy(ram, in, ram_size);
    memcpy(core->cart_rtc, in + ram_size, 5);
    memcpy(core->latched_rtc, core->cart_rtc, 5);
    return true;
}
size_t emu_gb_state_size(void) {
    return core ? gb_get_state_size(core) + sizeof(struct AudioSnapshot) : 0;
}
size_t emu_gb_legacy_state_size(void) {
    return core ? gb_get_state_size(core) : 0;
}
bool emu_gb_state_export(uint8_t *out, size_t size) {
    if (!core || !out || size != emu_gb_state_size())
        return fail("state size mismatch");
    gb_state_save(core, (char *)out);
    struct AudioSnapshot snapshot = {0};
    snapshot.magic = 0x31555041;
    snapshot.version = 1;
    snapshot.rate = EMU_AUDIO_RATE;
    snapshot.phase = audio_phase;
    snapshot.apu = apu;
    memcpy(out + gb_get_state_size(core), &snapshot, sizeof(snapshot));
    return true;
}
bool emu_gb_state_import(const uint8_t *in, size_t size) {
    if (!core || !in || (size != emu_gb_state_size() && size != emu_gb_legacy_state_size()))
        return fail("state size mismatch");
    const size_t base_size = gb_get_state_size(core);
    struct AudioSnapshot snapshot = {0};
    const bool has_audio = size != base_size;
    if (has_audio) {
        memcpy(&snapshot, in + base_size, sizeof(snapshot));
        if (snapshot.magic != 0x31555041 || snapshot.version != 1 ||
            snapshot.rate != EMU_AUDIO_RATE || snapshot.phase >= (1u << 22))
            return fail("incompatible audio state");
    }
    // Only this port's current ABI is supported. Never invoke upstream legacy
    // upgrade code on untrusted SD bytes before checking the expected size.
    const StateHeader *header = (const void *)in;
    if (header->version != PGB_VERSION)
        return fail("incompatible state version");
    const char *error = gb_state_load(core, (const char *)in, base_size);
    if (error)
        return fail(error);
    core->direct.joypad = 0xff;
    core->direct.sound = 1;
    if (has_audio) {
        apu = snapshot.apu;
        audio_phase = snapshot.phase;
        pcm_frames = 0;
    } else
        reset_audio(); // Compatibility with the previously released mute image.
    ready = true;
    return true;
}
