#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TxtPage {
    std::string text;
    uint32_t start_offset = 0;
    uint32_t next_offset = 0;
    bool eof = false;
};

struct TxtBook {
    std::string title;

    bool open(const char *path);
    void close();
    bool refresh_layout_settings();

    TxtPage read_page(uint32_t offset);
    uint32_t previous_page_offset(uint32_t offset);
    uint32_t seek_percent(uint8_t percent);
    void save_progress(uint32_t offset, const std::vector<uint32_t> &history);
    uint32_t load_progress(std::vector<uint32_t> *history);
    void save_progress_offset(uint32_t offset);
    uint32_t load_progress_offset();
    uint32_t file_size() const { return _file_size; }

private:
    std::string _path;
    std::string _progress_path;
    uint32_t _file_size = 0;
    uint32_t _content_offset = 0;
    uint32_t _layout_sig = 0;
    int _enc = 0;

    static std::string cache_key(const char *path);
    static void ensure_progress_root();
    uint32_t layout_signature() const;
    uint32_t find_window_start(uint32_t offset, uint32_t back_bytes) const;
    uint32_t align_page_offset_near(uint32_t offset);
};
