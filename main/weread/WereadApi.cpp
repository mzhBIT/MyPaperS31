#include "weread/WereadApi.h"

#include "weread/WereadJson.h"
#include "wifi/wifi_setting.h"

#include <esp_log.h>
#include "time/network_time.h"
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <string_view>

namespace weread {
namespace {

constexpr const char *TAG = "WEREAD_API";
constexpr const char *WEB_HOST = "https://weread.qq.com";
constexpr const char *GATEWAY_HOST = "https://i.weread.qq.com/api/agent/gateway";
constexpr const char *SKILL_VERSION = "1.0.5";
constexpr const char *READER_TOKEN = "3c5c8717f3daf09iop3423zafeqoi";
constexpr const char *WEB_USER_AGENT =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 "
    "Safari/537.36 Edg/135.0.0.0";
constexpr std::time_t MIN_VALID_UNIX_TIME = 1704067200;  // 2024-01-01 UTC

bool ensure_signed_request_time(std::string *error)
{
    std::time_t now = std::time(nullptr);
    if (now >= MIN_VALID_UNIX_TIME) return true;

    ESP_LOGI(TAG, "system clock invalid (%lld), synchronizing SNTP",
             static_cast<long long>(now));
    esp_err_t result = network_time_sync(10000);
    now = std::time(nullptr);
    if (result == ESP_OK && now >= MIN_VALID_UNIX_TIME) {
        ESP_LOGI(TAG, "SNTP synchronized unix=%lld", static_cast<long long>(now));
        return true;
    }
    ESP_LOGE(TAG, "SNTP synchronization failed: %s unix=%lld",
             esp_err_to_name(result), static_cast<long long>(now));
    if (error) *error = "network time synchronization failed";
    return false;
}

std::string_view response_view(const HttpResponse &response)
{
    return {reinterpret_cast<const char *>(response.data()), response.size()};
}

bool field_view(std::string_view object, const char *name, std::string_view *value)
{
    return json::member(object, name, value);
}

bool field_string(std::string_view object, const char *name, std::string *value)
{
    std::string_view raw;
    if (!field_view(object, name, &raw)) return false;
    if (json::string(raw, value)) return true;
    int64_t number = 0;
    if (!json::integer(raw, &number)) return false;
    *value = std::to_string(number);
    return true;
}

int field_int(std::string_view object, const char *name, int fallback = 0)
{
    std::string_view raw;
    int64_t value = fallback;
    return field_view(object, name, &raw) && json::integer(raw, &value)
               ? static_cast<int>(value) : fallback;
}

int64_t field_int64(std::string_view object, const char *name, int64_t fallback = 0)
{
    std::string_view raw;
    int64_t value = fallback;
    return field_view(object, name, &raw) && json::integer(raw, &value) ? value : fallback;
}

bool field_bool(std::string_view object, const char *name, bool fallback = false)
{
    std::string_view raw;
    bool value = fallback;
    return field_view(object, name, &raw) && json::boolean(raw, &value) ? value : fallback;
}

std::string json_escape(const std::string &value)
{
    std::string output;
    output.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '"' || c == '\\') { output.push_back('\\'); output.push_back(c); }
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else output.push_back(c);
    }
    return output;
}

std::string params_json(const std::vector<std::pair<std::string, std::string>> &params)
{
    std::string body = "{";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) body.push_back(',');
        const auto &key = params[i].first;
        const auto &value = params[i].second;
        body += "\"" + key + "\":";
        // WeRead's signed chapter endpoint is type-sensitive. These fields
        // are JSON numbers in the web client; encoding them as strings makes
        // an otherwise correctly signed request return an empty object.
        const bool numeric = key == "sc" || key == "st";
        const bool boolean = key == "prevChapter";
        if (numeric || boolean) body += value;
        else body += "\"" + json_escape(value) + "\"";
    }
    body.push_back('}');
    return body;
}

struct ReaderState {
    std::string psvts;
    std::string pclts;
    std::string token;
};

bool extract_reader_state(std::string_view html, ReaderState *state)
{
    if (!state) return false;
    constexpr std::string_view marker = "window.__INITIAL_STATE__";
    size_t position = html.find(marker);
    if (position == std::string_view::npos) return false;
    position = html.find('=', position + marker.size());
    if (position == std::string_view::npos) return {};
    do { ++position; } while (position < html.size() &&
        (html[position] == ' ' || html[position] == '\t' || html[position] == '\r' || html[position] == '\n'));
    if (position >= html.size() || html[position] != '{') return false;

    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    size_t end = position;
    for (; end < html.size(); ++end) {
        const char c = html[end];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') quoted = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) { ++end; break; }
    }
    if (depth != 0) return false;

    const std::string_view initial = html.substr(position, end - position);
    std::string_view reader;
    ReaderState parsed;
    if (!json::member(initial, "reader", &reader) ||
        !field_string(reader, "psvts", &parsed.psvts) || parsed.psvts.empty()) return false;
    // pclts and token are session hints. They are optional in some reader
    // pages, in which case the Web client uses its documented fallbacks.
    field_string(reader, "pclts", &parsed.pclts);
    field_string(reader, "token", &parsed.token);
    *state = std::move(parsed);
    return true;
}

std::string utf8_prefix(std::string_view text, size_t max_chars)
{
    size_t offset = 0;
    size_t count = 0;
    while (offset < text.size() && count < max_chars) {
        const unsigned char c = static_cast<unsigned char>(text[offset]);
        size_t width = 1;
        if ((c & 0xe0) == 0xc0) width = 2;
        else if ((c & 0xf0) == 0xe0) width = 3;
        else if ((c & 0xf8) == 0xf0) width = 4;
        if (offset + width > text.size()) break;
        bool valid = true;
        for (size_t i = 1; i < width; ++i) {
            if ((static_cast<unsigned char>(text[offset + i]) & 0xc0) != 0x80) valid = false;
        }
        if (!valid) width = 1;
        offset += width;
        ++count;
    }
    return std::string(text.substr(0, offset));
}

bool is_zip(const uint8_t *data, size_t size)
{
    return data && size >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 3 && data[3] == 4;
}

bool likely_txt_metadata(const HttpResponse &response)
{
    if (!response.data() || !response.size() || response.data()[0] != '{') return false;
    return response_view(response).find("\"bookId\"") != std::string_view::npos;
}

bool empty_json_object(const HttpResponse &response)
{
    if (!response.data()) return false;
    const std::string_view body = response_view(response);
    const size_t first = body.find_first_not_of(" \t\r\n");
    const size_t last = body.find_last_not_of(" \t\r\n");
    return first != std::string_view::npos && last != std::string_view::npos &&
           body.substr(first, last - first + 1) == "{}";
}

std::string request_error(const char *operation, const HttpResponse &response)
{
    if (!response.error.empty()) return std::string(operation) + ": " + response.error;
    std::string result = std::string(operation) + " HTTP " + std::to_string(response.status);
    if (response.data() && response.size()) {
        const std::string_view root = response_view(response);
        const int code = field_int(root, "errCode", field_int(root, "errcode"));
        std::string message;
        if (!field_string(root, "errMsg", &message)) {
            if (!field_string(root, "errmsg", &message)) field_string(root, "message", &message);
        }
        if (code != 0) result += " errCode=" + std::to_string(code);
        if (!message.empty() && message != std::to_string(code)) result += " " + message;
    }
    return result;
}

// Lua's JSON encoder in the reference implementation preserves the catalog's
// numeric chapterUid type. Our compact catalog model stores every UID as text,
// so restore a canonical decimal UID to a JSON number for gateway requests.
// Non-decimal identifiers remain strings.
std::string chapter_uid_json(const std::string &uid)
{
    const bool decimal = !uid.empty() &&
        std::all_of(uid.begin(), uid.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) &&
        (uid.size() == 1 || uid.front() != '0');
    return decimal ? uid : "\"" + json_escape(uid) + "\"";
}

std::string sha256_hex(const std::string &value)
{
    uint8_t digest[32];
    mbedtls_sha256(reinterpret_cast<const uint8_t *>(value.data()), value.size(), digest, 0);
    static const char hex[] = "0123456789abcdef";
    std::string output(64, '0');
    for (size_t i = 0; i < sizeof(digest); ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 15];
    }
    return output;
}

std::string web_app_id()
{
    const std::string ua = WEB_USER_AGENT;
    std::string prefix;
    size_t start = 0;
    int count = 0;
    for (size_t i = 0; i <= ua.size() && count < 12; ++i) {
        if (i == ua.size() || ua[i] == ' ' || ua[i] == '\t') {
            if (i > start) { prefix += static_cast<char>('0' + ((i - start) % 10)); ++count; }
            start = i + 1;
        }
    }
    uint32_t hash = 0;
    for (unsigned char c : ua) hash = static_cast<uint32_t>((0x83ULL * hash + c) & 0x7fffffff);
    return "wb" + prefix + "h" + std::to_string(hash);
}

}  // namespace

bool Api::create_login(std::string *uid, std::string *confirm_url, std::string *error)
{
    if (!_client || !uid || !confirm_url) return false;
    const std::string skills_url = std::string(WEB_HOST) + "/r/weread-skills";
    HttpResponse bootstrap = _client->get_html(skills_url, std::string(WEB_HOST) + "/");
    if (!bootstrap.ok()) {
        if (error) *error = request_error("login bootstrap", bootstrap);
        return false;
    }
    ESP_LOGI(TAG, "login bootstrap complete cookies=%s", _client->cookie_names().c_str());
    HttpResponse response = _client->get(std::string(WEB_HOST) + "/api/auth/getLoginUid", skills_url);
    if (!response.ok()) {
        if (error) *error = request_error("getLoginUid", response);
        return false;
    }
    std::string_view root = response_view(response);
    if (!field_string(root, "uid", uid)) {
        std::string_view data;
        if (!field_view(root, "data", &data) || !field_string(data, "uid", uid)) {
            if (error) *error = "login response has no uid";
            return false;
        }
    }
    *confirm_url = std::string(WEB_HOST) + "/web/confirm?uid=" + *uid;
    return true;
}

LoginPoll Api::poll_login(const std::string &uid)
{
    LoginPoll result;
    if (!_client || uid.empty()) { result.state = LoginState::Failed; result.message = "invalid login uid"; return result; }
    const Session before_request = _client->session();
    HttpResponse response = _client->get(std::string(WEB_HOST) + "/api/auth/getLoginInfo?uid=" +
                                         url_encode(uid) + "&otp=");
    if (!response.ok()) { result.message = request_error("getLoginInfo", response); return result; }
    std::string_view root = response_view(response);
    std::string_view object = root;
    std::string_view data;
    if (field_view(root, "data", &data) && !data.empty() && data.front() == '{') object = data;

    bool success = field_bool(object, "succeed");
    std::string vid, token, refresh_token, logic;
    if (!field_string(object, "webLoginVid", &vid)) {
        if (!field_string(object, "vid", &vid)) field_string(object, "userVid", &vid);
    }
    field_string(object, "accessToken", &token);
    field_string(object, "refreshToken", &refresh_token);
    field_string(object, "logicCode", &logic);
    if (success && !vid.empty() && !token.empty()) {
        Session session = _client->session();
        // A bearer key belongs to the signed-in WeRead account. Never retain
        // one across a QR login, even if the cookie jar was renewed in-place.
        session.api_key.clear();
        if (session.vid.empty() || session.vid == before_request.vid) session.vid = vid;
        if (session.skey.empty() || session.skey == before_request.skey) session.skey = token;
        session.cookies["wr_vid"] = session.vid;
        session.cookies["wr_skey"] = session.skey;
        session.cookies["wr_ql"] = "0";
        if (!refresh_token.empty()) {
            session.refresh_token = url_encode(refresh_token);
            session.cookies["wr_rt"] = session.refresh_token;
        }
        _client->set_session(session);
        if (!save_session(session)) {
            result.state = LoginState::Failed;
            result.message = "failed to save login session";
        } else {
            std::string key_error;
            if (!ensure_api_key(&key_error)) {
                // The ordinary web reader remains usable without the official
                // API key, so a gateway provisioning failure must not invalidate
                // an otherwise successful login.
                ESP_LOGW(TAG, "official API key unavailable after login: %s",
                         key_error.c_str());
            }
            result.state = LoginState::Success;
        }
        return result;
    }
    if (logic == "NEED_OTP" || logic == "OTP_EXPIRED" || logic == "OTP_NOT_MATCH") {
        result.state = LoginState::NeedsOtp;
        result.message = logic;
    } else if (logic == "LOGIN_TIMEOUT") {
        result.state = LoginState::Expired;
        result.message = logic;
    } else if (!logic.empty()) {
        result.state = LoginState::Failed;
        result.message = logic;
    }
    return result;
}

bool Api::ensure_api_key(std::string *error)
{
    if (!_client) {
        if (error) *error = "HTTP client is unavailable";
        return false;
    }
    if (!_client->session().api_key.empty()) return true;
    if (!_client->session().valid()) {
        if (error) *error = "WeRead login is required";
        return false;
    }

    const std::string skills_url = std::string(WEB_HOST) + "/r/weread-skills";
    std::string key;
    std::string response_message;
    for (int attempt = 0; attempt < 3 && key.empty(); ++attempt) {
        HttpResponse response = _client->get_account_authenticated(
            std::string(WEB_HOST) + "/api/skills/apikeyGet?only_show=1", skills_url);
        if (!response.ok()) {
            response_message = request_error("API key", response);
        } else {
            const std::string_view root = response_view(response);
            if (!field_string(root, "apikey", &key)) field_string(root, "apiKey", &key);
            if (key.empty()) {
                std::string_view data;
                if (field_view(root, "data", &data)) {
                    if (!field_string(data, "apikey", &key)) field_string(data, "apiKey", &key);
                }
            }
            if (key.empty()) {
                if (!field_string(root, "errmsg", &response_message)) {
                    field_string(root, "message", &response_message);
                }
            }
        }
        if (key.empty() && attempt < 2) vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (key.empty() || key.size() > 2048) {
        if (error) *error = response_message.empty() ? "API key response has no key" : response_message;
        return false;
    }

    Session session = _client->session();
    session.api_key = std::move(key);
    _client->set_session(session);
    if (!save_session(session)) {
        session.api_key.clear();
        _client->set_session(session);
        if (error) *error = "failed to save API key";
        return false;
    }
    ESP_LOGI(TAG, "official API key stored length=%u",
             static_cast<unsigned>(_client->session().api_key.size()));
    return true;
}

bool Api::get_shelf(std::vector<Book> *books, std::string *error)
{
    if (!_client || !books) return false;
    books->clear();
    const bool cellular = wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR;
    const bool use_gateway = cellular && !_client->session().api_key.empty();
    HttpResponse response = use_gateway
                                ? gateway("/shelf/sync", {}, error)
                                : _client->get(std::string(WEB_HOST) + "/web/shelf/sync");
    if (!response.ok()) { if (error) *error = request_error("shelf", response); return false; }
    std::string_view root = response_view(response);
    // The official agent gateway may wrap the API result in a data object,
    // whereas the legacy Web endpoint returns the shelf object directly.
    std::string_view gateway_data;
    if (use_gateway && field_view(root, "data", &gateway_data) &&
        !gateway_data.empty() && gateway_data.front() == '{') {
        root = gateway_data;
    }
    ESP_LOGI(TAG, "shelf transport=%s status=%d bytes=%u",
             use_gateway ? "official-gateway/4G" : (cellular ? "web/4G" : "web/Wi-Fi"),
             response.status, static_cast<unsigned>(response.size()));
    if (field_int(root, "errcode", field_int(root, "errCode")) == -2012) {
        if (error) *error = "login expired";
        return false;
    }
    std::string_view array;
    if (!field_view(root, "books", &array)) { if (error) *error = "shelf has no books"; return false; }
    json::ObjectIterator iterator(array);
    std::string_view object;
    while (iterator.next(&object)) {
        Book book;
        field_string(object, "bookId", &book.id);
        field_string(object, "title", &book.title);
        field_string(object, "author", &book.author);
        field_string(object, "cover", &book.cover_url);
        field_string(object, "format", &book.format);
        if (!book.id.empty()) books->push_back(std::move(book));
    }

    if (field_view(root, "bookProgress", &array)) {
        json::ObjectIterator progress_iterator(array);
        while (progress_iterator.next(&object)) {
            std::string id;
            if (!field_string(object, "bookId", &id)) continue;
            auto found = std::find_if(books->begin(), books->end(), [&](const Book &book) { return book.id == id; });
            if (found == books->end()) continue;
            found->progress = field_int(object, "progress");
            field_string(object, "chapterUid", &found->progress_chapter_uid);
            found->update_time = static_cast<uint32_t>(field_int(object, "updateTime"));
        }
    }
    std::stable_sort(books->begin(), books->end(), [](const Book &lhs, const Book &rhs) {
        return lhs.update_time > rhs.update_time;
    });
    return !books->empty();
}

bool Api::get_chapters(const std::string &book_id, std::vector<Chapter> *chapters,
                       std::string *format, std::string *error)
{
    if (!_client || !chapters || book_id.empty()) return false;
    chapters->clear();
    const std::string body = "{\"bookIds\":[\"" + json_escape(book_id) + "\"]}";
    HttpResponse response = _client->post_json(std::string(WEB_HOST) + "/web/book/chapterInfos", body);
    if (!response.ok()) { if (error) *error = request_error("chapterInfos", response); return false; }
    const std::string_view root = response_view(response);
    std::string_view data;
    if (!field_view(root, "data", &data)) { if (error) *error = "chapterInfos has no data"; return false; }
    json::ObjectIterator books(data);
    std::string_view item;
    while (books.next(&item)) {
        std::string id;
        field_string(item, "bookId", &id);
        if (id.empty()) {
            std::string_view nested;
            if (field_view(item, "book", &nested)) field_string(nested, "bookId", &id);
        }
        if (id != book_id) continue;
        if (format) field_string(item, "format", format);
        std::string_view list;
        if (!field_view(item, "updated", &list)) field_view(item, "chapterInfos", &list);
        json::ObjectIterator chapter_iterator(list);
        std::string_view chapter_object;
        while (chapter_iterator.next(&chapter_object)) {
            Chapter chapter;
            field_string(chapter_object, "chapterUid", &chapter.uid);
            field_string(chapter_object, "title", &chapter.title);
            field_string(chapter_object, "tar", &chapter.resource_url);
            chapter.index = field_int(chapter_object, "chapterIdx");
            chapter.word_count = field_int(chapter_object, "wordCount");
            chapter.paid = field_bool(chapter_object, "paid");
            if (!chapter.uid.empty()) chapters->push_back(std::move(chapter));
        }
        break;
    }
    if (chapters->empty() && error) *error = "book has no readable chapters";
    return !chapters->empty();
}

bool Api::get_book_reviews(const std::string &book_id, int list_type, int count,
                           std::vector<Review> *reviews, std::string *error)
{
    if (!_client || !reviews || book_id.empty()) return false;
    reviews->clear();
    list_type = list_type == 2 ? 2 : 3;
    count = std::clamp(count, 1, 30);
    const std::string url = std::string(WEB_HOST) + "/web/review/list?bookId=" +
                            url_encode(book_id) + "&listType=" + std::to_string(list_type) +
                            "&maxIdx=0&count=" + std::to_string(count);
    HttpResponse response = _client->get(url, std::string(WEB_HOST) + "/web/bookDetail/" + book_id);
    if (!response.ok()) {
        if (error) *error = request_error("review/list", response);
        return false;
    }
    const std::string_view root = response_view(response);
    const int errcode = field_int(root, "errCode", field_int(root, "errcode"));
    if (errcode != 0) {
        if (error) *error = "review/list errCode=" + std::to_string(errcode);
        return false;
    }
    std::string_view array;
    if (!field_view(root, "reviews", &array)) {
        if (error) *error = "review/list has no reviews";
        return false;
    }
    json::ObjectIterator iterator(array);
    std::string_view outer;
    while (iterator.next(&outer)) {
        std::string_view object = outer;
        std::string_view nested;
        if (field_view(outer, "review", &nested)) object = nested;
        Review review;
        field_string(object, "reviewId", &review.id);
        field_string(object, "content", &review.content);
        review.create_time = field_int(object, "createTime");
        review.rating = field_int(object, "star");
        std::string_view author;
        if (field_view(object, "author", &author)) field_string(author, "name", &review.author);
        if (review.author.empty()) review.author = "Anonymous";
        if (!review.content.empty()) reviews->push_back(std::move(review));
    }
    ESP_LOGI(TAG, "book reviews loaded book=%s type=%d count=%u",
             book_id.c_str(), list_type, static_cast<unsigned>(reviews->size()));
    return true;
}

HttpResponse Api::gateway(const std::string &api_name, const std::string &parameters,
                          std::string *error)
{
    if (!_client || _client->session().api_key.empty()) {
        if (error) *error = "WeRead official API key is unavailable";
        return {};
    }
    std::string body = "{\"api_name\":\"" + json_escape(api_name) +
                       "\",\"skill_version\":\"" + SKILL_VERSION + "\"";
    if (!parameters.empty()) {
        body.push_back(',');
        body += parameters;
    }
    body.push_back('}');
    HttpResponse response = _client->post_json_bearer(
        GATEWAY_HOST, body, _client->session().api_key, std::string(WEB_HOST) + "/r/weread-skills");
    if (!response.ok() && error) *error = request_error(api_name.c_str(), response);
    return response;
}

bool Api::get_chapter_annotations(const std::string &book_id, const Chapter &chapter,
                                  std::vector<ChapterAnnotation> *annotations,
                                  std::string *error)
{
    if (!_client || !annotations || book_id.empty() || chapter.uid.empty()) return false;
    annotations->clear();
    if (!ensure_api_key(error)) return false;

    const std::string common = "\"bookId\":\"" + json_escape(book_id) +
                               "\",\"chapterUid\":" + chapter_uid_json(chapter.uid);
    HttpResponse underline_response = gateway("/book/underlines", common, error);
    if (!underline_response.ok()) return false;
    std::string_view underline_root = response_view(underline_response);
    std::string_view data;
    if (field_view(underline_root, "data", &data) && !data.empty() && data.front() == '{') {
        underline_root = data;
    }
    const int underline_error = field_int(underline_root, "errCode",
                                           field_int(underline_root, "errcode"));
    if (underline_error != 0) {
        if (error) *error = "underlines errCode=" + std::to_string(underline_error);
        return false;
    }
    std::string_view underline_array;
    if (!field_view(underline_root, "underlines", &underline_array)) {
        // An empty successful response is a valid chapter with no annotations.
        ESP_LOGI(TAG, "chapter annotations book=%s chapter=%s ranges=0 comments=0",
                 book_id.c_str(), chapter.uid.c_str());
        return true;
    }

    json::ObjectIterator underline_iterator(underline_array);
    std::string_view underline;
    while (underline_iterator.next(&underline)) {
        ChapterAnnotation annotation;
        field_string(underline, "range", &annotation.range);
        if (!annotation.range.empty() && annotation.range.find('-') != std::string::npos) {
            annotations->push_back(std::move(annotation));
        }
    }
    if (annotations->empty()) return true;

    // Public comments are intentionally not downloaded for every underline
    // here. A heavily annotated chapter can contain hundreds of ranges, which
    // turns opening one chapter into dozens of gateway calls and trips the
    // official API's request-frequency limit. Comments are fetched and cached
    // for one range when the reader actually taps that underline.
    ESP_LOGI(TAG, "chapter annotations book=%s chapter=%s ranges=%u comments=deferred",
             book_id.c_str(), chapter.uid.c_str(),
             static_cast<unsigned>(annotations->size()));
    return true;
}

bool Api::get_annotation_comments(const std::string &book_id, const Chapter &chapter,
                                  ChapterAnnotation *annotation, std::string *error,
                                  int count, bool append)
{
    if (!_client || !annotation || book_id.empty() || chapter.uid.empty() ||
        annotation->range.empty()) return false;
    count = std::clamp(count, 1, 10);
    if (!append) {
        annotation->popup_text.clear();
        annotation->comment_count = 0;
        annotation->total_count = -1;
        annotation->max_idx = 0;
        annotation->synckey = 0;
        annotation->has_more = false;
        annotation->comments_loaded = false;
    }
    if (!ensure_api_key(error)) return false;

    const std::string common = "\"bookId\":\"" + json_escape(book_id) +
                               "\",\"chapterUid\":" + chapter_uid_json(chapter.uid);
    const std::string request = common + ",\"reviews\":[{\"range\":\"" +
        json_escape(annotation->range) +
        "\",\"maxIdx\":" + std::to_string(annotation->max_idx) +
        ",\"count\":" + std::to_string(count) +
        ",\"synckey\":" + std::to_string(annotation->synckey) + "}]";
    HttpResponse response = gateway("/book/readreviews", request, error);
    if (!response.ok()) return false;

    std::string_view root = response_view(response);
    std::string_view data;
    if (field_view(root, "data", &data) && !data.empty() && data.front() == '{') root = data;
    const int response_error = field_int(root, "errCode", field_int(root, "errcode"));
    if (response_error != 0) {
        if (error) *error = "readreviews errCode=" + std::to_string(response_error);
        return false;
    }

    bool matched_range = false;
    bool saw_page_reviews = false;
    int added = 0;
    std::string_view ranges_array;
    if (field_view(root, "reviews", &ranges_array)) {
        json::ObjectIterator range_iterator(ranges_array);
        std::string_view range_review;
        while (range_iterator.next(&range_review)) {
            std::string range;
            field_string(range_review, "range", &range);
            if (!range.empty() && range != annotation->range) continue;
            matched_range = true;
            annotation->total_count = field_int(range_review, "totalCount", annotation->total_count);
            annotation->has_more = field_bool(range_review, "hasMore", false);
            const int previous_max_idx = annotation->max_idx;
            annotation->max_idx = field_int(range_review, "maxIdx", annotation->max_idx);
            annotation->synckey = field_int64(range_review, "synckey", annotation->synckey);
            std::string_view pages;
            if (!field_view(range_review, "pageReviews", &pages)) continue;
            saw_page_reviews = true;
            json::ObjectIterator page_iterator(pages);
            std::string_view page_review;
            while (page_iterator.next(&page_review)) {
                std::string_view review;
                if (!field_view(page_review, "review", &review)) review = page_review;
                std::string author = "Anonymous";
                std::string_view author_object;
                if (field_view(review, "author", &author_object)) {
                    if (!field_string(author_object, "nick", &author)) {
                        field_string(author_object, "name", &author);
                    }
                }
                std::string content;
                field_string(review, "content", &content);
                if (content.empty()) continue;
                if (!annotation->popup_text.empty()) {
                    annotation->popup_text += ANNOTATION_COMMENT_SEPARATOR;
                }
                annotation->popup_text += author + "\n" + content;
                const int likes = field_int(page_review, "likesCount");
                if (likes > 0) annotation->popup_text += ANNOTATION_LIKES_SEPARATOR + std::to_string(likes);
                ++annotation->comment_count;
                ++added;
            }
            if (added > 0 && annotation->max_idx == previous_max_idx) {
                annotation->max_idx = previous_max_idx + added;
            }
        }
    }

    if (!matched_range) {
        if (error) *error = "readreviews response has no matching range";
        return false;
    }
    if (annotation->total_count >= 0) {
        annotation->has_more = annotation->has_more ||
                               annotation->comment_count < annotation->total_count;
    }
    if (annotation->total_count == 0) {
        annotation->popup_text = "No public comments for this underline.";
        annotation->has_more = false;
    } else if (!saw_page_reviews || (added == 0 && annotation->popup_text.empty())) {
        if (error) *error = "readreviews returned an unrecognized or empty comment page";
        return false;
    }
    annotation->comments_loaded = true;
    ESP_LOGI(TAG,
             "annotation comments chapter=%s range=%s added=%d loaded=%d total=%d more=%d max_idx=%d synckey=%lld",
             chapter.uid.c_str(), annotation->range.c_str(), added,
             annotation->comment_count, annotation->total_count,
             annotation->has_more ? 1 : 0, annotation->max_idx,
             static_cast<long long>(annotation->synckey));
    return true;
}

bool Api::fetch_psvts(const std::string &book_id, const std::string &chapter_uid,
                      std::string *reader_url, std::string *psvts, std::string *error,
                      bool allow_cache)
{
    *reader_url = std::string(WEB_HOST) + "/web/reader/" + encode_id(book_id);
    if (!chapter_uid.empty()) *reader_url += "k" + encode_id(chapter_uid);
    if (allow_cache && _psvts_book == book_id && _psvts_chapter == chapter_uid && !_psvts.empty()) {
        *psvts = _psvts;
        return true;
    }
    HttpResponse response = _client->get(*reader_url, *reader_url);
    if (!response.ok()) { if (error) *error = request_error("reader", response); return false; }
    ReaderState state;
    if (!extract_reader_state(response_view(response), &state)) {
        if (error) *error = "reader page has no psvts";
        return false;
    }
    *psvts = state.psvts;
    if (psvts->empty()) { if (error) *error = "reader page has no psvts"; return false; }
    if (*psvts == "dangerMode") {
        ESP_LOGW(TAG, "reader page entered dangerMode");
        // A shelf-only legacy login can remain valid while the Web Reader
        // rejects it. Drop that incomplete session so the next entry runs the
        // full browser-style bootstrap and QR login flow.
        _client->clear_session();
        if (error) *error = "Web Reader login is incomplete (dangerMode). Reopen WeRead and scan again.";
        return false;
    }
    _psvts_book = book_id;
    _psvts_chapter = chapter_uid;
    _psvts = *psvts;
    _pclts = std::move(state.pclts);
    _reader_token = std::move(state.token);
    _reader_entered = false;
    return true;
}

bool Api::get_chapter(const std::string &book_id, const Chapter &chapter,
                      ChapterPayload *payload, std::string *error)
{
    if (!_client || !payload) return false;
    if (!ensure_signed_request_time(error)) return false;
    std::string reader, psvts;
    if (!fetch_psvts(book_id, chapter.uid, &reader, &psvts, error, false)) return false;
    auto request_shard = [&](const char *path) {
        const int64_t request_time = static_cast<int64_t>(std::time(nullptr));
        const auto params = make_content_params(book_id, chapter.uid, psvts, request_time, esp_random());
        const std::string body = params_json(params);
        HttpResponse response = _client->post_json(std::string(WEB_HOST) + path, body, reader);
        std::string signature;
        for (const auto &param : params) {
            if (param.first == "s") { signature = param.second; break; }
        }
        ESP_LOGI(TAG,
                 "chapter shard path=%s status=%d bytes=%u unix=%lld psvts=%u body=%u sig=%s",
                 path, response.status, static_cast<unsigned>(response.size()),
                 static_cast<long long>(request_time), static_cast<unsigned>(psvts.size()),
                 static_cast<unsigned>(body.size()), signature.c_str());
        if (std::strcmp(path, "/web/book/chapter/e_0") == 0 &&
            response.ok() && empty_json_object(response)) {
            ESP_LOGW(TAG, "e_0 request reader=%s cookies=%s body=%s",
                     reader.c_str(), _client->cookie_names().c_str(), body.c_str());
        }
        return response;
    };

    HttpResponse part0 = request_shard("/web/book/chapter/e_0");
    if (!part0.ok() || empty_json_object(part0)) {
        _psvts.clear();
        _psvts_book.clear();
        _psvts_chapter.clear();
        if (!fetch_psvts(book_id, chapter.uid, &reader, &psvts, error, false)) return false;
        part0 = request_shard("/web/book/chapter/e_0");
    }
    if (!part0.ok()) { if (error) *error = request_error("e_0", part0); return false; }
    if (empty_json_object(part0)) {
        HttpResponse session_probe = _client->get(std::string(WEB_HOST) + "/web/shelf/sync");
        bool expired = !session_probe.ok();
        if (session_probe.ok()) {
            const std::string_view root = response_view(session_probe);
            expired = field_int(root, "errcode", field_int(root, "errCode")) == -2012;
        }
        if (expired) {
            _client->clear_session();
            if (error) *error = "Login session expired. Tap to scan the QR code again.";
            ESP_LOGW(TAG, "e_0 rejected and session validation failed");
        } else {
            if (error) {
                *error = "Chapter request was rejected although the login session is valid. "
                         "See serial diagnostics.";
            }
            ESP_LOGW(TAG, "e_0 rejected but shelf session remains valid");
        }
        return false;
    }
    if (is_zip(part0.data(), part0.size())) {
        payload->kind = ChapterPayloadKind::EpubZip;
        if (!payload->content.copy_from(part0.data(), part0.size())) {
            if (error) *error = "EPUB PSRAM allocation failed";
            return false;
        }
        return true;
    }

    if (likely_txt_metadata(part0)) {
        HttpResponse text0 = request_shard("/web/book/chapter/t_0");
        HttpResponse text1 = request_shard("/web/book/chapter/t_1");
        if (!text0.ok() || !text1.ok()) {
            if (error) *error = !text0.ok() ? request_error("t_0", text0) : request_error("t_1", text1);
            return false;
        }
        payload->kind = ChapterPayloadKind::Txt;
        return decode_content_shards(text0.data(), text0.size(), text1.data(), text1.size(),
                                     nullptr, 0, &payload->content, error);
    }

    HttpResponse part1 = request_shard("/web/book/chapter/e_1");
    HttpResponse part3 = request_shard("/web/book/chapter/e_3");
    if (!part1.ok() || !part3.ok()) {
        if (error) *error = !part1.ok() ? request_error("e_1", part1) : request_error("e_3", part3);
        return false;
    }
    payload->kind = ChapterPayloadKind::Xhtml;
    if (!decode_content_shards(part0.data(), part0.size(), part1.data(), part1.size(),
                               part3.data(), part3.size(), &payload->content, error)) return false;
    if (is_zip(payload->content.data(), payload->content.size())) payload->kind = ChapterPayloadKind::EpubZip;
    return true;
}

bool Api::upload_progress(const std::string &book_id, const Chapter &chapter,
                          int progress_percent, uint32_t active_seconds,
                          uint64_t chapter_offset, const std::string &summary,
                          std::string *error)
{
    if (!_client || book_id.empty() || chapter.uid.empty()) return false;
    if (!ensure_signed_request_time(error)) return false;
    std::string reader, psvts;
    if (!fetch_psvts(book_id, chapter.uid, &reader, &psvts, error, false)) return false;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const std::string pclts = _pclts.empty() || _pclts == "0"
        ? encode_id(std::to_string(now)) : _pclts;
    const std::string text = utf8_prefix(summary, 20);
    std::vector<std::pair<std::string, std::string>> base = {
        {"appId", web_app_id()}, {"b", encode_id(book_id)}, {"c", encode_id(chapter.uid)},
        {"ci", std::to_string(chapter.index)}, {"co", std::to_string(chapter_offset)},
        {"sm", text}, {"pr", std::to_string(std::clamp(progress_percent, 0, 100))},
        {"ct", std::to_string(now)}, {"ps", psvts}, {"pc", pclts},
    };
    auto post_read = [&](std::vector<std::pair<std::string, std::string>> params,
                         const char *stage) -> bool {
        params.push_back({"s", sign_query(sorted_query(params))});
        std::string body = "{";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) body.push_back(',');
            body += "\"" + params[i].first + "\":";
            const std::string &key = params[i].first;
            const bool numeric = key == "ci" || key == "co" || key == "pr" || key == "rt" ||
                                 key == "ts" || key == "rn" || key == "ct";
            body += numeric ? params[i].second : "\"" + json_escape(params[i].second) + "\"";
        }
        body.push_back('}');
        HttpResponse response = _client->post_json(std::string(WEB_HOST) + "/web/book/read", body, reader);
        if (!response.ok()) { if (error) *error = request_error(stage, response); return false; }
        const std::string_view root = response_view(response);
        const int success = field_int(root, "succ", 0);
        ESP_LOGI(TAG, "reader %s: succ=%d bytes=%u offset=%llu progress=%d rt=%u",
                 stage, success, static_cast<unsigned>(response.size()),
                 static_cast<unsigned long long>(chapter_offset), progress_percent,
                 static_cast<unsigned>(active_seconds));
        if (success != 1) {
            if (error) *error = std::string(stage) + " update rejected";
            return false;
        }
        return true;
    };
    if (!_reader_entered) {
        if (!post_read(base, "enter")) return false;
        _reader_entered = true;
    }
    const uint32_t random = esp_random() % 1000;
    const std::string timestamp = std::to_string(now * 1000 + random);
    const std::string random_text = std::to_string(esp_random() % 1000);
    std::vector<std::pair<std::string, std::string>> params = base;
    params.push_back({"rt", std::to_string(active_seconds)});
    params.push_back({"ts", timestamp});
    params.push_back({"rn", random_text});
    params.push_back({"sg", sha256_hex(timestamp + random_text +
                                         (_reader_token.empty() ? READER_TOKEN : _reader_token))});
    if (!post_read(std::move(params), "report")) return false;
    return true;
}

}  // namespace weread
