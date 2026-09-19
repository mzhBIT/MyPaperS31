#pragma once

#include "weread/WereadApi.h"

#include <string>
#include <vector>

namespace weread::store {

struct PendingProgress {
    std::string book_id;
    Chapter chapter;
    int percent = 0;
    // Monotonic, foreground reading time accumulated locally since the last
    // successful report. This deliberately has no dependency on wall-clock
    // time or an RTC.
    uint32_t active_seconds = 0;
    uint64_t chapter_offset = 0;
    std::string summary;
};

bool ensure_directories();
bool load_shelf(std::vector<Book> *books);
bool save_shelf(const std::vector<Book> &books);
bool load_chapters(const std::string &book_id, std::vector<Chapter> *chapters,
                   std::string *format);
bool save_chapters(const std::string &book_id, const std::vector<Chapter> &chapters,
                   const std::string &format);
std::string manifest_path(const std::string &book_id);
bool save_manifest(const Book &book, const std::vector<Chapter> &chapters,
                   const std::string &format);
bool load_manifest(const std::string &path, Book *book, std::vector<Chapter> *chapters,
                   std::string *format);

std::string book_directory(const std::string &book_id);
std::string chapter_path(const std::string &book_id, const Chapter &chapter,
                         ChapterPayloadKind kind);
bool chapter_cached(const std::string &book_id, const Chapter &chapter,
                    ChapterPayloadKind *kind = nullptr, std::string *path = nullptr);
bool save_chapter(const std::string &book_id, const Chapter &chapter,
                  const ChapterPayload &payload, std::string *path = nullptr);
bool save_annotations(const std::string &book_id, const Chapter &chapter,
                      const std::vector<ChapterAnnotation> &annotations);
bool load_annotations(const std::string &book_id, const Chapter &chapter,
                      std::vector<ChapterAnnotation> *annotations);
bool annotations_cached(const std::string &book_id, const Chapter &chapter);
bool annotation_text(const std::string &book_id, const Chapter &chapter,
                     const std::string &range, std::string *text);
bool load_annotation(const std::string &book_id, const Chapter &chapter,
                     const std::string &range, ChapterAnnotation *annotation);
bool update_annotation(const std::string &book_id, const Chapter &chapter,
                       const ChapterAnnotation &annotation);
bool public_comments_enabled();
bool set_public_comments_enabled(bool enabled);

std::string cover_path(const std::string &url);
std::string epub_payload_path(const std::string &book_id);
bool save_response_file(const std::string &path, const uint8_t *data, size_t size);
bool save_pending_progress(const PendingProgress &progress);
bool load_pending_progress(PendingProgress *progress);
void clear_pending_progress();

}  // namespace weread::store
