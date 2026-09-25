#include "weread/WereadHttpClient.h"
#include "wifi/wifi_setting.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string_view>

#include <lwip/inet.h>
#include <lwip/netdb.h>

namespace weread {
namespace {

constexpr const char *TAG = "WEREAD_HTTP";
constexpr const char *DEFAULT_REFERER = "https://weread.qq.com/";
constexpr const char *USER_AGENT =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 "
    "Safari/537.36 Edg/135.0.0.0";
constexpr size_t INITIAL_CAPACITY = 64 * 1024;
constexpr size_t MAX_RESPONSE_BYTES = 4 * 1024 * 1024;

// The web reader may issue additional device/risk-control cookies besides the
// three credentials persisted in NVS. Keep them across the short-lived HTTP
// client instances used by shelf/catalog/chapter jobs, matching the reference
// implementation's process-wide cookie jar.
std::map<std::string, std::string> runtime_cookies;

bool allowed_cookie(const std::string &name)
{
    return name.rfind("wr_", 0) == 0 || name == "ptcz" || name == "RK" || name == "pgv_pvid";
}

bool contains_expired(const HttpResponse &response)
{
    if (!response.data() || response.size() > 4096) return false;
    const std::string body(reinterpret_cast<const char *>(response.data()), response.size());
    return body.find("-2012") != std::string::npos;
}

std::string trim_copy(std::string value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.pop_back();
    return value;
}

std::string url_host(const std::string &url)
{
    const size_t scheme = url.find("://");
    const size_t begin = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t end = url.find_first_of("/:?#", begin);
    return url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

void log_cellular_endpoint(const std::string &url, int socket_errno)
{
    const std::string host = url_host(url);
    if (host.empty()) return;
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    const int dns_result = getaddrinfo(host.c_str(), "443", &hints, &addresses);
    if (dns_result != 0 || addresses == nullptr) {
        ESP_LOGW(TAG, "4G endpoint host=%s errno=%d dns=%d", host.c_str(), socket_errno,
                 dns_result);
        if (addresses) freeaddrinfo(addresses);
        return;
    }
    unsigned index = 0;
    for (addrinfo *item = addresses; item != nullptr && index < 4; item = item->ai_next) {
        if (item->ai_family != AF_INET || item->ai_addr == nullptr) continue;
        char address[INET_ADDRSTRLEN] = {};
        const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(item->ai_addr);
        if (inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address))) {
            ESP_LOGW(TAG, "4G endpoint host=%s ipv4[%u]=%s errno=%d",
                     host.c_str(), index++, address, socket_errno);
        }
    }
    freeaddrinfo(addresses);
}

}  // namespace

struct HttpClient::Collector {
    uint8_t *data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    bool overflow = false;
    std::string set_cookie;
    std::string wr_ticket;
    std::string wr_wrpa;

    ~Collector() { reset(); }
    void reset()
    {
        if (data) heap_caps_free(data);
        data = nullptr;
        size = 0;
        capacity = 0;
        overflow = false;
        set_cookie.clear();
        wr_ticket.clear();
        wr_wrpa.clear();
    }
    bool append(const void *source, size_t bytes)
    {
        if (!bytes) return true;
        if (size > MAX_RESPONSE_BYTES || bytes > MAX_RESPONSE_BYTES - size) {
            overflow = true;
            return false;
        }
        const size_t required = size + bytes + 1;
        if (required > capacity) {
            size_t next = capacity ? capacity : INITIAL_CAPACITY;
            while (next < required && next < MAX_RESPONSE_BYTES + 1) next *= 2;
            next = std::min(next, MAX_RESPONSE_BYTES + 1);
            uint8_t *resized = static_cast<uint8_t *>(
                heap_caps_realloc(data, next, MALLOC_CAP_SPIRAM));
            if (!resized) {
                overflow = true;
                return false;
            }
            data = resized;
            capacity = next;
        }
        std::memcpy(data + size, source, bytes);
        size += bytes;
        data[size] = 0;
        return true;
    }
};

static esp_err_t http_event(esp_http_client_event_t *event)
{
    auto *collector = static_cast<HttpClient::Collector *>(event->user_data);
    if (!collector) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        collector->append(event->data, static_cast<size_t>(event->data_len));
    } else if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        std::string key(event->header_key);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == "set-cookie") {
            if (!collector->set_cookie.empty()) collector->set_cookie.push_back('\n');
            collector->set_cookie += event->header_value;
        } else if (key == "x-wr-ticket") collector->wr_ticket = event->header_value;
        else if (key == "x-wrpa-0") collector->wr_wrpa = event->header_value;
    }
    return ESP_OK;
}

HttpResponse::~HttpResponse()
{
    if (_data) heap_caps_free(_data);
}

HttpResponse::HttpResponse(HttpResponse &&other) noexcept
    : status(other.status), error(std::move(other.error)), _data(other._data), _size(other._size)
{
    other._data = nullptr;
    other._size = 0;
}

HttpResponse &HttpResponse::operator=(HttpResponse &&other) noexcept
{
    if (this != &other) {
        if (_data) heap_caps_free(_data);
        status = other.status;
        error = std::move(other.error);
        _data = other._data;
        _size = other._size;
        other._data = nullptr;
        other._size = 0;
    }
    return *this;
}

HttpClient::HttpClient()
{
    _collector = new Collector();
    load_session(&_session);
    _cookies = runtime_cookies;
    for (const auto &[name, value] : _session.cookies) _cookies[name] = value;
    set_session(_session);
}

HttpClient::~HttpClient()
{
    close();
    delete _collector;
}

bool HttpClient::ensure_client()
{
    if (_client) return true;
    const bool cellular = wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR;
    esp_http_client_config_t config = {};
    config.url = "https://weread.qq.com/";
    config.event_handler = http_event;
    config.user_data = _collector;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    // ML307A exposes a carrier NAT through RNDIS. Long-lived TLS sockets and
    // especially sockets reused while switching WeRead/CDN hosts are less
    // reliable there. Wi-Fi keeps the faster persistent-client behaviour;
    // cellular requests deliberately use short-lived connections.
    config.keep_alive_enable = !cellular;
    if (cellular) {
        // The ML307A RNDIS link currently has IPv4 connectivity plus only an
        // IPv6 link-local address. Keep Tencent TLS negotiation on the usable
        // address family and explicitly stay on the project's TLS 1.2 path.
        config.addr_type = HTTP_ADDR_TYPE_INET;
        config.tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2;
    }
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    _client = esp_http_client_init(&config);
    return _client != nullptr;
}

void HttpClient::close()
{
    if (_client) {
        esp_http_client_close(_client);
        esp_http_client_cleanup(_client);
        _client = nullptr;
    }
}

std::string HttpClient::cookie_header() const
{
    std::string cookie;
    for (const auto &[name, value] : _cookies) {
        if (value.empty() || !allowed_cookie(name)) continue;
        if (!cookie.empty()) cookie += "; ";
        cookie += name;
        cookie.push_back('=');
        cookie += value;
    }
    return cookie;
}

std::string HttpClient::cookie_names() const
{
    std::string names;
    for (const auto &[name, value] : _cookies) {
        if (value.empty() || !allowed_cookie(name)) continue;
        if (!names.empty()) names.push_back(',');
        names += name;
    }
    return names;
}

void HttpClient::set_session(const Session &session)
{
    _session = session;
    for (const auto &[name, value] : session.cookies) {
        if (allowed_cookie(name) && !value.empty()) {
            _cookies[name] = value;
            runtime_cookies[name] = value;
        }
    }
    auto update = [&](const char *name, const std::string &value) {
        if (value.empty()) {
            _cookies.erase(name);
            runtime_cookies.erase(name);
        } else {
            _cookies[name] = value;
            runtime_cookies[name] = value;
        }
    };
    update("wr_vid", session.vid);
    update("wr_skey", session.skey);
    update("wr_rt", session.refresh_token);
    _session.cookies = _cookies;
}

void HttpClient::clear_session()
{
    _session.clear();
    _cookies.clear();
    runtime_cookies.clear();
    weread::clear_session();
}

void HttpClient::absorb_cookie_headers(const std::string &headers)
{
    bool changed = false;
    size_t start = 0;
    while (start < headers.size()) {
        const size_t end = headers.find('\n', start);
        std::string line = headers.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t semicolon = line.find(';');
        if (semicolon != std::string::npos) line.resize(semicolon);
        const size_t equals = line.find('=');
        if (equals != std::string::npos) {
            const std::string name = trim_copy(line.substr(0, equals));
            const std::string value = trim_copy(line.substr(equals + 1));
            if (allowed_cookie(name)) {
                if (value.empty()) {
                    _cookies.erase(name);
                    runtime_cookies.erase(name);
                } else {
                    _cookies[name] = value;
                    runtime_cookies[name] = value;
                }
                _session.cookies = _cookies;
                changed = true;
                if (name == "wr_vid" && _session.vid != value) { _session.vid = value; changed = true; }
                else if (name == "wr_skey" && _session.skey != value) { _session.skey = value; changed = true; }
                else if (name == "wr_rt" && _session.refresh_token != value) {
                    _session.refresh_token = value;
                    changed = true;
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (changed && !save_session(_session)) ESP_LOGW(TAG, "failed to persist renewed session");
}

HttpResponse HttpClient::perform_once(const char *method, const std::string &url,
                                      const std::string *body, const std::string &referer,
                                      bool html_response, bool persist_response_cookies,
                                      bool account_headers, const std::string *bearer,
                                      bool send_cookies)
{
    HttpResponse response;
    if (_cancelled) {
        response.error = "cancelled";
        return response;
    }
    const bool cellular = wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR;
    if (cellular) close();
    if (!ensure_client()) {
        response.error = "HTTP client allocation failed";
        return response;
    }
    _collector->reset();
    esp_http_client_set_url(_client, url.c_str());
    esp_http_client_set_method(_client, std::strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(_client, "User-Agent", USER_AGENT);
    esp_http_client_set_header(_client, "Accept", html_response
        ? "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
        : "application/json, text/plain, */*");
    if (body) esp_http_client_set_header(_client, "Origin", "https://weread.qq.com");
    else esp_http_client_delete_header(_client, "Origin");
    esp_http_client_set_header(_client, "Referer", referer.empty() ? DEFAULT_REFERER : referer.c_str());
    if (cellular) esp_http_client_set_header(_client, "Connection", "close");
    else esp_http_client_delete_header(_client, "Connection");
    const std::string cookie = send_cookies ? cookie_header() : std::string();
    if (!cookie.empty()) esp_http_client_set_header(_client, "Cookie", cookie.c_str());
    else esp_http_client_delete_header(_client, "Cookie");
    if (account_headers) {
        esp_http_client_set_header(_client, "X-Vid", _session.vid.c_str());
        esp_http_client_set_header(_client, "X-Skey", _session.skey.c_str());
    } else {
        esp_http_client_delete_header(_client, "X-Vid");
        esp_http_client_delete_header(_client, "X-Skey");
    }
    if (bearer && !bearer->empty()) {
        const std::string authorization = "Bearer " + *bearer;
        esp_http_client_set_header(_client, "Authorization", authorization.c_str());
    } else {
        esp_http_client_delete_header(_client, "Authorization");
    }
    if (body) {
        esp_http_client_set_header(_client, "Content-Type", "application/json;charset=UTF-8");
        esp_http_client_set_post_field(_client, body->data(), static_cast<int>(body->size()));
    } else {
        esp_http_client_delete_header(_client, "Content-Type");
        esp_http_client_set_post_field(_client, nullptr, 0);
    }

    const esp_err_t result = esp_http_client_perform(_client);
    if (result != ESP_OK) {
        const int socket_errno = esp_http_client_get_errno(_client);
        response.error = esp_err_to_name(result);
        ESP_LOGW(TAG, "%s request failed transport=%s url=%s error=%s errno=%d",
                 method, cellular ? "4G/fresh" : "Wi-Fi/reuse", url.c_str(),
                 response.error.c_str(), socket_errno);
        if (cellular) log_cellular_endpoint(url, socket_errno);
        if (cellular) close();
        return response;
    }
    response.status = esp_http_client_get_status_code(_client);
    if (_collector->overflow) {
        response.error = "response exceeds PSRAM limit";
        return response;
    }
    if (persist_response_cookies) absorb_cookie_headers(_collector->set_cookie);
    response._data = _collector->data;
    response._size = _collector->size;
    _collector->data = nullptr;
    _collector->size = 0;
    _collector->capacity = 0;
    if (cellular) close();
    return response;
}

HttpResponse HttpClient::request(const char *method, const std::string &url,
                                 const std::string *body, const std::string &referer,
                                 bool allow_renewal, bool html_response,
                                 bool persist_response_cookies, bool account_headers,
                                 const std::string *bearer, bool send_cookies)
{
    HttpResponse response = perform_once(method, url, body, referer, html_response,
                                         persist_response_cookies, account_headers, bearer,
                                         send_cookies);
    if (!response.error.empty() && response.error != "cancelled") {
        ESP_LOGW(TAG, "request failed, rebuilding session: %s", response.error.c_str());
        close();
        response = perform_once(method, url, body, referer, html_response,
                                persist_response_cookies, account_headers, bearer,
                                send_cookies);
    }
    if (allow_renewal && response.ok() && contains_expired(response) && renew_session())
        response = request(method, url, body, referer, false, html_response,
                           persist_response_cookies, account_headers, bearer, send_cookies);
    return response;
}

HttpResponse HttpClient::get(const std::string &url, const std::string &referer)
{
    return request("GET", url, nullptr, referer, true);
}

HttpResponse HttpClient::get_account_authenticated(const std::string &url,
                                                   const std::string &referer)
{
    return request("GET", url, nullptr, referer, true, false, true, true);
}

HttpResponse HttpClient::get_html(const std::string &url, const std::string &referer)
{
    return request("GET", url, nullptr, referer, true, true);
}

HttpResponse HttpClient::post_json(const std::string &url, const std::string &body,
                                   const std::string &referer)
{
    return request("POST", url, &body, referer, true);
}

HttpResponse HttpClient::post_json_bearer(const std::string &url, const std::string &body,
                                          const std::string &bearer,
                                          const std::string &referer)
{
    return request("POST", url, &body, referer, false, false, false, false,
                   &bearer, false);
}

bool HttpClient::renew_session()
{
    if (_renewing || _session.refresh_token.empty()) return false;
    _renewing = true;
    const std::string body = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";
    HttpResponse response = request("POST", "https://weread.qq.com/web/login/renewal",
                                    &body, DEFAULT_REFERER, false, false, false);
    _renewing = false;
    if (!response.ok() || contains_expired(response) || !response.data()) return false;
    const std::string_view payload(reinterpret_cast<const char *>(response.data()), response.size());
    if (payload.find("\"succ\":1") == std::string_view::npos &&
        payload.find("\"succ\": 1") == std::string_view::npos) return false;
    absorb_cookie_headers(_collector->set_cookie);
    bool auth_changed = false;
    if (!_collector->wr_ticket.empty() && _session.wr_ticket != _collector->wr_ticket) {
        _session.wr_ticket = _collector->wr_ticket;
        auth_changed = true;
    }
    if (!_collector->wr_wrpa.empty() && _session.wr_wrpa != _collector->wr_wrpa) {
        _session.wr_wrpa = _collector->wr_wrpa;
        auth_changed = true;
    }
    if (auth_changed && !save_session(_session)) ESP_LOGW(TAG, "failed to persist renewal headers");
    return _session.valid();
}

}  // namespace weread
