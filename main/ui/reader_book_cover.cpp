#include "ui/reader_book_cover.h"
#include "ui/UiLocale.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <nvs.h>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr const char *NVS_NAMESPACE = "reader_cover";
constexpr const char *NVS_PATH_KEY = "path";

bool initialized = false;
std::string selected_path;

bool supported_image(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp";
}

bool regular_file(const std::string &path)
{
    struct stat info = {};
    return !path.empty() && stat(path.c_str(), &info) == 0 && !S_ISDIR(info.st_mode);
}

bool save_path(const std::string &path)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return false;
    if (path.empty()) error = nvs_erase_key(handle, NVS_PATH_KEY);
    else error = nvs_set_str(handle, NVS_PATH_KEY, path.c_str());
    if (error == ESP_ERR_NVS_NOT_FOUND && path.empty()) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

void ensure_initialized()
{
    if (initialized) return;
    initialized = true;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = 0;
    if (nvs_get_str(handle, NVS_PATH_KEY, nullptr, &length) == ESP_OK && length > 1) {
        std::vector<char> value(length);
        if (nvs_get_str(handle, NVS_PATH_KEY, value.data(), &length) == ESP_OK &&
            supported_image(value.data()) && regular_file(value.data())) {
            selected_path = value.data();
        }
    }
    nvs_close(handle);
}

} // namespace

bool reader_book_cover_enabled()
{
    ensure_initialized();
    return !selected_path.empty();
}

const char *reader_book_cover_path()
{
    ensure_initialized();
    return selected_path.c_str();
}

bool reader_book_cover_set(const char *path, std::string *message)
{
    ensure_initialized();
    const std::string selected = path ? path : "";
    if (!supported_image(selected)) {
        if (message) *message = ui_locale::text(ui_locale::TextId::ImageFormatsHint);
        return false;
    }
    if (!regular_file(selected)) {
        if (message) *message = ui_locale::text(ui_locale::TextId::ImageOpenFailed);
        return false;
    }
    if (!save_path(selected)) {
        if (message) *message = ui_locale::text(ui_locale::TextId::BookCoverSaveFailed);
        return false;
    }
    selected_path = selected;
    if (message) *message = ui_locale::text(ui_locale::TextId::BookCoverSet);
    return true;
}

bool reader_book_cover_clear()
{
    ensure_initialized();
    if (!save_path({})) return false;
    selected_path.clear();
    return true;
}

bool reader_book_cover_matches(const char *path)
{
    ensure_initialized();
    return path && selected_path == path;
}

void reader_book_cover_file_renamed(const char *old_path, const char *new_path)
{
    if (!reader_book_cover_matches(old_path) || !new_path) return;
    const std::string renamed = new_path;
    if (!supported_image(renamed) || !regular_file(renamed) || !save_path(renamed)) return;
    selected_path = renamed;
}

void reader_book_cover_file_deleted(const char *path)
{
    if (reader_book_cover_matches(path)) reader_book_cover_clear();
}
