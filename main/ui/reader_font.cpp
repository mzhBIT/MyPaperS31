#include "reader_font.h"
#include "ui/EmbeddedFont.h"
#include "storage/app_paths.h"

#include <esp_log.h>
#include <nvs.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <string>
#include <vector>
#include <algorithm>

static const char *TAG = "READER_FONT";
static const char *DEFAULT_BUILTIN_FONT_PATH = embedded_font::path;
static const char *FALLBACK_BUILTIN_FONT_PATH = "builtin:reader.bin";
static char s_font_path[192] = "builtin:system-ui";

static void trim_line(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) {
        s[--len] = '\0';
    }
}

static bool file_exists(const char *path)
{
    struct stat st = {};
    return path && path[0] && stat(path, &st) == 0 && st.st_size > 0;
}

static bool is_builtin_font_path(const char *path)
{
    return path && (embedded_font::matches(path) ||
                    strcmp(path, FALLBACK_BUILTIN_FONT_PATH) == 0);
}

static const char *select_default_font_path()
{
    // The embedded UI subset is not a complete book font. Prefer a complete
    // SD font for fresh installs and migration from the removed LV bitmap.
    static std::string selected;
    for (const char *name : {"/sdcard/fonts/pingxianzhensong.ttf", "/sdcard/fonts/Hei.ttf"}) {
        if (file_exists(name)) { selected = name; return selected.c_str(); }
    }
    std::vector<std::string> candidates;
    DIR *dir = opendir("/sdcard/fonts");
    if (dir) {
        while (auto *entry = readdir(dir)) {
            const char *ext = strrchr(entry->d_name, '.');
            if (!ext || (strcasecmp(ext, ".ttf") && strcasecmp(ext, ".otf"))) continue;
            std::string path = std::string("/sdcard/fonts/") + entry->d_name;
            if (file_exists(path.c_str())) candidates.push_back(std::move(path));
        }
        closedir(dir);
    }
    if (!candidates.empty()) {
        std::sort(candidates.begin(), candidates.end());
        selected = candidates.front();
        return selected.c_str();
    }
    return DEFAULT_BUILTIN_FONT_PATH;
}

void reader_font_init()
{
    strncpy(s_font_path, select_default_font_path(), sizeof(s_font_path) - 1);
    s_font_path[sizeof(s_font_path) - 1] = '\0';

    FILE *fp = fopen(app_paths::reader_font_file(), "r");
    if (!fp) {
        ESP_LOGI(TAG, "using default font: %s", s_font_path);
        return;
    }

    char line[sizeof(s_font_path)] = {};
    if (fgets(line, sizeof(line), fp)) {
        trim_line(line);
        if (line[0]) {
            if (embedded_font::matches(line)) {
                snprintf(s_font_path, sizeof(s_font_path), "%s", select_default_font_path());
            } else if (is_builtin_font_path(line) || file_exists(line)) {
                strncpy(s_font_path, line, sizeof(s_font_path) - 1);
                s_font_path[sizeof(s_font_path) - 1] = '\0';
            } else {
                strncpy(s_font_path, select_default_font_path(), sizeof(s_font_path) - 1);
                s_font_path[sizeof(s_font_path) - 1] = '\0';
                ESP_LOGW(TAG, "saved font missing, fallback to default: saved=%s default=%s exists=%d",
                         line,
                         s_font_path,
                         is_builtin_font_path(s_font_path) || file_exists(s_font_path) ? 1 : 0);
            }
        }
    }
    fclose(fp);
    ESP_LOGI(TAG, "loaded font: %s", s_font_path);
}

static char s_rss_font_path[192] = {};
static bool s_rss_font_initialized = false;

static bool save_rss_font(const char *path)
{
    nvs_handle_t h;
    if (nvs_open("rss_reader", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "font", path);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

const char *reader_font_path(ReaderProfile profile)
{
    if (profile != ReaderProfile::Rss) return s_font_path;
    if (!s_rss_font_initialized) {
        s_rss_font_initialized = true;
        snprintf(s_rss_font_path, sizeof(s_rss_font_path), "%s", s_font_path);
        nvs_handle_t h;
        bool loaded = false;
        if (nvs_open("rss_reader", NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(s_rss_font_path);
            loaded = nvs_get_str(h, "font", s_rss_font_path, &len) == ESP_OK;
            nvs_close(h);
        }
        if (embedded_font::matches(s_rss_font_path) ||
            (!is_builtin_font_path(s_rss_font_path) && !file_exists(s_rss_font_path)))
            snprintf(s_rss_font_path, sizeof(s_rss_font_path), "%s", select_default_font_path());
        if (!loaded) save_rss_font(s_rss_font_path);
    }
    return s_rss_font_path;
}

const char *reader_font_default_path()
{
    return select_default_font_path();
}

bool reader_font_set(const char *path, ReaderProfile profile)
{
    if (!path || !path[0]) return false;
    if (profile == ReaderProfile::Rss) {
        reader_font_path(profile);
        if (!save_rss_font(path)) return false;
        snprintf(s_rss_font_path, sizeof(s_rss_font_path), "%s", path);
        return true;
    }

    app_paths::ensure_data_dir();
    FILE *fp = fopen(app_paths::reader_font_file(), "w");
    if (!fp) {
        ESP_LOGE(TAG, "cannot save font config: %s", app_paths::reader_font_file());
        return false;
    }
    const bool written = fprintf(fp, "%s\n", path) >= 0;
    const bool closed = fclose(fp) == 0;
    if (!written || !closed) return false;
    strncpy(s_font_path, path, sizeof(s_font_path) - 1);
    s_font_path[sizeof(s_font_path) - 1] = '\0';
    ESP_LOGI(TAG, "selected font: %s", s_font_path);
    return true;
}
