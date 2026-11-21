#include "weread/WereadHttpClient.h"
#include "weread/WereadJson.h"
#include <mutex>
#include "weread/WereadOffline.h"
#include "wifi/wifi_setting.h"
#include "wifi/WifiRequestPowerGuard.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_tls.h>
#include <esp_wifi.h>
#include <arpa/inet.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string_view>

namespace weread {
namespace {

constexpr const char *TAG = "WEREAD_HTTP";
constexpr const char *WEB_API_HOST = "weread.qq.com";
constexpr const char *DEFAULT_REFERER = "https://weread.qq.com/";
constexpr const char *USER_AGENT =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 "
    "Safari/537.36 Edg/135.0.0.0";
constexpr size_t INITIAL_CAPACITY = 64 * 1024;
constexpr size_t MAX_RESPONSE_BYTES = 4 * 1024 * 1024;
constexpr int WEB_REQUEST_TIMEOUT_MS = 15000;
#if CONFIG_IDF_TARGET_ESP32S31
constexpr int WEB_WIFI_ENTRY_CONNECT_TIMEOUT_MS = 6000;
#endif

// The web reader may issue additional device/risk-control cookies besides the
// three credentials persisted in NVS. Keep them across the short-lived HTTP
// client instances used by shelf/catalog/chapter jobs, matching the reference
// implementation's process-wide cookie jar.
std::map<std::string, std::string> runtime_cookies;
std::mutex cookie_mutex;
std::mutex http_mutex;

constexpr const char *NATIVE_API_HOST = "i.weread.qq.com";
constexpr const char *NATIVE_API_PREFIX = "https://i.weread.qq.com/";
constexpr const char *PUBLIC_DNS_HOST = "dns.alidns.com";
constexpr const char *PUBLIC_DNS_URL = "https://223.5.5.5/resolve?name=";
constexpr int PUBLIC_DNS_TIMEOUT_MS = 4000;

struct DnsAddress {
    std::string ip;
    int64_t expires_us = 0;
};

std::mutex dns_mutex;
DnsAddress native_dns_cache[2];
DnsAddress web_cellular_dns_cache;

DnsAddress cached_dns(const DnsAddress &entry)
{
    const std::lock_guard<std::mutex> lock(dns_mutex);
    return entry.expires_us > esp_timer_get_time() ? entry : DnsAddress{};
}

void update_dns(DnsAddress &cache, DnsAddress entry)
{
    const std::lock_guard<std::mutex> lock(dns_mutex);
    cache = std::move(entry);
}

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

}  // namespace

struct HttpClient::Collector {
    uint8_t *data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    bool overflow = false;
    size_t limit = MAX_RESPONSE_BYTES;
    bool collect_headers = true;
    int response_timeout_ms = 0;
    int64_t connected_us = -1;
    const std::atomic_bool *cancelled = nullptr;
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
        connected_us = -1;
        set_cookie.clear();
        wr_ticket.clear();
        wr_wrpa.clear();
    }
    bool append(const void *source, size_t bytes)
    {
        if (!bytes) return true;
        if (size > limit || bytes > limit - size) {
            overflow = true;
            return false;
        }
        const size_t required = size + bytes + 1;
        if (required > capacity) {
            size_t next = capacity ? capacity : std::min(INITIAL_CAPACITY, limit + 1);
            while (next < required) next = std::min(next * 2, limit + 1);
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
    if (collector->cancelled && *collector->cancelled) return ESP_FAIL;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        collector->connected_us = esp_timer_get_time();
    }
    if (collector->response_timeout_ms > 0 &&
        (event->event_id == HTTP_EVENT_ON_CONNECTED || event->event_id == HTTP_EVENT_HEADERS_SENT)) {
        // Reused sockets skip ON_CONNECTED, so restore the read timeout after
        // HEADERS_SENT as well.
        esp_http_client_set_timeout_ms(event->client, collector->response_timeout_ms);
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        collector->append(event->data, static_cast<size_t>(event->data_len));
    } else if (collector->collect_headers && event->event_id == HTTP_EVENT_ON_HEADER &&
               event->header_key && event->header_value) {
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


static DnsAddress parse_public_dns(std::string_view body)
{
    std::string_view status, answers, value;
    int64_t number = 0;
    if (!json::member(body, "Status", &status) || !json::integer(status, &number) || number != 0 ||
        !json::member(body, "Answer", &answers) || answers.empty() || answers.front() != '[') return {};
    bool truncated = false;
    if (json::member(body, "TC", &value) && json::boolean(value, &truncated) && truncated) return {};

    json::ObjectIterator entries(answers);
    std::string_view entry;
    while (entries.next(&entry)) {
        if (!json::member(entry, "type", &value) || !json::integer(value, &number) || number != 1) continue;
        DnsAddress result;
        if (!json::member(entry, "data", &value) || !json::string(value, &result.ip)) continue;
        in_addr ip{};
        if (result.ip.size() >= INET_ADDRSTRLEN || result.ip.find('\0') != std::string::npos ||
            inet_pton(AF_INET, result.ip.c_str(), &ip) != 1 || ip.s_addr == 0) continue;
        int64_t ttl = 0;
        if (json::member(entry, "TTL", &value)) json::integer(value, &ttl);
        result.expires_us = esp_timer_get_time() + std::clamp<int64_t>(ttl, 0, 300) * 1000000;
        return result;
    }
    return {};
}

static DnsAddress resolve_public_dns(const char *hostname, const std::atomic_bool &cancelled)
{
    std::lock_guard<std::mutex> transaction(http_mutex);
    if (cancelled || offline_mode()) return {};

    HttpClient::Collector collector;
    collector.limit = 8192;
    collector.collect_headers = false;
    collector.cancelled = &cancelled;
    const std::string url = std::string(PUBLIC_DNS_URL) + hostname + "&type=A";
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = http_event;
    config.user_data = &collector;
    config.common_name = PUBLIC_DNS_HOST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    config.addr_type = HTTP_ADDR_TYPE_INET;
    config.timeout_ms = PUBLIC_DNS_TIMEOUT_MS;
    auto client = esp_http_client_init(&config);
    if (!client) return {};

    esp_http_client_set_header(client, "Host", PUBLIC_DNS_HOST);
    esp_http_client_set_header(client, "Accept", "application/dns-json");
    const auto start_us = esp_timer_get_time();
    const auto status = esp_http_client_perform(client);
    const int http = esp_http_client_get_status_code(client);
    const int socket_errno = esp_http_client_get_errno(client);
    int tls_code = 0;
    const auto tls_error = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, nullptr);
    DnsAddress address;
    if (!cancelled && status == ESP_OK && http == 200 && collector.data &&
        !collector.overflow && esp_http_client_is_complete_data_received(client)) {
        address = parse_public_dns({reinterpret_cast<const char *>(collector.data), collector.size});
    }
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "public DNS host=%s result=%s http=%d time=%lld ms error=%s errno=%d tls=%s/%d",
             hostname, address.ip.empty() ? "unavailable" : "resolved", http,
             static_cast<long long>((esp_timer_get_time() - start_us) / 1000),
             esp_err_to_name(status), socket_errno, esp_err_to_name(tls_error), tls_code);
    return address;
}

HttpResponse::~HttpResponse()
{
    if (_data) heap_caps_free(_data);
}

HttpResponse::HttpResponse(HttpResponse &&other) noexcept
    : status(other.status), timed_out(other.timed_out), error(std::move(other.error)),
      _dns_failed(other._dns_failed), _data(other._data), _size(other._size)
{
    other._data = nullptr;
    other._size = 0;
}

HttpResponse &HttpResponse::operator=(HttpResponse &&other) noexcept
{
    if (this != &other) {
        if (_data) heap_caps_free(_data);
        status = other.status;
        timed_out = other.timed_out;
        error = std::move(other.error);
        _dns_failed = other._dns_failed;
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
    { std::lock_guard<std::mutex> lock(cookie_mutex); _cookies = runtime_cookies; }
    for (const auto &[name, value] : _session.cookies) _cookies[name] = value;
    set_session(_session);
}

HttpClient::~HttpClient()
{
    close();
    delete _collector;
}

bool HttpClient::ensure_client(const char *tls_host)
{
    if (_client) return true;
    const bool cellular = wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR;
    esp_http_client_config_t config = {};
    config.url = "https://weread.qq.com/";
    config.event_handler = http_event;
    config.user_data = _collector;
    config.timeout_ms = WEB_REQUEST_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.common_name = tls_host;
    config.disable_auto_redirect = tls_host != nullptr;
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
    std::lock_guard<std::mutex> lock(cookie_mutex);
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
    std::lock_guard<std::mutex> lock(cookie_mutex);
    _session.clear();
    _cookies.clear();
    runtime_cookies.clear();
    weread::clear_session();
}

void HttpClient::absorb_cookie_headers(const std::string &headers)
{
    std::lock_guard<std::mutex> lock(cookie_mutex);
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

HttpResponse HttpClient::request_native(const std::string &url, const char *stage, size_t limit,
                                        int timeout_ms, const std::atomic_bool &cancelled,
                                        const std::string *body, const std::string &vid,
                                        const std::string &access_token, bool retry_read)
{
    const bool use_dns_fallback = url.rfind(NATIVE_API_PREFIX, 0) == 0;
    const bool cellular = wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR;
    wifi::RequestPowerGuard wifi_awake(TAG, retry_read && !cellular && !cancelled && !offline_mode());
    auto &cache = native_dns_cache[cellular];
    const auto cached = use_dns_fallback ? cached_dns(cache) : DnsAddress{};
    const int first_connect_ms = retry_read ? std::min(timeout_ms, 7000) : timeout_ms;
#if CONFIG_IDF_TARGET_ESP32S31
    // Match the RSS connection policy for native shelf/catalog reads.
    const bool first_ipv4 = retry_read && use_dns_fallback;
#else
    constexpr bool first_ipv4 = false;
#endif
    NativeFailure failure = NativeFailure::None;
    auto response = perform_native(url, stage, limit, first_connect_ms, timeout_ms, cancelled, body,
                                   vid, access_token, first_ipv4, cached.ip, failure);
    if (cancelled || offline_mode()) return response;

    if (!cached.ip.empty() && failure != NativeFailure::None) update_dns(cache, {});
    const int retry_connect_ms = std::min(timeout_ms, 8000);
    // A DNS failure precedes TCP, so no login or refresh POST could have been sent.
    if (use_dns_fallback && failure == NativeFailure::Dns) {
        auto address = resolve_public_dns(NATIVE_API_HOST, cancelled);
        if (address.ip.empty() || cancelled || wifi_setting_get_network_mode() !=
            (cellular ? WIFI_SETTING_NETWORK_CELLULAR : WIFI_SETTING_NETWORK_WIFI)) return response;

        auto resolved = perform_native(url, stage, limit, retry_connect_ms, timeout_ms, cancelled, body,
                                       vid, access_token, true, address.ip, failure);
        if (!cancelled && resolved.status > 0) update_dns(cache, std::move(address));
        return resolved;
    }

    if (!retry_read || failure == NativeFailure::None) return response;
    ESP_LOGI(TAG, "native stage=%s retrying read over IPv4", stage);
    return perform_native(url, stage, limit, retry_connect_ms, timeout_ms, cancelled, body,
                          vid, access_token, true, {}, failure);
}

HttpResponse HttpClient::perform_native(const std::string &url, const char *stage, size_t limit,
                                        int connect_timeout_ms, int response_timeout_ms,
                                        const std::atomic_bool &cancelled,
                                        const std::string *body, const std::string &vid,
                                        const std::string &access_token, bool force_ipv4,
                                        const std::string &resolved_ip, NativeFailure &failure)
{
    std::lock_guard<std::mutex> transaction(http_mutex);
    failure = NativeFailure::None;
    HttpResponse response;
    if (cancelled || offline_mode()) {
        response.error = cancelled ? "cancelled" : "Offline mode";
        return response;
    }
    const bool weread_host = url.rfind(NATIVE_API_PREFIX, 0) == 0;
    if (!weread_host && url.rfind("https://open.weixin.qq.com/", 0) != 0 &&
        url.rfind("https://long.open.weixin.qq.com/", 0) != 0) {
        response.error = "unexpected native host";
        return response;
    }

    Collector collector;
    collector.limit = std::min(limit, MAX_RESPONSE_BYTES);
    collector.collect_headers = false;
    collector.response_timeout_ms = response_timeout_ms;
    collector.cancelled = &cancelled;
    const bool cellular = wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR;
    const bool use_resolved_ip = weread_host && !resolved_ip.empty();
    const std::string connect_url = use_resolved_ip
        ? "https://" + resolved_ip + url.substr(std::strlen(NATIVE_API_PREFIX) - 1) : url;
    esp_http_client_config_t config = {};
    config.url = connect_url.c_str();
    if (use_resolved_ip) config.common_name = NATIVE_API_HOST;
    config.event_handler = http_event;
    config.user_data = &collector;
    config.timeout_ms = connect_timeout_ms;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    config.buffer_size = 4096;
    config.method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    if (cellular || force_ipv4 || use_resolved_ip) config.addr_type = HTTP_ADDR_TYPE_INET;
    if (cellular) {
        config.tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2;
    }
    auto client = esp_http_client_init(&config);
    if (!client) {
        response.error = "HTTP client allocation failed";
        return response;
    }

    esp_http_client_set_header(client, "User-Agent", "MyPaperS31");
    esp_http_client_set_header(client, "Accept", "application/json");
    if (weread_host) {
        if (use_resolved_ip) esp_http_client_set_header(client, "Host", NATIVE_API_HOST);
        esp_http_client_set_header(client, "appver", "2.1.0");
        if (!vid.empty() && !access_token.empty()) {
            esp_http_client_set_header(client, "vid", vid.c_str());
            esp_http_client_set_header(client, "accessToken", access_token.c_str());
        }
        if (body) {
            esp_http_client_set_header(client, "Content-Type", "application/json;charset=UTF-8");
            esp_http_client_set_post_field(client, body->data(), static_cast<int>(body->size()));
        }
    }

    const auto start_us = esp_timer_get_time();
    const esp_err_t status = esp_http_client_perform(client);
    const auto elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    const bool connected = collector.connected_us >= start_us;
    response.status = esp_http_client_get_status_code(client);
    if (cancelled) {
        response.error = "cancelled";
    } else if (collector.overflow) {
        response.error = "response exceeds PSRAM limit";
    } else if (status != ESP_OK) {
        const int socket_errno = esp_http_client_get_errno(client);
        response.timed_out = connected &&
                             (status == ESP_ERR_HTTP_EAGAIN || socket_errno == EAGAIN || socket_errno == ETIMEDOUT);
#ifdef ESP_ERR_HTTP_READ_TIMEOUT
        response.timed_out = response.timed_out || status == ESP_ERR_HTTP_READ_TIMEOUT;
#endif
        response.error = esp_err_to_name(status);
        if (!response.timed_out || std::strcmp(stage, "poll") != 0) {
            int tls_code = 0;
            const auto tls_error = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, nullptr);
            const bool connect_failure = status == ESP_ERR_HTTP_CONNECT && !connected &&
                (tls_error == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT ||
                 tls_error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME ||
                 tls_error == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST);
            const bool header_timeout = connected && response.status <= 0 && response.timed_out;
            if (connect_failure && tls_error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
                failure = NativeFailure::Dns;
            } else if (connect_failure || header_timeout) {
                failure = NativeFailure::Retryable;
            }
            const char *phase = !connected ? "connect" : response.status <= 0 ? "headers" : "body";
            const auto connect_ms = connected ? (collector.connected_us - start_us) / 1000 : elapsed_ms;
            ESP_LOGW(TAG, "native stage=%s transport=%s family=%s phase=%s time=%lld ms "
                          "connect=%lld ms http=%d error=%s errno=%d tls=%s/%d",
                     stage, cellular ? "4G" : "Wi-Fi", use_resolved_ip ? "IPv4/DoH" :
                     cellular || force_ipv4 ? "IPv4" : "auto", phase,
                     static_cast<long long>(elapsed_ms), static_cast<long long>(connect_ms),
                     response.status, response.error.c_str(), socket_errno, esp_err_to_name(tls_error), tls_code);
            if (!cellular && failure != NativeFailure::None) {
                wifi_ap_record_t ap{};
                wifi_ps_type_t ps = WIFI_PS_NONE;
                const auto ap_status = esp_wifi_sta_get_ap_info(&ap);
                const auto ps_status = esp_wifi_get_ps(&ps);
                int8_t tx_power = 0;
                const auto tx_status = esp_wifi_get_max_tx_power(&tx_power);
                auto *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                auto *route = esp_netif_get_default_netif();
                esp_netif_ip_info_t ip{};
                if (sta) esp_netif_get_ip_info(sta, &ip);
                ESP_LOGI(TAG, "native Wi-Fi link ap=%s rssi=%d channel=%u ps=%d/%s "
                              "tx_qdbm=%d/%s route=%s ip=" IPSTR " gateway=" IPSTR,
                         esp_err_to_name(ap_status), ap.rssi, static_cast<unsigned>(ap.primary),
                         static_cast<int>(ps), esp_err_to_name(ps_status),
                         static_cast<int>(tx_power), esp_err_to_name(tx_status),
                         route ? esp_netif_get_ifkey(route) : "none", IP2STR(&ip.ip), IP2STR(&ip.gw));
                for (int i = ESP_NETIF_DNS_MAIN; i < ESP_NETIF_DNS_MAX; ++i) {
                    esp_netif_dns_info_t dns{};
                    if (esp_netif_get_dns_info(nullptr, static_cast<esp_netif_dns_type_t>(i), &dns) == ESP_OK &&
                        dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
                        ESP_LOGI(TAG, "native Wi-Fi active DNS[%d]=" IPSTR, i, IP2STR(&dns.ip.u_addr.ip4));
                    }
                }
            }
        }
    } else if (!esp_http_client_is_complete_data_received(client)) {
        response.error = "Incomplete HTTP response";
    } else {
        response._data = collector.data;
        response._size = collector.size;
        collector.data = nullptr;
    }
    esp_http_client_cleanup(client);
    return response;
}

HttpResponse HttpClient::perform_once(const char *method, const std::string &url,
                                      const std::string *body, const std::string &referer,
                                      bool html_response, bool persist_response_cookies,
                                      bool account_headers, const std::string *bearer,
                                      bool send_cookies, const std::string &resolved_ip,
                                      int connect_timeout_ms)
{
    // Reader heartbeat/comments/shelf can now run on separate workers. Keep
    // only one active TLS transaction, and protect the shared cookie jar.
    std::lock_guard<std::mutex> transaction(http_mutex);
    HttpResponse response;
    if (_cancelled) {
        response.error = "cancelled";
        return response;
    }
    const bool cellular = wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR;
    const bool use_resolved_ip = cellular && !resolved_ip.empty() && url.rfind(DEFAULT_REFERER, 0) == 0;
    const std::string connect_url = use_resolved_ip
        ? "https://" + resolved_ip + url.substr(std::strlen(DEFAULT_REFERER) - 1) : url;
    if (cellular) close();
    if (!ensure_client(use_resolved_ip ? WEB_API_HOST : nullptr)) {
        response.error = "HTTP client allocation failed";
        return response;
    }
    _collector->reset();
    _collector->response_timeout_ms = connect_timeout_ms > 0 ? WEB_REQUEST_TIMEOUT_MS : 0;
    const int timeout_ms = connect_timeout_ms > 0 ? connect_timeout_ms : WEB_REQUEST_TIMEOUT_MS;
    esp_http_client_set_timeout_ms(_client, timeout_ms);
    esp_http_client_set_url(_client, connect_url.c_str());
    if (use_resolved_ip) esp_http_client_set_header(_client, "Host", WEB_API_HOST);
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

    const int64_t start_us = esp_timer_get_time();
    const esp_err_t result = esp_http_client_perform(_client);
    const auto elapsed_ms = static_cast<long long>((esp_timer_get_time() - start_us) / 1000);
    if (result != ESP_OK) {
        const int socket_errno = esp_http_client_get_errno(_client);
        int tls_code = 0;
        // Read the saved failure from this request without making another DNS query.
        const esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(_client, &tls_code, nullptr);
        response._dns_failed = tls_error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME;
        response.error = esp_err_to_name(result);
        ESP_LOGW(TAG, "%s request failed transport=%s time=%lld ms connect_budget=%d ms url=%s error=%s errno=%d tls=%s tls_code=%d",
                 method, use_resolved_ip ? "4G/DoH" : cellular ? "4G/fresh" : "Wi-Fi/reuse",
                 elapsed_ms, timeout_ms, url.c_str(),
                 response.error.c_str(), socket_errno, esp_err_to_name(tls_error), tls_code);
        if (cellular) close();
        return response;
    }
    response.status = esp_http_client_get_status_code(_client);
    if (use_resolved_ip)
        ESP_LOGI(TAG, "web 4G/DoH http=%d time=%lld ms", response.status, elapsed_ms);
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
    if (offline_mode()) {
        HttpResponse blocked;
        blocked.error = "Offline mode";
        return blocked;
    }
    const bool web_cellular = wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR &&
                              url.rfind(DEFAULT_REFERER, 0) == 0;
    const auto cached = web_cellular ? cached_dns(web_cellular_dns_cache) : DnsAddress{};
    int first_connect_timeout_ms = 0;
#if CONFIG_IDF_TARGET_ESP32S31
    if (wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_WIFI &&
        (url == "https://weread.qq.com/web/shelf/sync" ||
         url == "https://weread.qq.com/web/book/chapterInfos")) {
        first_connect_timeout_ms = WEB_WIFI_ENTRY_CONNECT_TIMEOUT_MS;
    }
#endif
    auto perform = [&](const std::string &ip, int connect_timeout_ms = 0) {
        return perform_once(method, url, body, referer, html_response, persist_response_cookies,
                            account_headers, bearer, send_cookies, ip, connect_timeout_ms);
    };
    HttpResponse response = perform(cached.ip, first_connect_timeout_ms);
    if (web_cellular && response._dns_failed && !_cancelled && !offline_mode()) {
        // DNS failed before any request was sent. Recover using an A record
        // for this host, while preserving its TLS identity and HTTP Host.
        auto address = resolve_public_dns(WEB_API_HOST, _cancelled);
        if (!address.ip.empty() && !_cancelled && !offline_mode() &&
            wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR) {
            response = perform(address.ip);
            if (response.status > 0) update_dns(web_cellular_dns_cache, std::move(address));
        }
    } else if (!response.error.empty() && !_cancelled && !offline_mode()) {
        if (!cached.ip.empty()) update_dns(web_cellular_dns_cache, {});
        ESP_LOGW(TAG, "request failed, rebuilding session: %s", response.error.c_str());
        close();
        response = perform({});
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

HttpResponse HttpClient::post_json_once(const std::string &url, const std::string &body,
                                       const std::string &referer)
{
    if (offline_mode()) { HttpResponse r; r.error = "Offline mode"; return r; }
    return perform_once("POST", url, &body, referer, false, true, false, nullptr, true);
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
