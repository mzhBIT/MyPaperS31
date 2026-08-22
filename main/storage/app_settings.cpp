#include "storage/app_settings.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "SETTINGS";
static reader_settings_t g_settings;

static bool g_loaded = false;

static void ensure_defaults(void) {
    if (g_settings.font_size < 12) g_settings.font_size = 24;
    if (!g_settings.font_name[0]) strcpy(g_settings.font_name, "");
    if (g_settings.line_spacing < 0) g_settings.line_spacing = 4;
    if (g_settings.paragraph_spacing < 0) g_settings.paragraph_spacing = 8;
    if (g_settings.margin_left < 0) g_settings.margin_left = 10;
    if (g_settings.margin_right < 0) g_settings.margin_right = 10;
    if (g_settings.margin_top < 0) g_settings.margin_top = 10;
    if (g_settings.margin_bottom < 0) g_settings.margin_bottom = 10;
}

bool app_settings_load(const char *path) {
    g_settings = reader_settings_default();
    ensure_defaults();

    if (!path || !path[0]) {
        ESP_LOGW(TAG, "No settings path, using defaults");
        return false;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        ESP_LOGW(TAG, "No settings file at %s, using defaults", path);
        return false;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char key[64] = {0};
        int val = 0;
        if (sscanf(line, "%63[^=]=%d", key, &val) == 2) {
            if (strcmp(key, "font_size") == 0) g_settings.font_size = val;
            else if (strcmp(key, "line_spacing") == 0) g_settings.line_spacing = val;
            else if (strcmp(key, "para_spacing") == 0) g_settings.paragraph_spacing = val;
            else if (strcmp(key, "underline") == 0) g_settings.underline = (val != 0);
            else if (strcmp(key, "direction") == 0) g_settings.direction = (text_direction_t)val;
            else if (strcmp(key, "margin_left") == 0) g_settings.margin_left = val;
            else if (strcmp(key, "margin_right") == 0) g_settings.margin_right = val;
            else if (strcmp(key, "margin_top") == 0) g_settings.margin_top = val;
            else if (strcmp(key, "margin_bottom") == 0) g_settings.margin_bottom = val;
        }
        // font_name line
        if (strncmp(line, "font_name=", 10) == 0) {
            size_t len = strlen(line) - 10;
            while (len > 0 && (line[10+len-1] == '\n' || line[10+len-1] == '\r')) len--;
            memcpy(g_settings.font_name, line + 10, len < 63 ? len : 63);
        }
    }
    fclose(fp);

    ensure_defaults();
    g_loaded = true;
    ESP_LOGI(TAG, "Settings loaded from %s", path);
    return true;
}

bool app_settings_save(const char *path) {
    if (!path || !path[0]) return false;

    FILE *fp = fopen(path, "w");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot write settings to %s", path);
        return false;
    }

    fprintf(fp, "font_size=%d\n", g_settings.font_size);
    fprintf(fp, "font_name=%s\n", g_settings.font_name);
    fprintf(fp, "line_spacing=%d\n", g_settings.line_spacing);
    fprintf(fp, "para_spacing=%d\n", g_settings.paragraph_spacing);
    fprintf(fp, "underline=%d\n", g_settings.underline ? 1 : 0);
    fprintf(fp, "direction=%d\n", (int)g_settings.direction);
    fprintf(fp, "margin_left=%d\n", g_settings.margin_left);
    fprintf(fp, "margin_right=%d\n", g_settings.margin_right);
    fprintf(fp, "margin_top=%d\n", g_settings.margin_top);
    fprintf(fp, "margin_bottom=%d\n", g_settings.margin_bottom);

    fclose(fp);
    ESP_LOGI(TAG, "Settings saved to %s", path);
    return true;
}

reader_settings_t app_settings_get(void) {
    return g_settings;
}

void app_settings_set(const reader_settings_t *s) {
    if (s) g_settings = *s;
}
