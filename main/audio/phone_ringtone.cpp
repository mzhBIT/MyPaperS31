#include "audio/phone_ringtone.h"

#include "audio/audio_file_formats.h"
#include "audio/audio_radio.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {

constexpr char TAG[] = "PHONE_RING";
constexpr char NVS_NAMESPACE[] = "phone";
constexpr char NVS_KEY[] = "ringtone";
constexpr size_t PATH_SIZE = 384;

enum class CommandType : uint8_t { Start, Stop };
struct Command {
    CommandType type;
    char path[PATH_SIZE];
};

QueueHandle_t s_commands = nullptr;

bool playable_file(const char *path)
{
    if (!path || std::strncmp(path, "/sdcard/", 8) != 0 ||
        !audio_file_formats::is_supported(path)) return false;
    struct stat st{};
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void worker(void *)
{
    bool active = false;
    char current[PATH_SIZE]{};
    for (;;) {
        Command command{};
        if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (command.type == CommandType::Stop) {
                if (active) {
                    audio_radio_stop();
                    audio_radio_deinit();
                    audio_radio_force_speaker(false);
                    active = false;
                    current[0] = '\0';
                }
            } else if (playable_file(command.path)) {
                if (active && std::strcmp(current, command.path) == 0) continue;
                if (active) {
                    audio_radio_stop();
                    audio_radio_deinit();
                } else if (audio_radio_get_state() == AUDIO_PLAYBACK_PLAYING ||
                           audio_radio_get_state() == AUDIO_PLAYBACK_PAUSED) {
                    // A hidden Music/Radio page may still own the shared player.
                    // Stop it before changing the temporary output routing.
                    audio_radio_stop();
                }
                audio_radio_force_speaker(true);
                std::snprintf(current, sizeof(current), "%s", command.path);
                const esp_err_t ret = audio_radio_play_file(current);
                active = ret == ESP_OK;
                if (!active) {
                    ESP_LOGW(TAG, "Ringtone playback failed: %s", esp_err_to_name(ret));
                    audio_radio_deinit();
                    audio_radio_force_speaker(false);
                }
            }
        }
        if (active) {
            const auto state = audio_radio_get_state();
            if (state == AUDIO_PLAYBACK_FINISHED || state == AUDIO_PLAYBACK_STOPPED) {
                // A short file repeats until the call leaves the incoming state.
                const esp_err_t ret = audio_radio_play_file(current);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Ringtone repeat failed: %s", esp_err_to_name(ret));
                    audio_radio_deinit();
                    audio_radio_force_speaker(false);
                    active = false;
                }
            } else if (state == AUDIO_PLAYBACK_ERROR) {
                ESP_LOGW(TAG, "Ringtone decoder stopped with an error");
                audio_radio_deinit();
                audio_radio_force_speaker(false);
                active = false;
            }
        }
    }
}

bool ensure_worker()
{
    if (s_commands) return true;
    s_commands = xQueueCreate(1, sizeof(Command));
    if (!s_commands) return false;
    // audio_radio_play_file() reads NVS and may disable the SPI flash/PSRAM
    // cache. A task executing that path must have an internal-RAM stack.
    if (xTaskCreatePinnedToCore(worker, "phone_ring", 8192, nullptr, 4,
                                nullptr, 0) == pdPASS) return true;
    vQueueDelete(s_commands);
    s_commands = nullptr;
    return false;
}

} // namespace

extern "C" esp_err_t phone_ringtone_set_path(const char *path)
{
    if (!path || std::strlen(path) >= PATH_SIZE ||
        (path[0] && !playable_file(path))) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = path[0] ? nvs_set_str(handle, NVS_KEY, path) : nvs_erase_key(handle, NVS_KEY);
    if (ret == ESP_ERR_NVS_NOT_FOUND && !path[0]) ret = ESP_OK;
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

extern "C" esp_err_t phone_ringtone_get_path(char *path, unsigned size)
{
    if (!path || size == 0) return ESP_ERR_INVALID_ARG;
    path[0] = '\0';
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;
    size_t length = size;
    ret = nvs_get_str(handle, NVS_KEY, path, &length);
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) path[0] = '\0';
    return ret;
}

extern "C" void phone_ringtone_start(void)
{
    Command command{};
    if (phone_ringtone_get_path(command.path, sizeof(command.path)) != ESP_OK ||
        !command.path[0]) return;
    if (!ensure_worker()) {
        ESP_LOGW(TAG, "Ringtone worker unavailable");
        return;
    }
    command.type = CommandType::Start;
    xQueueOverwrite(s_commands, &command);
}

extern "C" void phone_ringtone_stop(void)
{
    if (!s_commands) return;
    Command command{};
    command.type = CommandType::Stop;
    xQueueOverwrite(s_commands, &command);
}
