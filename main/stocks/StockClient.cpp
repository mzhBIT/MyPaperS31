#include "StockClient.h"
#include "parser/gbk_decode.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <nvs.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace stocks {
bool normalize_symbol(const std::string &input, std::string &out)
{
    std::string s;
    for (unsigned char c : input) {
        if (std::isspace(c)) continue;
        if (!std::isalnum(c) && c != '.' && c != '-') return false;
        s += static_cast<char>(std::tolower(c));
    }
    if (s.empty() || s.size() > 18) return false;
    const auto digits = [](const std::string &v) {
        return std::all_of(v.begin(), v.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    };
    if (s.size() == 6 && digits(s)) {
        s = ((s[0] == '6' || s[0] == '9') ? "sh" :
             (s[0] == '4' || s[0] == '8') ? "bj" : "sz") + s;
    }
    if (s.compare(0, 2, "sh") == 0 || s.compare(0, 2, "sz") == 0 || s.compare(0, 2, "bj") == 0) {
        if (s.size() != 8 || !digits(s.substr(2))) return false;
    } else {
        if (s.compare(0, 2, "us") != 0) s = "us" + s;
        if (s.size() <= 2 || !std::isalpha(static_cast<unsigned char>(s[2]))) return false;
        for (size_t i = 2; i < s.size(); ++i) s[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
    }
    out = s;
    return true;
}

static std::string decode_name(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = static_cast<unsigned char>(s[i++]);
        if (cp >= 0x80) {
            cp = i < s.size() ? gbk_decode_codepoint(cp, static_cast<unsigned char>(s[i++])) : 0xFFFD;
            if (!cp) cp = 0xFFFD;
        }
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63)); }
        else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
    }
    return out;
}
static bool number(const std::string &s, double &v)
{
    if (s.empty()) return false;
    char *end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(v);
}
std::vector<Quote> parse_quotes(const std::string &body, const std::vector<std::string> &symbols)
{
    std::vector<Quote> result;
    for (const auto &symbol : symbols) {
        Quote q; q.symbol = symbol;
        const size_t begin = body.find("v_" + symbol + "=\"");
        if (begin != std::string::npos) {
            const size_t start = body.find('"', begin) + 1;
            const size_t end = body.find('"', start);
            std::vector<std::string> fields;
            if (end != std::string::npos) {
                size_t p = start;
                while (p <= end) {
                    size_t next = body.find('~', p);
                    if (next == std::string::npos || next > end) next = end;
                    fields.push_back(body.substr(p, next - p));
                    if (next == end) break;
                    p = next + 1;
                }
            }
            if (fields.size() > 34) {
                q.name = decode_name(fields[1]); q.time = fields[30];
                q.valid = number(fields[3], q.last) && number(fields[4], q.previous) && q.last > 0 && q.previous > 0;
                number(fields[5], q.open); number(fields[33], q.high); number(fields[34], q.low);
            }
        }
        result.push_back(std::move(q));
    }
    return result;
}
bool fetch(const std::vector<std::string> &symbols, std::vector<Quote> &quotes, std::string &error)
{
    if (symbols.empty() || symbols.size() > 20) { error = "Invalid watchlist"; return false; }
    std::string url = "https://qt.gtimg.cn/q=";
    for (const auto &s : symbols) {
        std::string safe;
        if (!normalize_symbol(s, safe) || safe != s) { error = "Invalid symbol"; return false; }
        if (url.back() != '=') url += ',';
        url += s;
    }
    esp_http_client_config_t config = {};
    config.url = url.c_str(); config.timeout_ms = 8000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    auto client = esp_http_client_init(&config);
    if (!client) { error = "HTTP allocation failed"; return false; }
    esp_http_client_set_header(client, "Referer", "https://gu.qq.com/");
    const int64_t started = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, 0);
    std::string body;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) { error = "HTTP " + std::to_string(status); err = ESP_FAIL; }
        char buffer[1024];
        while (err == ESP_OK) {
            if (esp_timer_get_time() - started > 15000000) { error = "Request deadline exceeded"; err = ESP_ERR_TIMEOUT; break; }
            const int read = esp_http_client_read(client, buffer, sizeof(buffer));
            if (read < 0) { err = ESP_FAIL; break; }
            if (!read) break;
            if (body.size() + read > 32768) { error = "Response too large"; err = ESP_FAIL; break; }
            body.append(buffer, read);
        }
        if (err == ESP_OK && !esp_http_client_is_complete_data_received(client)) err = ESP_FAIL;
    }
    esp_http_client_close(client); esp_http_client_cleanup(client);
    ESP_LOGI("STOCKS_HTTP", "quotes=%u bytes=%u time=%lld ms result=%s", static_cast<unsigned>(symbols.size()),
             static_cast<unsigned>(body.size()), (esp_timer_get_time() - started) / 1000, esp_err_to_name(err));
    if (err != ESP_OK) { if (error.empty()) error = esp_err_to_name(err); return false; }
    quotes = parse_quotes(body, symbols);
    if (std::none_of(quotes.begin(), quotes.end(), [](const Quote &q) { return q.valid; })) {
        error = "No quotes returned"; return false;
    }
    return true;
}
std::vector<std::string> load_watchlist()
{
    nvs_handle_t handle;
    std::vector<std::string> result;
    if (nvs_open("stocks", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = 0;
        const esp_err_t loaded = nvs_get_str(handle, "watchlist", nullptr, &size);
        if (loaded == ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            return {"sh000001", "sz399001", "sh600519", "usAAPL", "usMSFT"};
        }
        if (loaded == ESP_OK && size > 0 && size <= 512) {
            std::string csv(size, '\0');
            if (nvs_get_str(handle, "watchlist", &csv[0], &size) == ESP_OK) {
                size_t start = 0;
                while (start < csv.size() - 1 && result.size() < 20) {
                    const size_t end = csv.find(',', start);
                    std::string s;
                    if (normalize_symbol(csv.substr(start, (end == std::string::npos ? csv.size() - 1 : end) - start), s) &&
                        std::find(result.begin(), result.end(), s) == result.end()) result.push_back(s);
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
            }
        }
        nvs_close(handle);
        // A stored empty list is intentional.
        return result;
    }
    return {"sh000001", "sz399001", "sh600519", "usAAPL", "usMSFT"};
}
bool save_watchlist(const std::vector<std::string> &symbols)
{
    std::string csv;
    for (const auto &s : symbols) { if (!csv.empty()) csv += ','; csv += s; }
    nvs_handle_t handle;
    if (nvs_open("stocks", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(handle, "watchlist", csv.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}
}
