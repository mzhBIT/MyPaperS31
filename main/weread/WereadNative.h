#pragma once

#include "weread/WereadApi.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace weread::native {

struct Session {
    std::string vid;
    std::string access_token;
    std::string refresh_token;
    bool valid() const;
};

struct LoginQr {
    std::string uuid;
    std::vector<uint8_t> image;
};

enum class Poll { Waiting, Scanned, Confirmed, Expired, Rejected, Failed };
enum class ReadStatus { Loaded, Expired, Failed };

bool enabled();
bool set_enabled(bool value);
bool load_session(Session *session);
bool save_session(const Session &session);

bool create_qr(const std::atomic_bool &cancelled, LoginQr *qr, std::string *error);
Poll poll_qr(const std::atomic_bool &cancelled, const std::string &uuid,
             bool scanned, std::string *code, std::string *error);
bool login(const std::atomic_bool &cancelled, const std::string &code, Session *session, std::string *error);
ReadStatus get_shelf(const std::atomic_bool &cancelled, Session *session,
                std::vector<Book> *books, std::string *error);

ReadStatus get_catalog(const std::atomic_bool &cancelled, Session *session,
                       const std::string &book_id, std::vector<Chapter> *chapters,
                       std::string *format, std::string *error);

}  // namespace weread::native
