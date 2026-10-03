#include "map/AmapClient.h"
#include "map/AmapRoute.h"
#include "map/AmapImageProcessor.h"
#include "map/MapScale.h"
#include "map/MapCrc.h"

#include "storage/app_paths.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include "time/network_time.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <esp_heap_caps.h>

namespace {

constexpr const char *TAG = "AMAP_HTTP";
constexpr int64_t MIN_REQUEST_INTERVAL_US = 500000;
constexpr int JSON_CONNECT_TIMEOUT_MS = 8000;
constexpr int64_t JSON_ATTEMPT_TIMEOUT_US = 15000000;
constexpr size_t MAX_MAP_BYTES = 4 * 1024 * 1024;
constexpr std::time_t MIN_VALID_UNIX_TIME = 1704067200;
portMUX_TYPE s_rate_lock = portMUX_INITIALIZER_UNLOCKED;
int64_t s_next_request_us = 0;

bool cancelled(const std::atomic<bool> *cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

bool wait_for_request_slot(const std::atomic<bool> *cancel)
{
    const int64_t now = esp_timer_get_time();
    int64_t scheduled = now;
    portENTER_CRITICAL(&s_rate_lock);
    if (scheduled < s_next_request_us) scheduled = s_next_request_us;
    s_next_request_us = scheduled + MIN_REQUEST_INTERVAL_US;
    portEXIT_CRITICAL(&s_rate_lock);
    if (scheduled > now) {
        const int64_t delay_us = scheduled - now;
        ESP_LOGI(TAG, "rate limit wait=%lld ms", delay_us / 1000);
        while (esp_timer_get_time() < scheduled) {
            if (cancelled(cancel)) return false;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return !cancelled(cancel);
}

bool ensure_request_time(std::string *error)
{
    std::time_t now = std::time(nullptr);
    if (now >= MIN_VALID_UNIX_TIME) return true;
    ESP_LOGI(TAG, "system clock invalid, synchronizing SNTP");
    esp_err_t result = network_time_sync(10000);
    now = std::time(nullptr);
    if (result == ESP_OK && now >= MIN_VALID_UNIX_TIME) return true;
    if (error) *error = "Network time synchronization failed";
    return false;
}

AmapImageFormat probe_image_format(const char *path)
{
    FILE *file = std::fopen(path, "rb");
    if (!file) return AmapImageFormat::Unknown;
    uint8_t magic[8] = {};
    const size_t read = std::fread(magic, 1, sizeof(magic), file);
    std::fclose(file);
    if (read >= 3 && magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) {
        return AmapImageFormat::Jpeg;
    }
    static constexpr uint8_t PNG_MAGIC[8] = {0x89, 0x50, 0x4E, 0x47,
                                              0x0D, 0x0A, 0x1A, 0x0A};
    if (read == sizeof(PNG_MAGIC) && std::equal(magic, magic + 8, PNG_MAGIC)) {
        return AmapImageFormat::Png;
    }
    return AmapImageFormat::Unknown;
}

std::string describe_api_error(const char *path)
{
    FILE *file = std::fopen(path, "rb");
    if (!file) return "AMap returned an invalid image";
    char buffer[1025] = {};
    const size_t count = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    const std::string body(buffer, count);
    if (body.find("10001") != std::string::npos ||
        body.find("INVALID_USER_KEY") != std::string::npos) {
        return "AMap API key is invalid or expired";
    }
    if (body.find("10003") != std::string::npos ||
        body.find("DAILY_QUERY_OVER_LIMIT") != std::string::npos) {
        return "AMap daily request limit reached";
    }
    if (body.find("10004") != std::string::npos ||
        body.find("10021") != std::string::npos ||
        body.find("ACCESS_TOO_FREQUENT") != std::string::npos) {
        return "AMap request rate limit reached";
    }
    return "AMap returned an invalid image";
}

struct DownloadSink {
    FILE *file = nullptr;
    size_t bytes = 0;
    bool write_ok = true;
    bool overflow = false;
    bool connected = false;
    uint32_t crc_state = ~0u;
};

bool header_is(const char *actual, const char *expected)
{
    if (!actual || !expected) return false;
    const size_t actual_length = std::char_traits<char>::length(actual);
    size_t expected_length = std::char_traits<char>::length(expected);
    if (actual_length != expected_length) return false;
    for (size_t i = 0; i < expected_length; ++i) {
        char a = actual[i];
        char b = expected[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

esp_err_t http_event(esp_http_client_event_t *event)
{
    if (!event || !event->user_data) return ESP_OK;
    auto *sink = static_cast<DownloadSink *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        sink->connected = true;
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        (header_is(event->header_key, "Connection") ||
         header_is(event->header_key, "Keep-Alive"))) {
        ESP_LOGI(TAG, "response header %s=%s", event->header_key,
                 event->header_value ? event->header_value : "");
        return ESP_OK;
    }
    // Body reads are explicit so cancellation is checked between chunks.
    return ESP_OK;
}

} // namespace

AmapClient::~AmapClient()
{
    close();
}

void AmapClient::close()
{
    if (!_client) return;
    esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(_client));
    _client = nullptr;
    ESP_LOGI(TAG, "persistent HTTPS client closed");
}

AmapImageFormat AmapClient::probe_image(const char *path)
{
    return probe_image_format(path);
}

AmapSearchResult AmapClient::search_places(const AmapConfig &config,
                                           const std::string &keyword,
                                           const std::atomic<bool> *cancel)
{
    AmapSearchResult result;
    if (config.api_key.empty()) { result.error = "AMap API key is not configured"; return result; }
    if (keyword.empty() || keyword.size() > 240) { result.error = "Invalid search keyword"; return result; }
    auto encode = [](const std::string &text) {
        const char hex[] = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : text) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += c;
            else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
        }
        return out;
    };
    const std::string url = "https://restapi.amap.com/v5/place/text?keywords=" + encode(keyword) +
        "&page_size=10&page_num=1&output=json&key=" + encode(config.api_key);
    return fetch_place_json(config, url, 0, cancel);
}

AmapSearchResult AmapClient::lookup_location(const AmapConfig &config, bool ip,
                                             const std::atomic<bool> *cancel)
{
    AmapSearchResult result;
    if (config.api_key.empty()) { result.error = "AMap API key is not configured"; return result; }
    // Keys are provisioned as ASCII alphanumerics; reject control/query injection.
    if (config.api_key.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos) {
        result.error = "AMap API key is invalid or expired"; return result;
    }
    if (!std::isfinite(config.longitude) || !std::isfinite(config.latitude) ||
        std::abs(config.longitude) > 180 || std::abs(config.latitude) > 85.051129) {
        result.error = "Invalid location response"; return result;
    }
    char position[96];
    std::snprintf(position, sizeof(position), "location=%.6f,%.6f&extensions=base&radius=1000&", config.longitude, config.latitude);
    const std::string url = std::string("https://restapi.amap.com") +
        (ip ? "/v3/ip?" : "/v3/geocode/regeo?") + (ip ? "" : position) +
        "output=json&key=" + config.api_key;
    return fetch_place_json(config, url, ip ? 1 : 2, cancel);
}

AmapSearchResult AmapClient::fetch_place_json(const AmapConfig &config, const std::string &url,
                                             int kind, const std::atomic<bool> *cancel)
{
    AmapSearchResult result;
    fetch_json(url,32768,cancel,[&](std::string_view body) {
        auto parsed = kind == 0 ? parse_search_response(body)
            : parse_location_response(body,kind == 1,config.longitude,config.latitude);
        result = std::move(parsed);
    },result.error);
    return result;
}

bool AmapClient::fetch_json(const std::string &url, size_t capacity, const std::atomic<bool> *cancel,
                           const std::function<void(std::string_view)> &parse, std::string &error)
{
    if (cancelled(cancel) || !ensure_request_time(&error)) return false;
    const size_t CAP = capacity;
    std::unique_ptr<char, decltype(&std::free)> body(
        static_cast<char *>(heap_caps_malloc(CAP+1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), &std::free);
    if (!body) { error = "Not enough memory for map"; return false; }
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str(); cfg.timeout_ms = JSON_CONNECT_TIMEOUT_MS; cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048; cfg.user_agent = "MyPaper Map";
    close();
    const int64_t query_begin = esp_timer_get_time();
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!wait_for_request_slot(cancel)) return false;
        auto client = esp_http_client_init(&cfg); _client = client;
        if (!client) { error = "Failed to create AMap HTTPS client"; return false; }
        // Never log the URL, search text or API key.
        ESP_LOGI(TAG, "map JSON query begin cap=%u attempt=%d connect_timeout=%d ms",
                 unsigned(capacity), attempt + 1, JSON_CONNECT_TIMEOUT_MS);
        const int64_t begin = esp_timer_get_time();
        esp_err_t err = esp_http_client_open(client, 0);
        const int64_t opened = esp_timer_get_time();
        if (err == ESP_OK && !cancelled(cancel) && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
        const int64_t headers = esp_timer_get_time();
        const int status = esp_http_client_get_status_code(client);
        size_t used = 0;
        esp_http_client_set_timeout_ms(client, 1000);
        while (err == ESP_OK && !cancelled(cancel)) {
            if (esp_timer_get_time()-begin > JSON_ATTEMPT_TIMEOUT_US) { err = ESP_ERR_TIMEOUT; break; }
            char chunk[1024];
            const int n = esp_http_client_read(client, chunk, sizeof(chunk));
            if (n == -ESP_ERR_HTTP_EAGAIN) continue;
            if (n < 0) { err = ESP_FAIL; break; }
            if (n == 0) { if (!esp_http_client_is_complete_data_received(client)) err = ESP_FAIL; break; }
            if (size_t(n) > CAP-used) { err = ESP_ERR_INVALID_SIZE; break; }
            std::copy(chunk, chunk+n, body.get()+used); used += n;
        }
        const int64_t received = esp_timer_get_time();
        close(); // Release transport before retrying, parsing or UI publication.
        ESP_LOGI(TAG, "map JSON transport attempt=%d status=%d bytes=%u open=%lld ms headers=%lld ms read=%lld ms total=%lld ms err=%s",
                 attempt + 1, status, unsigned(used), (opened-begin)/1000,
                 (headers-opened)/1000, (received-headers)/1000,
                 (received-begin)/1000, esp_err_to_name(err));
        if (cancelled(cancel)) return false;
        if (err == ESP_OK) {
            if (status != 200) { error = "AMap HTTP error " + std::to_string(status); return false; }
            body.get()[used] = 0;
            parse(std::string_view(body.get(),used));
            ESP_LOGI(TAG,"map JSON query complete bytes=%u attempts=%d time=%lld ms",
                     unsigned(used), attempt + 1, (esp_timer_get_time()-query_begin)/1000);
            return true;
        }
        error = std::string("AMap connection failed: ")+esp_err_to_name(err);
        // Only retry transport failures. HTTP/API errors, capacity limits and
        // allocation failures cannot be fixed by sending the same request again.
        if (attempt != 0 || err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_NO_MEM ||
            (status > 0 && status != 200)) return false;
        ESP_LOGW(TAG, "map JSON transport failed; rebuilding once after 500 ms");
        const int64_t retry_at = esp_timer_get_time() + MIN_REQUEST_INTERVAL_US;
        while (esp_timer_get_time() < retry_at) {
            if (cancelled(cancel)) return false;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return false;
}

AmapRoute AmapClient::plan_route(const AmapConfig &config, const AmapPlace &origin,
                                 const AmapPlace &destination, AmapTravelMode mode,
                                 const std::atomic<bool> *cancel)
{
    AmapRoute result;
    if (config.api_key.empty()) { result.error="AMap API key is not configured"; return result; }
    if (config.api_key.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")!=std::string::npos) {
        result.error="AMap API key is invalid or expired"; return result;
    }
    for (const auto *p : {&origin,&destination}) {
        if (!std::isfinite(p->longitude) || !std::isfinite(p->latitude) ||
            std::abs(p->longitude)>180 || std::abs(p->latitude)>85.051129) {
            result.error="Invalid route endpoints"; return result;
        }
    }
    if (origin.longitude==destination.longitude && origin.latitude==destination.latitude) {
        result.error="Start and destination are the same"; return result;
    }
    const char *type=mode==AmapTravelMode::Driving ? "driving" : mode==AmapTravelMode::Walking ? "walking" : "bicycling";
    char position[160];
    std::snprintf(position,sizeof(position),"?origin=%.6f,%.6f&destination=%.6f,%.6f&show_fields=cost,polyline&",
                  origin.longitude,origin.latitude,destination.longitude,destination.latitude);
    const std::string url=std::string("https://restapi.amap.com/v5/direction/")+type+position+
        (mode==AmapTravelMode::Driving ? "strategy=0&" : "")+"key="+config.api_key;
    fetch_json(url,256*1024,cancel,[&](std::string_view body) { result=parse_amap_route(body); },result.error);
    result.origin=origin; result.destination=destination; result.mode=mode;
    return result;
}

AmapMapResult AmapClient::fetch_static_map(const AmapConfig &config,
                                           int output_width,
                                           int output_height,
                                           const std::atomic<bool> *cancel)
{
    AmapMapResult result;
    if (config.api_key.empty()) {
        result.error = "AMap API key is not configured";
        return result;
    }
    const int request_width = std::max(1, std::min(1024, (output_width + 1) / 2));
    const int request_height = std::max(1, std::min(1024, (output_height + 1) / 2));
    constexpr int server_scale = MYPAPER_AMAP_LOW_RES_PIPELINE ? 1 : 2;
    const uint8_t request_zoom=map_scale::request_zoom(config.zoom);
    char url[512] = {};
    std::snprintf(url, sizeof(url),
                  "https://restapi.amap.com/v3/staticmap?location=%.6f,%.6f"
                  "&zoom=%u&size=%d*%d&scale=%d&key=%s",
                  config.longitude, config.latitude, static_cast<unsigned>(request_zoom),
                  request_width, request_height, server_scale, config.api_key.c_str());

    app_paths::ensure_dir(app_paths::map_cache_dir());
    const std::string temporary = std::string(app_paths::map_image_file()) + ".tmp";
    std::remove(temporary.c_str());

    if (cancelled(cancel) || !ensure_request_time(&result.error)) return result;
    constexpr size_t SD_WRITE_BUFFER_BYTES = 32 * 1024;
    // Keep the stdio buffer alive through every fclose, including early exits.
    // Network reads remain small/cancellable; SD writes are coalesced separately.
    std::unique_ptr<char, decltype(&std::free)> sd_buffer(
        static_cast<char *>(heap_caps_malloc(SD_WRITE_BUFFER_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), &std::free);
    DownloadSink sink;
    esp_err_t perform_error = ESP_FAIL;
    int64_t request_time_ms = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (cancelled(cancel)) break;
        std::remove(temporary.c_str());
        sink = DownloadSink{};
        sink.file = std::fopen(temporary.c_str(), "wb");
        if (!sink.file) {
            result.error = "Failed to create map cache file";
            return result;
        }
        const size_t sd_buffer_bytes = sd_buffer &&
            std::setvbuf(sink.file, sd_buffer.get(), _IOFBF, SD_WRITE_BUFFER_BYTES) == 0
                ? SD_WRITE_BUFFER_BYTES : 0;

        esp_http_client_handle_t client = static_cast<esp_http_client_handle_t>(_client);
        const bool reuse_attempt = client != nullptr;
        if (!client) {
            esp_http_client_config_t http_config = {};
            http_config.url = url;
            http_config.method = HTTP_METHOD_GET;
            http_config.timeout_ms = 3000;
            http_config.crt_bundle_attach = esp_crt_bundle_attach;
            http_config.user_agent = "MyPaperP4 Map";
            http_config.buffer_size = 4096;
            http_config.event_handler = http_event;
            http_config.user_data = &sink;
            http_config.keep_alive_enable = true;
            http_config.keep_alive_idle = 15;
            http_config.keep_alive_interval = 5;
            http_config.keep_alive_count = 3;
            client = esp_http_client_init(&http_config);
            _client = client;
            if (client) {
                esp_http_client_set_header(client, "Accept",
                                           "image/png,image/jpeg,application/json");
                esp_http_client_set_header(client, "Connection", "keep-alive");
            }
        } else {
            esp_http_client_set_url(client, url);
            esp_http_client_set_user_data(client, &sink);
        }
        if (!client) {
            std::fclose(sink.file);
            sink.file = nullptr;
            result.error = "Failed to create AMap HTTPS client";
            return result;
        }

        if (!wait_for_request_slot(cancel)) {
            std::fclose(sink.file);
            sink.file = nullptr;
            break;
        }
        ESP_LOGI(TAG,
                 "static map begin center=%.6f,%.6f display_z=%u request_z=%u request=%dx%d scale=%d "
                 "transport=%s attempt=%d",
                 config.longitude, config.latitude, static_cast<unsigned>(config.zoom),
                 static_cast<unsigned>(request_zoom),
                 request_width, request_height, server_scale,
                 reuse_attempt ? "reuse" : "new", attempt + 1);
        const int64_t started = esp_timer_get_time();
        esp_http_client_set_timeout_ms(client, 3000);
        perform_error = esp_http_client_open(client, 0);
        const int64_t opened = esp_timer_get_time();
        if (perform_error == ESP_OK && !cancelled(cancel)) {
            if (esp_http_client_fetch_headers(client) < 0) perform_error = ESP_FAIL;
        }
        const int64_t headers = esp_timer_get_time();
        int64_t read_us = 0, write_us = 0, max_read_us = 0, first_byte_us = -1;
        unsigned read_calls = 0, again_count = 0;
        uint8_t buffer[4096];
        esp_http_client_set_timeout_ms(client, 1000);
        while (perform_error == ESP_OK && !cancelled(cancel)) {
            if (esp_timer_get_time() - started > 25000000) {
                perform_error = ESP_ERR_TIMEOUT;
                break;
            }
            const int64_t read_begin = esp_timer_get_time();
            const int count = esp_http_client_read(client, reinterpret_cast<char *>(buffer), sizeof(buffer));
            const int64_t read_end = esp_timer_get_time();
            const int64_t read_elapsed = read_end - read_begin;
            read_us += read_elapsed;
            max_read_us = std::max(max_read_us, read_elapsed);
            ++read_calls;
            if (count == -ESP_ERR_HTTP_EAGAIN) { ++again_count; continue; }
            if (count < 0) { perform_error = ESP_FAIL; break; }
            if (count == 0) {
                if (!esp_http_client_is_complete_data_received(client)) perform_error = ESP_FAIL;
                break;
            }
            if (static_cast<size_t>(count) > MAX_MAP_BYTES - sink.bytes) {
                sink.overflow = true; perform_error = ESP_FAIL; break;
            }
            if (first_byte_us < 0) first_byte_us = read_end - started;
            const int64_t write_begin = esp_timer_get_time();
            const size_t written = std::fwrite(buffer, 1, count, sink.file);
            write_us += esp_timer_get_time() - write_begin;
            if (written != static_cast<size_t>(count)) {
                sink.write_ok = false; perform_error = ESP_FAIL; break;
            }
            sink.bytes += count;
            sink.crc_state = map_crc::update(sink.crc_state, buffer, static_cast<size_t>(count));
        }
        if (cancelled(cancel)) perform_error = ESP_ERR_INVALID_STATE;
        request_time_ms = (esp_timer_get_time() - started) / 1000;
        result.http_status = esp_http_client_get_status_code(client);
        result.bytes = sink.bytes;
        const int64_t close_begin = esp_timer_get_time();
        if (std::fclose(sink.file) != 0) sink.write_ok = false;
        const int64_t file_close_us = esp_timer_get_time() - close_begin;
        sink.file = nullptr;
        esp_http_client_set_user_data(client, nullptr);

        ESP_LOGI(TAG, "static map timing: attempt=%d open=%lld headers=%lld read=%lld sd_write=%lld sd_close=%lld first_byte=%lld max_read=%lld us calls=%u again=%u sd_buffer=%u",
                 attempt + 1, opened-started, headers-opened, read_us, write_us,
                 file_close_us, first_byte_us, max_read_us, read_calls, again_count,
                 unsigned(sd_buffer_bytes));
        ESP_LOGI(TAG,
                 "static map transfer status=%d bytes=%u time=%lld ms socket=%s err=%s",
                 result.http_status, static_cast<unsigned>(result.bytes), request_time_ms,
                 sink.connected ? "new" : "reused", esp_err_to_name(perform_error));
        if (perform_error == ESP_OK) break;

        close();
        std::remove(temporary.c_str());
        if (cancelled(cancel)) break;
        if (attempt == 0) {
            ESP_LOGW(TAG, "persistent connection failed, rebuilding once");
            continue;
        }
    }

    if (perform_error != ESP_OK) {
        std::remove(temporary.c_str());
        if (cancelled(cancel)) { result.error = "Cancelled"; return result; }
        result.error = std::string("AMap connection failed: ") + esp_err_to_name(perform_error);
        return result;
    }
    const bool write_ok = sink.write_ok && !sink.overflow;
    const int64_t content_length = esp_http_client_get_content_length(
        static_cast<esp_http_client_handle_t>(_client));
    if (result.http_status < 200 || result.http_status >= 300 ||
        content_length > static_cast<int64_t>(MAX_MAP_BYTES)) {
        result.error = result.http_status < 200 || result.http_status >= 300
                           ? "AMap HTTP error " + std::to_string(result.http_status)
                           : "AMap image is too large";
        std::remove(temporary.c_str());
        return result;
    }

    result.format = write_ok ? probe_image_format(temporary.c_str())
                             : AmapImageFormat::Unknown;
    if (!write_ok || result.bytes == 0 || result.format == AmapImageFormat::Unknown) {
        if (write_ok && result.bytes > 0) result.error = describe_api_error(temporary.c_str());
        std::remove(temporary.c_str());
        if (!write_ok) result.error = "Failed while downloading map image";
        ESP_LOGW(TAG, "static map rejected status=%d bytes=%u time=%lld ms",
                 result.http_status, static_cast<unsigned>(result.bytes),
                 request_time_ms);
        return result;
    }

    // The UI commits only the current generation; stale downloads stay temporary.
    result.path = temporary;
    result.crc = ~sink.crc_state;
    result.success = true;
    ESP_LOGI(TAG, "static map complete status=%d bytes=%u format=%s time=%lld ms",
             result.http_status, static_cast<unsigned>(result.bytes),
             result.format == AmapImageFormat::Png ? "png" : "jpeg",
             request_time_ms);
    return result;
}
