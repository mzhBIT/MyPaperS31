#pragma once
#include "weread/WereadApi.h"
#include "weread/WereadJson.h"
#include <algorithm>
#include <string_view>

namespace weread::report {
inline std::string_view object(std::string_view root) {
    std::string_view data;
    if (json::member(root, "data", &data) && !data.empty() && data.front() == '{') return data;
    return root;
}
inline int64_t number(std::string_view root, const char* key, int64_t fallback = -1) {
    std::string_view raw; int64_t n;
    return json::member(root, key, &raw) && json::integer(raw, &n) ? n : fallback;
}
inline int error_code(std::string_view root) {
    const auto data = object(root);
    for (auto value : {root, data})
        for (const char* key : {"errCode", "errcode", "errorCode"}) {
            const auto n = number(value, key, 0);
            if (n) return int(n);
        }
    return 0;
}
inline bool accepted(std::string_view root) {
    if (error_code(root)) return false;
    const auto data = object(root);
    for (auto value : {root, data}) {
        std::string_view raw; bool yes = false;
        if (json::member(value, "succ", &raw) &&
            (number(value, "succ", 0) == 1 || (json::boolean(raw, &yes) && yes))) return true;
        // Progress endpoints may acknowledge with a synchronization cursor.
        int64_t cursor = -1;
        if (json::member(value, "synckey", &raw) && json::integer(raw, &cursor) && cursor >= 0) return true;
    }
    return false; // HTTP 200 / {} is never an acknowledgement.
}
inline bool confirmed(const ReportReceipt& receipt, const RemoteProgress& remote) {
    if (receipt.state == ReportReceipt::State::Ready) return false;
    const bool exact_position = remote.position_known && remote.chapter_uid == receipt.chapter_uid &&
                                remote.chapter_offset == receipt.chapter_offset;
    // A later cloud position supersedes this accepted checkpoint. Do not keep
    // replaying an old position just because the user has since read elsewhere.
    const bool superseded = receipt.state == ReportReceipt::State::Accepted &&
                            remote.position_known && remote.update_time > receipt.submitted_at;
    const bool position = exact_position || superseded;
    const bool time = receipt.seconds == 0 ||
        (receipt.reading_before >= 0 && remote.reading_seconds >= receipt.reading_before &&
         remote.reading_seconds - receipt.reading_before >= receipt.seconds);
    return position && time;
}
inline int weighted_percent(const std::vector<Chapter>& chapters, size_t spine,
                            size_t page, size_t pages) {
    if (chapters.empty() || spine >= chapters.size()) return -1;
    uint64_t total = 0, before = 0;
    for (size_t i=0; i<chapters.size(); ++i) {
        if (chapters[i].word_count < 0) return -1;
        const auto words = uint64_t(chapters[i].word_count);
        total += words;
        if (i < spine) before += words;
    }
    if (!total) return -1; // Unknown is not permission to upload a guessed value.
    const double within = pages ? double(std::min(page, pages-1))/double(pages) : 0;
    return std::clamp(int(100.0*(double(before)+chapters[spine].word_count*within)/double(total)),0,100);
}
}
