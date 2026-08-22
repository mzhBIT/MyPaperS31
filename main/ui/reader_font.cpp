#include "reader_font.h"
#include "storage/app_paths.h"

#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static const char *TAG = "READER_FONT";
static const char *DEFAULT_BUILTIN_FONT_PATH = "builtin:source_han_sans_cn_36";
static const char *FALLBACK_BUILTIN_FONT_PATH = "builtin:reader.bin";
static char s_font_path[192] = "builtin:source_han_sans_cn_36";

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
    return path && (strcmp(path, DEFAULT_BUILTIN_FONT_PATH) == 0 ||
                    strcmp(path, FALLBACK_BUILTIN_FONT_PATH) == 0);
}

static const char *select_default_font_path()
{
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
            if (is_builtin_font_path(line) || file_exists(line)) {
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

const char *reader_font_path()
{
    return s_font_path;
}

const char *reader_font_default_path()
{
    return select_default_font_path();
}

bool reader_font_set(const char *path)
{
    if (!path || !path[0]) return false;

    strncpy(s_font_path, path, sizeof(s_font_path) - 1);
    s_font_path[sizeof(s_font_path) - 1] = '\0';

    app_paths::ensure_data_dir();
    FILE *fp = fopen(app_paths::reader_font_file(), "w");
    if (!fp) {
        ESP_LOGE(TAG, "cannot save font config: %s", app_paths::reader_font_file());
        return false;
    }
    fprintf(fp, "%s\n", s_font_path);
    fclose(fp);
    ESP_LOGI(TAG, "selected font: %s", s_font_path);
    return true;
}
