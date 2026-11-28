#include "xhs/XhsReader.h"

#include "renderer/TurboJpegDecoder.h"
#include "ui/Utf8Text.h"
#include "weread/WereadJson.h"
#include "wifi/WifiRequestPowerGuard.h"
#include "wifi/wifi_setting.h"

#include <M5GFX.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lgfx/utility/lgfx_miniz.h>
#include <nvs.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <strings.h>

namespace xhs {
namespace {

constexpr char tag[] = "XHS_READER";
constexpr char config_namespace[] = "xhs_reader";
constexpr unsigned psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr size_t json_limit = 128 * 1024;
constexpr size_t media_limit = 2 * 1024 * 1024;
constexpr size_t gray_limit = 1024 * 1024;
constexpr int request_timeout_ms = 7000;
namespace json = weread::json;

bool ascii_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool valid_id(std::string_view id)
{
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return ascii_alnum(c) || c == '-' || c == '_';
    });
}

std::string_view https_host(std::string_view url)
{
    if (url.substr(0, 8) != "https://") return {};
    const auto authority = url.substr(8, url.find_first_of("/?#", 8) - 8);
    if (authority.empty()) return {};
    const auto colon = authority.find(':');
    const auto host = authority.substr(0, colon);
    if (host.empty() || !std::all_of(host.begin(), host.end(), [](char c) {
            return ascii_alnum(c) || c == '.' || c == '-';
        })) return {};
    if (colon != std::string_view::npos) {
        const auto port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5) return {};
        unsigned number = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return {};
            number = number * 10 + (c - '0');
        }
        if (!number || number > 65535) return {};
    }
    return host;
}

bool clean_url(std::string_view url)
{
    return std::all_of(url.begin(), url.end(), [](unsigned char c) {
        return c > 0x20 && c < 0x7f && c != '\\' && c != '#';
    });
}

bool cdn_url(const std::string &url)
{
    if (!clean_url(url)) return false;
    const auto host = https_host(url);
    constexpr std::string_view suffix = ".xhscdn.com";
    return host == "ci.xiaohongshu.com" ||
        (host.size() > suffix.size() && host.substr(host.size() - suffix.size()) == suffix);
}

bool media_path(std::string_view path, std::string_view suffix)
{
    constexpr std::string_view prefix = "/v1/media/";
    if (path.substr(0, prefix.size()) != prefix ||
        path.size() != prefix.size() + 64 + suffix.size() ||
        path.substr(path.size() - suffix.size()) != suffix) return false;
    const auto id = path.substr(prefix.size(), 64);
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string read_setting(nvs_handle_t handle, const char *key, size_t limit)
{
    size_t size = 0;
    if (nvs_get_str(handle, key, nullptr, &size) != ESP_OK || size <= 1 || size > limit + 1)
        return {};
    std::string value(size, '\0');
    if (nvs_get_str(handle, key, value.data(), &size) != ESP_OK) return {};
    value.resize(size - 1);
    return value;
}

struct Response {
    std::unique_ptr<uint8_t, Image::Free> data;
    size_t size = 0;
    int status = 0;
    std::string content_type;
    bool complete = false;

    std::string_view json() const {
        return data ? std::string_view(reinterpret_cast<const char *>(data.get()), size)
                    : std::string_view{};
    }
};

std::string field(std::string_view object, const char *key, size_t limit)
{
    std::string_view encoded;
    std::string value;
    if (!json::member(object, key, &encoded) || encoded.size() > limit * 6 + 2 ||
        !json::string(encoded, &value) || !ui::utf8::valid(value)) return {};
    value.erase(std::remove(value.begin(), value.end(), '\0'), value.end());
    return value.size() > limit ? ui::utf8::truncate_bytes(value, limit) : value;
}

Response request(const std::string &url, const std::string *token, const std::string &body,
                 size_t limit, const std::atomic<bool> &cancel)
{
    if (cancel.load()) return {};
    Response result;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.method = body.empty() ? HTTP_METHOD_GET : HTTP_METHOD_POST;
    config.timeout_ms = request_timeout_ms;
    config.disable_auto_redirect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    config.user_data = &result;
    config.event_handler = [](esp_http_client_event_t *event) {
        if (event->event_id == HTTP_EVENT_ON_HEADER &&
            strcasecmp(event->header_key, "Content-Type") == 0) {
            static_cast<Response *>(event->user_data)->content_type =
                std::string(event->header_value, strnlen(event->header_value, 96));
        }
        return ESP_OK;
    };

    const auto cleanup = [](esp_http_client *client) { esp_http_client_cleanup(client); };
    std::unique_ptr<esp_http_client, decltype(cleanup)> client(esp_http_client_init(&config), cleanup);
    if (!client) return result;
    esp_http_client_set_header(client.get(), "Connection", "close");
    if (token) {
        const std::string authorization = "Bearer " + *token;
        if (esp_http_client_set_header(client.get(), "Authorization", authorization.c_str()) != ESP_OK)
            return result;
    }
    if (!body.empty()) esp_http_client_set_header(client.get(), "Content-Type", "application/json");

    const int64_t started = esp_timer_get_time();
    if (esp_http_client_open(client.get(), body.size()) != ESP_OK) return result;
    for (size_t written = 0; written < body.size() && !cancel.load();) {
        const int count = esp_http_client_write(client.get(), body.data() + written, body.size() - written);
        if (count <= 0) return result;
        written += count;
    }
    if (cancel.load()) return result;
    const int64_t length = esp_http_client_fetch_headers(client.get());
    result.status = esp_http_client_get_status_code(client.get());
    if ((length < 0 && !esp_http_client_is_chunked_response(client.get())) ||
        length > static_cast<int64_t>(limit)) return result;

    size_t capacity = 0;
    while (!cancel.load() && esp_timer_get_time() - started < 18000000 &&
           (length <= 0 || result.size < static_cast<size_t>(length))) {
        uint8_t block[2048];
        const int count = esp_http_client_read(client.get(), reinterpret_cast<char *>(block), sizeof(block));
        if (count <= 0) break;
        const size_t needed = result.size + count;
        if (needed > limit) return result;
        if (needed > capacity) {
            const size_t next = std::min(limit, std::max(needed,
                length > 0 ? static_cast<size_t>(length) : std::max<size_t>(4096, capacity * 2)));
            uint8_t *buffer = result.data.release();
            auto *resized = static_cast<uint8_t *>(heap_caps_realloc(buffer, next, psram));
            result.data.reset(resized ? resized : buffer);
            if (!resized) return result;
            capacity = next;
        }
        std::memcpy(result.data.get() + result.size, block, count);
        result.size = needed;
    }
    result.complete = !cancel.load() && esp_http_client_is_complete_data_received(client.get()) &&
        (length <= 0 || result.size == static_cast<size_t>(length));
    return result;
}

const char *action_name(Action action)
{
    switch (action) {
        case Action::Health: return "health";
        case Action::Open: return "open";
        case Action::Refresh: return "refresh";
        case Action::More: return "more";
        case Action::Detail: return "detail";
        case Action::Images: return "images";
    }
    return "";
}

std::string response_error(const Response &response)
{
    const auto error = field(response.json(), "detail", 64);
    if (valid_id(error)) return error;
    if (response.status == 401) return "invalid_token";
    if (response.status == 404) return "job_missing";
    if (response.status == 429) return "queue_full";
    return response.status ? "request_failed" : "network_error";
}

Media parse_media(std::string_view object)
{
    Media media;
    auto gray = field(object, "gray4", 128);
    if (media_path(gray, ".gray4")) media.gray4 = std::move(gray);
    auto cdn = field(object, "cdn_url", 2048);
    if (cdn_url(cdn)) media.cdn = std::move(cdn);
    return media;
}

// Walk the legacy string array using the same JSON string decoder.
std::vector<std::string> jpeg_paths(std::string_view array)
{
    std::vector<std::string> paths;
    if (array.empty() || array.front() != '[') return paths;
    size_t position = 1;
    while (paths.size() < 32) {
        const auto begin = array.find_first_not_of(" \t\r\n,", position);
        if (begin == std::string_view::npos || array[begin] != '"') break;
        size_t end = begin + 1;
        for (; end < array.size() && array[end] != '"'; ++end) {
            if (array[end] == '\\') ++end;
        }
        if (end >= array.size()) break;
        std::string path;
        json::string(array.substr(begin, end - begin + 1), &path);
        paths.push_back(media_path(path, ".jpg") ? std::move(path) : std::string{});
        position = end + 1;
    }
    return paths;
}

bool parse_result(Task &task, std::string_view result)
{
    if (task.action != Action::Detail) {
        task.session = field(result, "session_id", 64);
        std::string_view items;
        if (!valid_id(task.session) || !json::member(result, "items", &items) ||
            items.empty() || items.front() != '[') return false;
        json::ObjectIterator iterator(items);
        std::string_view item;
        while (task.cards.size() < batch_size && iterator.next(&item)) {
            Card card;
            card.id = field(item, "id", 64);
            if (!valid_id(card.id)) continue;
            card.title = field(item, "title", 240);
            card.author = field(item, "author", 120);
            card.likes = field(item, "likes", 24);
            std::string_view cover;
            if (json::member(item, "cover_media", &cover)) card.cover = parse_media(cover);
            auto jpeg = field(item, "cover", 128);
            if (media_path(jpeg, ".jpg")) card.cover.jpeg = std::move(jpeg);
            task.cards.push_back(std::move(card));
        }
        return true;
    }

    if (field(result, "id", 64) != task.note_id) return false;
    auto &note = task.note;
    note.title = field(result, "title", 360);
    note.author = field(result, "author", 120);
    note.body = field(result, "body", 48 * 1024);
    note.body.erase(std::remove(note.body.begin(), note.body.end(), '\r'), note.body.end());
    note.published_at = field(result, "published_at", 48);
    note.ip_location = field(result, "ip_location", 120);
    std::string_view counts, media, legacy;
    if (json::member(result, "counts", &counts)) note.likes = field(counts, "likes", 24);
    if (json::member(result, "image_media", &media)) {
        json::ObjectIterator iterator(media);
        std::string_view image;
        while (note.images.size() < 32 && iterator.next(&image)) note.images.push_back(parse_media(image));
    }
    if (json::member(result, "images", &legacy)) {
        auto paths = jpeg_paths(legacy);
        note.images.resize(std::max(note.images.size(), paths.size()));
        for (size_t i = 0; i < paths.size(); ++i) note.images[i].jpeg = std::move(paths[i]);
    }
    note.images.erase(std::remove_if(note.images.begin(), note.images.end(), [](const Media &m) {
        return m.gray4.empty() && m.cdn.empty() && m.jpeg.empty();
    }), note.images.end());
    return true;
}

void run_job(Task &task, const Config &config)
{
    if (task.action == Action::Health) {
        const auto result = request(config.address + "/v1/health", nullptr, {}, 2048, task.cancel);
        if (!result.complete || result.status != 200 || field(result.json(), "status", 24) != "ok")
            task.error = response_error(result);
        return;
    }

    char id[36];
    std::snprintf(id, sizeof(id), "%08lx%08lx%08lx%08lx",
        static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
    std::string body = std::string("{\"request_id\":\"") + id + "\",\"action\":\"" +
                       action_name(task.action) + '"';
    if (valid_id(task.session)) body += ",\"session_id\":\"" + task.session + '"';
    if (valid_id(task.note_id)) body += ",\"note_id\":\"" + task.note_id + '"';
    body += '}';

    const std::string url = config.address + "/v1/jobs";
    auto response = request(url, &config.token, body, 2048, task.cancel);
    if (!response.complete && (response.status == 0 || response.status == 202) && !task.cancel.load())
        response = request(url, &config.token, body, 2048, task.cancel);
    if (!response.complete || response.status != 202) {
        task.error = response_error(response);
        return;
    }
    const auto job_id = field(response.json(), "job_id", 64);
    response = {};
    if (!valid_id(job_id)) { task.error = "bad_response"; return; }

    const int64_t deadline = esp_timer_get_time() + 35000000;
    while (!task.cancel.load() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(400));
        response = request(url + '/' + job_id, &config.token, {}, json_limit, task.cancel);
        if (!response.complete && !response.status) continue;
        if (!response.complete || response.status != 200) {
            task.error = response_error(response);
            return;
        }
        const auto status = field(response.json(), "status", 16);
        if (status == "pending" || status == "running") continue;
        if (status == "error") {
            task.error = field(response.json(), "error", 64);
            if (!valid_id(task.error)) task.error = "request_failed";
            return;
        }
        std::string_view result;
        if (status != "done" || !json::member(response.json(), "result", &result) ||
            !parse_result(task, result)) task.error = "bad_response";
        return;
    }
    task.error = "timeout";
}

uint16_t be16(const uint8_t *p)
{
    return (uint16_t(p[0]) << 8) | p[1];
}

Image unpack_gray(const Response &response, int max_width, int max_height)
{
    if (response.size <= 8 || std::memcmp(response.data.get(), "G4Z1", 4) != 0) return {};
    const auto *data = response.data.get();
    const int width = be16(data + 4), height = be16(data + 6);
    if (width < 1 || height < 1 || width > max_width || height > max_height) return {};
    const size_t size = size_t((width + 1) / 2) * height;
    Image image;
    image.pixels.reset(static_cast<uint8_t *>(heap_caps_malloc(size, psram)));
    if (!image.pixels || lgfx_tinfl_decompress_mem_to_mem(image.pixels.get(), size,
            data + 8, response.size - 8, TINFL_FLAG_PARSE_ZLIB_HEADER) != size) return {};
    image.width = width;
    image.height = height;
    return image;
}

bool jpeg_size(const uint8_t *data, size_t length, int *width, int *height)
{
    if (length < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
    for (size_t position = 2; position + 4 <= length;) {
        if (data[position++] != 0xff) return false;
        while (position < length && data[position] == 0xff) ++position;
        if (position + 2 >= length) return false;
        const uint8_t mark = data[position++];
        if (mark == 0xda || mark == 0xd9) return false;
        const size_t size = be16(data + position);
        if (size < 2 || size > length - position) return false;
        if (mark == 0xc0 || mark == 0xc1 || mark == 0xc2) {
            if (size < 8) return false;
            *width = be16(data + position + 5);
            *height = be16(data + position + 3);
            return true;
        }
        position += size;
    }
    return false;
}

Image decode_photo(const Response &response, int box_width, int box_height)
{
    const auto *data = response.data.get();
    if (!data || response.size < 24) return {};
    int width = 0, height = 0;
    const bool jpeg = jpeg_size(data, response.size, &width, &height);
    const bool png = !jpeg && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0 &&
        std::memcmp(data + 12, "IHDR", 4) == 0;
    if (png) {
        auto dimension = [](const uint8_t *p) -> unsigned {
            return (unsigned(be16(p)) << 16) | be16(p + 2);
        };
        if (dimension(data + 16) > 4096 || dimension(data + 20) > 4096) return {};
        width = dimension(data + 16);
        height = dimension(data + 20);
    }
    if (width <= 0 || height <= 0 || int64_t(width) * height > 24000000) return {};
    const float scale = std::min({1.0f, float(box_width) / width, float(box_height) / height});
    Image image;
    image.width = std::max(1, int(width * scale));
    image.height = std::max(1, int(height * scale));
    const int stride = (image.width + 1) / 2;
    const size_t size = size_t(stride) * image.height;

    if (jpeg) {
        std::vector<uint8_t> pixels;
        if (!turbo_jpeg::decode_memory_gray4(data, response.size, "xhs", image.width, image.height,
                &pixels) || pixels.size() != size) return {};
        image.pixels.reset(static_cast<uint8_t *>(heap_caps_malloc(size, psram)));
        if (!image.pixels) return {};
        std::memcpy(image.pixels.get(), pixels.data(), size);
    } else {
        LGFX_Sprite sprite;
        sprite.setPsram(true);
        sprite.setColorDepth(16);
        if (!sprite.createSprite(image.width, image.height)) return {};
        sprite.fillScreen(TFT_WHITE);
        if (!sprite.drawPng(data, response.size, 0, 0, image.width, image.height, 0, 0, scale, scale))
            return {};
        image.pixels.reset(static_cast<uint8_t *>(heap_caps_malloc(size, psram)));
        if (!image.pixels) return {};
        for (int y = 0; y < image.height; ++y) {
            auto *row = image.pixels.get() + y * stride;
            for (int x = 0; x < image.width; x += 2) {
                auto gray = [&](int px) {
                    const uint16_t color = sprite.readPixel(px, y);
                    const unsigned r = (color >> 11) * 255 / 31;
                    const unsigned g = ((color >> 5) & 63) * 255 / 63;
                    const unsigned b = (color & 31) * 255 / 31;
                    return (r*77 + g*150 + b*29) >> 12;
                };
                row[x / 2] = (gray(x) << 4) | (x + 1 < image.width ? gray(x + 1) : 15);
            }
        }
    }
    return image;
}

Image load_image(const Config &config, const ImageRequest &image, const std::atomic<bool> &cancel)
{
    if (image.width < 1 || image.height < 1 || image.width > 1400 || image.height > 1400) return {};
    const auto &media = image.media;
    auto download = [&](const std::string &url, const std::string *token, bool gray, const char *source) {
        const int64_t begin = esp_timer_get_time();
        const auto response = request(url, token, {}, gray ? gray_limit : media_limit, cancel);
        Image result;
        if (response.complete && response.status == 200 && !cancel.load()) {
            if (gray && response.content_type.find("application/octet-stream") == 0)
                result = unpack_gray(response, image.width, image.height);
            else if (!gray && (response.content_type.empty() ||
                response.content_type.find("image/") == 0 ||
                response.content_type.find("application/octet-stream") == 0)) {
                result = decode_photo(response, image.width, image.height);
            }
        }
        ESP_LOGI(tag, "image path=%s status=%d bytes=%u size=%dx%d time=%lld ms ok=%d",
            source, response.status, unsigned(response.size), result.width, result.height,
            static_cast<long long>((esp_timer_get_time() - begin) / 1000), bool(result.pixels));
        return result;
    };

    if (config.image_mode == ImageMode::Cdn && !media.cdn.empty()) {
        auto result = download(media.cdn, nullptr, false, "cdn");
        if (result.pixels) return result;
    }
    if (!media.gray4.empty() && !cancel.load()) {
        const auto dimensions = "?width=" + std::to_string(image.width) + "&height=" +
                                std::to_string(image.height);
        auto result = download(config.address + media.gray4 + dimensions, &config.token, true, "gray4");
        if (result.pixels) return result;
    }
    if (!media.jpeg.empty() && !cancel.load())
        return download(config.address + media.jpeg, &config.token, false, "nas-jpeg");
    return {};
}

} // namespace

bool valid_address(const std::string &address)
{
    return address.size() <= address_limit && clean_url(address) &&
        !https_host(address).empty() && address.find('?') == std::string::npos;
}

bool valid_token(const std::string &token)
{
    return token.size() >= 16 && token.size() <= token_limit &&
        std::all_of(token.begin(), token.end(), [](unsigned char c) {
            return c >= 0x21 && c <= 0x7e;
        });
}

bool Config::ready() const { return valid_address(address) && valid_token(token); }

Config load_config()
{
    Config config;
    nvs_handle_t handle;
    if (nvs_open(config_namespace, NVS_READONLY, &handle) != ESP_OK) return config;
    config.address = read_setting(handle, "address", address_limit);
    config.token = read_setting(handle, "token", token_limit);
    uint8_t mode = 0;
    if (nvs_get_u8(handle, "image_mode", &mode) == ESP_OK && mode == uint8_t(ImageMode::Cdn))
        config.image_mode = ImageMode::Cdn;
    nvs_close(handle);
    return config;
}

bool save_config(const Config &config)
{
    if ((!config.address.empty() && !valid_address(config.address)) ||
        (!config.token.empty() && !valid_token(config.token))) return false;
    nvs_handle_t handle;
    if (nvs_open(config_namespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_str(handle, "address", config.address.c_str());
    if (result == ESP_OK) result = nvs_set_str(handle, "token", config.token.c_str());
    if (result == ESP_OK) result = nvs_set_u8(handle, "image_mode", uint8_t(config.image_mode));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

void Image::Free::operator()(uint8_t *data) const { heap_caps_free(data); }

class Network {
    std::mutex mutex;
    bool used = false;
public:
    bool connect() {
        std::lock_guard<std::mutex> lock(mutex);
        if (wifi_setting_is_connected()) {
            used = true;
            return true;
        }

        // Starting the radio can write PHY calibration data to flash. Keep
        // that call on an internal stack while HTTP/image processing uses PSRAM.
        struct Startup {
            SemaphoreHandle_t ready;
            esp_err_t result = ESP_FAIL;
        } startup{xSemaphoreCreateBinary()};
        if (!startup.ready) return false;
        const auto entry = [](void *argument) {
            auto &request = *static_cast<Startup *>(argument);
            request.result = wifi_setting_connect_saved(10000);
            xSemaphoreGive(request.ready);
            vTaskDelete(nullptr);
        };
        if (xTaskCreate(entry, "xhs_network", 6144, &startup, 3, nullptr) != pdPASS) {
            vSemaphoreDelete(startup.ready);
            return false;
        }
        xSemaphoreTake(startup.ready, portMAX_DELAY);
        vSemaphoreDelete(startup.ready);
        used = true;
        return startup.result == ESP_OK;
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        if (used) {
            used = false;
            wifi_setting_release();
        }
    }
    ~Network() { release(); }
};

std::shared_ptr<Network> make_network() { return std::make_shared<Network>(); }

namespace {
struct Work {
    std::shared_ptr<Task> task;
    Config config;
    std::shared_ptr<Network> network;
};

void work_entry(void *argument)
{
    {
        std::unique_ptr<Work> work(static_cast<Work *>(argument));
        const auto task = work->task;
        auto &config = work->config;
        if (!task->cancel.load()) {
            if (!work->network->connect()) task->error = "network_error";
            else if (!task->cancel.load()) {
                wifi::RequestPowerGuard power(tag, wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_WIFI);
                if (task->action == Action::Images) {
                    for (size_t i = 0; i < task->image_count && !task->cancel.load(); ++i) {
                        task->images[i] = load_image(config, task->requests[i], task->cancel);
                        task->images_ready.store(i + 1, std::memory_order_release);
                    }
                } else run_job(*task, config);
            }
        }
        if (task->action != Action::Images)
            ESP_LOGI(tag, "action=%s result=%s", action_name(task->action),
                task->error.empty() ? "ok" : task->error.c_str());
        work.reset();
        task->done.store(true, std::memory_order_release);
    }
    vTaskDeleteWithCaps(nullptr);
}
} // namespace

bool start(const std::shared_ptr<Task> &task, const Config &config,
           const std::shared_ptr<Network> &network)
{
    auto *work = new Work{task, config, network};
    if (xTaskCreatePinnedToCoreWithCaps(work_entry, "xhs_reader", 24576, work, 3,
            nullptr, 0, psram) == pdPASS) return true;
    delete work;
    return false;
}

} // namespace xhs
