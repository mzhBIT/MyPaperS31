#include "zhihu/ZhihuClient.h"

#include "weread/WereadJson.h"
#include "storage/app_paths.h"
#include "wifi/wifi_setting.h"

#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>

namespace {

constexpr const char *TAG = "ZHIHU_API";
constexpr const char *HOT_LIST_URL =
    "https://developer.zhihu.com/api/v1/content/hot_list?Limit=%d";
constexpr std::time_t MIN_VALID_UNIX_TIME = 1704067200;  // 2024-01-01 UTC
constexpr size_t MAX_RESPONSE_BYTES = 192 * 1024;
constexpr size_t MAX_WEB_RESPONSE_BYTES = 2 * 1024 * 1024;

class WifiShutdownGuard {
public:
    ~WifiShutdownGuard() { wifi_setting_deinit(true); }
};

bool ensure_request_time(std::string *error)
{
    std::time_t now = std::time(nullptr);
    if (now >= MIN_VALID_UNIX_TIME) return true;
    ESP_LOGI(TAG, "system clock invalid (%lld), synchronizing SNTP",
             static_cast<long long>(now));
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t result = esp_netif_sntp_init(&config);
    if (result == ESP_OK) {
        result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
        esp_netif_sntp_deinit();
    }
    now = std::time(nullptr);
    if (result == ESP_OK && now >= MIN_VALID_UNIX_TIME) {
        ESP_LOGI(TAG, "SNTP synchronized unix=%lld", static_cast<long long>(now));
        return true;
    }
    if (error) *error = "Network time synchronization failed";
    return false;
}

bool member_any(std::string_view object, const char *upper, const char *lower,
                std::string_view *value)
{
    return weread::json::member(object, upper, value) ||
           weread::json::member(object, lower, value);
}

bool string_any(std::string_view object, const char *upper, const char *lower,
                std::string *value)
{
    std::string_view raw;
    return member_any(object, upper, lower, &raw) && weread::json::string(raw, value);
}

int integer_any(std::string_view object, const char *upper, const char *lower, int fallback)
{
    std::string_view raw;
    int64_t value = fallback;
    return member_any(object, upper, lower, &raw) && weread::json::integer(raw, &value)
               ? static_cast<int>(value) : fallback;
}

void replace_all(std::string *text, const char *from, const char *to)
{
    size_t position = 0;
    const size_t from_size = std::char_traits<char>::length(from);
    while ((position = text->find(from, position)) != std::string::npos) {
        text->replace(position, from_size, to);
        position += std::char_traits<char>::length(to);
    }
}

std::string clean_text(const std::string &input)
{
    std::string output;
    output.reserve(input.size());
    bool in_tag = false;
    bool pending_space = false;
    for (unsigned char c : input) {
        if (c == '<') { in_tag = true; continue; }
        if (in_tag) {
            if (c == '>') in_tag = false;
            continue;
        }
        if (c == '\r' || c == '\n' || c == '\t') {
            pending_space = !output.empty();
            continue;
        }
        if (pending_space && c != ' ') output.push_back(' ');
        pending_space = false;
        output.push_back(static_cast<char>(c));
    }
    replace_all(&output, "&nbsp;", " ");
    replace_all(&output, "&amp;", "&");
    replace_all(&output, "&quot;", "\"");
    replace_all(&output, "&#39;", "'");
    replace_all(&output, "&lt;", "<");
    replace_all(&output, "&gt;", ">");
    while (!output.empty() && std::isspace(static_cast<unsigned char>(output.front()))) {
        output.erase(output.begin());
    }
    while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) {
        output.pop_back();
    }
    return output;
}

std::string html_to_text(const std::string &html)
{
    std::string output;
    output.reserve(html.size());
    bool in_tag = false;
    std::string tag;
    for (size_t i = 0; i < html.size(); ++i) {
        const char c = html[i];
        if (!in_tag && c == '<') {
            in_tag = true;
            tag.clear();
            continue;
        }
        if (in_tag) {
            if (c == '>') {
                in_tag = false;
                std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
                size_t start = 0;
                while (start < tag.size() && (tag[start] == '/' || std::isspace(
                           static_cast<unsigned char>(tag[start])))) ++start;
                const bool paragraph = tag.compare(start, 1, "p") == 0 ||
                                       tag.compare(start, 2, "br") == 0 ||
                                       tag.compare(start, 2, "li") == 0 ||
                                       tag.compare(start, 2, "h1") == 0 ||
                                       tag.compare(start, 2, "h2") == 0 ||
                                       tag.compare(start, 2, "h3") == 0;
                if (paragraph && !output.empty() && output.back() != '\n') output += "\n\n";
            } else if (tag.size() < 64) {
                tag.push_back(c);
            }
            continue;
        }
        output.push_back(c);
    }
    replace_all(&output, "&nbsp;", " ");
    replace_all(&output, "&amp;", "&");
    replace_all(&output, "&quot;", "\"");
    replace_all(&output, "&#39;", "'");
    replace_all(&output, "&lt;", "<");
    replace_all(&output, "&gt;", ">");

    std::string normalized;
    normalized.reserve(output.size());
    int newlines = 0;
    for (unsigned char c : output) {
        if (c == '\r') continue;
        if (c == '\n') {
            ++newlines;
            continue;
        }
        if (newlines > 0) {
            while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
            if (!normalized.empty()) normalized.append(newlines >= 2 ? "\n\n" : "\n");
            newlines = 0;
        }
        normalized.push_back(static_cast<char>(c));
    }
    size_t start = 0;
    while (start < normalized.size() &&
           std::isspace(static_cast<unsigned char>(normalized[start]))) ++start;
    size_t end = normalized.size();
    while (end > start && std::isspace(static_cast<unsigned char>(normalized[end - 1]))) --end;
    return normalized.substr(start, end - start);
}

bool decode_json_string(const std::string &source, size_t quote, std::string *value,
                        size_t *end)
{
    if (quote >= source.size() || source[quote] != '"') return false;
    bool escaped = false;
    for (size_t i = quote + 1; i < source.size(); ++i) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (source[i] == '\\') {
            escaped = true;
            continue;
        }
        if (source[i] == '"') {
            if (!weread::json::string(
                    std::string_view(source.data() + quote, i - quote + 1), value)) return false;
            if (end) *end = i + 1;
            return true;
        }
    }
    return false;
}

std::string extract_json_content(const std::string &html)
{
    std::string best;
    size_t position = 0;
    constexpr const char *MARKER = "\"content\"";
    while ((position = html.find(MARKER, position)) != std::string::npos) {
        size_t colon = html.find(':', position + std::char_traits<char>::length(MARKER));
        if (colon == std::string::npos || colon - position > 32) {
            position += std::char_traits<char>::length(MARKER);
            continue;
        }
        size_t quote = colon + 1;
        while (quote < html.size() && std::isspace(static_cast<unsigned char>(html[quote]))) ++quote;
        std::string candidate;
        size_t end = quote + 1;
        if (decode_json_string(html, quote, &candidate, &end) &&
            (candidate.find("<p") != std::string::npos ||
             candidate.find("<div") != std::string::npos) &&
            candidate.size() > best.size()) {
            best = std::move(candidate);
        }
        position = std::max(position + 1, end);
    }
    return best;
}

std::string meta_description(const std::string &html)
{
    size_t position = 0;
    while ((position = html.find("<meta", position)) != std::string::npos) {
        const size_t end = html.find('>', position + 5);
        if (end == std::string::npos) break;
        const std::string tag = html.substr(position, end - position + 1);
        if (tag.find("description") != std::string::npos) {
            size_t content = tag.find("content=");
            if (content != std::string::npos) {
                content += 8;
                while (content < tag.size() && std::isspace(static_cast<unsigned char>(tag[content]))) {
                    ++content;
                }
                if (content < tag.size() && (tag[content] == '"' || tag[content] == '\'')) {
                    const char delimiter = tag[content++];
                    const size_t close = tag.find(delimiter, content);
                    if (close != std::string::npos) {
                        return clean_text(tag.substr(content, close - content));
                    }
                }
            }
        }
        position = end + 1;
    }
    return {};
}

struct WebCollector {
    std::string body;
    bool overflow = false;
};

esp_err_t web_event(esp_http_client_event_t *event)
{
    if (!event || !event->user_data || event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    auto *collector = static_cast<WebCollector *>(event->user_data);
    if (event->data_len <= 0 || collector->overflow) return ESP_OK;
    const size_t available = MAX_WEB_RESPONSE_BYTES - collector->body.size();
    const size_t count = std::min<size_t>(static_cast<size_t>(event->data_len), available);
    collector->body.append(static_cast<const char *>(event->data), count);
    if (count < static_cast<size_t>(event->data_len)) collector->overflow = true;
    return ESP_OK;
}

bool parse_hot_list(std::string_view body, ZhihuHotListResult *result)
{
    result->api_code = integer_any(body, "Code", "code", 0);
    if (result->api_code != 0) {
        std::string message;
        string_any(body, "Message", "message", &message);
        result->error = "Zhihu API " + std::to_string(result->api_code);
        if (!message.empty()) result->error += ": " + message;
        ESP_LOGW(TAG, "hot list API error code=%d message=%s",
                 result->api_code, message.c_str());
        return false;
    }

    std::string_view data;
    if (!member_any(body, "Data", "data", &data)) {
        string_any(body, "Message", "message", &result->error);
        if (result->error.empty()) result->error = "Hot list response has no Data field";
        return false;
    }
    std::string_view items;
    if (!member_any(data, "Items", "items", &items)) {
        result->error = "Hot list response has no Items field";
        return false;
    }

    weread::json::ObjectIterator iterator(items);
    std::string_view object;
    while (iterator.next(&object)) {
        ZhihuHotItem item;
        string_any(object, "Title", "title", &item.title);
        string_any(object, "Url", "url", &item.url);
        string_any(object, "ThumbnailUrl", "thumbnail_url", &item.thumbnail_url);
        string_any(object, "Summary", "summary", &item.summary);
        item.title = clean_text(item.title);
        item.summary = clean_text(item.summary);
        if (!item.title.empty()) result->items.push_back(std::move(item));
    }
    if (result->items.empty()) {
        result->error = "Zhihu returned an empty hot list";
        return false;
    }
    return true;
}

bool load_hot_list_cache(ZhihuHotListResult *result)
{
    if (!result) return false;
    FILE *file = std::fopen(app_paths::zhihu_hot_list_file(), "rb");
    if (!file) return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size <= 0 || size > static_cast<long>(MAX_RESPONSE_BYTES) ||
        std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    std::string body(static_cast<size_t>(size), '\0');
    const bool read_ok = std::fread(body.data(), 1, body.size(), file) == body.size();
    std::fclose(file);
    if (!read_ok || !parse_hot_list(body, result)) return false;
    result->success = true;
    result->from_cache = true;
    ESP_LOGI(TAG, "hot list cache loaded bytes=%ld items=%u", size,
             static_cast<unsigned>(result->items.size()));
    return true;
}

bool save_hot_list_cache(const std::string &body)
{
    app_paths::ensure_data_dir();
    const std::string temporary = std::string(app_paths::zhihu_hot_list_file()) + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = std::fwrite(body.data(), 1, body.size(), file) == body.size();
    if (std::fclose(file) != 0) ok = false;
    if (ok) std::remove(app_paths::zhihu_hot_list_file());
    if (!ok || std::rename(temporary.c_str(), app_paths::zhihu_hot_list_file()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    ESP_LOGI(TAG, "hot list cache saved bytes=%u", static_cast<unsigned>(body.size()));
    return true;
}

}  // namespace

ZhihuHotListResult ZhihuClient::fetch_hot_list(const ZhihuConfig &config, int limit,
                                               bool force_network)
{
    ZhihuHotListResult result;
    if (config.access_secret.empty()) {
        result.error = "Zhihu Access Secret is not configured";
        return result;
    }
    limit = std::max(1, std::min(30, limit));
    if (!force_network && load_hot_list_cache(&result)) {
        if (result.items.size() > static_cast<size_t>(limit)) result.items.resize(limit);
        return result;
    }
    esp_err_t wifi_error = wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
    WifiShutdownGuard wifi_guard;
    if (wifi_error != ESP_OK) {
        result.error = std::string("WiFi connection failed: ") + esp_err_to_name(wifi_error);
        return result;
    }
    if (!ensure_request_time(&result.error)) return result;

    char url[160];
    std::snprintf(url, sizeof(url), HOT_LIST_URL, limit);
    esp_http_client_config_t http_config = {};
    http_config.url = url;
    http_config.method = HTTP_METHOD_GET;
    http_config.timeout_ms = 25000;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.user_agent = "MyPaperP4 Zhihu";
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        result.error = "Failed to create Zhihu HTTPS client";
        return result;
    }

    std::string authorization = "Bearer " + config.access_secret;
    const std::string timestamp = std::to_string(static_cast<long long>(std::time(nullptr)));
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "X-Request-Timestamp", timestamp.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t error = esp_http_client_open(client, 0);
    if (error != ESP_OK) {
        result.error = std::string("Zhihu HTTPS request failed: ") + esp_err_to_name(error);
        esp_http_client_cleanup(client);
        return result;
    }
    const int64_t content_length = esp_http_client_fetch_headers(client);
    result.http_status = esp_http_client_get_status_code(client);
    std::string body;
    if (content_length > 0 && content_length <= static_cast<int64_t>(MAX_RESPONSE_BYTES)) {
        body.reserve(static_cast<size_t>(content_length));
    }
    char buffer[2048];
    while (body.size() < MAX_RESPONSE_BYTES) {
        int read = esp_http_client_read(client, buffer,
                                        std::min(sizeof(buffer), MAX_RESPONSE_BYTES - body.size()));
        if (read <= 0) break;
        body.append(buffer, static_cast<size_t>(read));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "hot list status=%d bytes=%u limit=%d", result.http_status,
             static_cast<unsigned>(body.size()), limit);

    if (result.http_status < 200 || result.http_status >= 300) {
        string_any(body, "Message", "message", &result.error);
        if (result.error.empty()) {
            result.error = "Zhihu HTTP error " + std::to_string(result.http_status);
        }
        return result;
    }
    result.success = parse_hot_list(body, &result);
    // A Settings connection test requests just one item. Do not let that
    // overwrite the complete 30-item list used by the actual hot-list page.
    if (result.success && limit >= 30) save_hot_list_cache(body);
    return result;
}

ZhihuContentResult ZhihuClient::fetch_content(const std::string &url)
{
    ZhihuContentResult result;
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        result.error = "Invalid Zhihu content URL";
        return result;
    }
    esp_err_t wifi_error = wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
    WifiShutdownGuard wifi_guard;
    if (wifi_error != ESP_OK) {
        result.error = std::string("WiFi connection failed: ") + esp_err_to_name(wifi_error);
        return result;
    }

    WebCollector collector;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 25000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.user_agent =
        "Mozilla/5.0 (Linux; Android 13) AppleWebKit/537.36 Chrome/122 Mobile Safari/537.36";
    config.event_handler = web_event;
    config.user_data = &collector;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        result.error = "Failed to create content HTTPS client";
        return result;
    }
    esp_http_client_set_header(client, "Accept",
                               "text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.8");
    esp_http_client_set_header(client, "Accept-Language", "zh-CN,zh;q=0.9");
    esp_http_client_set_header(client, "Connection", "close");
    const esp_err_t error = esp_http_client_perform(client);
    result.http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "content status=%d bytes=%u overflow=%d url=%s",
             result.http_status, static_cast<unsigned>(collector.body.size()),
             collector.overflow ? 1 : 0, url.c_str());
    if (error != ESP_OK) {
        result.error = std::string("Content request failed: ") + esp_err_to_name(error);
        return result;
    }
    if (result.http_status < 200 || result.http_status >= 300) {
        result.error = "Zhihu page HTTP error " + std::to_string(result.http_status);
        return result;
    }

    std::string rich_html = extract_json_content(collector.body);
    if (!rich_html.empty()) result.text = html_to_text(rich_html);
    if (result.text.size() < 120) {
        std::string description = meta_description(collector.body);
        if (description.size() > result.text.size()) result.text = std::move(description);
    }
    if (result.text.size() < 40) {
        result.error = "No readable body found in Zhihu page";
        ESP_LOGW(TAG, "content extraction failed html=%u json_candidate=%u",
                 static_cast<unsigned>(collector.body.size()),
                 static_cast<unsigned>(rich_html.size()));
        return result;
    }
    result.success = true;
    ESP_LOGI(TAG, "content extracted text=%u source=%s",
             static_cast<unsigned>(result.text.size()),
             rich_html.empty() ? "meta" : "initial-json");
    return result;
}
