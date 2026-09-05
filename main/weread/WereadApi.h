#pragma once

#include "weread/WereadCrypto.h"
#include "weread/WereadHttpClient.h"

#include <cstdint>
#include <string>
#include <vector>

namespace weread {

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

class Api {
public:
    explicit Api(HttpClient *client) : _client(client) {}

    bool create_login(std::string *uid, std::string *confirm_url, std::string *error);
    LoginPoll poll_login(const std::string &uid);
    bool get_shelf(std::vector<Book> *books, std::string *error);
    bool get_chapters(const std::string &book_id, std::vector<Chapter> *chapters,
                      std::string *format, std::string *error);
    bool get_chapter(const std::string &book_id, const Chapter &chapter,
                     ChapterPayload *payload, std::string *error);
    bool upload_progress(const std::string &book_id, const Chapter &chapter,
                         int progress_percent, std::string *error);

private:
    bool fetch_psvts(const std::string &book_id, const std::string &chapter_uid,
                     std::string *reader_url, std::string *psvts, std::string *error,
                     bool allow_cache);
    HttpClient *_client = nullptr;
    std::string _psvts_book;
    std::string _psvts_chapter;
    std::string _psvts;
};

}  // namespace weread
