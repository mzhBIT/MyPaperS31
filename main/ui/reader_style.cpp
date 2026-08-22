#include "reader_style.h"
#include "storage/app_paths.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <nvs.h>

static const char *TAG = "READER_STYLE";
static const char *NVS_NS = "reader_style";
static const char *NVS_KEY = "style_v1";

static ReaderStyle s_style;

static bool load_style_from_sd(ReaderStyle *out)
{
    if (!out) return false;

    ReaderStyle style = reader_style_clamp(ReaderStyle());
    FILE *fp = fopen(app_paths::reader_style_file(), "r");
    if (!fp) return false;

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
    }
    fclose(fp);

    *out = reader_style_clamp(style);
    return true;
}

static bool load_style_from_nvs(ReaderStyle *out)
{
    if (!out) return false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;

    ReaderStyle style = {};
    size_t len = sizeof(style);
    err = nvs_get_blob(h, NVS_KEY, &style, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(style)) return false;

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

void reader_style_init()
{
    s_style = reader_style_clamp(ReaderStyle());

    if (load_style_from_nvs(&s_style)) {
        ESP_LOGI(TAG, "loaded style from NVS size=%d line=%d margins=%d,%d,%d,%d refresh=%s",
                 s_style.font_size,
                 s_style.line_spacing,
                 s_style.margin_left,
                 s_style.margin_right,
                 s_style.margin_top,
                 s_style.margin_bottom,
                 reader_style_refresh_mode_name(s_style.refresh_mode));
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
