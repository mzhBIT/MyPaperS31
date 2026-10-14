#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "activities/ReadingDays.h"

// Optional trailer appended to reflow-reader progress files. It keeps the
// bookshelf and the reader overlay on exactly the same display-progress scale.
static constexpr uint32_t READER_PROGRESS_SUMMARY_MAGIC = 0x52505331; // RPS1

struct RecentBook {
    std::string path;
    std::string type;
    std::string title;
    std::string cover_path;
};

struct RecentBookProgress {
    uint64_t reading_seconds = 0;
    int percent = 0;
    int chapter = 0;
    int chapter_count = 0;
};

// Monotonic, in-memory timing. Only finish() writes storage, on reader exit.
class BookReadingTimer {
public:
    void configure(const char *path);
    void resume();
    void pause();
    void finish();
private:
    std::string path_;
    uint64_t elapsed_us_ = 0;
    int64_t started_us_ = -1;
    int64_t started_wall_us_ = -1;
    int started_offset_minutes_ = 480;
    reading_days::History pending_days_;
};

// Old per-book totals cannot be backdated. Only newly timed, date-attributed
// sessions populate this history. Returns false when the system clock is invalid.
bool recent_reading_days(std::array<uint64_t,7> *seconds,int64_t *today);

bool recent_book_load(RecentBook *book);
size_t recent_books_load(std::vector<RecentBook> *books);
bool recent_book_save(const char *path, const char *type, const char *title = nullptr, const char *cover_path = nullptr);
bool recent_books_rename_path(const char *old_path, const char *new_path);
RecentBookProgress recent_book_progress(const RecentBook &book);
const char *recent_book_base_name(const char *path);
