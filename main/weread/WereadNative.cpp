#include "weread/WereadNative.h"

#include "weread/WereadHttpClient.h"
#include "weread/WereadJson.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <nvs.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string_view>
#include <sys/time.h>

namespace weread::native {
namespace {

constexpr const char *TAG = "WEREAD_NATIVE";
constexpr const char *API = "https://i.weread.qq.com";
// Public OAuth application identifier used for the WeRead QR ticket.
constexpr const char *APP_ID = "wxab9b71ad2b90ff34";
constexpr const char *NONCE = "weread";
constexpr const char *SCOPE = "snsapi_userinfo,snsapi_friend,snsapi_favorites";
constexpr const char *SESSION_NAMESPACE = "weread_native";
constexpr const char *SESSION_KEY = "session";
constexpr const char *PREF_NAMESPACE = "weread_pref";
// The previous preference defaulted to native, including on upgraded devices.
constexpr const char *MODE_KEY = "native_v2";
constexpr size_t MAX_TOKEN = 1024;
constexpr size_t MAX_ACCOUNT = 64;
constexpr size_t SMALL_RESPONSE = 16 * 1024;
constexpr size_t MAX_QR_IMAGE = 128 * 1024;
constexpr int REQUEST_TIMEOUT_MS = 12000;
constexpr int POLL_TIMEOUT_MS = 8000;

bool digits(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

bool token(std::string_view value)
{
    return !value.empty() && value.size() <= MAX_TOKEN &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return c >= 0x21 && c <= 0x7e && c != '"' && c != '\\';
           });
}

std::string_view response_body(const HttpResponse &response)
{
    if (!response.data()) return {};
    return {reinterpret_cast<const char *>(response.data()), response.size()};
}

bool field(std::string_view object, std::string_view key, std::string *out)
{
    std::string_view value;
    if (!json::member(object, key, &value) || value.empty()) return false;
    if (value.front() == '"') return json::string(value, out);
    if (!digits(value)) return false;
    out->assign(value);
    return true;
}

int64_t number(std::string_view object, std::string_view key, int64_t fallback = 0)
{
    std::string_view value;
    if (!json::member(object, key, &value)) return fallback;
    std::string decoded;
    if (!value.empty() && value.front() == '"') {
        if (!json::string(value, &decoded)) return fallback;
        value = decoded;
    }
    int64_t result = 0;
    return json::integer(value, &result) ? result : fallback;
}

int64_t business_code(std::string_view object)
{
    return number(object, "errcode", number(object, "errCode"));
}

bool expired(const HttpResponse &response, int64_t code)
{
    return response.error.empty() && (response.status == 401 || code == -2012 || code == -2041);
}

bool check_response(const char *stage, const HttpResponse &response, std::string *error)
{
    const int64_t code = business_code(response_body(response));
    ESP_LOGI(TAG, "stage=%s http=%d code=%lld bytes=%u", stage, response.status,
             static_cast<long long>(code), static_cast<unsigned>(response.size()));
    if (response.ok() && code == 0) return true;
    *error = std::string(stage) + (response.error.empty()
        ? " HTTP " + std::to_string(response.status) + (code ? " / " + std::to_string(code) : "")
        : " " + response.error);
    return false;
}

std::string hex_digest(const uint8_t *bytes, size_t count)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(count * 2, '0');
    for (size_t i = 0; i < count; ++i) {
        result[2 * i] = hex[bytes[i] >> 4];
        result[2 * i + 1] = hex[bytes[i] & 0xf];
    }
    return result;
}

bool sign_in(const std::atomic_bool &cancelled, const std::string &credential,
             const char *refresh_endpoint, Session *session, bool *session_expired, std::string *error)
{
    const bool refresh = refresh_endpoint != nullptr;
    *session_expired = false;
    if (!token(credential)) {
        *error = "Invalid login credential";
        return false;
    }

    timeval now{};
    gettimeofday(&now, nullptr);
    if (now.tv_sec < 1577836800) {
        *error = "Device clock is not set";
        return false;
    }
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        *error = "Device ID unavailable";
        return false;
    }
    const std::string device_id = "eink" + hex_digest(mac, sizeof(mac));
    const std::string timestamp = std::to_string(int64_t(now.tv_sec) * 1000 + now.tv_usec / 1000);
    const std::string nonce = std::to_string(esp_random() % 1000);
    const std::string signature = sha256_hex(timestamp + device_id + nonce);
    if (signature.empty()) {
        *error = "Login signature failed";
        return false;
    }

    const std::string common = ",\"deviceId\":\"" + device_id + "\",\"trackId\":\"\","
        "\"deviceName\":\"MyPaperS31\",\"timestamp\":" + timestamp +
        ",\"random\":" + nonce + ",\"signature\":\"" + signature + "\"}";
    const std::string body = refresh
        ? "{\"refreshToken\":\"" + credential + "\",\"wxToken\":0,\"inBackground\":0,"
          "\"kickType\":1,\"refCgi\":\"" + refresh_endpoint + "\"" + common
        : "{\"code\":\"" + credential + "\",\"isFromQrcode\":1,\"isAutoLogout\":0,\"deviceType\":3" + common;
    const char *stage = refresh ? "refresh" : "login";
    auto response = HttpClient::request_native(std::string(API) + "/login", stage, SMALL_RESPONSE,
                                                REQUEST_TIMEOUT_MS, cancelled, &body);
    const auto result = response_body(response);
    if (!check_response(stage, response, error)) {
        *session_expired = expired(response, business_code(result));
        return false;
    }
    Session updated;
    field(result, "vid", &updated.vid);
    field(result, "accessToken", &updated.access_token);
    field(result, "refreshToken", &updated.refresh_token);
    if (refresh && updated.refresh_token.empty()) updated.refresh_token = session->refresh_token;
    if (!updated.valid() || (refresh && updated.vid != session->vid)) {
        *error = "Invalid login response";
        return false;
    }
    *session = std::move(updated);
    return true;
}

ReadStatus request_shelf(const std::atomic_bool &cancelled, const Session &session,
                    std::vector<Book> *books, std::string *error)
{
    auto response = HttpClient::request_native(std::string(API) + "/shelf/sync?synckey=0&lectureSynckey=0",
                                                "shelf", 4 * 1024 * 1024, REQUEST_TIMEOUT_MS,
                                                cancelled, nullptr, session.vid, session.access_token, true);
    const auto root = response_body(response);
    if (!check_response("shelf", response, error)) {
        return expired(response, business_code(root)) ? ReadStatus::Expired : ReadStatus::Failed;
    }
    std::string vid;
    if (field(root, "vid", &vid) && vid != session.vid) {
        *error = "Shelf account mismatch";
        return ReadStatus::Failed;
    }

    std::string_view array;
    if (!json::member(root, "books", &array) || array.empty() || array.front() != '[') {
        *error = "Invalid shelf response";
        return ReadStatus::Failed;
    }
    std::vector<Book> loaded;
    std::map<std::string, size_t> index;
    json::ObjectIterator iterator(array);
    std::string_view object;
    while (iterator.next(&object)) {
        Book book;
        if (!field(object, "bookId", &book.id) || book.id.empty() || loaded.size() >= 10000) {
            *error = "Invalid shelf entry";
            return ReadStatus::Failed;
        }
        field(object, "title", &book.title);
        field(object, "author", &book.author);
        field(object, "cover", &book.cover_url);
        field(object, "format", &book.format);
        book.update_time = static_cast<uint32_t>(
            std::clamp<int64_t>(number(object, "readUpdateTime", number(object, "updateTime")), 0, UINT32_MAX));
        if (index.emplace(book.id, loaded.size()).second) loaded.push_back(std::move(book));
    }
    if (loaded.empty() && array.find_first_not_of(" \t\r\n", 1) != array.size() - 1) {
        *error = "Invalid shelf entries";
        return ReadStatus::Failed;
    }
    if (json::member(root, "bookProgress", &array)) {
        json::ObjectIterator progress(array);
        while (progress.next(&object)) {
            std::string id;
            if (!field(object, "bookId", &id)) continue;
            auto found = index.find(id);
            if (found == index.end()) continue;
            Book &book = loaded[found->second];
            book.progress = static_cast<int>(std::clamp<int64_t>(number(object, "progress"), 0, 100));
            field(object, "chapterUid", &book.progress_chapter_uid);
            book.update_time = static_cast<uint32_t>(
                std::clamp<int64_t>(number(object, "updateTime", book.update_time), 0, UINT32_MAX));
        }
    }
    std::stable_sort(loaded.begin(), loaded.end(), [](const Book &left, const Book &right) {
        return left.update_time > right.update_time;
    });
    *books = std::move(loaded);
    ESP_LOGI(TAG, "shelf loaded books=%u", static_cast<unsigned>(books->size()));
    return ReadStatus::Loaded;
}

std::string_view chapter_array(std::string_view object)
{
    for (const char *key : {"data", "updated"}) {
        std::string_view array;
        if (json::member(object, key, &array) && !array.empty() && array.front() == '[' &&
            array.find_first_not_of(" \t\r\n", 1) < array.size() - 1) return array;
    }
    return {};
}

ReadStatus request_catalog(const std::atomic_bool &cancelled, const Session &session,
                           const std::string &book_id, std::vector<Chapter> *chapters,
                           std::string *format, std::string *error)
{
    const std::string body = "{\"bookIds\":[\"" + book_id + "\"],\"synckeys\":[0]}";
    auto response = HttpClient::request_native(std::string(API) + "/book/chapterInfos",
                                                "chapterInfos", 4 * 1024 * 1024, REQUEST_TIMEOUT_MS,
                                                cancelled, &body, session.vid, session.access_token, true);
    const auto root = response_body(response);
    if (!check_response("chapterInfos", response, error))
        return expired(response, business_code(root)) ? ReadStatus::Expired : ReadStatus::Failed;

    json::ObjectIterator book_entries(chapter_array(root));
    std::string_view book;
    while (book_entries.next(&book)) {
        std::string id;
        std::string_view detail;
        json::member(book, "book", &detail);
        if (!field(book, "bookId", &id)) field(detail, "bookId", &id);
        if (id != book_id) continue;

        json::ObjectIterator entries(chapter_array(book));
        std::string_view entry;
        std::vector<Chapter> loaded;
        std::set<std::string> ids;
        while (entries.next(&entry)) {
            if (cancelled) return ReadStatus::Failed;
            Chapter chapter;
            const auto index = number(entry, "chapterIdx", -1);
            if (!field(entry, "chapterUid", &chapter.uid) || chapter.uid.empty() ||
                index < 0 || index > INT32_MAX || loaded.size() >= 10000 ||
                !ids.insert(chapter.uid).second) {
                *error = "Invalid native chapter entry";
                return ReadStatus::Failed;
            }
            chapter.index = static_cast<int>(index);
            field(entry, "title", &chapter.title);
            field(entry, "tar", &chapter.resource_url);
            chapter.word_count = static_cast<int>(std::clamp<int64_t>(number(entry, "wordCount"), 0, INT32_MAX));
            std::string_view paid;
            if (json::member(entry, "paid", &paid)) json::boolean(paid, &chapter.paid);
            loaded.push_back(std::move(chapter));
        }
        if (loaded.empty()) break;
        std::stable_sort(loaded.begin(), loaded.end(), [](const Chapter &lhs, const Chapter &rhs) {
            return lhs.index < rhs.index;
        });
        if (!field(book, "format", format)) field(detail, "format", format);
        *chapters = std::move(loaded);
        ESP_LOGI(TAG, "catalog loaded chapters=%u", static_cast<unsigned>(chapters->size()));
        return ReadStatus::Loaded;
    }

    *error = "book has no readable chapters";
    return ReadStatus::Failed;
}

template <class Request>
ReadStatus read_with_refresh(const std::atomic_bool &cancelled, Session *session,
                             const char *endpoint, std::string *error, Request request)
{
    if (!session->valid()) return ReadStatus::Expired;
    const auto result = request();
    if (result != ReadStatus::Expired || cancelled) return result;

    bool session_expired = false;
    if (!sign_in(cancelled, session->refresh_token, endpoint, session, &session_expired, error))
        return session_expired ? ReadStatus::Expired : ReadStatus::Failed;
    if (cancelled) return ReadStatus::Failed;
    if (!save_session(*session)) {
        *error = "Could not save native session";
        return ReadStatus::Failed;
    }
    return request();
}

}  // namespace

bool Session::valid() const
{
    return vid.size() <= MAX_ACCOUNT && digits(vid) && token(access_token) && token(refresh_token);
}

bool enabled()
{
    nvs_handle_t handle;
    if (nvs_open(PREF_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t value = 0;
    const auto result = nvs_get_u8(handle, MODE_KEY, &value);
    nvs_close(handle);
    return result == ESP_OK && value != 0;
}

bool set_enabled(bool value)
{
    nvs_handle_t handle;
    if (nvs_open(PREF_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    auto result = nvs_set_u8(handle, MODE_KEY, value ? 1 : 0);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool load_session(Session *session)
{
    *session = {};
    nvs_handle_t handle;
    if (nvs_open(SESSION_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = 0;
    auto result = nvs_get_str(handle, SESSION_KEY, nullptr, &size);
    if (result != ESP_OK || size < 6 || size > MAX_ACCOUNT + 2 * MAX_TOKEN + 3) {
        nvs_close(handle);
        return false;
    }
    std::string stored(size, '\0');
    result = nvs_get_str(handle, SESSION_KEY, stored.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) return false;
    stored.resize(size - 1);
    const auto first = stored.find('\n');
    const auto second = first == std::string::npos ? first : stored.find('\n', first + 1);
    if (second == std::string::npos) return false;
    Session loaded{stored.substr(0, first), stored.substr(first + 1, second - first - 1),
                   stored.substr(second + 1)};
    if (!loaded.valid()) return false;
    *session = std::move(loaded);
    return true;
}

bool save_session(const Session &session)
{
    if (!session.valid()) return false;
    nvs_handle_t handle;
    if (nvs_open(SESSION_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    const std::string stored = session.vid + '\n' + session.access_token + '\n' + session.refresh_token;
    auto result = nvs_set_str(handle, SESSION_KEY, stored.c_str());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool create_qr(const std::atomic_bool &cancelled, LoginQr *qr, std::string *error)
{
    std::string timestamp, signature;
    {
        auto ticket = HttpClient::request_native(std::string(API) + "/wxticket?nonceStr=" + NONCE,
                                                 "ticket", SMALL_RESPONSE, REQUEST_TIMEOUT_MS, cancelled);
        if (!check_response("ticket", ticket, error)) return false;
        const auto root = response_body(ticket);
        if (!field(root, "timeStamp", &timestamp) || !digits(timestamp) ||
            !field(root, "signature", &signature) || !token(signature)) {
            *error = "Invalid QR ticket";
            return false;
        }
    }
    const std::string url = std::string("https://open.weixin.qq.com/connect/sdk/qrconnect?appid=") +
        APP_ID + "&noncestr=" + NONCE + "&timestamp=" + url_encode(timestamp) +
        "&scope=" + url_encode(SCOPE) + "&signature=" + url_encode(signature);
    auto response = HttpClient::request_native(url, "qr", 2 * MAX_QR_IMAGE, REQUEST_TIMEOUT_MS, cancelled);
    if (!check_response("qr", response, error)) return false;

    const auto root = response_body(response);
    std::string_view image_object;
    std::string base64;
    if (!field(root, "uuid", &qr->uuid) || !token(qr->uuid) ||
        !json::member(root, "qrcode", &image_object) ||
        !field(image_object, "qrcodebase64", &base64)) {
        *error = "Invalid QR response";
        return false;
    }
    size_t bytes = 0;
    const auto *encoded = reinterpret_cast<const uint8_t *>(base64.data());
    if (mbedtls_base64_decode(nullptr, 0, &bytes, encoded, base64.size()) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL ||
        bytes < 4 || bytes > MAX_QR_IMAGE) {
        *error = "Invalid QR image";
        return false;
    }
    qr->image.resize(bytes);
    if (mbedtls_base64_decode(qr->image.data(), bytes, &bytes, encoded, base64.size()) != 0 ||
        bytes < 4 || qr->image[0] != 0xff || qr->image[1] != 0xd8) {
        qr->image.clear();
        *error = "Invalid QR image";
        return false;
    }
    qr->image.resize(bytes);
    return true;
}

Poll poll_qr(const std::atomic_bool &cancelled, const std::string &uuid,
             bool scanned, std::string *code, std::string *error)
{
    const std::string url = "https://long.open.weixin.qq.com/connect/l/qrconnect?f=json&uuid=" +
                            url_encode(uuid) + (scanned ? "&last=404" : "");
    auto response = HttpClient::request_native(url, "poll", SMALL_RESPONSE, POLL_TIMEOUT_MS, cancelled);
    if (response.timed_out) return Poll::Waiting;
    if (!response.ok()) {
        check_response("poll", response, error);
        return Poll::Failed;
    }
    const auto root = response_body(response);
    const auto status = number(root, "wx_errcode", -1);
    switch (status) {
        case 408: return Poll::Waiting;
        case 404: return Poll::Scanned;
        case 405:
            if (field(root, "wx_code", code) && token(*code)) return Poll::Confirmed;
            break;
        case 402: return Poll::Expired;
        case 403: return Poll::Rejected;
        default: break;
    }
    *error = "Invalid QR poll response " + std::to_string(status);
    return Poll::Failed;
}

bool login(const std::atomic_bool &cancelled, const std::string &code, Session *session, std::string *error)
{
    bool session_expired = false;
    return sign_in(cancelled, code, nullptr, session, &session_expired, error);
}

ReadStatus get_shelf(const std::atomic_bool &cancelled, Session *session,
                     std::vector<Book> *books, std::string *error)
{
    return read_with_refresh(cancelled, session, "/shelf/sync", error, [&] {
        return request_shelf(cancelled, *session, books, error);
    });
}

ReadStatus get_catalog(const std::atomic_bool &cancelled, Session *session,
                       const std::string &book_id, std::vector<Chapter> *chapters,
                       std::string *format, std::string *error)
{
    if (book_id.size() > 96 || !token(book_id)) {
        *error = "Invalid book ID";
        return ReadStatus::Failed;
    }
    return read_with_refresh(cancelled, session, "/book/chapterInfos", error, [&] {
        return request_catalog(cancelled, *session, book_id, chapters, format, error);
    });
}

}  // namespace weread::native
