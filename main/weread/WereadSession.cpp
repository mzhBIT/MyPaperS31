#include "weread/WereadSession.h"

#include <nvs.h>

namespace weread {
namespace {

constexpr const char *NVS_NAMESPACE = "weread";
constexpr const char *KEY_VID = "vid";
constexpr const char *KEY_SKEY = "skey";
constexpr const char *KEY_REFRESH = "refresh";
constexpr const char *KEY_COOKIES = "cookies";
constexpr const char *KEY_TICKET = "ticket";
constexpr const char *KEY_WRPA = "wrpa";
constexpr const char *KEY_API_KEY = "api_key";
constexpr size_t MAX_COOKIE_BYTES = 2048;
// NVS strings must fit in one entry page. The allow-list keeps the real jar
// much smaller than this, while the limit prevents a malformed header from
// making every later session save fail.
constexpr size_t MAX_COOKIE_JAR_BYTES = 3072;

bool allowed_cookie(const std::string &name)
{
    return name.rfind("wr_", 0) == 0 || name == "ptcz" || name == "RK" || name == "pgv_pvid";
}

std::string serialize_cookies(const std::map<std::string, std::string> &cookies)
{
    std::string data;
    for (const auto &[name, value] : cookies) {
        if (!allowed_cookie(name) || name.find_first_of("=\r\n") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos) continue;
        data += name;
        data.push_back('=');
        data += value;
        data.push_back('\n');
    }
    return data;
}

void deserialize_cookies(const std::string &data, std::map<std::string, std::string> *cookies)
{
    size_t start = 0;
    while (start < data.size()) {
        const size_t end = data.find('\n', start);
        const std::string line = data.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t equals = line.find('=');
        if (equals != std::string::npos) {
            const std::string name = line.substr(0, equals);
            if (allowed_cookie(name)) (*cookies)[name] = line.substr(equals + 1);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

bool read_string(nvs_handle_t handle, const char *key, std::string *value)
{
    size_t size = 0;
    if (nvs_get_str(handle, key, nullptr, &size) != ESP_OK || size <= 1 ||
        size > MAX_COOKIE_BYTES + 1) return false;
    std::string temporary(size, '\0');
    if (nvs_get_str(handle, key, temporary.data(), &size) != ESP_OK) return false;
    temporary.resize(size - 1);
    *value = std::move(temporary);
    return true;
}

esp_err_t write_or_erase(nvs_handle_t handle, const char *key, const std::string &value)
{
    if (!value.empty()) return nvs_set_str(handle, key, value.c_str());
    esp_err_t result = nvs_erase_key(handle, key);
    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}

}  // namespace

void Session::clear()
{
    vid.clear();
    skey.clear();
    refresh_token.clear();
    cookies.clear();
    wr_ticket.clear();
    wr_wrpa.clear();
    api_key.clear();
}

bool load_session(Session *session)
{
    if (!session) return false;
    session->clear();
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    read_string(handle, KEY_VID, &session->vid);
    read_string(handle, KEY_SKEY, &session->skey);
    read_string(handle, KEY_REFRESH, &session->refresh_token);
    std::string cookie_data;
    read_string(handle, KEY_COOKIES, &cookie_data);
    deserialize_cookies(cookie_data, &session->cookies);
    read_string(handle, KEY_TICKET, &session->wr_ticket);
    read_string(handle, KEY_WRPA, &session->wr_wrpa);
    read_string(handle, KEY_API_KEY, &session->api_key);
    nvs_close(handle);
    if (!session->vid.empty()) session->cookies["wr_vid"] = session->vid;
    if (!session->skey.empty()) session->cookies["wr_skey"] = session->skey;
    if (!session->refresh_token.empty()) session->cookies["wr_rt"] = session->refresh_token;
    return session->valid();
}

bool save_session(const Session &session)
{
    const std::string cookie_data = serialize_cookies(session.cookies);
    if (session.vid.size() > MAX_COOKIE_BYTES || session.skey.size() > MAX_COOKIE_BYTES ||
        session.refresh_token.size() > MAX_COOKIE_BYTES || session.wr_ticket.size() > MAX_COOKIE_BYTES ||
        session.wr_wrpa.size() > MAX_COOKIE_BYTES || session.api_key.size() > MAX_COOKIE_BYTES ||
        cookie_data.size() > MAX_COOKIE_JAR_BYTES) return false;
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = write_or_erase(handle, KEY_VID, session.vid);
    if (result == ESP_OK) result = write_or_erase(handle, KEY_SKEY, session.skey);
    if (result == ESP_OK) result = write_or_erase(handle, KEY_REFRESH, session.refresh_token);
    if (result == ESP_OK) result = write_or_erase(handle, KEY_COOKIES, cookie_data);
    if (result == ESP_OK) result = write_or_erase(handle, KEY_TICKET, session.wr_ticket);
    if (result == ESP_OK) result = write_or_erase(handle, KEY_WRPA, session.wr_wrpa);
    if (result == ESP_OK) result = write_or_erase(handle, KEY_API_KEY, session.api_key);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool clear_session()
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_erase_all(handle);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

}  // namespace weread
