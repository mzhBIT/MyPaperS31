#include "reader_style.h"
#include "storage/app_paths.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <nvs.h>

static const char *TAG = "READER_STYLE";
static const char *NVS_NS = "reader_style";
static const char *NVS_KEY = "style_v6";
static const char *NVS_KEY_V5 = "style_v5";
static const char *NVS_KEY_V4 = "style_v4";
static const char *NVS_KEY_V3 = "style_v3";
static const char *NVS_KEY_V2 = "style_v2";
static const char *NVS_KEY_V1 = "style_v1";

struct ReaderStyleV3 {
    int font_size;
    int line_spacing;
    int margin_left;
    int margin_right;
    int margin_top;
    int margin_bottom;
    int refresh_mode;
    int show_images;
    int first_line_indent;
    int paragraph_spacing;
};

struct ReaderStyleV4 {
    int font_size;
    int line_spacing;
    int margin_left;
    int margin_right;
    int margin_top;
    int margin_bottom;
    int refresh_mode;
    int show_images;
    int first_line_indent;
    int paragraph_spacing;
    int full_refresh_pages;
};

struct ReaderStyleV5 {
    int font_size;
    int line_spacing;
    int margin_left;
    int margin_right;
    int margin_top;
    int margin_bottom;
    int refresh_mode;
    int show_images;
    int first_line_indent;
    int paragraph_spacing;
    int full_refresh_pages;
    int underline_type;
};

struct ReaderStyleV1 {
    int font_size;
    int line_spacing;
    int margin_left;
    int margin_right;
    int margin_top;
    int margin_bottom;
    int refresh_mode;
    int show_images;
};

static ReaderStyle s_style;

static bool load_style_from_sd(ReaderStyle *out)
{
    if (!out) return false;

    ReaderStyle style = reader_style_clamp(ReaderStyle());
    FILE *fp = fopen(app_paths::reader_style_file(), "r");
    if (!fp) return false;

    bool has_legacy_paragraph_spacing = false;
    char key[48] = {};
    int val = 0;
    char line[128] = {};
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%47[^=]=%d", key, &val) != 2) continue;
        if (strcmp(key, "font_size") == 0) style.font_size = val;
        else if (strcmp(key, "line_spacing") == 0) style.line_spacing = val;
        else if (strcmp(key, "margin_left") == 0) style.margin_left = val;
        else if (strcmp(key, "margin_right") == 0) style.margin_right = val;
        else if (strcmp(key, "margin_top") == 0) style.margin_top = val;
        else if (strcmp(key, "margin_bottom") == 0) style.margin_bottom = val;
        else if (strcmp(key, "refresh_mode") == 0) style.refresh_mode = val;
        else if (strcmp(key, "show_images") == 0) style.show_images = val;
        else if (strcmp(key, "first_line_indent") == 0) style.first_line_indent = val;
        else if (strcmp(key, "paragraph_spacing") == 0) {
            style.paragraph_spacing = val;
            has_legacy_paragraph_spacing = true;
        }
        else if (strcmp(key, "full_refresh_pages") == 0) style.full_refresh_pages = val;
        else if (strcmp(key, "underline_type") == 0) style.underline_type = val;
        else if (strcmp(key, "hide_header") == 0) style.hide_header = val;
    }
    fclose(fp);

    if (has_legacy_paragraph_spacing) style.paragraph_spacing *= 5;
    *out = reader_style_clamp(style);
    return true;
}

static bool load_style_from_nvs(ReaderStyle *out, bool *migrated)
{
    if (!out) return false;
    if (migrated) *migrated = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;

    ReaderStyle style = {};
    size_t len = sizeof(style);
    bool loaded = false;
    err = nvs_get_blob(h, NVS_KEY, &style, &len);
    loaded = err == ESP_OK && len == sizeof(style);
    if (!loaded) {
        ReaderStyleV5 legacy_v5 = {};
        len = sizeof(legacy_v5);
        err = nvs_get_blob(h, NVS_KEY_V5, &legacy_v5, &len);
        if (err == ESP_OK && len == sizeof(legacy_v5)) {
            style = ReaderStyle();
            style.font_size = legacy_v5.font_size;
            style.line_spacing = legacy_v5.line_spacing;
            style.margin_left = legacy_v5.margin_left;
            style.margin_right = legacy_v5.margin_right;
            style.margin_top = legacy_v5.margin_top;
            style.margin_bottom = legacy_v5.margin_bottom;
            style.refresh_mode = legacy_v5.refresh_mode;
            style.show_images = legacy_v5.show_images;
            style.first_line_indent = legacy_v5.first_line_indent;
            style.paragraph_spacing = legacy_v5.paragraph_spacing;
            style.full_refresh_pages = legacy_v5.full_refresh_pages;
            style.underline_type = legacy_v5.underline_type;
            loaded = true;
            if (migrated) *migrated = true;
        }
    }
    if (!loaded) {
        ReaderStyleV4 legacy_v4 = {};
        len = sizeof(legacy_v4);
        err = nvs_get_blob(h, NVS_KEY_V4, &legacy_v4, &len);
        if (err == ESP_OK && len == sizeof(legacy_v4)) {
            style = ReaderStyle();
            style.font_size = legacy_v4.font_size;
            style.line_spacing = legacy_v4.line_spacing;
            style.margin_left = legacy_v4.margin_left;
            style.margin_right = legacy_v4.margin_right;
            style.margin_top = legacy_v4.margin_top;
            style.margin_bottom = legacy_v4.margin_bottom;
            style.refresh_mode = legacy_v4.refresh_mode;
            style.show_images = legacy_v4.show_images;
            style.first_line_indent = legacy_v4.first_line_indent;
            style.paragraph_spacing = legacy_v4.paragraph_spacing;
            style.full_refresh_pages = legacy_v4.full_refresh_pages;
            loaded = true;
            if (migrated) *migrated = true;
        }
    }
    if (!loaded) {
        ReaderStyleV3 legacy_v3 = {};
        len = sizeof(legacy_v3);
        err = nvs_get_blob(h, NVS_KEY_V3, &legacy_v3, &len);
        if (err == ESP_OK && len == sizeof(legacy_v3)) {
            style = ReaderStyle();
            style.font_size = legacy_v3.font_size;
            style.line_spacing = legacy_v3.line_spacing;
            style.margin_left = legacy_v3.margin_left;
            style.margin_right = legacy_v3.margin_right;
            style.margin_top = legacy_v3.margin_top;
            style.margin_bottom = legacy_v3.margin_bottom;
            style.refresh_mode = legacy_v3.refresh_mode;
            style.show_images = legacy_v3.show_images;
            style.first_line_indent = legacy_v3.first_line_indent;
            style.paragraph_spacing = legacy_v3.paragraph_spacing;
            loaded = true;
            if (migrated) *migrated = true;
        }
    }
    if (!loaded) {
        ReaderStyleV3 legacy_v2 = {};
        len = sizeof(legacy_v2);
        err = nvs_get_blob(h, NVS_KEY_V2, &legacy_v2, &len);
        if (err == ESP_OK && len == sizeof(legacy_v2)) {
            style = ReaderStyle();
            style.font_size = legacy_v2.font_size;
            style.line_spacing = legacy_v2.line_spacing;
            style.margin_left = legacy_v2.margin_left;
            style.margin_right = legacy_v2.margin_right;
            style.margin_top = legacy_v2.margin_top;
            style.margin_bottom = legacy_v2.margin_bottom;
            style.refresh_mode = legacy_v2.refresh_mode;
            style.show_images = legacy_v2.show_images;
            style.first_line_indent = legacy_v2.first_line_indent;
            style.paragraph_spacing = legacy_v2.paragraph_spacing;
            style.paragraph_spacing *= 5;
            loaded = true;
            if (migrated) *migrated = true;
        }
    }
    if (!loaded) {
        ReaderStyleV1 legacy = {};
        len = sizeof(legacy);
        err = nvs_get_blob(h, NVS_KEY_V1, &legacy, &len);
        if (err == ESP_OK && len == sizeof(legacy)) {
            style = ReaderStyle();
            style.font_size = legacy.font_size;
            style.line_spacing = legacy.line_spacing;
            style.margin_left = legacy.margin_left;
            style.margin_right = legacy.margin_right;
            style.margin_top = legacy.margin_top;
            style.margin_bottom = legacy.margin_bottom;
            style.refresh_mode = legacy.refresh_mode;
            style.show_images = legacy.show_images;
            loaded = true;
            if (migrated) *migrated = true;
        }
    }
    nvs_close(h);
    if (!loaded) return false;

    *out = reader_style_clamp(style);
    return true;
}

ReaderStyle reader_style_clamp(ReaderStyle style)
{
    style.font_size = std::min(56, std::max(24, style.font_size));
    style.line_spacing = std::min(32, std::max(0, style.line_spacing));
    style.margin_left = std::min(120, std::max(0, style.margin_left));
    style.margin_right = std::min(120, std::max(0, style.margin_right));
    style.margin_top = std::min(80, std::max(0, style.margin_top));
    style.margin_bottom = std::min(80, std::max(0, style.margin_bottom));
    style.refresh_mode = std::min(4, std::max(1, style.refresh_mode));
    style.show_images = style.show_images ? 1 : 0;
    style.first_line_indent = std::min(4, std::max(0, style.first_line_indent));
    style.paragraph_spacing = std::min(20, std::max(0, style.paragraph_spacing));
    style.full_refresh_pages = std::min(60, std::max(1, style.full_refresh_pages));
    style.underline_type = std::min(2, std::max(0, style.underline_type));
    style.hide_header = style.hide_header ? 1 : 0;
    return style;
}

const char *reader_style_refresh_mode_name(int mode)
{
    switch (mode) {
    case 1:
        return "Quality";
    case 2:
        return "Text";
    case 3:
        return "Fast";
    case 4:
        return "Fastest";
    default:
        return "Fast";
    }
}

int reader_style_next_refresh_mode(int mode, int delta)
{
    static constexpr int kOrder[] = {4, 3, 2, 1}; // Fastest, Fast, Text, Quality.
    int index = 1; // Default to Fast if the stored value is invalid.
    for (int i = 0; i < (int)(sizeof(kOrder) / sizeof(kOrder[0])); ++i) {
        if (kOrder[i] == mode) {
            index = i;
            break;
        }
    }

    constexpr int count = (int)(sizeof(kOrder) / sizeof(kOrder[0]));
    index = (index + delta) % count;
    if (index < 0) index += count;
    return kOrder[index];
}

const char *reader_style_underline_name(int type)
{
    switch (type) {
    case 1:
        return "Solid";
    case 2:
        return "Dashed";
    default:
        return "None";
    }
}

int reader_style_next_underline_type(int type, int delta)
{
    constexpr int count = 3;
    int index = std::min(2, std::max(0, type));
    index = (index + delta) % count;
    if (index < 0) index += count;
    return index;
}

void reader_style_init()
{
    s_style = reader_style_clamp(ReaderStyle());

    bool migrated = false;
    if (load_style_from_nvs(&s_style, &migrated)) {
        ESP_LOGI(TAG, "loaded style from NVS size=%d line=%d indent=%d paragraph=%d full_refresh=%d underline=%s hide_header=%d margins=%d,%d,%d,%d refresh=%s",
                 s_style.font_size,
                 s_style.line_spacing,
                 s_style.first_line_indent,
                 s_style.paragraph_spacing,
                 s_style.full_refresh_pages,
                 reader_style_underline_name(s_style.underline_type),
                 s_style.hide_header,
                 s_style.margin_left,
                 s_style.margin_right,
                 s_style.margin_top,
                 s_style.margin_bottom,
                 reader_style_refresh_mode_name(s_style.refresh_mode));
        if (migrated) {
            ESP_LOGI(TAG, "migrating reader style to v6");
            reader_style_save();
        }
        return;
    }

    if (load_style_from_sd(&s_style)) {
        ESP_LOGI(TAG, "loaded style from SD, migrating to NVS");
        reader_style_save();
        return;
    }

    ESP_LOGI(TAG, "using default reader style");
}

ReaderStyle reader_style_get()
{
    return s_style;
}

void reader_style_set(const ReaderStyle &style)
{
    s_style = reader_style_clamp(style);
}

bool reader_style_save()
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot open NVS for reader style: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(h, NVS_KEY, &s_style, sizeof(s_style));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot save reader style to NVS: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "saved reader style to NVS");
    return true;
}
