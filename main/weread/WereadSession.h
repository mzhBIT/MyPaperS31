#pragma once

#include <map>
#include <string>

namespace weread {

struct Session {
    std::string vid;
    std::string skey;
    std::string refresh_token;
    std::map<std::string, std::string> cookies;
    std::string wr_ticket;
    std::string wr_wrpa;
    std::string api_key;

    bool valid() const
    {
        const auto gid = cookies.find("wr_gid");
        return !vid.empty() && (!skey.empty() || (gid != cookies.end() && !gid->second.empty()));
    }
    void clear();
};

bool load_session(Session *session);
bool save_session(const Session &session);
bool clear_session();

}  // namespace weread
