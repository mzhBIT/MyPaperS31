#include "map/AmapClient.h"
#include "map/AmapImageProcessor.h"

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

namespace {

constexpr const char *TAG = "AMAP_HTTP";
constexpr int64_t MIN_REQUEST_INTERVAL_US = 500000;
constexpr size_t MAX_MAP_BYTES = 4 * 1024 * 1024;
constexpr std::time_t MIN_VALID_UNIX_TIME = 1704067200;
portMUX_TYPE s_rate_lock = portMUX_INITIALIZER_UNLOCKED;
int64_t s_next_request_us = 0;

void wait_for_request_slot()
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
        vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>((delay_us + 999) / 1000)));
    }
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
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 ||
        !sink->file || !sink->write_ok) {
        return ESP_OK;
    }
    const size_t count = static_cast<size_t>(event->data_len);
    if (count > MAX_MAP_BYTES - sink->bytes) {
        sink->overflow = true;
        sink->write_ok = false;
        return ESP_FAIL;
    }
    if (std::fwrite(event->data, 1, count, sink->file) != count) {
        sink->write_ok = false;
        return ESP_FAIL;
    }
    sink->bytes += count;
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

AmapMapResult AmapClient::fetch_static_map(const AmapConfig &config,
                                           int output_width,
                                           int output_height)
{
    AmapMapResult result;
    if (config.api_key.empty()) {
        result.error = "AMap API key is not configured";
        return result;
    }
    const int request_width = std::max(1, std::min(1024, (output_width + 1) / 2));
    const int request_height = std::max(1, std::min(1024, (output_height + 1) / 2));
    constexpr int server_scale = MYPAPER_AMAP_LOW_RES_PIPELINE ? 1 : 2;
    char url[512] = {};
    std::snprintf(url, sizeof(url),
                  "https://restapi.amap.com/v3/staticmap?location=%.6f,%.6f"
                  "&zoom=%u&size=%d*%d&scale=%d&key=%s",
                  config.longitude, config.latitude, static_cast<unsigned>(config.zoom),
                  request_width, request_height, server_scale, config.api_key.c_str());

    app_paths::ensure_dir(app_paths::map_cache_dir());
    const std::string temporary = std::string(app_paths::map_image_file()) + ".tmp";
    std::remove(temporary.c_str());

    if (!ensure_request_time(&result.error)) return result;
    DownloadSink sink;
    esp_err_t perform_error = ESP_FAIL;
    int64_t request_time_ms = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::remove(temporary.c_str());
        sink = DownloadSink{};
        sink.file = std::fopen(temporary.c_str(), "wb");
        if (!sink.file) {
            result.error = "Failed to create map cache file";
            return result;
        }

        esp_http_client_handle_t client = static_cast<esp_http_client_handle_t>(_client);
        const bool reuse_attempt = client != nullptr;
        if (!client) {
            esp_http_client_config_t http_config = {};
            http_config.url = url;
            http_config.method = HTTP_METHOD_GET;
            http_config.timeout_ms = 25000;
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

        wait_for_request_slot();
        ESP_LOGI(TAG,
                 "static map begin center=%.6f,%.6f zoom=%u request=%dx%d scale=%d "
                 "transport=%s attempt=%d",
                 config.longitude, config.latitude, static_cast<unsigned>(config.zoom),
                 request_width, request_height, server_scale,
                 reuse_attempt ? "reuse" : "new", attempt + 1);
        const int64_t started = esp_timer_get_time();
        perform_error = esp_http_client_perform(client);
        request_time_ms = (esp_timer_get_time() - started) / 1000;
        result.http_status = esp_http_client_get_status_code(client);
        result.bytes = sink.bytes;
        if (std::fclose(sink.file) != 0) sink.write_ok = false;
        sink.file = nullptr;
        esp_http_client_set_user_data(client, nullptr);

        ESP_LOGI(TAG,
                 "static map transfer status=%d bytes=%u time=%lld ms socket=%s err=%s",
                 result.http_status, static_cast<unsigned>(result.bytes), request_time_ms,
                 sink.connected ? "new" : "reused", esp_err_to_name(perform_error));
        if (perform_error == ESP_OK) break;

        close();
        std::remove(temporary.c_str());
        if (attempt == 0) {
            ESP_LOGW(TAG, "persistent connection failed, rebuilding once");
            continue;
        }
    }

    if (perform_error != ESP_OK) {
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

    std::remove(app_paths::map_image_file());
    if (std::rename(temporary.c_str(), app_paths::map_image_file()) != 0) {
        std::remove(temporary.c_str());
        result.error = "Failed to commit map cache file";
        return result;
    }
    result.path = app_paths::map_image_file();
    result.success = true;
    ESP_LOGI(TAG, "static map complete status=%d bytes=%u format=%s time=%lld ms",
             result.http_status, static_cast<unsigned>(result.bytes),
             result.format == AmapImageFormat::Png ? "png" : "jpeg",
             request_time_ms);
    return result;
}
