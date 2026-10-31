#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace emulator {
struct RomEntry {
    std::string name, path;
    bool directory = false;
};
class RomStorage {
  public:
    ~RomStorage();
    bool mount();
    void unmount();
    bool list(const std::string &directory, std::vector<RomEntry> &entries);
    bool read_rom(const std::string &path, uint8_t *&data, size_t &size, uint32_t &crc);
    bool write_save(const std::string &rom, uint32_t rom_crc, uint32_t kind, const uint8_t *data,
                    size_t size);
    bool read_save(const std::string &rom, uint32_t rom_crc, uint32_t kind, uint8_t *data,
                   size_t size, bool &found);
    const std::string &error() const {
        return error_;
    }
    static uint32_t checksum(const uint8_t *data, size_t size);

  private:
    bool fail(const std::string &error) {
        error_ = error;
        return false;
    }
    void *card_ = nullptr;
    std::string error_;
};
} // namespace emulator
