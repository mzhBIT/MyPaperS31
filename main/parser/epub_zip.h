#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

class EpubZip {
public:
    struct Entry {
        uint16_t method = 0;
        uint32_t compressed_size = 0;
        uint32_t uncompressed_size = 0;
        uint32_t local_header_offset = 0;
    };

    bool open(const char *path);
    void close();
    bool has_entry(const std::string &name) const;
    bool entry_info(const std::string &name, Entry *entry) const;
    bool read_entry_to_string(const std::string &name, std::string *out, bool trailing_null = false);
    bool extract_entry_to_file(const std::string &name, const std::string &out_path, uint32_t max_size = 12 * 1024 * 1024);
    const std::string &path() const { return _path; }

private:
    std::string _path;
    std::unordered_map<std::string, Entry> _entries;

    bool load_central_directory(FILE *fp);
    bool read_entry_data_offset(FILE *fp, const Entry &entry, uint32_t *offset);
    static std::string normalize_path(const std::string &path);
};
