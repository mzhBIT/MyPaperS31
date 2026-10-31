#include "RomStorage.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "boards/MyPaperS31/StoragePins.h"
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace emulator {
namespace {
constexpr size_t MaxRomBytes = 4u * 1024 * 1024, MaxSaveBytes = 512u * 1024;
constexpr uint32_t SaveVersion = 1;
struct SaveHeader {
    uint32_t magic, version, rom_crc, kind, size, crc, reserved[2];
};
static_assert(sizeof(SaveHeader) == 32);
std::string save_path(const std::string &rom, uint32_t kind) {
    return rom + (kind == 1 ? ".s31.sav" : ".s31.state");
}
bool exists(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}
bool read_blob(const std::string &path, uint32_t rom_crc, uint32_t kind, uint8_t *data,
               size_t size) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    SaveHeader h{};
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) && h.magic == 0x31534247 &&
              h.version == SaveVersion && h.rom_crc == rom_crc && h.kind == kind && h.size == size;
    if (ok)
        ok = fread(data, 1, size, f) == size && fgetc(f) == EOF && !ferror(f) &&
             RomStorage::checksum(data, size) == h.crc;
    if (fclose(f) != 0)
        ok = false;
    return ok;
}
} // namespace
RomStorage::~RomStorage() {
    unmount();
}
bool RomStorage::mount() {
#ifdef ESP_PLATFORM
    if (card_)
        return true;
    for (int pin : {MYPAPER_S31_SD_CLK, MYPAPER_S31_SD_CMD, MYPAPER_S31_SD_D0, MYPAPER_S31_SD_D1,
                    MYPAPER_S31_SD_D2, MYPAPER_S31_SD_D3})
        gpio_hold_dis(gpio_num_t(pin));
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.io_voltage = 3.3f;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = gpio_num_t(MYPAPER_S31_SD_CLK);
    slot.cmd = gpio_num_t(MYPAPER_S31_SD_CMD);
    slot.d0 = gpio_num_t(MYPAPER_S31_SD_D0);
    slot.d1 = gpio_num_t(MYPAPER_S31_SD_D1);
    slot.d2 = gpio_num_t(MYPAPER_S31_SD_D2);
    slot.d3 = gpio_num_t(MYPAPER_S31_SD_D3);
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t config{};
    config.format_if_mount_failed = false;
    config.max_files = 6;
    config.allocation_unit_size = 16384;
    sdmmc_card_t *card = nullptr;
    const esp_err_t result = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &config, &card);
    if (result != ESP_OK)
        return fail(std::string("SD mount failed: ") + esp_err_to_name(result));
    card_ = card;
    if (mkdir("/sdcard/roms", 0775) != 0 && errno != EEXIST)
        return fail("Cannot create /sdcard/roms");
#endif
    return true;
}
void RomStorage::unmount() {
#ifdef ESP_PLATFORM
    if (card_)
        esp_vfs_fat_sdcard_unmount("/sdcard", static_cast<sdmmc_card_t *>(card_));
#endif
    card_ = nullptr;
}
bool RomStorage::list(const std::string &directory, std::vector<RomEntry> &entries) {
    entries.clear();
    DIR *d = opendir(directory.c_str());
    if (!d)
        return fail("Cannot read ROM directory");
    bool ok = true;
    struct dirent *entry;
    size_t budget = 0;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        RomEntry item;
        item.name = entry->d_name;
        item.path = directory + "/" + item.name;
        struct stat st{};
        if (stat(item.path.c_str(), &st) != 0)
            continue;
        item.directory = S_ISDIR(st.st_mode);
        const char *ext = strrchr(item.name.c_str(), '.');
        if (!item.directory) {
            if (!S_ISREG(st.st_mode) || !ext)
                continue;
            std::string suffix = ext;
            for (char &c : suffix)
                if (c >= 'A' && c <= 'Z')
                    c += 32;
            if (suffix != ".gb" && suffix != ".gbc")
                continue;
        }
        budget += item.name.size() + item.path.size() + sizeof(RomEntry);
        if (entries.size() >= 1024 || budget > 256u * 1024) {
            ok = fail("Directory exceeds 1024 entries / 256 KiB; split into folders");
            break;
        }
        entries.push_back(std::move(item));
    }
    closedir(d);
    if (!ok) {
        entries.clear();
        return false;
    }
    std::sort(entries.begin(), entries.end(), [](const RomEntry &a, const RomEntry &b) {
        return a.directory != b.directory ? a.directory > b.directory : a.name < b.name;
    });
    return true;
}
uint32_t RomStorage::checksum(const uint8_t *data, size_t size) {
    static const auto table = []() {
        struct Table {
            uint32_t value[256];
        } t{};
        for (unsigned i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c >> 1) ^ (0xedb88320u & -(c & 1));
            t.value[i] = c;
        }
        return t;
    }();
    uint32_t crc = ~0u;
    for (size_t i = 0; i < size; ++i)
        crc = (crc >> 8) ^ table.value[(crc ^ data[i]) & 255];
    return ~crc;
}
bool RomStorage::read_rom(const std::string &path, uint8_t *&data, size_t &size, uint32_t &crc) {
    data = nullptr;
    size = 0;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 32768 ||
        uint64_t(st.st_size) > MaxRomBytes)
        return fail("ROM must be 32 KiB to 4 MiB");
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return fail("Cannot open ROM");
    const size_t length = st.st_size;
#ifdef ESP_PLATFORM
    data = static_cast<uint8_t *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    data = static_cast<uint8_t *>(malloc(length));
#endif
    if (!data) {
        fclose(f);
        return fail("Not enough PSRAM for ROM");
    }
    size_t done = 0;
    while (done < length) {
        const size_t bytes = std::min(length - done, size_t(65536));
        if (fread(data + done, 1, bytes, f) != bytes)
            break;
        done += bytes;
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#endif
    }
    const bool ok = done == length && !ferror(f);
    const int close = fclose(f);
    if (!ok || close != 0) {
        free(data);
        data = nullptr;
        return fail("ROM read failed");
    }
    size = length;
    crc = checksum(data, size);
    return true;
}
bool RomStorage::write_save(const std::string &rom, uint32_t rom_crc, uint32_t kind,
                            const uint8_t *data, size_t size) {
    if (!data || !size || size > MaxSaveBytes || (kind != 1 && kind != 2))
        return fail("Invalid save payload");
    const std::string path = save_path(rom, kind), temp = path + ".tmp", backup = path + ".bak";
    FILE *f = fopen(temp.c_str(), "wb");
    if (!f)
        return fail("Cannot write save file");
    const SaveHeader header{0x31534247,     SaveVersion,          rom_crc, kind,
                            uint32_t(size), checksum(data, size), {0, 0}};
    bool ok =
        fwrite(&header, 1, sizeof(header), f) == sizeof(header) && fwrite(data, 1, size, f) == size;
    if (fflush(f) != 0)
        ok = false;
    if (fsync(fileno(f)) != 0)
        ok = false;
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        remove(temp.c_str());
        return fail("Save write/flush failed");
    }
    bool moved = false;
    if (exists(path)) {
        // Keep a recovery generation until the new, flushed file is in place.
        bool preserve_backup = false;
        if (exists(backup)) {
            auto *probe = static_cast<uint8_t *>(malloc(size));
            if (!probe)
                return fail("Cannot validate save generation");
            preserve_backup = !read_blob(path, rom_crc, kind, probe, size);
            free(probe);
        }
        if (preserve_backup) {
            // A recovered backup may be the only good previous generation.
            // Never replace it with a known damaged primary.
            if (remove(path.c_str()) != 0)
                return fail("Cannot replace damaged primary save");
        } else {
            if (exists(backup) && remove(backup.c_str()) != 0)
                return fail("Cannot rotate save backup");
            if (rename(path.c_str(), backup.c_str()) != 0)
                return fail("Cannot back up previous save");
            moved = true;
        }
    }
    if (rename(temp.c_str(), path.c_str()) != 0) {
        if (moved)
            rename(backup.c_str(), path.c_str());
        return fail("Cannot install new save");
    }
    // Retain backup for recovery after an interrupted FAT directory update.
    return true;
}
bool RomStorage::read_save(const std::string &rom, uint32_t rom_crc, uint32_t kind, uint8_t *data,
                           size_t size, bool &found) {
    found = false;
    if (!data || !size || size > MaxSaveBytes || (kind != 1 && kind != 2))
        return fail("Invalid save size");
    const std::string path = save_path(rom, kind), backup = path + ".bak", temp = path + ".tmp";
    found = exists(path) || exists(backup) || exists(temp);
    if (!found)
        return true;
    if (read_blob(path, rom_crc, kind, data, size) ||
        read_blob(backup, rom_crc, kind, data, size) || read_blob(temp, rom_crc, kind, data, size))
        return true;
    return fail("Save is damaged, incompatible, or belongs to another ROM");
}
} // namespace emulator
