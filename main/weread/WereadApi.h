#pragma once

#include "weread/WereadCrypto.h"
#include "weread/WereadHttpClient.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace weread {

inline constexpr const char *ANNOTATION_COMMENT_SEPARATOR = "\x1eWR_COMMENT\x1e";
inline constexpr const char *ANNOTATION_LIKES_SEPARATOR = "\x1eWR_LIKES\x1e";

struct Book {
    std::string id;
    std::string title;
    std::string author;
    std::string cover_url;
    std::string format;
    std::string progress_chapter_uid;
    int progress = 0;
    uint32_t update_time = 0;
};

struct Chapter {
    std::string uid;
    std::string title;
    std::string resource_url;
    int index = 0;
    int word_count = 0;
    bool paid = false;
};

enum class LoginState { Pending, Success, NeedsOtp, Expired, Failed };

struct LoginPoll {
    LoginState state = LoginState::Pending;
    std::string message;
};

enum class ChapterPayloadKind { Txt, Xhtml, EpubZip };

struct ChapterPayload {
    ChapterPayloadKind kind = ChapterPayloadKind::Xhtml;
    OwnedBuffer content;
};

struct Review {
    std::string id;
    std::string author;
    std::string content;
    int create_time = 0;
    int rating = 0;
};

struct ChapterAnnotation {
    std::string range;
    std::string popup_text;
    int comment_count = 0;
    int total_count = -1;
    int max_idx = 0;
    int64_t synckey = 0;
    bool has_more = false;
    bool comments_loaded = false;
};

struct RemoteProgress {
    std::string chapter_uid;
    uint64_t chapter_offset = 0;
    int percent = -1;
    int64_t reading_seconds = -1;
    int64_t update_time = 0;
    bool position_known = false;
};

struct ReportReceipt {
    enum class State : uint32_t { Ready, InFlight, Accepted };
    State state = State::Ready;
    uint32_t seconds = 0;
    uint64_t revision = 0;
    int64_t reading_before = -1;
    int64_t submitted_at = 0;
    std::string chapter_uid;
    uint64_t chapter_offset = 0;
    int percent = 0;
};

class Api {
public:
    explicit Api(HttpClient *client) : _client(client) {}

    bool create_login(std::string *uid, std::string *confirm_url, std::string *error);
    LoginPoll poll_login(const std::string &uid);
    bool ensure_api_key(std::string *error = nullptr);
    bool get_shelf(std::vector<Book> *books, std::string *error);
    bool get_chapters(const std::string &book_id, std::vector<Chapter> *chapters,
                      std::string *format, std::string *error);
    bool get_chapter(const std::string &book_id, const Chapter &chapter,
                     ChapterPayload *payload, std::string *error);
    bool get_book_reviews(const std::string &book_id, int list_type, int count,
                          std::vector<Review> *reviews, std::string *error);
    bool get_chapter_annotations(const std::string &book_id, const Chapter &chapter,
                                 std::vector<ChapterAnnotation> *annotations,
                                 std::string *error);
    bool get_annotation_comments(const std::string &book_id, const Chapter &chapter,
                                 ChapterAnnotation *annotation, std::string *error,
                                 int count = 8, bool append = false);
    // Establish once at actual reading start; subsequent reports keep this
    // context instead of entering a new session immediately before each rt.
    bool begin_reading(const std::string &book_id, const Chapter &chapter,
                       int percent, uint64_t offset, const std::string &summary,
                       std::string *error);
    bool upload_progress(const std::string &book_id, const Chapter &chapter,
                         int progress_percent, uint32_t active_seconds,
                         uint64_t chapter_offset, const std::string &summary,
                         std::string *error, ReportReceipt *receipt,
                         const std::function<bool(const ReportReceipt&)> &checkpoint);
    bool get_progress(const std::string &book_id, RemoteProgress *progress,
                      std::string *error);

private:
    HttpResponse gateway(const std::string &api_name, const std::string &parameters,
                         std::string *error);
    bool fetch_psvts(const std::string &book_id, const std::string &chapter_uid,
                     std::string *reader_url, std::string *psvts, std::string *error,
                     bool allow_cache);
    bool fetch_report_token(const std::string &referer, std::string *error);
    std::vector<std::pair<std::string, std::string>> reading_params(
        const std::string &book_id, const Chapter &chapter, int percent,
        uint64_t offset, const std::string &summary) const;
    HttpClient *_client = nullptr;
    std::string _psvts_book;
    std::string _psvts_chapter;
    std::string _psvts;
    std::string _pclts;
    std::string _reader_token;
    bool _reader_entered = false;
};

}  // namespace weread
