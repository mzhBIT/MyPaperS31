#pragma once

#include "weread/WereadSession.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

struct esp_http_client;
typedef struct esp_http_client *esp_http_client_handle_t;

namespace weread {

class HttpResponse {
public:
    HttpResponse() = default;
    ~HttpResponse();
    HttpResponse(HttpResponse &&other) noexcept;
    HttpResponse &operator=(HttpResponse &&other) noexcept;
    HttpResponse(const HttpResponse &) = delete;
    HttpResponse &operator=(const HttpResponse &) = delete;

    int status = 0;
    bool timed_out = false;
    std::string error;
    const uint8_t *data() const { return _data; }
    size_t size() const { return _size; }
    bool ok() const { return status >= 200 && status < 300 && error.empty(); }

private:
    friend class HttpClient;
    bool _dns_failed = false;
    uint8_t *_data = nullptr;
    size_t _size = 0;
};

class HttpClient {
public:
    struct Collector;

    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient &) = delete;
    HttpClient &operator=(const HttpClient &) = delete;

    HttpResponse get(const std::string &url, const std::string &referer = {});
    HttpResponse get_account_authenticated(const std::string &url,
                                           const std::string &referer = {});
    HttpResponse get_html(const std::string &url, const std::string &referer = {});
    HttpResponse post_json(const std::string &url, const std::string &body,
                           const std::string &referer = {});
    // Non-idempotent reading reports must not retry an ambiguous transport error.
    HttpResponse post_json_once(const std::string &url, const std::string &body,
                               const std::string &referer = {});
    HttpResponse post_json_bearer(const std::string &url, const std::string &body,
                                  const std::string &bearer,
                                  const std::string &referer = {});
    // Native requests use independent authentication and never follow redirects.
    static HttpResponse request_native(const std::string &url, const char *stage, size_t limit,
                                       int timeout_ms, const std::atomic_bool &cancelled,
                                       const std::string *body = nullptr, const std::string &vid = {},
                                       const std::string &access_token = {}, bool retry_read = false);
    bool refresh_session() { return renew_session(); }
    void close();
    void request_cancel() { _cancelled = true; }
    void clear_cancel() { _cancelled = false; }
    const Session &session() const { return _session; }
    std::string cookie_names() const;
    void set_session(const Session &session);
    void clear_session();

private:
    enum class NativeFailure { None, Dns, Retryable };
    static HttpResponse perform_native(const std::string &url, const char *stage, size_t limit,
                                       int connect_timeout_ms, int response_timeout_ms,
                                       const std::atomic_bool &cancelled,
                                       const std::string *body, const std::string &vid,
                                       const std::string &access_token, bool force_ipv4,
                                       const std::string &resolved_ip, NativeFailure &failure);
    HttpResponse request(const char *method, const std::string &url,
                         const std::string *body, const std::string &referer,
                         bool allow_renewal, bool html_response = false,
                         bool persist_response_cookies = true,
                         bool account_headers = false,
                         const std::string *bearer = nullptr,
                         bool send_cookies = true);
    HttpResponse perform_once(const char *method, const std::string &url,
                              const std::string *body, const std::string &referer,
                              bool html_response, bool persist_response_cookies,
                              bool account_headers, const std::string *bearer,
                              bool send_cookies, const std::string &resolved_ip = {},
                              int connect_timeout_ms = 0);
    bool ensure_client(const char *tls_host = nullptr);
    bool renew_session();
    void absorb_cookie_headers(const std::string &headers);
    std::string cookie_header() const;

    esp_http_client_handle_t _client = nullptr;
    Collector *_collector = nullptr;
    Session _session;
    std::map<std::string, std::string> _cookies;
    std::atomic_bool _cancelled{false};
    bool _renewing = false;
};

}  // namespace weread
