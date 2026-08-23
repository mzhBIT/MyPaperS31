#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "reader_font.h"
#include "reader_style.h"
#include "renderer/DocumentLayout.h"
#include "renderer/TextRenderer.h"
#include "storage/app_paths.h"
#include "ui/SystemFont.h"
#include "wifi/wifi_setting.h"

#include <M5Unified.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

static const char *TAG = "RSS";
static const char *WIFI_CONFIG_PATH = "/sdcard/wifi.txt";
static const char *RSS_CONFIG_PATH = "/sdcard/rss.txt";
static const char *DEFAULT_STA_SSID = "TP-LINK_A027";
static const char *DEFAULT_STA_PASSWORD = "18557530408";
static const int TOP_H = 50;
static const int FEED_ROW_H = 78;
static const int NEWS_ROW_H = 150;
static const int PAD = 12;
static const int MAX_FEED_BYTES = 256 * 1024;
static const int MAX_IMAGE_BYTES = 2 * 1024 * 1024;
static const int MAX_RSS_IMAGES_PER_ITEM = 6;
static const char *RSS_IMAGE_SAVE_DIR = "/sdcard/images";

struct RssImageRef {
    std::string url;
    std::string before_text;
};

struct RssFeed {
    std::string title;
    std::string url;
};

struct RssItem {
    std::string title;
    std::string link;
    std::string date;
    std::string text;
    std::string image_url;
    std::string image_before_text;
    std::vector<RssImageRef> images;
};

struct ArticleImage {
    std::string url;
    std::string before_text;
    std::string path;
    std::string ext;
    int width = 0;
    int height = 0;
    bool progressive = false;
};

static std::vector<RssFeed> g_feeds;
static std::vector<RssItem> g_items;
static int g_focus = 0;
static int g_scroll = 0;
static int g_page = 0;
static std::vector<ArticleImage> g_article_images;
static document_layout::Pages g_article_pages;
static std::string g_status;

extern void launch_reader_settings();

static epd_mode_t reader_epd_mode()
{
    switch (reader_style_get().refresh_mode) {
    case 1:
        return epd_mode_t::epd_quality;
    case 2:
        return epd_mode_t::epd_text;
    case 3:
        return epd_mode_t::epd_fast;
    case 4:
        return epd_mode_t::epd_fastest;
    default:
        return epd_mode_t::epd_fast;
    }
}

static std::string trim_copy(const std::string &s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start])) start++;
    size_t end = s.size();
    while (end > start && std::isspace((unsigned char)s[end - 1])) end--;
    return s.substr(start, end - start);
}

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t hash32_string(const std::string &s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

static std::string lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static void log_tls_heap(const char *phase)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "TLS heap %s: internal=%u largest=%u dma=%u dma_largest=%u spiram=%u",
             phase ? phase : "?",
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)dma_free,
             (unsigned)dma_largest,
             (unsigned)spiram_free);
}

struct HttpDocumentEndDetector {
    enum class Kind { Unknown, Xml, Json };

    Kind kind = Kind::Unknown;
    std::string xml_tail;
    int json_depth = 0;
    bool json_string = false;
    bool json_escape = false;

    bool consume(char c)
    {
        if (kind == Kind::Unknown && !std::isspace((unsigned char)c)) {
            if (c == '{' || c == '[') kind = Kind::Json;
            else if (c == '<') kind = Kind::Xml;
        }

        if (kind == Kind::Json) {
            if (json_string) {
                if (json_escape) json_escape = false;
                else if (c == '\\') json_escape = true;
                else if (c == '"') json_string = false;
                return false;
            }
            if (c == '"') {
                json_string = true;
            } else if (c == '{' || c == '[') {
                ++json_depth;
            } else if (c == '}' || c == ']') {
                if (json_depth > 0) --json_depth;
                return json_depth == 0;
            }
            return false;
        }

        if (kind == Kind::Xml) {
            char lower = (char)std::tolower((unsigned char)c);
            xml_tail.push_back(lower);
            if (xml_tail.size() > 12) xml_tail.erase(0, xml_tail.size() - 12);
            return (xml_tail.size() >= 6 && xml_tail.compare(xml_tail.size() - 6, 6, "</rss>") == 0) ||
                   (xml_tail.size() >= 7 && xml_tail.compare(xml_tail.size() - 7, 7, "</feed>") == 0) ||
                   (xml_tail.size() >= 10 && xml_tail.compare(xml_tail.size() - 10, 10, "</channel>") == 0) ||
                   (xml_tail.size() >= 10 && xml_tail.compare(xml_tail.size() - 10, 10, "</rdf:rdf>") == 0);
        }
        return false;
    }
};

static std::string image_ext_from_url(const std::string &url)
{
    size_t end = url.find_first_of("?#");
    std::string path = end == std::string::npos ? url : url.substr(0, end);
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = lower_copy(path.substr(dot));
        if (ext == ".jpeg") return ".jpg";
        if (ext == ".jpg" || ext == ".png" || ext == ".bmp") return ext;
    }
    std::string lower = lower_copy(url.substr(0, url.find('#')));
    if (lower.find(".jpeg") != std::string::npos) return ".jpg";
    if (lower.find(".jpg") != std::string::npos) return ".jpg";
    if (lower.find(".png") != std::string::npos) return ".png";
    if (lower.find(".bmp") != std::string::npos) return ".bmp";
    return std::string();
}

static std::string cache_path_for_image_url(const std::string &url, const std::string &ext)
{
    char name[48];
    std::snprintf(name, sizeof(name), "/%08lx%s", (unsigned long)hash32_string(url), ext.c_str());
    return std::string(app_paths::rss_image_cache_dir()) + name;
}

static bool file_exists(const std::string &path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

static bool probe_png_size(FILE *fp, int *w, int *h)
{
    uint8_t hdr[24];
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (memcmp(hdr, sig, sizeof(sig)) != 0 || memcmp(hdr + 12, "IHDR", 4) != 0) return false;
    *w = (int)(((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) | ((uint32_t)hdr[18] << 8) | hdr[19]);
    *h = (int)(((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) | ((uint32_t)hdr[22] << 8) | hdr[23]);
    return *w > 0 && *h > 0;
}

static bool probe_bmp_size(FILE *fp, int *w, int *h)
{
    uint8_t hdr[26];
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) return false;
    if (hdr[0] != 'B' || hdr[1] != 'M') return false;
    int32_t bw = (int32_t)le32(hdr + 18);
    int32_t bh = (int32_t)le32(hdr + 22);
    if (bw == 0 || bh == 0) return false;
    *w = bw < 0 ? -bw : bw;
    *h = bh < 0 ? -bh : bh;
    return *w > 0 && *h > 0;
}

static bool probe_jpeg_size(FILE *fp, int *w, int *h, bool *progressive)
{
    if (progressive) *progressive = false;
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    int a = fgetc(fp);
    int b = fgetc(fp);
    if (a != 0xFF || b != 0xD8) return false;

    while (true) {
        int c = fgetc(fp);
        while (c == 0xFF) c = fgetc(fp);
        if (c == EOF || c == 0xD9 || c == 0xDA) return false;
        uint8_t lenbuf[2];
        if (fread(lenbuf, 1, 2, fp) != 2) return false;
        int len = be16(lenbuf);
        if (len < 2) return false;
        bool sof = (c >= 0xC0 && c <= 0xC3) || (c >= 0xC5 && c <= 0xC7) ||
                   (c >= 0xC9 && c <= 0xCB) || (c >= 0xCD && c <= 0xCF);
        if (sof) {
            if (progressive) *progressive = c == 0xC2 || c == 0xC6 || c == 0xCA || c == 0xCE;
            uint8_t dim[5];
            if (fread(dim, 1, sizeof(dim), fp) != sizeof(dim)) return false;
            *h = be16(dim + 1);
            *w = be16(dim + 3);
            return *w > 0 && *h > 0;
        }
        if (fseek(fp, len - 2, SEEK_CUR) != 0) return false;
    }
}

static bool probe_image_size(const std::string &path, const std::string &ext, int *w, int *h, bool *jpg_progressive)
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (jpg_progressive) *jpg_progressive = false;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    bool ok = false;
    if (ext == ".jpg") ok = probe_jpeg_size(fp, w, h, jpg_progressive);
    else if (ext == ".png") ok = probe_png_size(fp, w, h);
    else if (ext == ".bmp") ok = probe_bmp_size(fp, w, h);
    fclose(fp);
    return ok;
}

static std::string xml_unescape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    size_t next_yield = 16 * 1024;
    for (size_t i = 0; i < s.size();) {
        if (i >= next_yield) {
            vTaskDelay(1);
            next_yield = i + 16 * 1024;
        }
        if (s[i] == '&') {
            // Search only inside the maximum supported entity length. The old
            // unbounded find(';') made input containing many bare '&' bytes
            // quadratic and could starve IDLE0 long enough to trip the WDT.
            size_t semi = i + 1;
            size_t limit = std::min(s.size(), i + 13);
            while (semi < limit && s[semi] != ';' && s[semi] != '&' &&
                   !std::isspace((unsigned char)s[semi])) {
                ++semi;
            }
            if (semi < limit && s[semi] == ';') {
                size_t start = i + 1;
                size_t len = semi - start;
                char decoded = 0;
                if (len == 3 && s.compare(start, len, "amp") == 0) decoded = '&';
                else if (len == 2 && s.compare(start, len, "lt") == 0) decoded = '<';
                else if (len == 2 && s.compare(start, len, "gt") == 0) decoded = '>';
                else if (len == 4 && s.compare(start, len, "quot") == 0) decoded = '"';
                else if (len == 4 && s.compare(start, len, "apos") == 0) decoded = '\'';
                else if (len == 4 && s.compare(start, len, "nbsp") == 0) decoded = ' ';
                if (decoded != 0) {
                    out.push_back(decoded);
                    i = semi + 1;
                    continue;
                }
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

static std::string strip_cdata(const std::string &s)
{
    const char *a = "<![CDATA[";
    const char *b = "]]>";
    if (s.rfind(a, 0) == 0 && s.size() >= std::strlen(a) + std::strlen(b) &&
        s.compare(s.size() - std::strlen(b), std::strlen(b), b) == 0) {
        return s.substr(std::strlen(a), s.size() - std::strlen(a) - std::strlen(b));
    }
    return s;
}

static std::string strip_html(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    size_t next_yield = 16 * 1024;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i >= next_yield) {
            vTaskDelay(1);
            next_yield = i + 16 * 1024;
        }
        char c = s[i];
        if (c == '<') {
            in_tag = true;
            if (i + 3 < s.size()) {
                char a = (char)std::tolower((unsigned char)s[i + 1]);
                char b = (char)std::tolower((unsigned char)s[i + 2]);
                if (a == 'p') out += "\n\n";
                else if (a == 'b' && b == 'r') out.push_back('\n');
            }
            continue;
        }
        if (c == '>') {
            in_tag = false;
            continue;
        }
        if (!in_tag) out.push_back(c);
    }
    return xml_unescape(out);
}

static bool should_keep_ascii_space_between(char prev, char next)
{
    unsigned char p = (unsigned char)prev;
    unsigned char n = (unsigned char)next;
    if (p >= 0x80 || n >= 0x80) return false;
    if (std::ispunct(n) && n != '(' && n != '[' && n != '{') return false;
    return std::isalnum(p) || std::isalnum(n);
}

static bool should_keep_cjk_gap_between(char prev, char next)
{
    unsigned char p = (unsigned char)prev;
    unsigned char n = (unsigned char)next;
    if (p < 0x80 && std::ispunct(p)) return false;
    if (n < 0x80 && std::ispunct(n) && n != '(' && n != '[' && n != '{') return false;
    return p >= 0x80 || n >= 0x80;
}

static std::string normalize_article_text(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    size_t next_yield = 16 * 1024;
    for (size_t i = 0; i < s.size();) {
        if (i >= next_yield) {
            vTaskDelay(1);
            next_yield = i + 16 * 1024;
        }
        unsigned char c = (unsigned char)s[i];
        if (std::isspace(c)) {
            int newlines = 0;
            bool had_plain_space = false;
            while (i < s.size() && std::isspace((unsigned char)s[i])) {
                if (s[i] == '\n' || s[i] == '\r') newlines++;
                else if (s[i] == ' ' || s[i] == '\t') had_plain_space = true;
                i++;
            }

            while (!out.empty() && out.back() == ' ') out.pop_back();
            if (newlines >= 2) {
                while (!out.empty() && out.back() == '\n') out.pop_back();
                if (!out.empty()) out += "\n\n";
            } else if (newlines == 0 && had_plain_space && !out.empty() && i < s.size() &&
                       out.back() != '\n' && should_keep_cjk_gap_between(out.back(), s[i])) {
                out += "\xE3\x80\x80";
            } else if (!out.empty() && i < s.size() && out.back() != '\n' &&
                       should_keep_ascii_space_between(out.back(), s[i])) {
                out.push_back(' ');
            }
            continue;
        }
        out.push_back(s[i++]);
    }
    return trim_copy(out);
}

static bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2) return is_leap_year(year) ? 29 : 28;
    if (month < 1 || month > 12) return 30;
    return days[month - 1];
}

static int month_from_name(const char *name)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    for (int i = 0; i < 12; ++i) {
        if (std::tolower((unsigned char)name[0]) == std::tolower((unsigned char)months[i][0]) &&
            std::tolower((unsigned char)name[1]) == std::tolower((unsigned char)months[i][1]) &&
            std::tolower((unsigned char)name[2]) == std::tolower((unsigned char)months[i][2])) {
            return i + 1;
        }
    }
    return 0;
}

static void add_minutes(int *year, int *month, int *day, int *hour, int *minute, int delta)
{
    if (!year || !month || !day || !hour || !minute) return;
    int total = *hour * 60 + *minute + delta;
    while (total < 0) {
        total += 24 * 60;
        (*day)--;
        if (*day < 1) {
            (*month)--;
            if (*month < 1) {
                *month = 12;
                (*year)--;
            }
            *day = days_in_month(*year, *month);
        }
    }
    while (total >= 24 * 60) {
        total -= 24 * 60;
        (*day)++;
        int dim = days_in_month(*year, *month);
        if (*day > dim) {
            *day = 1;
            (*month)++;
            if (*month > 12) {
                *month = 1;
                (*year)++;
            }
        }
    }
    *hour = total / 60;
    *minute = total % 60;
}

static int parse_timezone_offset_minutes(const std::string &tz)
{
    if (tz == "GMT" || tz == "UTC" || tz == "UT" || tz == "Z") return 0;
    if (tz.size() == 5 && (tz[0] == '+' || tz[0] == '-') &&
        std::isdigit((unsigned char)tz[1]) && std::isdigit((unsigned char)tz[2]) &&
        std::isdigit((unsigned char)tz[3]) && std::isdigit((unsigned char)tz[4])) {
        int hh = (tz[1] - '0') * 10 + (tz[2] - '0');
        int mm = (tz[3] - '0') * 10 + (tz[4] - '0');
        int sign = tz[0] == '-' ? -1 : 1;
        return sign * (hh * 60 + mm);
    }
    return 0;
}

static std::string beijing_date(const std::string &date)
{
    if (date.empty()) return date;

    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    char mon_name[8] = {};
    char tz[16] = {};
    int matched = std::sscanf(date.c_str(), "%*[^,], %d %7s %d %d:%d:%d %15s",
                              &day, mon_name, &year, &hour, &minute, &second, tz);
    if (matched < 6) {
        matched = std::sscanf(date.c_str(), "%d %7s %d %d:%d:%d %15s",
                              &day, mon_name, &year, &hour, &minute, &second, tz);
    }
    int month = month_from_name(mon_name);
    if (matched < 6 || month == 0) return date;
    if (matched < 7) std::snprintf(tz, sizeof(tz), "GMT");

    int source_offset = parse_timezone_offset_minutes(tz);
    add_minutes(&year, &month, &day, &hour, &minute, 8 * 60 - source_offset);

    char out[32];
    std::snprintf(out, sizeof(out), "%04d-%02d-%02d %02d:%02d", year, month, day, hour, minute);
    return out;
}

static bool tag_name_match(const std::string &xml, size_t lt, const char *tag)
{
    size_t i = lt + 1;
    if (i < xml.size() && xml[i] == '/') i++;
    const char *p = tag;
    while (*p && i < xml.size()) {
        if (std::tolower((unsigned char)xml[i]) != std::tolower((unsigned char)*p)) return false;
        i++;
        p++;
    }
    if (*p) return false;
    return i < xml.size() && (xml[i] == '>' || xml[i] == ' ' || xml[i] == '\t' || xml[i] == '\r' || xml[i] == '\n');
}

static std::string extract_tag(const std::string &xml, const char *tag, size_t from = 0)
{
    std::string close = std::string("</") + tag + ">";
    for (size_t lt = xml.find('<', from); lt != std::string::npos; lt = xml.find('<', lt + 1)) {
        if (!tag_name_match(xml, lt, tag)) continue;
        size_t gt = xml.find('>', lt);
        if (gt == std::string::npos) return std::string();
        size_t end = xml.find(close, gt + 1);
        if (end == std::string::npos) return std::string();
        return trim_copy(xml_unescape(strip_cdata(xml.substr(gt + 1, end - gt - 1))));
    }
    return std::string();
}

static std::string extract_any_tag(const std::string &xml, const char *a, const char *b)
{
    std::string v = extract_tag(xml, a);
    return v.empty() ? extract_tag(xml, b) : v;
}

static std::string attr_value(const std::string &tag, const char *attr)
{
    std::string lower = lower_copy(tag);
    std::string key = lower_copy(attr);
    size_t p = lower.find(key);
    while (p != std::string::npos) {
        bool boundary_before = p == 0 || std::isspace((unsigned char)lower[p - 1]) || lower[p - 1] == '<';
        size_t after = p + key.size();
        if (boundary_before && after < lower.size()) {
            while (after < lower.size() && std::isspace((unsigned char)lower[after])) after++;
            if (after < lower.size() && lower[after] == '=') {
                after++;
                while (after < tag.size() && std::isspace((unsigned char)tag[after])) after++;
                if (after < tag.size() && (tag[after] == '"' || tag[after] == '\'')) {
                    char quote = tag[after++];
                    size_t end = tag.find(quote, after);
                    if (end != std::string::npos) return xml_unescape(tag.substr(after, end - after));
                }
            }
        }
        p = lower.find(key, p + 1);
    }
    return std::string();
}

static std::string first_tag_attr_value(const std::string &xml, const char *tag_name, const char *attr)
{
    for (size_t lt = xml.find('<'); lt != std::string::npos; lt = xml.find('<', lt + 1)) {
        if (!tag_name_match(xml, lt, tag_name)) continue;
        size_t gt = xml.find('>', lt);
        if (gt == std::string::npos) return std::string();
        std::string v = attr_value(xml.substr(lt, gt - lt + 1), attr);
        if (!v.empty()) return v;
    }
    return std::string();
}

static std::string normalize_image_url(const std::string &url)
{
    std::string u = trim_copy(url);
    if (u.rfind("//", 0) == 0) return "https:" + u;
    if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) return u;
    return std::string();
}

static std::string extract_image_url(const std::string &item_xml,
                                     const std::string &body_html)
{
    std::string url = normalize_image_url(first_tag_attr_value(item_xml, "media:content", "url"));
    if (!url.empty() && !image_ext_from_url(url).empty()) return url;
    url = normalize_image_url(first_tag_attr_value(item_xml, "media:thumbnail", "url"));
    if (!url.empty() && !image_ext_from_url(url).empty()) return url;
    url = normalize_image_url(first_tag_attr_value(item_xml, "enclosure", "url"));
    if (!url.empty() && !image_ext_from_url(url).empty()) return url;

    url = normalize_image_url(first_tag_attr_value(body_html, "img", "src"));
    if (!url.empty() && !image_ext_from_url(url).empty()) return url;
    return std::string();
}

static void add_image_ref(std::vector<RssImageRef> *refs, const std::string &url, const std::string &before_text)
{
    if (!refs) return;
    std::string u = normalize_image_url(url);
    if (u.empty() || image_ext_from_url(u).empty()) return;
    for (const auto &ref : *refs) {
        if (ref.url == u) return;
    }
    if ((int)refs->size() >= MAX_RSS_IMAGES_PER_ITEM) return;
    refs->push_back({u, before_text});
}

static size_t first_img_tag_pos(const std::string &html)
{
    for (size_t lt = html.find('<'); lt != std::string::npos; lt = html.find('<', lt + 1)) {
        if (tag_name_match(html, lt, "img")) return lt;
    }
    return std::string::npos;
}

static std::string text_before_first_img(const std::string &html)
{
    size_t pos = first_img_tag_pos(html);
    if (pos == std::string::npos) return std::string();
    return strip_html(html.substr(0, pos));
}

static std::vector<RssImageRef> extract_image_refs(const std::string &item_xml, const std::string &body_html)
{
    std::vector<RssImageRef> refs;
    add_image_ref(&refs, first_tag_attr_value(item_xml, "media:content", "url"), "");
    add_image_ref(&refs, first_tag_attr_value(item_xml, "media:thumbnail", "url"), "");
    add_image_ref(&refs, first_tag_attr_value(item_xml, "enclosure", "url"), "");

    for (size_t lt = body_html.find('<'); lt != std::string::npos; lt = body_html.find('<', lt + 1)) {
        if (!tag_name_match(body_html, lt, "img")) continue;
        size_t gt = body_html.find('>', lt);
        if (gt == std::string::npos) break;
        std::string src = attr_value(body_html.substr(lt, gt - lt + 1), "src");
        add_image_ref(&refs, src, strip_html(body_html.substr(0, lt)));
    }
    return refs;
}

static bool parse_rss_items(const std::string &xml, std::vector<RssItem> *items)
{
    if (!items) return false;
    int64_t t0 = esp_timer_get_time();
    items->clear();
    size_t pos = 0;
    unsigned processed = 0;
    while (items->size() < 40) {
        size_t start = xml.find("<item", pos);
        if (start == std::string::npos) break;
        size_t start_gt = xml.find('>', start);
        size_t end = xml.find("</item>", start_gt == std::string::npos ? start : start_gt);
        if (start_gt == std::string::npos || end == std::string::npos) break;
        std::string item_xml = xml.substr(start_gt + 1, end - start_gt - 1);
        RssItem item;
        std::string body_html = extract_any_tag(item_xml, "content:encoded", "description");
        item.title = strip_html(extract_tag(item_xml, "title"));
        item.link = strip_html(extract_tag(item_xml, "link"));
        item.date = strip_html(extract_any_tag(item_xml, "pubDate", "updated"));
        item.text = strip_html(body_html);
        item.image_url = extract_image_url(item_xml, body_html);
        item.image_before_text = text_before_first_img(body_html);
        item.images = extract_image_refs(item_xml, body_html);
        if (!item.title.empty()) items->push_back(item);
        pos = end + 7;
        if ((++processed & 3u) == 0) vTaskDelay(1);
    }
    ESP_LOGI(TAG, "parse feed bytes=%u items=%u time=%lld ms",
             (unsigned)xml.size(),
             (unsigned)items->size(),
             (esp_timer_get_time() - t0) / 1000);
    return !items->empty();
}

static std::string json_string_value(const std::string &obj, const char *key)
{
    std::string needle = std::string("\"") + key + "\"";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return std::string();
    p = obj.find(':', p + needle.size());
    if (p == std::string::npos) return std::string();
    p = obj.find('"', p + 1);
    if (p == std::string::npos) return std::string();
    std::string out;
    for (++p; p < obj.size(); ++p) {
        char c = obj[p];
        if (c == '"') break;
        if (c == '\\' && p + 1 < obj.size()) {
            char e = obj[++p];
            if (e == 'n') out.push_back('\n');
            else if (e == 'r') out.push_back('\r');
            else if (e == 't') out.push_back('\t');
            else out.push_back(e);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static int json_hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void append_utf8_codepoint(std::string *out, uint32_t cp)
{
    if (!out) return;
    if (cp < 0x80) {
        out->push_back((char)cp);
    } else if (cp < 0x800) {
        out->push_back((char)(0xC0 | (cp >> 6)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back((char)(0xE0 | (cp >> 12)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out->push_back((char)(0xF0 | (cp >> 18)));
        out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static bool parse_json_u16(const std::string &s, size_t pos, uint32_t *out)
{
    if (!out || pos + 4 > s.size()) return false;
    uint32_t cp = 0;
    for (int i = 0; i < 4; ++i) {
        int v = json_hex_value(s[pos + i]);
        if (v < 0) return false;
        cp = (cp << 4) | (uint32_t)v;
    }
    *out = cp;
    return true;
}

static std::string json_string_value_utf8(const std::string &obj, const char *key)
{
    std::string needle = std::string("\"") + key + "\"";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return std::string();
    p = obj.find(':', p + needle.size());
    if (p == std::string::npos) return std::string();
    p = obj.find('"', p + 1);
    if (p == std::string::npos) return std::string();
    std::string out;
    for (++p; p < obj.size(); ++p) {
        char c = obj[p];
        if (c == '"') break;
        if (c == '\\' && p + 1 < obj.size()) {
            char e = obj[++p];
            if (e == 'n') out.push_back('\n');
            else if (e == 'r') out.push_back('\r');
            else if (e == 't') out.push_back('\t');
            else if (e == 'b') out.push_back('\b');
            else if (e == 'f') out.push_back('\f');
            else if (e == 'u') {
                uint32_t cp = 0;
                if (parse_json_u16(obj, p + 1, &cp)) {
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 < obj.size() &&
                        obj[p + 1] == '\\' && obj[p + 2] == 'u') {
                        uint32_t lo = 0;
                        if (parse_json_u16(obj, p + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    append_utf8_codepoint(&out, cp);
                }
            } else {
                out.push_back(e);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static std::string bing_date_text(const std::string &startdate)
{
    if (startdate.size() != 8) return startdate;
    return startdate.substr(0, 4) + "-" + startdate.substr(4, 2) + "-" + startdate.substr(6, 2) + " 00:00";
}

static std::string bing_image_url_from_fields(const std::string &url, const std::string &urlbase)
{
    std::string image_url;
    if (!urlbase.empty()) {
        image_url = urlbase + "_1280x720.jpg";
    } else {
        image_url = url;
        size_t query_extra = image_url.find('&');
        if (query_extra != std::string::npos) image_url.erase(query_extra);
    }
    if (image_url.empty()) return std::string();
    return image_url.rfind("http", 0) == 0 ? image_url : ("https://www.bing.com" + image_url);
}

static bool parse_bing_wallpaper_items(const std::string &json, std::vector<RssItem> *items)
{
    if (!items) return false;
    items->clear();
    size_t arr = json.find("\"images\"");
    if (arr == std::string::npos) return false;
    size_t pos = json.find('{', arr);
    while (pos != std::string::npos && items->size() < 16) {
        int depth = 0;
        bool in_str = false;
        bool esc = false;
        size_t end = pos;
        for (; end < json.size(); ++end) {
            char c = json[end];
            if (esc) {
                esc = false;
                continue;
            }
            if (c == '\\' && in_str) {
                esc = true;
                continue;
            }
            if (c == '"') in_str = !in_str;
            if (in_str) continue;
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) break;
            }
        }
        if (end >= json.size()) break;

        std::string obj = json.substr(pos, end - pos + 1);
        std::string url = json_string_value_utf8(obj, "url");
        std::string urlbase = json_string_value_utf8(obj, "urlbase");
        std::string full_url = bing_image_url_from_fields(url, urlbase);
        std::string title = json_string_value_utf8(obj, "title");
        std::string copyright = json_string_value_utf8(obj, "copyright");
        std::string date = json_string_value_utf8(obj, "startdate");
        if (!full_url.empty()) {
            RssItem item;
            item.title = title.empty() ? "Bing Wallpaper" : title;
            item.date = bing_date_text(date);
            item.text = copyright.empty() ? item.title : copyright;
            item.link = json_string_value_utf8(obj, "copyrightlink");
            item.image_url = full_url;
            item.images.push_back({full_url, ""});
            items->push_back(item);
        }
        pos = json.find('{', end + 1);
    }
    return !items->empty();
}

static bool load_wifi_config(std::string *ssid, std::string *password)
{
    if (!ssid || !password) return false;
    FILE *fp = std::fopen(WIFI_CONFIG_PATH, "rb");
    if (!fp) return false;
    char line[256];
    while (std::fgets(line, sizeof(line), fp)) {
        std::string s = trim_copy(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) {
            if (ssid->empty()) *ssid = s;
            else if (password->empty()) *password = s;
            continue;
        }
        std::string key = trim_copy(s.substr(0, eq));
        std::string val = trim_copy(s.substr(eq + 1));
        if (key == "ssid") *ssid = val;
        else if (key == "password" || key == "pass") *password = val;
    }
    std::fclose(fp);
    return !ssid->empty();
}

static void load_feeds()
{
    g_feeds.clear();
    FILE *fp = std::fopen(RSS_CONFIG_PATH, "rb");
    if (fp) {
        char line[384];
        while (std::fgets(line, sizeof(line), fp)) {
            std::string s = trim_copy(line);
            if (s.empty() || s[0] == '#') continue;
            size_t bar = s.find('|');
            RssFeed feed;
            if (bar == std::string::npos) {
                feed.title = s;
                feed.url = s;
            } else {
                feed.title = trim_copy(s.substr(0, bar));
                feed.url = trim_copy(s.substr(bar + 1));
            }
            if (!feed.url.empty()) g_feeds.push_back(feed);
        }
        std::fclose(fp);
    }
    if (g_feeds.empty()) {
        g_feeds.push_back({"IT Home", "https://www.ithome.com/rss"});
        g_feeds.push_back({"Zaobao China", "https://plink.anyfeeder.com/zaobao/realtime/china"});
        g_feeds.push_back({"Zaobao World", "https://plink.anyfeeder.com/zaobao/realtime/world"});
        g_feeds.push_back({"The Paper", "https://plink.anyfeeder.com/thepaper"});
        g_feeds.push_back({"Fortune China", "https://plink.anyfeeder.com/fortunechina"});
        g_feeds.push_back({"Bing Wallpaper", "https://rsshub.app/bing"});
        g_feeds.push_back({"Wallstreetcn Global", "https://feed.wallstreetcn.com/wallstreetcn/news/global"});
    }
}

class RssReaderActivity : public Activity {
public:
    ~RssReaderActivity() override
    {
        restore_display_rotation();
        stop_wifi();
    }

    void on_create() override
    {
        _base_rotation = M5.Display.getRotation();
        _rotation_changed = false;
        load_feeds();
        draw_status("RSS", "Loading WiFi...");
        _wifi_ok = connect_wifi();
        if (!_wifi_ok) g_status = "Create /sdcard/wifi.txt";
        _view = View::Feeds;
        draw_feeds();
    }

    void on_show() override
    {
        if (!_seen_show) {
            _seen_show = true;
            return;
        }
        if (_view == View::Article) {
            rebuild_article_lines(false);
            draw_article();
        } else {
            redraw_list();
        }
    }

    bool on_key(int key) override
    {
        if (_view == View::Article) return article_key(key);
        if (key == APP_KEY_BACK) {
            if (_view == View::Items) {
                _view = View::Feeds;
                g_focus = _feed_focus;
                g_scroll = _feed_scroll;
                redraw_list();
                return true;
            }
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_UP) {
            move_focus(-1);
            return true;
        }
        if (key == APP_KEY_DOWN) {
            move_focus(1);
            return true;
        }
        if (key == APP_KEY_OK) {
            if (_view == View::Feeds) open_feed();
            else open_item();
            return true;
        }
        return false;
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            on_key(APP_KEY_BACK);
            return true;
        }

        if (event.type == TouchEventType::Tap && event.y < TOP_H && event.x < 140) {
            on_key(APP_KEY_BACK);
            return true;
        }

        if (_view == View::Article) {
            if (event.type == TouchEventType::SwipeLeft) {
                next_article_page();
                return true;
            }
            if (event.type == TouchEventType::SwipeRight) {
                prev_article_page();
                return true;
            }
            if (event.type != TouchEventType::Tap) return false;
            int w = M5.Display.width();
            if (event.x >= w / 3 && event.x <= (w * 2) / 3) {
                launch_reader_settings();
            } else if (event.x > w / 2) {
                next_article_page();
            } else {
                prev_article_page();
            }
            return true;
        }

        if (event.type == TouchEventType::SwipeUp) {
            scroll_list(visible_rows());
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            scroll_list(-visible_rows());
            return true;
        }
        if (event.type == TouchEventType::Tap) {
            int idx = hit_row(event.y);
            if (idx >= 0) {
                g_focus = idx;
                if (_view == View::Feeds) open_feed();
                else open_item();
                return true;
            }
        }
        return false;
    }

private:
    enum class View { Feeds, Items, Article };

    View _view = View::Feeds;
    bool _seen_show = false;
    bool _wifi_ok = false;
    bool _current_feed_is_bing = false;
    bool _rotation_changed = false;
    uint8_t _base_rotation = 0;
    int _feed_focus = 0;
    int _feed_scroll = 0;

    bool connect_wifi()
    {
        std::string ssid = DEFAULT_STA_SSID;
        std::string pass = DEFAULT_STA_PASSWORD;
        load_wifi_config(&ssid, &pass);
        if (ssid.empty()) {
            ESP_LOGW(TAG, "Wi-Fi SSID is empty");
            return false;
        }

        esp_err_t err = wifi_setting_connect(ssid.c_str(), pass.c_str(), CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    void stop_wifi()
    {
        // RSS does not need background connectivity after the activity exits.
        wifi_setting_deinit(true);
    }

    bool fetch_url_once(const std::string &url, std::string *body)
    {
        if (!body) return false;
        body->clear();
        esp_http_client_config_t cfg = {};
        cfg.url = url.c_str();
        cfg.timeout_ms = 12000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.user_agent = "MyPaperS3 RSS Reader";
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) return false;
        // Every RSS request uses a fresh handle, so keeping the HTTP/1.1
        // connection alive only makes unframed feeds wait for a timeout.
        esp_http_client_set_header(client, "Connection", "close");
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "open failed %s: %s", url.c_str(), esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return false;
        }
        const int64_t content_length = esp_http_client_fetch_headers(client);
        const bool chunked = esp_http_client_is_chunked_response(client);
        const int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "feed response status=%d content_length=%lld chunked=%d",
                 status, (long long)content_length, chunked ? 1 : 0);
        // The connection/handshake may legitimately need the longer timeout,
        // but a news feed that sends no body data for two seconds has stalled.
        esp_http_client_set_timeout_ms(client, 2000);
        char *buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (content_length > 0) {
            body->reserve((size_t)std::min<int64_t>(content_length, MAX_FEED_BYTES));
        } else {
            body->reserve(64 * 1024);
        }
        const bool length_framed = !chunked && content_length >= 0;
        HttpDocumentEndDetector document_end;
        bool document_complete = false;
        while ((int)body->size() < MAX_FEED_BYTES && !document_complete) {
            if (esp_http_client_is_complete_data_received(client)) break;
            int want = 4096;
            if (length_framed) {
                const int64_t remaining = content_length - static_cast<int64_t>(body->size());
                if (remaining <= 0) break;
                want = static_cast<int>(std::min<int64_t>(want, remaining));
            }
            int n = esp_http_client_read(client, buf, want);
            if (n <= 0) break;
            body->append(buf, n);
            if (!length_framed) {
                for (int i = 0; i < n; ++i) {
                    if (document_end.consume(buf[i])) {
                        document_complete = true;
                        break;
                    }
                }
                // esp_http_client_read() normally fills `want`. A shorter
                // unframed read means the transport timed out or closed after
                // returning its last partial block; do not immediately wait
                // for a second timeout.
                if (n < want && !document_complete &&
                    !esp_http_client_is_complete_data_received(client)) {
                    break;
                }
            }
        }
        const bool transport_complete = esp_http_client_is_complete_data_received(client);
        const bool response_complete = length_framed
                                           ? content_length >= 0 &&
                                                 static_cast<int64_t>(body->size()) >= content_length
                                           : document_complete || transport_complete;
        if (!length_framed && !document_complete && !body->empty()) {
            const size_t tail_start = body->size() > 96 ? body->size() - 96 : 0;
            std::string tail = body->substr(tail_start);
            for (char &c : tail) {
                unsigned char uc = (unsigned char)c;
                if (uc < 0x20 || uc == 0x7f) c = ' ';
            }
            ESP_LOGW(TAG, "feed stream ended without document marker bytes=%u tail=%s",
                     (unsigned)body->size(), tail.c_str());
        }
        heap_caps_free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return status >= 200 && status < 300 && !body->empty() && response_complete;
    }

    bool fetch_url(const std::string &url, std::string *body)
    {
        if (fetch_url_once(url, body)) return true;
        std::string best_partial = body ? *body : std::string();
        if (url.rfind("https://", 0) == 0) {
            std::string http_url = "http://" + url.substr(8);
            ESP_LOGW(TAG, "retry without TLS: %s", http_url.c_str());
            if (fetch_url_once(http_url, body)) return true;
            if (body && best_partial.size() > body->size()) *body = std::move(best_partial);
        }
        // Both transports may be backed by the same streaming proxy. Preserve
        // only completely closed RSS items from the better partial response;
        // parse_rss_items() intentionally ignores the unfinished final item.
        if (body && !body->empty()) {
            const size_t last_item = body->rfind("</item>");
            if (last_item != std::string::npos) {
                body->resize(last_item + 7);
                ESP_LOGW(TAG, "using partial feed snapshot bytes=%u",
                         (unsigned)body->size());
                return true;
            }
        }
        return false;
    }

    bool download_url_to_file_once(const std::string &url, const std::string &path)
    {
        esp_http_client_config_t cfg = {};
        cfg.url = url.c_str();
        cfg.timeout_ms = 15000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.user_agent = "MyPaperS3 RSS Reader";
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) return false;
        esp_http_client_set_header(client, "Connection", "close");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "image open failed %s: %s", url.c_str(), esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return false;
        }
        const int64_t content_length = esp_http_client_fetch_headers(client);
        const bool chunked = esp_http_client_is_chunked_response(client);
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "image http status=%d url=%s", status, url.c_str());
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (!chunked && content_length > MAX_IMAGE_BYTES) {
            ESP_LOGW(TAG, "image too large content_length=%lld url=%s",
                     (long long)content_length, url.c_str());
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }

        FILE *fp = fopen(path.c_str(), "wb");
        if (!fp) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }

        char *buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf) {
            fclose(fp);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }

        int total = 0;
        bool ok = true;
        while (total < MAX_IMAGE_BYTES) {
            if (esp_http_client_is_complete_data_received(client)) break;
            int want = 4096;
            if (!chunked && content_length >= 0) {
                const int64_t remaining = content_length - total;
                if (remaining <= 0) break;
                want = static_cast<int>(std::min<int64_t>(want, remaining));
            }
            int n = esp_http_client_read(client, buf, want);
            if (n < 0) {
                ok = false;
                break;
            }
            if (n == 0) break;
            if (fwrite(buf, 1, n, fp) != (size_t)n) {
                ok = false;
                break;
            }
            total += n;
        }
        if (total >= MAX_IMAGE_BYTES) ok = false;

        heap_caps_free(buf);
        fclose(fp);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (!ok || total == 0) {
            remove(path.c_str());
            return false;
        }
        ESP_LOGI(TAG, "image downloaded url=%s bytes=%d path=%s", url.c_str(), total, path.c_str());
        return true;
    }

    bool download_url_to_file(const std::string &url, const std::string &path)
    {
        const bool https = url.rfind("https://", 0) == 0;
        if (https) log_tls_heap("image-before");
        const bool downloaded = download_url_to_file_once(url, path);
        if (https) log_tls_heap(downloaded ? "image-ok" : "image-failed");
        if (downloaded) return true;
        if (url.rfind("https://", 0) == 0) {
            std::string http_url = "http://" + url.substr(8);
            ESP_LOGW(TAG, "retry image without TLS: %s", http_url.c_str());
            return download_url_to_file_once(http_url, path);
        }
        return false;
    }

    bool load_single_article_image(const RssImageRef &ref, ArticleImage *image)
    {
        if (!image) return false;
        if (ref.url.empty()) return false;
        std::string ext = image_ext_from_url(ref.url);
        if (ext.empty()) return false;
        app_paths::ensure_dir(app_paths::rss_image_cache_dir());
        std::string path = cache_path_for_image_url(ref.url, ext);
        if (!file_exists(path) && !download_url_to_file(ref.url, path)) {
            ESP_LOGW(TAG, "image download failed url=%s", ref.url.c_str());
            return false;
        }
        ArticleImage out;
        out.url = ref.url;
        out.before_text = ref.before_text;
        out.path = path;
        out.ext = ext;
        if (!probe_image_size(path, ext, &out.width, &out.height, &out.progressive)) {
            ESP_LOGW(TAG, "image probe failed path=%s", path.c_str());
            remove(path.c_str());
            return false;
        }
        *image = out;
        return true;
    }

    bool load_article_images(const RssItem &item)
    {
        g_article_images.clear();
        std::vector<RssImageRef> refs = item.images;
        if (refs.empty() && !item.image_url.empty()) {
            refs.push_back({item.image_url, item.image_before_text});
        }
        for (const auto &ref : refs) {
            ArticleImage image;
            if (load_single_article_image(ref, &image)) {
                g_article_images.push_back(image);
            }
        }
        return !g_article_images.empty();
    }

    void draw_status(const char *title, const char *msg)
    {
        restore_display_rotation();
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        system_font::draw_center(M5.Display, title, M5.Display.width() / 2, 140);
        system_font::draw_center(M5.Display, msg, M5.Display.width() / 2, 210);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void draw_topbar(const char *title)
    {
        M5.Display.fillRect(0, 0, M5.Display.width(), TOP_H, TFT_BLACK);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        system_font::draw_left(M5.Display, "<", 12, 3, TFT_WHITE);
        system_font::draw_center(M5.Display, title, M5.Display.width() / 2, 3, TFT_WHITE);
    }

    int item_count() const
    {
        return _view == View::Feeds ? (int)g_feeds.size() : (int)g_items.size();
    }

    std::string row_label(int idx) const
    {
        if (_view == View::Feeds) return g_feeds[idx].title;
        return g_items[idx].title;
    }

    static std::string compact_date(const std::string &date)
    {
        std::string bj = beijing_date(date);
        for (size_t i = 0; i + 4 < bj.size(); ++i) {
            if (std::isdigit((unsigned char)bj[i]) &&
                std::isdigit((unsigned char)bj[i + 1]) &&
                bj[i + 2] == ':' &&
                std::isdigit((unsigned char)bj[i + 3]) &&
                std::isdigit((unsigned char)bj[i + 4])) {
                return bj.substr(i, 5);
            }
        }
        return bj.size() > 10 ? bj.substr(0, 10) : bj;
    }

    static std::string ellipsize_utf8(const std::string &s, size_t max_chars)
    {
        size_t pos = 0;
        size_t chars = 0;
        while (pos < s.size() && chars < max_chars) {
            unsigned char c = (unsigned char)s[pos];
            size_t n = 1;
            if ((c & 0xE0) == 0xC0) n = 2;
            else if ((c & 0xF0) == 0xE0) n = 3;
            else if ((c & 0xF8) == 0xF0) n = 4;
            if (pos + n > s.size()) break;
            pos += n;
            chars++;
        }
        if (pos >= s.size()) return s;
        return s.substr(0, pos) + "...";
    }

    static size_t utf8_advance(const std::string &s, size_t pos)
    {
        if (pos >= s.size()) return pos;
        unsigned char c = (unsigned char)s[pos];
        size_t n = 1;
        if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        if (pos + n > s.size()) n = 1;
        return pos + n;
    }

    static std::string normalize_title_text(const std::string &title)
    {
        std::string out;
        out.reserve(title.size());
        bool pending_space = false;
        for (size_t i = 0; i < title.size();) {
            unsigned char c = (unsigned char)title[i];
            if (std::isspace(c)) {
                pending_space = !out.empty();
                while (i < title.size() && std::isspace((unsigned char)title[i])) i++;
                continue;
            }
            if (pending_space) {
                out.push_back(' ');
                pending_space = false;
            }
            size_t next = utf8_advance(title, i);
            out.append(title, i, next - i);
            i = next;
        }
        return trim_copy(out);
    }

    static std::vector<std::string> split_title_lines(const std::string &title, int max_width)
    {
        std::vector<std::string> lines;
        std::string clean = normalize_title_text(title);
        size_t pos = 0;

        for (int line = 0; line < 3 && pos < clean.size(); ++line) {
            size_t start = pos;
            size_t last_fit = pos;
            int width = 0;
            while (pos < clean.size()) {
                size_t next = utf8_advance(clean, pos);
                std::string ch = clean.substr(pos, next - pos);
                int cw = system_font::measure(ch);
                if (cw <= 0) cw = (clean[pos] & 0x80) ? 36 : 20;
                if (width > 0 && width + cw > max_width) break;
                width += cw;
                pos = next;
                last_fit = pos;
            }
            if (last_fit == start) {
                pos = utf8_advance(clean, pos);
                last_fit = pos;
            }
            std::string part = clean.substr(start, last_fit - start);
            pos = last_fit;
            while (!part.empty() && part.back() == ' ') part.pop_back();
            while (pos < clean.size() && clean[pos] == ' ') pos++;
            if (line == 2 && pos < clean.size()) part += "...";
            lines.push_back(part);
        }
        return lines;
    }

    int row_height() const
    {
        return _view == View::Items ? NEWS_ROW_H : FEED_ROW_H;
    }

    void ensure_focus_visible()
    {
        int vis = visible_rows();
        if (g_focus < g_scroll) g_scroll = g_focus;
        if (g_focus >= g_scroll + vis) g_scroll = g_focus - vis + 1;
        int max_scroll = std::max(0, item_count() - vis);
        g_scroll = std::max(0, std::min(g_scroll, max_scroll));
    }

    int visible_rows() const
    {
        return std::max(1, ((int)M5.Display.height() - TOP_H - 6) / row_height());
    }

    void clamp_scroll()
    {
        int max_scroll = std::max(0, item_count() - visible_rows());
        g_scroll = std::max(0, std::min(g_scroll, max_scroll));
    }

    int hit_row(int ty) const
    {
        int y0 = TOP_H + 6;
        if (ty < y0) return -1;
        int idx = g_scroll + (ty - y0) / row_height();
        return idx >= 0 && idx < item_count() ? idx : -1;
    }

    void move_focus(int delta)
    {
        int n = item_count();
        if (n <= 0) return;
        g_focus = std::max(0, std::min(n - 1, g_focus + delta));
        ensure_focus_visible();
        redraw_list();
    }

    void scroll_list(int delta_rows)
    {
        if (item_count() <= 0) return;
        g_scroll += delta_rows;
        clamp_scroll();
        redraw_list();
    }

    void draw_feeds()
    {
        _view = View::Feeds;
        g_focus = 0;
        g_scroll = 0;
        redraw_list();
    }

    void redraw_list()
    {
        restore_display_rotation();
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_topbar(_view == View::Feeds ? "RSS Feeds" : "RSS News");
        int w = M5.Display.width();
        int y0 = TOP_H + 6;
        int rh = row_height();
        int vis = std::max(1, ((int)M5.Display.height() - y0) / rh);
        for (int i = 0; i < vis; ++i) {
            int idx = g_scroll + i;
            int y = y0 + i * rh;
            if (idx >= item_count()) {
                M5.Display.fillRect(0, y, w, rh, TFT_WHITE);
                continue;
            }
            M5.Display.fillRoundRect(PAD, y, w - PAD * 2, rh - 3, 6, TFT_WHITE);
            M5.Display.drawRoundRect(PAD, y, w - PAD * 2, rh - 3, 6, TFT_BLACK);
            M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
            std::string label = row_label(idx);
            std::string date;
            if (_view == View::Items && idx >= 0 && idx < (int)g_items.size()) {
                date = compact_date(g_items[idx].date);
            }
            if (_view == View::Items) {
                int title_right = date.empty() ? (w - PAD - 14) : (w - PAD - 120);
                int title_max_w = std::max(80, title_right - (PAD + 12));
                auto lines = split_title_lines(label, title_max_w);
                for (int li = 0; li < (int)lines.size(); ++li) {
                    system_font::draw_left(M5.Display, lines[li], PAD + 12, y + 8 + li * 42);
                }
                if (!date.empty()) {
                    system_font::draw_right(M5.Display, date, w - PAD - 14, y + 12);
                }
            } else {
                std::string line = system_font::ellipsize(label, w - PAD * 2 - 24);
                system_font::draw_left(M5.Display, line, PAD + 12, y + 12);
            }
        }
        if (!g_status.empty() && item_count() == 0) {
            M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
            system_font::draw_center(M5.Display, g_status, w / 2, 180);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }

    void open_feed()
    {
        if (g_focus < 0 || g_focus >= (int)g_feeds.size()) return;
        _feed_focus = g_focus;
        _feed_scroll = g_scroll;
        if (!_wifi_ok) {
            draw_status("RSS WiFi", "Create /sdcard/wifi.txt");
            vTaskDelay(pdMS_TO_TICKS(1200));
            redraw_list();
            return;
        }
        RssFeed feed = g_feeds[g_focus];
        _current_feed_is_bing = feed.url == "https://rsshub.app/bing";
        draw_status(feed.title.c_str(), "Fetching...");
        std::string xml;
        g_items.clear();
        bool ok = false;
        if (feed.url == "https://rsshub.app/bing") {
            ok = fetch_url("https://www.bing.com/HPImageArchive.aspx?format=js&idx=0&n=8&mkt=zh-CN", &xml) &&
                 parse_bing_wallpaper_items(xml, &g_items);
            if (!ok) {
                xml.clear();
                ok = fetch_url(feed.url, &xml) && parse_rss_items(xml, &g_items);
            }
        } else {
            ok = fetch_url(feed.url, &xml) && parse_rss_items(xml, &g_items);
        }
        if (!ok) {
            g_status = "Fetch or parse failed";
        } else {
            g_status.clear();
        }
        _view = View::Items;
        g_focus = 0;
        g_scroll = 0;
        redraw_list();
    }

    void open_item()
    {
        if (g_focus < 0 || g_focus >= (int)g_items.size()) return;
        draw_status("RSS", "Loading article...");
        if (reader_style_get().show_images) {
            load_article_images(g_items[g_focus]);
        } else {
            clear_article_images();
        }
        rebuild_article_lines(true);
        _view = View::Article;
        draw_article();
    }

    void clear_article_images()
    {
        g_article_images.clear();
    }

    void rebuild_article_lines(bool reset_page)
    {
        if (g_focus < 0 || g_focus >= (int)g_items.size()) return;
        const RssItem &item = g_items[g_focus];
        if (reader_style_get().show_images) {
            if (g_article_images.empty()) load_article_images(item);
        } else {
            clear_article_images();
        }
        ReaderTextStyle ts;
        ts.font_path = reader_font_path();
        ReaderStyle rs = reader_style_get();
        ts.font_size = rs.font_size;
        ts.line_spacing = rs.line_spacing;
        reader_text_renderer().begin(ts);
        std::vector<document_layout::Block> blocks;
        uint64_t content_position = 0;
        auto append_paragraphs = [&](const std::string &text, bool heading,
                                     int final_gap, bool allow_indent) {
            const size_t first_block = blocks.size();
            size_t start = 0;
            bool pending_gap = false;
            while (start <= text.size()) {
                size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                std::string paragraph = trim_copy(text.substr(start, end - start));
                if (paragraph.empty()) {
                    if (!blocks.empty()) pending_gap = true;
                } else {
                    document_layout::Block block;
                    block.type = document_layout::BlockType::Paragraph;
                    block.text = std::move(paragraph);
                    block.content_position = content_position + start;
                    block.blank_before = pending_gap ? 1 : 0;
                    block.blank_after = 0;
                    block.heading = heading;
                    block.heading_level = heading ? 1 : 0;
                    block.allow_first_line_indent = !heading && allow_indent;
                    block.paragraph_spacing_after = !heading && allow_indent;
                    blocks.push_back(std::move(block));
                    pending_gap = false;
                }
                if (end == text.size()) break;
                start = end + 1;
            }
            if (blocks.size() > first_block) {
                blocks.back().blank_after = final_gap;
            }
            content_position += text.size() + 1;
        };

        append_paragraphs(item.title, true, 0, false);
        if (!item.date.empty()) append_paragraphs(beijing_date(item.date), false, 1, false);
        std::string article_text = normalize_article_text(item.text.empty() ? item.link : item.text);
        struct PositionedImage {
            size_t text_position = 0;
            ArticleImage *image = nullptr;
        };
        std::vector<PositionedImage> positioned_images;
        positioned_images.reserve(g_article_images.size());
        for (auto &image : g_article_images) {
            std::string prefix = normalize_article_text(image.before_text);
            size_t position = 0;
            if (!prefix.empty()) {
                if (prefix.size() <= article_text.size() &&
                    article_text.compare(0, prefix.size(), prefix) == 0) {
                    position = prefix.size();
                } else {
                    // Feeds occasionally normalize whitespace around <img>
                    // differently. A common-prefix position is safer than
                    // moving the image to an unrelated later occurrence.
                    size_t limit = std::min(prefix.size(), article_text.size());
                    while (position < limit && prefix[position] == article_text[position]) ++position;
                    while (position > 0 && position < article_text.size() &&
                           ((unsigned char)article_text[position] & 0xC0) == 0x80) {
                        --position;
                    }
                }
            }
            positioned_images.push_back({position, &image});
        }
        std::sort(positioned_images.begin(), positioned_images.end(), [](const PositionedImage &a,
                                                                         const PositionedImage &b) {
            if (a.text_position != b.text_position) return a.text_position < b.text_position;
            return a.image->url < b.image->url;
        });

        size_t body_cursor = 0;
        const uint64_t body_position = content_position;
        for (const auto &positioned : positioned_images) {
            size_t image_position = std::max(body_cursor,
                                             std::min(positioned.text_position, article_text.size()));
            if (image_position > body_cursor) {
                content_position = body_position + body_cursor;
                append_paragraphs(article_text.substr(body_cursor, image_position - body_cursor), false, 0, true);
            }
            document_layout::Block image_block;
            image_block.type = document_layout::BlockType::Image;
            image_block.resource = positioned.image->path;
            image_block.content_position = body_position + image_position;
            image_block.image_height = std::min(320, std::max(120, (int)M5.Display.height() / 3));
            image_block.blank_after = 0;
            blocks.push_back(std::move(image_block));
            body_cursor = image_position;
        }
        if (body_cursor < article_text.size()) {
            content_position = body_position + body_cursor;
            append_paragraphs(article_text.substr(body_cursor), false, 1, true);
        }

        int line_height = reader_text_renderer().lineHeight();
        if (line_height <= 0) line_height = 48;
        document_layout::Options options;
        options.content_width = std::max(100, (int)M5.Display.width() - rs.margin_left - rs.margin_right);
        const int footer_height = 50;
        options.content_height = std::max(
            line_height,
            (int)M5.Display.height() - TOP_H - rs.margin_top -
                std::max(rs.margin_bottom, footer_height));
        options.line_height = line_height;
        int fullwidth_advance = reader_text_renderer().measureFullwidthCellForLayout();
        options.first_line_indent = fullwidth_advance * rs.first_line_indent;
        options.paragraph_spacing_percent = rs.paragraph_spacing * 50;
        options.layout_paragraph = [](void *context, const std::string &text, int width, int indent) {
            return static_cast<TextRenderer *>(context)->layoutUtf8Paragraph(text, width, indent);
        };
        options.text_context = &reader_text_renderer();
        options.resolve_image = [](void *, const std::string &resource,
                                   document_layout::ImageInfo *info) {
            if (!info) return false;
            for (const auto &image : g_article_images) {
                if (image.path == resource && image.width > 0 && image.height > 0) {
                    info->width = image.width;
                    info->height = image.height;
                    return true;
                }
            }
            return false;
        };
        g_article_pages = document_layout::paginate(blocks, options);

        int pages = article_page_count();
        if (reset_page) g_page = 0;
        else if (g_page >= pages) g_page = pages - 1;
        ESP_LOGI(TAG, "article layout blocks=%u pages=%d images=%u",
                 (unsigned)blocks.size(), pages, (unsigned)g_article_images.size());
    }

    bool bing_wallpaper_mode() const
    {
        return _current_feed_is_bing && reader_style_get().show_images && !g_article_images.empty();
    }

    void restore_display_rotation()
    {
        if (_rotation_changed) {
            M5.Display.setRotation(_base_rotation);
            _rotation_changed = false;
        }
    }

    void apply_wallpaper_rotation(const ArticleImage &image)
    {
        restore_display_rotation();
        int screen_w = M5.Display.width();
        int screen_h = M5.Display.height();
        bool image_landscape = image.width >= image.height;
        bool screen_landscape = screen_w >= screen_h;
        if (image_landscape != screen_landscape) {
            M5.Display.setRotation((_base_rotation + 1) & 3);
            _rotation_changed = true;
        }
    }

    bool draw_bing_wallpaper()
    {
        if (!bing_wallpaper_mode()) return false;
        const ArticleImage &image = g_article_images[0];
        if (image.path.empty() || image.width <= 0 || image.height <= 0) return false;
        apply_wallpaper_rotation(image);

        M5.Display.startWrite();
        M5.Display.setEpdMode(reader_epd_mode());
        M5.Display.fillScreen(TFT_WHITE);

        int screen_w = M5.Display.width();
        int screen_h = M5.Display.height();
        float scale = std::max((float)screen_w / (float)image.width, (float)screen_h / (float)image.height);
        if (scale <= 0.0f) scale = 1.0f;
        int draw_w = std::max(1, (int)((float)image.width * scale + 0.5f));
        int draw_h = std::max(1, (int)((float)image.height * scale + 0.5f));
        int x = (screen_w - draw_w) / 2;
        int y = (screen_h - draw_h) / 2;

        bool ok = false;
        if (image.ext == ".jpg" && !image.progressive) {
            ok = M5.Display.drawJpgFile(image.path.c_str(), x, y, draw_w, draw_h, 0, 0, scale, scale);
        } else if (image.ext == ".png") {
            ok = M5.Display.drawPngFile(image.path.c_str(), x, y, draw_w, draw_h, 0, 0, scale, scale);
        } else if (image.ext == ".bmp") {
            ok = M5.Display.drawBmpFile(image.path.c_str(), x, y, draw_w, draw_h, 0, 0, scale, scale);
        }
        if (!ok) {
            M5.Display.fillScreen(TFT_WHITE);
            M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
            system_font::draw_center(M5.Display,
                                     image.progressive ? "Progressive JPEG unsupported" : "Image decode failed",
                                     screen_w / 2,
                                     screen_h / 2 - 18);
        }
        M5.Display.display();
        M5.Display.endWrite();
        return true;
    }

    std::string bing_wallpaper_save_path(const ArticleImage &image) const
    {
        std::string stamp;
        if (g_focus >= 0 && g_focus < (int)g_items.size()) {
            for (char c : g_items[g_focus].date) {
                if (std::isdigit((unsigned char)c)) stamp.push_back(c);
                if (stamp.size() >= 8) break;
            }
        }
        if (stamp.empty()) {
            char hash[16];
            std::snprintf(hash, sizeof(hash), "%08lx", (unsigned long)hash32_string(image.url));
            stamp = hash;
        }
        char hash[16];
        std::snprintf(hash, sizeof(hash), "%08lx", (unsigned long)hash32_string(image.url));
        std::string ext = image.ext.empty() ? ".jpg" : image.ext;
        return std::string(RSS_IMAGE_SAVE_DIR) + "/bing_" + stamp + "_" + hash + ext;
    }

    bool copy_file(const std::string &src, const std::string &dst)
    {
        FILE *in = std::fopen(src.c_str(), "rb");
        if (!in) return false;
        FILE *out = std::fopen(dst.c_str(), "wb");
        if (!out) {
            std::fclose(in);
            return false;
        }

        char buf[4096];
        bool ok = true;
        while (true) {
            size_t n = std::fread(buf, 1, sizeof(buf), in);
            if (n > 0 && std::fwrite(buf, 1, n, out) != n) {
                ok = false;
                break;
            }
            if (n < sizeof(buf)) {
                if (std::ferror(in)) ok = false;
                break;
            }
        }
        if (std::fclose(out) != 0) ok = false;
        std::fclose(in);
        if (!ok) remove(dst.c_str());
        return ok;
    }

    void save_bing_wallpaper()
    {
        if (!bing_wallpaper_mode()) return;
        const ArticleImage &image = g_article_images[0];
        mkdir(RSS_IMAGE_SAVE_DIR, 0775);
        std::string dst = bing_wallpaper_save_path(image);
        bool ok = copy_file(image.path, dst);
        ESP_LOGI(TAG, "save bing wallpaper src=%s dst=%s ok=%d", image.path.c_str(), dst.c_str(), ok ? 1 : 0);
        draw_status("Bing Wallpaper", ok ? "Saved to /sdcard/images" : "Save failed");
        vTaskDelay(pdMS_TO_TICKS(900));
        draw_article();
    }

    int article_page_count() const
    {
        if (bing_wallpaper_mode()) return 1;
        return std::max(1, (int)g_article_pages.size());
    }

    void draw_article()
    {
        if (draw_bing_wallpaper()) return;
        restore_display_rotation();
        M5.Display.startWrite();
        M5.Display.setEpdMode(reader_epd_mode());
        M5.Display.fillScreen(TFT_WHITE);
        draw_topbar("RSS");
        ReaderStyle rs = reader_style_get();
        int lh = reader_text_renderer().lineHeight();
        if (lh <= 0) lh = 48;
        int y = TOP_H + rs.margin_top;
        const int content_width = std::max(100, (int)M5.Display.width() - rs.margin_left - rs.margin_right);
        const document_layout::Page *page = nullptr;
        if (g_page >= 0 && g_page < (int)g_article_pages.size()) {
            page = &g_article_pages[(size_t)g_page];
        }
        std::string page_text;
        if (page) {
            for (const auto &layout_item : *page) {
                if (layout_item.type == document_layout::PageItemType::Text &&
                    !layout_item.text.empty()) {
                    page_text += layout_item.text;
                    page_text += '\n';
                }
            }
        }
        if (!page_text.empty()) reader_text_renderer().preparePage(page_text);

        if (page) {
            for (size_t i = 0; i < page->size(); ++i) {
                if ((i & 7u) == 0) vTaskDelay(1);
                const auto &layout_item = (*page)[i];
                if (layout_item.type == document_layout::PageItemType::Text) {
                    if (!layout_item.text.empty()) {
                        int text_x = rs.margin_left + layout_item.x_offset;
                        if (layout_item.centered) {
                            int text_width = reader_text_renderer().measureUtf8(layout_item.text);
                            text_x = rs.margin_left + std::max(0, (content_width - text_width) / 2);
                        }
                        reader_text_renderer().drawUtf8Line(
                            M5.Display, layout_item.text, text_x, y, TFT_BLACK,
                            layout_item.justify ? layout_item.width : 0);
                    }
                    y += lh;
                } else if (layout_item.type == document_layout::PageItemType::Space) {
                    y += layout_item.height > 0 ? layout_item.height : lh;
                } else if (layout_item.type == document_layout::PageItemType::HorizontalRule) {
                    int rule_y = y + layout_item.height / 2;
                    int inset = std::max(20, content_width / 8);
                    M5.Display.drawFastHLine(rs.margin_left + inset, rule_y,
                                             std::max(1, content_width - inset * 2), TFT_BLACK);
                    y += layout_item.height;
                } else if (layout_item.type == document_layout::PageItemType::Image) {
                    const ArticleImage *image = nullptr;
                    for (const auto &candidate : g_article_images) {
                        if (candidate.path == layout_item.resource) {
                            image = &candidate;
                            break;
                        }
                    }
                    if (!image) {
                        y += layout_item.height;
                        continue;
                    }
                    int draw_w = layout_item.width;
                    int draw_h = layout_item.height;
                    float scale = layout_item.scale;
                    int x = rs.margin_left + std::max(0, (content_width - draw_w) / 2);
                    bool ok = false;
                    if (image->ext == ".jpg" && !image->progressive) {
                        ok = M5.Display.drawJpgFile(image->path.c_str(), x, y,
                                                   draw_w, draw_h, 0, 0, scale, scale);
                    } else if (image->ext == ".png") {
                        ok = M5.Display.drawPngFile(image->path.c_str(), x, y,
                                                   draw_w, draw_h, 0, 0, scale, scale);
                    } else if (image->ext == ".bmp") {
                        ok = M5.Display.drawBmpFile(image->path.c_str(), x, y,
                                                   draw_w, draw_h, 0, 0, scale, scale);
                    }
                    if (!ok) {
                        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
                        system_font::draw_center(
                            M5.Display,
                            image->progressive ? "Progressive JPEG unsupported" : "Image decode failed",
                            M5.Display.width() / 2,
                            y + std::max(0, draw_h / 2 - 18));
                    }
                    y += draw_h;
                }
            }
        }
        char pct[24];
        int pages = article_page_count();
        std::snprintf(pct, sizeof(pct), "%d/%d", g_page + 1, pages);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        system_font::draw_right(M5.Display, pct, M5.Display.width() - 10, M5.Display.height() - 50);
        if (g_page + 1 >= pages) {
            system_font::draw_center(M5.Display, "Last page", M5.Display.width() / 2, M5.Display.height() - 50);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }

    void next_article_page()
    {
        int pages = article_page_count();
        if (g_page + 1 < pages) {
            g_page++;
            draw_article();
        }
    }

    void prev_article_page()
    {
        if (g_page > 0) {
            g_page--;
            draw_article();
        }
    }

    bool article_key(int key)
    {
        if (key == APP_KEY_BACK) {
            restore_display_rotation();
            _view = View::Items;
            redraw_list();
            return true;
        }
        if (key == APP_KEY_EXTRA) {
            restore_display_rotation();
            launch_reader_settings();
            return true;
        }
        if (key == APP_KEY_UP) {
            prev_article_page();
            return true;
        }
        if (key == APP_KEY_OK && bing_wallpaper_mode()) {
            save_bing_wallpaper();
            return true;
        }
        if (key == APP_KEY_DOWN || key == APP_KEY_OK) {
            next_article_page();
            return true;
        }
        return false;
    }
};

void launch_rss_reader()
{
    ActivityManager::instance().push(new RssReaderActivity());
}
