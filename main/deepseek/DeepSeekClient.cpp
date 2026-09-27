#include "deepseek/DeepSeekClient.h"

#include "wifi/wifi_setting.h"

#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace {

constexpr const char *TAG = "DEEPSEEK_API";
constexpr const char *CHAT_URL = "https://api.deepseek.com/chat/completions";
constexpr const char *MODELS_URL = "https://api.deepseek.com/models";
// A 4 MiB source image expands to about 5.34 MiB in Base64. This remains well
// under DeepSeek's 48 MiB inline limit while keeping PSRAM use bounded.
constexpr size_t MAX_REQUEST_BYTES = 6 * 1024 * 1024;
constexpr size_t MAX_INLINE_IMAGE_BYTES = 4 * 1024 * 1024;
constexpr size_t MAX_RESPONSE_BYTES = 256 * 1024;

class WifiShutdownGuard {
public:
    ~WifiShutdownGuard() { wifi_setting_release(); }
};

std::string json_escape(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 32);
    char escaped[7];
    for (unsigned char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                out += escaped;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

const char *image_mime_type(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return nullptr;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    return nullptr;
}

bool base64_image(const std::string &path, std::string *encoded, std::string *error)
{
    const char *mime = image_mime_type(path);
    struct stat st = {};
    if (!mime || stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        *error = "Attached image cannot be opened";
        return false;
    }
    if (static_cast<size_t>(st.st_size) > MAX_INLINE_IMAGE_BYTES) {
        *error = "Attached image exceeds the 4 MiB device limit";
        return false;
    }
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) { *error = "Attached image cannot be read"; return false; }
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    encoded->clear();
    encoded->reserve(std::strlen(mime) + 32 + (static_cast<size_t>(st.st_size) + 2) / 3 * 4);
    *encoded = std::string("data:") + mime + ";base64,";
    uint8_t bytes[3];
    while (true) {
        const size_t count = fread(bytes, 1, sizeof(bytes), file);
        if (!count) break;
        encoded->push_back(table[bytes[0] >> 2]);
        encoded->push_back(table[((bytes[0] & 0x03) << 4) | (count > 1 ? bytes[1] >> 4 : 0)]);
        encoded->push_back(count > 1 ? table[((bytes[1] & 0x0f) << 2) | (count > 2 ? bytes[2] >> 6 : 0)] : '=');
        encoded->push_back(count > 2 ? table[bytes[2] & 0x3f] : '=');
        if (count < sizeof(bytes)) break;
    }
    const bool ok = !ferror(file);
    fclose(file);
    if (!ok) { encoded->clear(); *error = "Attached image read failed"; }
    return ok;
}

bool encoded_message(const DeepSeekMessage &message, std::string *out, std::string *error)
{
    if (message.image_path.empty()) {
        *out = "{\"role\":\"" + json_escape(message.role) +
               "\",\"content\":\"" + json_escape(message.content) + "\"}";
        return true;
    }
    std::string image;
    if (!base64_image(message.image_path, &image, error)) return false;
    *out = "{\"role\":\"" + json_escape(message.role) + "\",\"content\":[{\"type\":\"text\",\"text\":\"" +
           json_escape(message.content) + "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"" +
           image + "\"}}]}";
    return true;
}

bool build_request(DeepSeekModel model, bool thinking,
                   const std::vector<DeepSeekMessage> &messages,
                   std::string *body, std::string *error)
{
    std::vector<std::string> encoded;
    encoded.reserve(messages.size());
    size_t used = 320;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (!it->image_path.empty() && model != DeepSeekModel::Flash) {
            *error = "Image understanding requires V4 Flash";
            return false;
        }
        std::string item;
        if (!encoded_message(*it, &item, error)) return false;
        if (used + item.size() + 1 > MAX_REQUEST_BYTES) break;
        used += item.size() + 1;
        encoded.push_back(std::move(item));
    }
    std::reverse(encoded.begin(), encoded.end());
    if (!encoded.empty()) {
        size_t first_index = messages.size() - encoded.size();
        if (messages[first_index].role == "assistant") encoded.erase(encoded.begin());
    }

    body->clear();
    body->reserve(used);
    *body = "{\"model\":\"";
    *body += deepseek_model_id(model);
    *body += "\",\"messages\":[";
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (i) body->push_back(',');
        *body += encoded[i];
    }
    *body += thinking
                ? "],\"thinking\":{\"type\":\"enabled\"},\"reasoning_effort\":\"high\","
                : "],\"thinking\":{\"type\":\"disabled\"},";
    *body += "\"stream\":true,\"max_tokens\":2048}";
    return true;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_hex4(const std::string &text, size_t pos, uint32_t *value)
{
    if (pos + 4 > text.size()) return false;
    uint32_t result = 0;
    for (size_t i = 0; i < 4; ++i) {
        int digit = hex_value(text[pos + i]);
        if (digit < 0) return false;
        result = (result << 4) | static_cast<uint32_t>(digit);
    }
    *value = result;
    return true;
}

void append_utf8(std::string *out, uint32_t cp)
{
    if (cp <= 0x7f) out->push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

bool json_string_value(const std::string &json, const char *key, std::string *value)
{
    std::string marker = std::string("\"") + key + "\":";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;
    pos += marker.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    std::string out;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (c == '"') {
            *value = std::move(out);
            return true;
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++pos >= json.size()) return false;
        c = json[pos];
        if (c == 'n') out.push_back('\n');
        else if (c == 'r') out.push_back('\r');
        else if (c == 't') out.push_back('\t');
        else if (c == 'b') out.push_back('\b');
        else if (c == 'f') out.push_back('\f');
        else if (c == '"' || c == '\\' || c == '/') out.push_back(c);
        else if (c == 'u') {
            uint32_t cp = 0;
            if (!parse_hex4(json, pos + 1, &cp)) return false;
            pos += 4;
            if (cp >= 0xd800 && cp <= 0xdbff && pos + 6 < json.size() &&
                json[pos + 1] == '\\' && json[pos + 2] == 'u') {
                uint32_t low = 0;
                if (parse_hex4(json, pos + 3, &low) && low >= 0xdc00 && low <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    pos += 6;
                }
            }
            append_utf8(&out, cp);
        }
    }
    return false;
}

bool consume_sse_line(const std::string &line, std::string *content)
{
    if (line.rfind("data:", 0) != 0) return false;
    size_t pos = 5;
    while (pos < line.size() && line[pos] == ' ') ++pos;
    if (line.compare(pos, 6, "[DONE]") == 0) return true;
    std::string delta;
    if (json_string_value(line.substr(pos), "content", &delta) &&
        content->size() + delta.size() <= MAX_RESPONSE_BYTES) {
        content->append(delta);
    }
    return false;
}

size_t utf8_codepoint_count(const std::string &text)
{
    size_t count = 0;
    for (size_t pos = 0; pos < text.size(); ++count) {
        const unsigned char c = static_cast<unsigned char>(text[pos]);
        size_t bytes = (c & 0x80) == 0 ? 1
                     : (c & 0xe0) == 0xc0 ? 2
                     : (c & 0xf0) == 0xe0 ? 3
                     : (c & 0xf8) == 0xf0 ? 4 : 1;
        pos += std::min(bytes, text.size() - pos);
    }
    return count;
}

std::string take_utf8_prefix(std::string *text, size_t codepoints)
{
    size_t pos = 0;
    for (size_t count = 0; pos < text->size() && count < codepoints; ++count) {
        const unsigned char c = static_cast<unsigned char>((*text)[pos]);
        size_t bytes = (c & 0x80) == 0 ? 1
                     : (c & 0xe0) == 0xc0 ? 2
                     : (c & 0xf0) == 0xe0 ? 3
                     : (c & 0xf8) == 0xf0 ? 4 : 1;
        pos += std::min(bytes, text->size() - pos);
    }
    std::string prefix = text->substr(0, pos);
    text->erase(0, pos);
    return prefix;
}

}  // namespace

DeepSeekClientResult DeepSeekClient::validate(const DeepSeekConfig &config)
{
    DeepSeekClientResult result;
    if (config.api_key.empty()) {
        result.error = "DeepSeek API key is not configured";
        return result;
    }
    esp_err_t wifi_err = wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
    WifiShutdownGuard wifi_guard;
    if (wifi_err != ESP_OK) {
        result.error = std::string("WiFi connection failed: ") + esp_err_to_name(wifi_err);
        return result;
    }

    esp_http_client_config_t http_config = {};
    http_config.url = MODELS_URL;
    http_config.method = HTTP_METHOD_GET;
    http_config.timeout_ms = 20000;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.user_agent = "MyPaperP4 DeepSeek";
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        result.error = "Failed to create HTTPS client";
        return result;
    }
    std::string authorization = "Bearer " + config.api_key;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Connection", "close");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        result.error = std::string("HTTPS request failed: ") + esp_err_to_name(err);
        esp_http_client_cleanup(client);
        return result;
    }
    esp_http_client_fetch_headers(client);
    result.http_status = esp_http_client_get_status_code(client);
    char buffer[1024];
    std::string body;
    while (body.size() < 16 * 1024) {
        int count = esp_http_client_read(client, buffer, sizeof(buffer));
        if (count <= 0) break;
        body.append(buffer, std::min<size_t>(count, 16 * 1024 - body.size()));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result.http_status < 200 || result.http_status >= 300) {
        std::string api_error;
        result.error = json_string_value(body, "message", &api_error)
                           ? api_error
                           : "DeepSeek HTTP error " + std::to_string(result.http_status);
        return result;
    }
    bool flash = body.find("deepseek-v4-flash") != std::string::npos;
    bool pro = body.find("deepseek-v4-pro") != std::string::npos;
    if (!flash || !pro) {
        result.error = "API key is valid, but Flash or Pro is unavailable";
        return result;
    }
    result.success = true;
    result.content = "API key valid: V4 Flash and V4 Pro available";
    return result;
}

DeepSeekClientResult DeepSeekClient::chat(const DeepSeekConfig &config,
                                          DeepSeekModel model,
                                          bool thinking,
                                          const std::vector<DeepSeekMessage> &messages,
                                          const volatile bool *cancel_requested,
                                          DeepSeekStreamCallback stream_callback,
                                          void *stream_context)
{
    DeepSeekClientResult result;
    if (config.api_key.empty()) {
        result.error = "DeepSeek API key is not configured";
        return result;
    }
    if (messages.empty()) {
        result.error = "Conversation is empty";
        return result;
    }

    // This function runs on a PSRAM-backed task stack. NVS/flash access can
    // disable the external-memory cache, so the Activity must establish Wi-Fi
    // from its internal stack before creating this worker.
    if (!wifi_setting_is_connected()) {
        result.error = "WiFi was not prepared before starting the chat task";
        return result;
    }
    if (cancel_requested && *cancel_requested) {
        result.canceled = true;
        result.error = "Canceled";
        return result;
    }

    std::string request;
    if (!build_request(model, thinking, messages, &request, &result.error)) return result;
    esp_http_client_config_t http_config = {};
    http_config.url = CHAT_URL;
    http_config.method = HTTP_METHOD_POST;
    http_config.timeout_ms = 30000;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.user_agent = "MyPaperP4 DeepSeek";
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        result.error = "Failed to create HTTPS client";
        return result;
    }

    std::string authorization = "Bearer " + config.api_key;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_open(client, request.size());
    if (err == ESP_OK) {
        int offset = 0;
        while (offset < static_cast<int>(request.size())) {
            int written = esp_http_client_write(client, request.data() + offset,
                                                request.size() - offset);
            if (written <= 0) {
                err = ESP_FAIL;
                break;
            }
            offset += written;
        }
    }
    if (err != ESP_OK) {
        result.error = std::string("HTTPS request failed: ") + esp_err_to_name(err);
        esp_http_client_cleanup(client);
        return result;
    }

    esp_http_client_fetch_headers(client);
    result.http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "response status=%d model=%s", result.http_status,
             deepseek_model_id(model));

    char buffer[2048];
    std::string pending;
    std::string stream_batch;
    std::string error_body;
    bool stream_done = false;
    bool stream_delivery_failed = false;
    while (!stream_done && !stream_delivery_failed &&
           !(cancel_requested && *cancel_requested)) {
        int count = esp_http_client_read(client, buffer, sizeof(buffer));
        if (count <= 0) break;
        if (result.http_status >= 200 && result.http_status < 300) {
            pending.append(buffer, count);
            size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t content_before = result.content.size();
                stream_done = consume_sse_line(line, &result.content) || stream_done;
                if (result.content.size() > content_before) {
                    stream_batch.append(result.content, content_before,
                                        result.content.size() - content_before);
                    while (stream_callback && utf8_codepoint_count(stream_batch) >= 30) {
                        std::string chunk = take_utf8_prefix(&stream_batch, 30);
                        if (!stream_callback(stream_context, chunk)) {
                            stream_delivery_failed = true;
                            break;
                        }
                    }
                }
                if (stream_delivery_failed) break;
            }
        } else if (error_body.size() < 8192) {
            error_body.append(buffer, std::min<size_t>(count, 8192 - error_body.size()));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!stream_delivery_failed && stream_callback && !stream_batch.empty()) {
        stream_delivery_failed = !stream_callback(stream_context, stream_batch);
    }

    if (cancel_requested && *cancel_requested) {
        result.canceled = true;
        result.error = "Canceled";
        return result;
    }
    if (stream_delivery_failed) {
        result.error = "DeepSeek stream display was interrupted";
        return result;
    }
    if (result.http_status < 200 || result.http_status >= 300) {
        std::string api_error;
        result.error = json_string_value(error_body, "message", &api_error)
                           ? api_error
                           : "DeepSeek HTTP error " + std::to_string(result.http_status);
        return result;
    }
    if (!stream_done) {
        result.error = "DeepSeek response was interrupted";
        return result;
    }
    if (result.content.empty()) {
        result.error = "DeepSeek returned an empty response";
        return result;
    }
    result.success = true;
    return result;
}
