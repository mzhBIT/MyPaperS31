#pragma once

#include "weread/WereadCrypto.h"
#include "weread/WereadHttpClient.h"

#include <cstdint>
#include <string>
#include <vector>

namespace weread {

inline constexpr const char *ANNOTATION_COMMENT_SEPARATOR = "\x1eWR_COMMENT\x1e";

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
    bool upload_progress(const std::string &book_id, const Chapter &chapter,
                         int progress_percent, std::string *error);

private:
    HttpResponse gateway(const std::string &api_name, const std::string &parameters,
                         std::string *error);
    bool fetch_psvts(const std::string &book_id, const std::string &chapter_uid,
                     std::string *reader_url, std::string *psvts, std::string *error,
                     bool allow_cache);
    HttpClient *_client = nullptr;
    std::string _psvts_book;
    std::string _psvts_chapter;
    std::string _psvts;
};

}  // namespace weread
