#pragma once

#include "weread/WereadSession.h"

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
    std::string error;
    const uint8_t *data() const { return _data; }
    size_t size() const { return _size; }
    bool ok() const { return status >= 200 && status < 300 && error.empty(); }

private:
    friend class HttpClient;
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
    HttpResponse get_html(const std::string &url, const std::string &referer = {});
    HttpResponse post_json(const std::string &url, const std::string &body,
                           const std::string &referer = {});
    bool refresh_session() { return renew_session(); }
    void close();
    void request_cancel() { _cancelled = true; }
    void clear_cancel() { _cancelled = false; }
    const Session &session() const { return _session; }
    std::string cookie_names() const;
    void set_session(const Session &session);
    void clear_session();

private:
    HttpResponse request(const char *method, const std::string &url,
                         const std::string *body, const std::string &referer,
                         bool allow_renewal, bool html_response = false,
                         bool persist_response_cookies = true);
    HttpResponse perform_once(const char *method, const std::string &url,
                              const std::string *body, const std::string &referer,
                              bool html_response, bool persist_response_cookies);
    bool ensure_client();
    bool renew_session();
    void absorb_cookie_headers(const std::string &headers);
    std::string cookie_header() const;

    esp_http_client_handle_t _client = nullptr;
    Collector *_collector = nullptr;
    Session _session;
    std::map<std::string, std::string> _cookies;
    volatile bool _cancelled = false;
    bool _renewing = false;
};

}  // namespace weread
