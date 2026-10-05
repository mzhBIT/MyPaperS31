#include "activities/WereadActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/SettingActivity.h"
#include "renderer/CoverThumbnail.h"
#include "storage/app_paths.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/UiLocale.h"
#include "ui/ReaderOpening.h"
#include "weread/WereadApi.h"
#include "weread/WereadCrypto.h"
#include "weread/WereadQr.h"
#include "weread/WereadStore.h"
#include "weread/WereadOffline.h"
#include "wifi/wifi_setting.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_task.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>

extern void launch_weread_reader(const char *path, int initial_spine,
                                 ReaderOpeningWhite *opening);

namespace {

constexpr const char *TAG = "WEREAD_UI";
constexpr int HEADER_H = header_layout::height;
constexpr int LIST_TOP = HEADER_H;
constexpr int LIST_BOTTOM = 1124;
constexpr int LIST_ROWS = 5;
constexpr int LIST_ROW_H = (LIST_BOTTOM - LIST_TOP) / LIST_ROWS;
constexpr int FOOTER_Y = 1138;
constexpr int REVIEW_TABS_TOP = HEADER_H;
constexpr int REVIEW_TABS_H = 72;
constexpr int REVIEW_LIST_TOP = REVIEW_TABS_TOP + REVIEW_TABS_H + 12;
constexpr int REVIEW_ROWS = 4;
constexpr int REVIEW_ROW_H = (LIST_BOTTOM - REVIEW_LIST_TOP) / REVIEW_ROWS;
// This task reads NVS and may enter other flash-backed ESP-IDF APIs. Its stack
// therefore must remain accessible while the flash cache is disabled.
constexpr uint32_t WEREAD_TASK_STACK_BYTES = 8192;
constexpr uint32_t EPUB_PROGRESS_MAGIC = 0x45504231; // EPB1
constexpr uint32_t EPUB_PROGRESS_VERSION = 2;

bool load_local_reader_spine(const std::string &manifest_path, int chapter_count, int *spine)
{
    if (spine) *spine = 0;
    if (manifest_path.empty() || chapter_count <= 0) return false;

    size_t hash = 0;
    for (unsigned char byte : manifest_path) hash = hash * 31 + byte;
    char key[32];
    std::snprintf(key, sizeof(key), "%08lx", static_cast<unsigned long>(hash));
    const std::string path = std::string(app_paths::epub_progress_dir()) + "/" + key + ".pos";

    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t magic = 0, version = 0, layout = 0, saved_spine = 0;
    uint64_t position = 0;
    const bool ok = std::fread(&magic, 1, sizeof(magic), file) == sizeof(magic) &&
                    std::fread(&version, 1, sizeof(version), file) == sizeof(version) &&
                    std::fread(&layout, 1, sizeof(layout), file) == sizeof(layout) &&
                    std::fread(&saved_spine, 1, sizeof(saved_spine), file) == sizeof(saved_spine) &&
                    std::fread(&position, 1, sizeof(position), file) == sizeof(position);
    std::fclose(file);
    (void)layout;
    (void)position;
    if (!ok || magic != EPUB_PROGRESS_MAGIC || version != EPUB_PROGRESS_VERSION ||
        saved_spine >= static_cast<uint32_t>(chapter_count)) return false;
    if (spine) *spine = static_cast<int>(saved_spine);
    return true;
}

enum class WorkKind { LoadShelf, LoadCatalog, LoadChapter, LoadCovers, LoadReviews };
enum class ResultKind { LoginQr, Shelf, Catalog, OpenReader, CoversReady, Reviews, Error, Cancelled };

struct WorkResult {
    ResultKind kind = ResultKind::Error;
    std::string message;
    std::string login_url;
    std::vector<weread::Book> books;
    std::vector<weread::Chapter> chapters;
    std::vector<weread::Review> reviews;
    std::string format;
    bool cached = false;
    bool offline = false;
    std::string reader_path;
    int reader_spine = 0;
    int cover_page = -1;
    bool covers_changed = false;
    bool cover_network_failed = false;
    int review_list_type = 3;
};

struct WorkJob {
    WorkKind kind = WorkKind::LoadShelf;
    weread::Book book;
    weread::Chapter chapter;
    int chapter_index = 0;
    bool resume_saved_position = false;
    int cover_page = -1;
    int review_list_type = 3;
    std::vector<weread::Book> cover_books;
    QueueHandle_t result_queue = nullptr;
    const volatile bool *cancelled = nullptr;
    bool external_stack = false;
};

bool send_result(WorkJob *job, std::unique_ptr<WorkResult> result)
{
    WorkResult *raw = result.release();
    if (xQueueSend(job->result_queue, &raw, pdMS_TO_TICKS(1000)) == pdTRUE) return true;
    delete raw;
    return false;
}

void delete_task(bool external_stack)
{
    if (external_stack) vTaskDeleteWithCaps(nullptr);
    else vTaskDelete(nullptr);
}

[[noreturn]] void send_final(WorkJob *job, std::unique_ptr<WorkResult> result,
                             bool external_stack, weread::HttpClient *client = nullptr)
{
    if (client) client->close();
    send_result(job, std::move(result));
    delete_task(external_stack);
    __builtin_unreachable();
}

void weread_task(void *argument)
{
    std::unique_ptr<WorkJob> job(static_cast<WorkJob *>(argument));
    const bool external_stack = job->external_stack;
    auto error_result = [&](const std::string &message) {
        auto result = std::make_unique<WorkResult>();
        result->kind = ResultKind::Error;
        result->message = message;
        return result;
    };

    if (!weread::crypto_self_test()) {
        send_final(job.get(), error_result("Protocol self-test failed"), external_stack);
    }
    if (job->kind == WorkKind::LoadCatalog) {
        auto cached = std::make_unique<WorkResult>();
        cached->kind = ResultKind::Catalog;
        if (weread::store::load_chapters(job->book.id, &cached->chapters, &cached->format)) {
            weread::store::save_manifest(job->book, cached->chapters, cached->format);
            cached->cached = true;
            cached->offline = weread::offline_mode() || !wifi_setting_is_connected();
            send_final(job.get(), std::move(cached), external_stack);
        }
    }
    if (job->kind == WorkKind::LoadChapter && !job->chapter.uid.empty()) {
        const auto &chapter = job->chapter;
        struct stat epub_info = {};
        const bool whole_epub = stat(weread::store::epub_payload_path(job->book.id).c_str(), &epub_info) == 0 &&
                                epub_info.st_size > 0;
        if (whole_epub || weread::store::chapter_cached(job->book.id, chapter)) {
            auto ready = std::make_unique<WorkResult>();
            ready->kind = ResultKind::OpenReader;
            ready->reader_path = weread::store::manifest_path(job->book.id);
            ready->reader_spine = job->resume_saved_position ? -1 : job->chapter_index;
            // Never leave the WeRead HTTP task alive while the EPD reader is
            // rendering. The two pipelines contend for internal RAM and can
            // starve the waveform producer. Cached body content wins; optional
            // annotation metadata must not delay or overlap reader startup.
            send_final(job.get(), std::move(ready), external_stack);
        }
    }

    if (weread::offline_mode()) {
        send_final(job.get(), error_result(job->kind == WorkKind::LoadChapter
            ? "Chapter is not cached; connect online to download" : "Unavailable offline"), external_stack);
    }
    if (wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS) != ESP_OK) {
        if (job->kind == WorkKind::LoadCovers) {
            auto result = std::make_unique<WorkResult>();
            result->kind = ResultKind::CoversReady;
            result->cover_page = job->cover_page;
            result->cover_network_failed = true;
            send_final(job.get(), std::move(result), external_stack);
        }
        if (job->kind == WorkKind::LoadShelf) {
            auto cached = std::make_unique<WorkResult>();
            cached->kind = ResultKind::Shelf;
            if (weread::store::load_shelf(&cached->books)) {
                cached->cached = true;
                cached->offline = true;
                send_final(job.get(), std::move(cached), external_stack);
            }
        }
        send_final(job.get(), error_result("WiFi connection failed and no offline cache is available"),
                   external_stack);
    }

    if (*job->cancelled) {
        auto cancelled = std::make_unique<WorkResult>();
        cancelled->kind = ResultKind::Cancelled;
        send_final(job.get(), std::move(cancelled), external_stack);
    }
    weread::HttpClient client;
    if (job->kind == WorkKind::LoadCovers) {
        auto result = std::make_unique<WorkResult>();
        result->kind = ResultKind::CoversReady;
        result->cover_page = job->cover_page;
        for (const auto &book : job->cover_books) {
            if (*job->cancelled) break;
            if (book.cover_url.empty()) continue;
            const std::string cover = weread::store::cover_path(book.cover_url);
            struct stat info = {};
            if (stat(cover.c_str(), &info) == 0 && info.st_size > 0) continue;
            weread::HttpResponse image = client.get(book.cover_url);
            if (image.ok() && weread::store::save_response_file(
                    cover, image.data(), image.size())) {
                result->covers_changed = true;
                ESP_LOGI(TAG, "shelf cover cached book=%s bytes=%u",
                         book.id.c_str(), static_cast<unsigned>(image.size()));
            } else {
                ESP_LOGW(TAG, "shelf cover download failed book=%s status=%d",
                         book.id.c_str(), image.status);
            }
        }
        send_final(job.get(), std::move(result), external_stack, &client);
    }
    weread::Api api(&client);
    if (client.session().valid() && client.session().api_key.empty()) {
        std::string key_error;
        if (!api.ensure_api_key(&key_error)) {
            ESP_LOGW(TAG, "official API key bootstrap deferred: %s", key_error.c_str());
        }
    }
    auto sync_pending_progress = [&]() {
        weread::store::PendingProgress pending;
        if (!weread::store::load_pending_progress(&pending)) return;
        std::string sync_error;
        if (api.upload_progress(pending.book_id, pending.chapter, pending.percent,
                                pending.active_seconds, pending.chapter_offset, pending.summary,
                                &sync_error)) {
            weread::store::clear_pending_progress();
            ESP_LOGI(TAG, "pending progress uploaded: %d%% active=%u s", pending.percent,
                     static_cast<unsigned>(pending.active_seconds));
        } else {
            ESP_LOGW(TAG, "pending progress retained: %s", sync_error.c_str());
        }
    };
    if (client.session().valid()) sync_pending_progress();
    if (job->kind == WorkKind::LoadReviews) {
        auto result = std::make_unique<WorkResult>();
        result->kind = ResultKind::Reviews;
        result->review_list_type = job->review_list_type;
        std::string error;
        if (!api.get_book_reviews(job->book.id, job->review_list_type, 20,
                                  &result->reviews, &error)) {
            send_final(job.get(), error_result(error), external_stack, &client);
        }
        send_final(job.get(), std::move(result), external_stack, &client);
    }
    if (job->kind == WorkKind::LoadChapter) {
        if (job->chapter.uid.empty()) {
            send_final(job.get(), error_result("Invalid chapter index"), external_stack, &client);
        } else {
            const auto &chapter = job->chapter;
            struct stat cached_epub_info = {};
            const bool cached_epub = stat(weread::store::epub_payload_path(job->book.id).c_str(),
                                          &cached_epub_info) == 0 && cached_epub_info.st_size > 0;
            if (cached_epub || weread::store::chapter_cached(job->book.id, chapter)) {
                auto ready = std::make_unique<WorkResult>();
                ready->kind = ResultKind::OpenReader;
                ready->reader_path = weread::store::manifest_path(job->book.id);
                ready->reader_spine = job->resume_saved_position ? -1 : job->chapter_index;
                send_final(job.get(), std::move(ready), external_stack, &client);
            }
            weread::ChapterPayload payload;
            std::string error;
            if (!api.get_chapter(job->book.id, chapter, &payload, &error)) {
                send_final(job.get(), error_result(error), external_stack, &client);
            } else {
                if (!job->book.cover_url.empty()) {
                    const std::string cover = weread::store::cover_path(job->book.cover_url);
                    struct stat cover_info = {};
                    if (stat(cover.c_str(), &cover_info) != 0 || cover_info.st_size <= 0) {
                        weread::HttpResponse image = client.get(job->book.cover_url);
                        if (image.ok()) weread::store::save_response_file(cover, image.data(), image.size());
                    }
                    cover_thumbnail::ensure(cover, cover_thumbnail::cache_path_for(cover));
                }
                bool saved = false;
                if (payload.kind == weread::ChapterPayloadKind::EpubZip) {
                    saved = weread::store::save_response_file(
                        weread::store::epub_payload_path(job->book.id),
                        payload.content.data(), payload.content.size());
                } else {
                    saved = weread::store::save_chapter(job->book.id, chapter, payload);
                }
                if (!saved) {
                    send_final(job.get(), error_result("Failed to save chapter cache"), external_stack, &client);
                } else {
                    auto ready = std::make_unique<WorkResult>();
                    ready->kind = ResultKind::OpenReader;
                    ready->reader_path = weread::store::manifest_path(job->book.id);
                    ready->reader_spine = job->resume_saved_position ? -1 : job->chapter_index;
                    send_final(job.get(), std::move(ready), external_stack, &client);
                }
            }
        }
    }
    if (job->kind == WorkKind::LoadCatalog) {
        auto result = std::make_unique<WorkResult>();
        result->kind = ResultKind::Catalog;
        std::string error;
        if (!api.get_chapters(job->book.id, &result->chapters, &result->format, &error)) {
            send_final(job.get(), error_result(error), external_stack, &client);
        } else {
            if (!job->book.cover_url.empty()) {
                const std::string cover = weread::store::cover_path(job->book.cover_url);
                struct stat info = {};
                if (stat(cover.c_str(), &info) != 0 || info.st_size <= 0) {
                    weread::HttpResponse image = client.get(job->book.cover_url);
                    if (image.ok()) weread::store::save_response_file(cover, image.data(), image.size());
                }
                cover_thumbnail::ensure(cover, cover_thumbnail::cache_path_for(cover));
            }
            weread::store::save_chapters(job->book.id, result->chapters, result->format);
            weread::store::save_manifest(job->book, result->chapters, result->format);
            send_final(job.get(), std::move(result), external_stack, &client);
        }
    }

    std::string shelf_error;
    auto fallback_offline_shelf = [&]() {
        if (job->kind != WorkKind::LoadShelf || *job->cancelled) return;
        auto cached = std::make_unique<WorkResult>();
        cached->kind = ResultKind::Shelf;
        if (!weread::store::load_shelf(&cached->books)) return;
        cached->cached = true;
        cached->offline = true;
        ESP_LOGI(TAG, "online shelf unavailable; using offline cache");
        send_final(job.get(), std::move(cached), external_stack, &client);
    };
    auto load_shelf = [&]() -> std::unique_ptr<WorkResult> {
        auto result = std::make_unique<WorkResult>();
        result->kind = ResultKind::Shelf;
        shelf_error.clear();
        if (!api.get_shelf(&result->books, &shelf_error)) return nullptr;
        weread::store::save_shelf(result->books);
        return result;
    };

    if (client.session().valid()) {
        auto shelf = load_shelf();
        if (shelf) send_final(job.get(), std::move(shelf), external_stack, &client);
    }
    if (client.session().valid() && shelf_error != "login expired") {
        fallback_offline_shelf();
        send_final(job.get(), error_result(shelf_error.empty() ? "Shelf loading failed" : shelf_error),
                   external_stack, &client);
    }

    if (*job->cancelled) {
        auto cancelled = std::make_unique<WorkResult>();
        cancelled->kind = ResultKind::Cancelled;
        send_final(job.get(), std::move(cancelled), external_stack, &client);
    }
    std::string uid, login_url, error;
    if (!api.create_login(&uid, &login_url, &error)) {
        fallback_offline_shelf();
        send_final(job.get(), error_result(error), external_stack, &client);
    }
    auto qr = std::make_unique<WorkResult>();
    qr->kind = ResultKind::LoginQr;
    qr->login_url = login_url;
    if (!send_result(job.get(), std::move(qr))) {
        send_final(job.get(), error_result("Login view closed"), external_stack, &client);
    }

    for (int attempt = 0; attempt < 120; ++attempt) {
        for (int tick = 0; tick < 20; ++tick) {
            if (*job->cancelled) {
                auto result = std::make_unique<WorkResult>();
                result->kind = ResultKind::Cancelled;
                send_final(job.get(), std::move(result), external_stack, &client);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        const weread::LoginPoll poll = api.poll_login(uid);
        if (poll.state == weread::LoginState::Pending) continue;
        if (poll.state == weread::LoginState::Success) {
            sync_pending_progress();
            auto shelf = load_shelf();
            if (shelf) send_final(job.get(), std::move(shelf), external_stack, &client);
            fallback_offline_shelf();
            send_final(job.get(), error_result("Login succeeded, but shelf loading failed"),
                       external_stack, &client);
        } else {
            send_final(job.get(), error_result(poll.message.empty() ? "Login failed" : poll.message),
                       external_stack, &client);
        }
    }
    send_final(job.get(), error_result("QR code expired"), external_stack, &client);
}

std::string localized_weread_error(const std::string &message)
{
    using ui_locale::TextId;
    struct ErrorText { const char *source; TextId id; };
    static constexpr ErrorText known[] = {
        {"Protocol self-test failed", TextId::ProtocolSelfTestFailed},
        {"WiFi connection failed and no offline cache is available", TextId::NoNetworkOrOfflineCache},
        {"Invalid chapter index", TextId::InvalidChapterIndex},
        {"Failed to save chapter cache", TextId::ChapterCacheSaveFailed},
        {"Shelf loading failed", TextId::ShelfLoadingFailed},
        {"Login view closed", TextId::LoginViewClosed},
        {"Login succeeded, but shelf loading failed", TextId::LoginShelfFailed},
        {"Login failed", TextId::LoginFailed},
        {"Chapter is not cached; connect online to download", TextId::WeReadChapterNotCached},
        {"Unavailable offline", TextId::WeReadOfflineUnavailable},
        {"QR code expired", TextId::QrExpired},
    };
    for (const auto &entry : known) {
        if (message == entry.source) return ui_locale::text(entry.id);
    }
    return ui_locale::localize_message(message);
}

void draw_header(const std::string &title)
{
    const int width = M5.Display.width();
    M5.Display.fillRect(0, 0, width, HEADER_H, TFT_WHITE);
    system_font::draw_left(M5.Display, "<", 18, header_layout::text_y(), TFT_BLACK);
    system_font::draw_center(M5.Display, system_font::ellipsize(title, width - 150),
                             width / 2, header_layout::text_y(), TFT_BLACK);
    header_layout::draw_separator(M5.Display);
}

size_t utf8_char_bytes(const std::string &text, size_t position)
{
    const unsigned char c = static_cast<unsigned char>(text[position]);
    if ((c & 0xE0) == 0xC0) return std::min<size_t>(2, text.size() - position);
    if ((c & 0xF0) == 0xE0) return std::min<size_t>(3, text.size() - position);
    if ((c & 0xF8) == 0xF0) return std::min<size_t>(4, text.size() - position);
    return 1;
}

std::vector<std::string> wrap_text(const std::string &text, int max_width)
{
    std::vector<std::string> lines;
    std::string line;
    for (size_t position = 0; position < text.size();) {
        if (text[position] == '\r') { ++position; continue; }
        if (text[position] == '\n') {
            lines.push_back(line);
            line.clear();
            ++position;
            continue;
        }
        const size_t bytes = utf8_char_bytes(text, position);
        const std::string glyph = text.substr(position, bytes);
        if (!line.empty() && system_font::measure(line + glyph) > max_width) {
            lines.push_back(line);
            line.clear();
        }
        line += glyph;
        position += bytes;
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}

class WereadActivity final : public Activity {
public:
    ~WereadActivity() override
    {
        weread::set_offline_mode(false);
        _cancelled = true;
        if (_queue) {
            WorkResult *result = nullptr;
            while (xQueueReceive(_queue, &result, 0) == pdTRUE) delete result;
            vQueueDelete(_queue);
        }
        // Keep Wi-Fi and the ESP-Hosted C6 alive while navigating between the
        // shelf, catalog and reader. Power them down only when the WeRead
        // activity itself is actually left.
        wifi_setting_release();
    }

    void on_create() override
    {
        _queue = xQueueCreate(3, sizeof(WorkResult *));
        weread::set_offline_mode(false);
        // Keep cached metadata ready for the QR offline button, but always
        // attempt online sync first. Cache reads never validate the Cookie.
        weread::store::load_shelf(&_books);
        start_work(WorkKind::LoadShelf, {});
    }

    void loop() override
    {
        if (_view == View::OfflineNotice) {
            if (esp_timer_get_time() >= _offline_notice_until_us) {
                _busy = false;
                _view = View::Shelf;
                draw_shelf();
            }
            return;
        }
        if (!_queue) return;
        WorkResult *raw = nullptr;
        if (xQueueReceive(_queue, &raw, 0) != pdTRUE) return;
        std::unique_ptr<WorkResult> result(raw);
        if (_enter_offline_when_done && result->kind != ResultKind::LoginQr) {
            _task = nullptr;
            _busy = false;
            _cover_prefetch_active = false;
            _enter_offline_when_done = false;
            enter_offline();
            return;
        }
        if (result->kind == ResultKind::LoginQr) {
            if (_enter_offline_when_done) return;
            _view = View::Login;
            _login_url = std::move(result->login_url);
            draw_login();
            return;
        }
        if (result->kind == ResultKind::CoversReady) {
            _task = nullptr;
            _cover_prefetch_active = false;
            _cancelled = false;
            if (result->cover_network_failed) _cover_prefetch_disabled = true;
            if (_exit_when_done) {
                ActivityManager::instance().pop();
                return;
            }
            if (_pending_work.valid) {
                PendingWork pending = std::move(_pending_work);
                _pending_work = PendingWork();
                _busy = false;
                start_work(pending.kind, pending.book, pending.chapter_index,
                           pending.resume_saved_position, false, pending.review_list_type);
                return;
            }
            if (result->covers_changed && _view == View::Shelf && result->cover_page == _page) {
                draw_shelf();
            } else {
                schedule_cover_prefetch();
            }
            return;
        }
        _busy = false;
        _task = nullptr;
        if (_exit_when_done || result->kind == ResultKind::Cancelled) {
            ActivityManager::instance().pop();
            return;
        }
        if (result->kind == ResultKind::OpenReader) {
            // The vectors are released while the reader is active to leave as
            // much RAM as possible to the reflow/render pipeline. Remember
            // that the next on_show() is the return from that reader so the
            // catalog is restored and explicitly redrawn.
            _reader_open = true;
            _view = _return_to_shelf_after_reader ? View::Shelf : View::Catalog;
            _return_to_shelf_after_reader = false;
            _chapters.clear();
            _chapters.shrink_to_fit();
            _reviews.clear();
            _reviews.shrink_to_fit();
            _books.clear();
            _books.shrink_to_fit();
            launch_weread_reader(result->reader_path.c_str(), result->reader_spine,
                                 _reader_opening.get());
            _reader_opening.reset();
            return;
        }
        if (result->kind == ResultKind::Shelf) {
            _books = std::move(result->books);
            _page = 0;
            _shelf_page = 0;
            _view = View::Shelf;
            _cached = result->cached;
            _offline = result->offline;
            if (_offline) weread::set_offline_mode(true);
            _cover_pages_attempted.clear();
            _cover_prefetch_disabled = false;
            restore_selected_book_shelf_page();
            if (_offline) {
                _view = View::OfflineNotice;
                _busy = true;
                draw_loading(ui_locale::text(ui_locale::TextId::WeReadNetworkFailedOffline));
                // Keep the notice visible after scanout without blocking the
                // activity loop or losing the Back action.
                M5.Display.waitDisplay();
                _offline_notice_until_us = esp_timer_get_time() + 1500000;
            } else {
                draw_shelf();
            }
        } else if (result->kind == ResultKind::Catalog) {
            _chapters = std::move(result->chapters);
            _format = std::move(result->format);
            _cached = result->cached;
            _offline = result->offline;
            _page = 0;
            if (!_selected_book.progress_chapter_uid.empty()) {
                const auto found = std::find_if(_chapters.begin(), _chapters.end(), [&](const auto &chapter) {
                    return chapter.uid == _selected_book.progress_chapter_uid;
                });
                if (found != _chapters.end()) {
                    _page = static_cast<int>(std::distance(_chapters.begin(), found)) / LIST_ROWS;
                }
            }
            _view = View::Catalog;
            if (_resume_after_catalog && !_chapters.empty()) {
                _resume_after_catalog = false;
                int local_spine = 0;
                const std::string manifest = weread::store::manifest_path(_selected_book.id);
                const bool has_local = load_local_reader_spine(
                    manifest, static_cast<int>(_chapters.size()), &local_spine);
                int cloud_spine = -1;
                if (!_selected_book.progress_chapter_uid.empty()) {
                    const auto found = std::find_if(_chapters.begin(), _chapters.end(), [&](const auto &chapter) {
                        return chapter.uid == _selected_book.progress_chapter_uid;
                    });
                    if (found != _chapters.end()) {
                        cloud_spine = static_cast<int>(std::distance(_chapters.begin(), found));
                    }
                }
                if (cloud_spine < 0 && _selected_book.progress > 0) {
                    cloud_spine = std::clamp(
                        (_selected_book.progress * static_cast<int>(_chapters.size())) / 100,
                        0, static_cast<int>(_chapters.size()) - 1);
                }
                weread::store::PendingProgress pending;
                const bool local_is_pending = has_local &&
                    weread::store::load_pending_progress(&pending) &&
                    pending.book_id == _selected_book.id;
                const bool resume_saved = has_local &&
                    (weread::offline_mode() || local_is_pending || cloud_spine < 0 || cloud_spine == local_spine);
                const int resume_spine = resume_saved ? local_spine :
                    (cloud_spine >= 0 ? cloud_spine : 0);
                ESP_LOGI(TAG, "resume book: source=%s spine=%d/%u cloud=%d%%",
                         resume_saved ? "local" : (cloud_spine >= 0 ? "cloud" : "start"),
                         resume_spine + 1,
                         static_cast<unsigned>(_chapters.size()),
                         _selected_book.progress);
                _return_to_shelf_after_reader = true;
                start_work(WorkKind::LoadChapter, _selected_book, resume_spine, resume_saved);
                return;
            }
            _resume_after_catalog = false;
            draw_catalog();
        } else if (result->kind == ResultKind::Reviews) {
            _reviews = std::move(result->reviews);
            _review_list_type = result->review_list_type;
            _page = 0;
            _view = View::Reviews;
            draw_reviews();
        } else {
            _return_to_shelf_after_reader = false;
            _view = View::Error;
            _status = result->message.empty()
                ? ui_locale::text(ui_locale::TextId::UnknownError)
                : localized_weread_error(result->message);
            draw_error();
        }
    }

    void on_show() override
    {
        // on_show() is also called immediately after on_create(). Only restore
        // the parent screen when a child reader has actually been closed.
        if (!_reader_open) return;
        _reader_open = false;

        if (_view == View::Catalog && !_selected_book.id.empty()) {
            _chapters.clear();
            if (weread::store::load_chapters(_selected_book.id, &_chapters, &_format)) {
                // OpenReader releases both vectors for RAM. Restore the shelf
                // as well so a later Back from this catalog can return to the
                // selected book instead of showing an empty shelf.
                _books.clear();
                weread::store::load_shelf(&_books);
                _cached = true;
                _offline = weread::offline_mode() || !wifi_setting_is_connected();
                ESP_LOGI(TAG, "reader returned: catalog restored chapters=%u",
                         static_cast<unsigned>(_chapters.size()));
                draw_catalog();
                return;
            }

            _view = View::Error;
            _status = ui_locale::text(ui_locale::TextId::RestoreChapterListFailed);
            ESP_LOGE(TAG, "reader returned: failed to restore catalog book=%s",
                     _selected_book.id.c_str());
            draw_error();
            return;
        }

        if (_view == View::Shelf) {
            _books.clear();
            if (weread::store::load_shelf(&_books)) {
                weread::store::PendingProgress pending;
                if (weread::store::load_pending_progress(&pending)) {
                    const auto found = std::find_if(_books.begin(), _books.end(), [&](const auto &book) {
                        return book.id == pending.book_id;
                    });
                    if (found != _books.end()) {
                        found->progress = pending.percent;
                        found->progress_chapter_uid = pending.chapter.uid;
                        weread::store::save_shelf(_books);
                    }
                }
                _cached = true;
                _offline = weread::offline_mode() || !wifi_setting_is_connected();
                _cover_pages_attempted.clear();
                _cover_prefetch_disabled = false;
                restore_selected_book_shelf_page();
                draw_shelf();
                return;
            }
        }

        // Never leave the reader's last framebuffer visible if the parent
        // state is unexpectedly incomplete.
        _view = View::Error;
        _status = ui_locale::text(ui_locale::TextId::RestoreWereadPageFailed);
        draw_error();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Tap && event.y >= 920 && event.y < 1010 &&
            (_view == View::Login || _view == View::Error) &&
            !_books.empty()) {
            enter_offline();
            return true;
        }
        if (_book_menu_visible) return book_menu_touch(event);
        if (event.type == TouchEventType::Back ||
            (event.type == TouchEventType::Tap && header_layout::left_hit(event.x, event.y, 110))) {
            go_back();
            return true;
        }
        if (_busy) return true;
        if (_view == View::Error && event.type == TouchEventType::Tap) {
            weread::set_offline_mode(false);
            start_work(WorkKind::LoadShelf, {});
            return true;
        }
        if (_view == View::Shelf) return shelf_touch(event);
        if (_view == View::Catalog) return catalog_touch(event);
        if (_view == View::Reviews) return reviews_touch(event);
        if (_view == View::ReviewDetail) return review_detail_touch(event);
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) { go_back(); return true; }
        return false;
    }

    bool allow_system_back() const override { return !_busy; }

private:
    enum class View { Loading, Login, Shelf, Catalog, Reviews, ReviewDetail, Error, OfflineNotice };
    struct PendingWork {
        bool valid = false;
        WorkKind kind = WorkKind::LoadShelf;
        weread::Book book;
        int chapter_index = 0;
        bool resume_saved_position = false;
        int review_list_type = 3;
    };
    View _view = View::Loading;
    QueueHandle_t _queue = nullptr;
    TaskHandle_t _task = nullptr;
    volatile bool _cancelled = false;
    bool _busy = false;
    bool _exit_when_done = false;
    bool _offline = false;
    int64_t _offline_notice_until_us = 0;
    bool _enter_offline_when_done = false;
    bool _cached = false;
    bool _reader_open = false;
    bool _resume_after_catalog = false;
    bool _return_to_shelf_after_reader = false;
    bool _cover_prefetch_active = false;
    bool _cover_prefetch_disabled = false;
    bool _book_menu_visible = false;
    int _page = 0;
    int _shelf_page = 0;
    int _review_list_type = 3;
    int _review_index = -1;
    int _review_detail_page = 0;
    PendingWork _pending_work;
    std::vector<bool> _cover_pages_attempted;
    std::string _status;
    std::string _login_url;
    std::string _format;
    weread::Book _selected_book;
    std::vector<weread::Book> _books;
    std::vector<weread::Chapter> _chapters;
    std::vector<weread::Review> _reviews;
    std::unique_ptr<ReaderOpeningWhite> _reader_opening;

    void draw_offline_button()
    {
        if (_books.empty()) return;
        M5.Display.drawRoundRect(80, 920, M5.Display.width() - 160, 90, 10, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::WeReadOffline),
                                 M5.Display.width() / 2, 944, TFT_BLACK);
    }

    void enter_offline()
    {
        if (_task) {
            // Wait for the worker to release its queue/client before changing
            // policy or opening a reader. Never forcibly delete a live worker.
            _enter_offline_when_done = true;
            _cancelled = true;
            _pending_work = PendingWork();
            _view = View::Loading;
            draw_loading(ui_locale::text(ui_locale::TextId::Cancelling));
            return;
        }
        if (!weread::store::load_shelf(&_books)) {
            _view = View::Error;
            _status = ui_locale::text(ui_locale::TextId::WeReadNoOfflineShelf);
            draw_error();
            return;
        }
        weread::set_offline_mode(true);
        _cancelled = false;
        _busy = false;
        _exit_when_done = false;
        _offline = _cached = true;
        _resume_after_catalog = false;
        _return_to_shelf_after_reader = false;
        _book_menu_visible = false;
        _view = View::Shelf;
        restore_selected_book_shelf_page();
        draw_shelf();
    }

    void begin_frame(epd_mode_t mode = epd_mode_t::epd_text)
    {
        // Any error/cancel/catalog screen must finish the outstanding white
        // scan before it changes panel mode or writes the shared framebuffer.
        if (_reader_opening) {
            _reader_opening->finish();
            _reader_opening.reset();
        }
        M5.Display.startWrite();
        M5.Display.setEpdMode(mode);
        M5.Display.fillScreen(TFT_WHITE);
    }

    void end_frame()
    {
        M5.Display.display();
        M5.Display.endWrite();
    }

    void draw_loading(const char *message)
    {
        begin_frame(epd_mode_t::epd_quality);
        draw_header(ui_locale::text(ui_locale::TextId::WeRead));
        const auto lines = wrap_text(message, M5.Display.width() - 80);
        const int line_height = system_font::line_height() + 10;
        for (size_t i = 0; i < lines.size(); ++i) {
            system_font::draw_center(M5.Display, lines[i], M5.Display.width() / 2,
                                     500 + static_cast<int>(i) * line_height, TFT_BLACK);
        }
        end_frame();
    }

    void draw_login()
    {
        begin_frame(epd_mode_t::epd_quality);
        draw_header(ui_locale::text(ui_locale::TextId::WeReadLogin));
        const int size = std::min(520, static_cast<int>(M5.Display.width()) - 80);
        const int x = (M5.Display.width() - size) / 2;
        if (!weread::draw_qr(M5.Display, _login_url, x, 180, size)) {
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::QrGenerationFailed), M5.Display.width() / 2, 470);
        }
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ScanWithWeChat),
                                 M5.Display.width() / 2, 760, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::WaitingForConfirmation),
                                 M5.Display.width() / 2, 820, TFT_BLACK);
        draw_offline_button();
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapBackToCancel), M5.Display.width() / 2,
                                 1060, TFT_DARKGRAY);
        end_frame();
    }

    void draw_shelf()
    {
        begin_frame();
        draw_header(ui_locale::text(ui_locale::TextId::WeReadShelf));
        const int total_pages = std::max(1, static_cast<int>((_books.size() + LIST_ROWS - 1) / LIST_ROWS));
        _page = std::clamp(_page, 0, total_pages - 1);
        _shelf_page = _page;
        for (int row = 0; row < LIST_ROWS; ++row) {
            const int index = _page * LIST_ROWS + row;
            const int y = LIST_TOP + row * LIST_ROW_H;
            if (index < static_cast<int>(_books.size())) {
                const auto &book = _books[index];
                const std::string cover = weread::store::cover_path(book.cover_url);
                const std::string thumbnail = cover_thumbnail::cache_path_for(cover);
                struct stat image_info = {};
                if ((stat(thumbnail.c_str(), &image_info) != 0 || image_info.st_size <= 0) &&
                    stat(cover.c_str(), &image_info) == 0 && image_info.st_size > 0) {
                    cover_thumbnail::ensure(cover, thumbnail);
                }
                if (stat(thumbnail.c_str(), &image_info) == 0 && image_info.st_size > 0) {
                    M5.Display.drawJpgFile(thumbnail.c_str(), 24, y + 18, 104, 146,
                                           0, 0, 104.0f / 240.0f, 146.0f / 336.0f);
                } else {
                    M5.Display.drawRect(24, y + 18, 104, 146, TFT_DARKGRAY);
                    system_font::draw_center(M5.Display, "WR", 76, y + 67, TFT_DARKGRAY);
                }
                system_font::draw_left(M5.Display,
                    system_font::ellipsize(book.title, M5.Display.width() - 180), 154, y + 32, TFT_BLACK);
                std::string secondary = book.author;
                if (book.progress > 0) secondary += "  " + std::to_string(book.progress) + "%";
                system_font::draw_left(M5.Display,
                    system_font::ellipsize(secondary, M5.Display.width() - 180), 154, y + 100, TFT_DARKGRAY);
            }
            M5.Display.drawFastHLine(20, y + LIST_ROW_H - 1, M5.Display.width() - 40, TFT_BLACK);
        }
        system_font::draw_center(M5.Display, std::to_string(_page + 1) + "/" + std::to_string(total_pages),
                                 M5.Display.width() / 2, FOOTER_Y, TFT_BLACK);
        if (_offline) {
            system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::Offline), M5.Display.width() - 24,
                                    FOOTER_Y, TFT_DARKGRAY);
        } else if (_cached) {
            system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::Cached), M5.Display.width() - 24,
                                    FOOTER_Y, TFT_DARKGRAY);
        }
        if (_book_menu_visible) draw_book_menu_overlay();
        end_frame();
        if (!_book_menu_visible) schedule_cover_prefetch();
    }

    void draw_book_menu_overlay()
    {
        const int width = M5.Display.width();
        const int box_x = 32;
        const int box_w = width - 64;
        const int box_y = 420;
        const int box_h = 300;
        M5.Display.fillRoundRect(box_x, box_y, box_w, box_h, 18, TFT_WHITE);
        M5.Display.drawRoundRect(box_x, box_y, box_w, box_h, 18, TFT_BLACK);
        system_font::draw_center(M5.Display,
            system_font::ellipsize(_selected_book.title, box_w - 50), width / 2, box_y + 24, TFT_BLACK);
        M5.Display.drawFastHLine(box_x + 20, box_y + 92, box_w - 40, TFT_BLACK);
        M5.Display.drawRect(box_x + 24, box_y + 126, (box_w - 64) / 2, 104, TFT_BLACK);
        M5.Display.drawRect(box_x + 40 + (box_w - 64) / 2, box_y + 126,
                           (box_w - 64) / 2, 104, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ChapterCatalog),
                                 box_x + 24 + (box_w - 64) / 4, box_y + 154, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::BookReviews),
                                 box_x + 40 + 3 * (box_w - 64) / 4, box_y + 154, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapBlankToClose), width / 2, box_y + 250, TFT_DARKGRAY);
    }

    void draw_reviews()
    {
        begin_frame();
        draw_header(_selected_book.title);
        const int width = M5.Display.width();
        const int half = width / 2;
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Recommended), half / 2, REVIEW_TABS_TOP + 15, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Latest), half + half / 2, REVIEW_TABS_TOP + 15, TFT_BLACK);
        M5.Display.drawFastHLine(0, REVIEW_TABS_TOP + REVIEW_TABS_H - 1, width, TFT_LIGHTGRAY);
        const int active_x = _review_list_type == 3 ? 20 : half + 20;
        M5.Display.fillRect(active_x, REVIEW_TABS_TOP + REVIEW_TABS_H - 5, half - 40, 5, TFT_BLACK);

        const int total_pages = std::max(1, static_cast<int>((_reviews.size() + REVIEW_ROWS - 1) / REVIEW_ROWS));
        _page = std::clamp(_page, 0, total_pages - 1);
        for (int row = 0; row < REVIEW_ROWS; ++row) {
            const int index = _page * REVIEW_ROWS + row;
            const int y = REVIEW_LIST_TOP + row * REVIEW_ROW_H;
            if (index < static_cast<int>(_reviews.size())) {
                const auto &review = _reviews[static_cast<size_t>(index)];
                system_font::draw_left(M5.Display,
                    system_font::ellipsize(review.author, width - 56), 28, y + 14, TFT_BLACK);
                const auto lines = wrap_text(review.content, width - 56);
                const int line_height = system_font::line_height() + 4;
                const int max_lines = std::min(3, static_cast<int>(lines.size()));
                for (int line = 0; line < max_lines; ++line) {
                    std::string text = lines[static_cast<size_t>(line)];
                    if (line == max_lines - 1 && static_cast<int>(lines.size()) > max_lines) text += "...";
                    system_font::draw_left(M5.Display,
                        system_font::ellipsize(text, width - 56), 28,
                        y + 65 + line * line_height, TFT_DARKGRAY);
                }
            }
            M5.Display.drawFastHLine(20, y + REVIEW_ROW_H - 1, width - 40, TFT_BLACK);
        }
        if (_reviews.empty()) {
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::NoBookReviews), width / 2, 520, TFT_DARKGRAY);
        }
        system_font::draw_center(M5.Display, std::to_string(_page + 1) + "/" + std::to_string(total_pages),
                                 width / 2, FOOTER_Y, TFT_BLACK);
        end_frame();
    }

    void draw_review_detail()
    {
        if (_review_index < 0 || _review_index >= static_cast<int>(_reviews.size())) return;
        begin_frame();
        draw_header(ui_locale::text(ui_locale::TextId::BookReviews));
        const auto &review = _reviews[static_cast<size_t>(_review_index)];
        const int width = M5.Display.width();
        system_font::draw_left(M5.Display,
            system_font::ellipsize(review.author, width - 56), 28, 104, TFT_BLACK);
        M5.Display.drawFastHLine(20, 165, width - 40, TFT_BLACK);
        const auto lines = wrap_text(review.content, width - 56);
        const int line_height = system_font::line_height() + 8;
        const int content_top = 192;
        const int lines_per_page = std::max(1, (LIST_BOTTOM - content_top) / line_height);
        const int total_pages = std::max(1, static_cast<int>((lines.size() + lines_per_page - 1) / lines_per_page));
        _review_detail_page = std::clamp(_review_detail_page, 0, total_pages - 1);
        const int begin = _review_detail_page * lines_per_page;
        const int end = std::min(static_cast<int>(lines.size()), begin + lines_per_page);
        for (int line = begin; line < end; ++line) {
            system_font::draw_left(M5.Display, lines[static_cast<size_t>(line)], 28,
                                   content_top + (line - begin) * line_height, TFT_BLACK);
        }
        system_font::draw_center(M5.Display,
            std::to_string(_review_detail_page + 1) + "/" + std::to_string(total_pages),
            width / 2, FOOTER_Y, TFT_BLACK);
        end_frame();
    }

    void draw_catalog()
    {
        begin_frame();
        draw_header(_selected_book.title);
        const int total_pages = std::max(1, static_cast<int>((_chapters.size() + LIST_ROWS - 1) / LIST_ROWS));
        _page = std::clamp(_page, 0, total_pages - 1);
        for (int row = 0; row < LIST_ROWS; ++row) {
            const int index = _page * LIST_ROWS + row;
            const int y = LIST_TOP + row * LIST_ROW_H;
            if (index < static_cast<int>(_chapters.size())) {
                const auto &chapter = _chapters[index];
                std::string prefix = std::to_string(index + 1) + ". ";
                system_font::draw_left(M5.Display,
                    system_font::ellipsize(prefix + chapter.title, M5.Display.width() - 64),
                    28, y + 55, TFT_BLACK);
            }
            M5.Display.drawFastHLine(20, y + LIST_ROW_H - 1, M5.Display.width() - 40, TFT_BLACK);
        }
        system_font::draw_center(M5.Display, std::to_string(_page + 1) + "/" + std::to_string(total_pages),
                                 M5.Display.width() / 2, FOOTER_Y, TFT_BLACK);
        if (_offline) {
            system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::Offline), M5.Display.width() - 24,
                                    FOOTER_Y, TFT_DARKGRAY);
        } else if (_cached) {
            system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::Cached), M5.Display.width() - 24,
                                    FOOTER_Y, TFT_DARKGRAY);
        }
        end_frame();
    }

    void draw_error()
    {
        begin_frame();
        draw_header(ui_locale::text(ui_locale::TextId::WeReadError));
        const auto lines = wrap_text(_status, M5.Display.width() - 80);
        const int line_height = system_font::line_height() + 10;
        const int first_y = 430 - static_cast<int>(lines.size() - 1) * line_height / 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            system_font::draw_center(M5.Display, lines[i], M5.Display.width() / 2,
                                     first_y + static_cast<int>(i) * line_height, TFT_BLACK);
        }
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapToRetry), M5.Display.width() / 2,
                                 first_y + static_cast<int>(lines.size()) * line_height + 45, TFT_BLACK);
        draw_offline_button();
        end_frame();
    }

    bool shelf_touch(const TouchEvent &event)
    {
        if (event.type == TouchEventType::SwipeUp) { ++_page; draw_shelf(); return true; }
        if (event.type == TouchEventType::SwipeDown) { --_page; draw_shelf(); return true; }
        if ((event.type != TouchEventType::Tap && event.type != TouchEventType::LongPress) ||
            event.y < LIST_TOP || event.y >= LIST_BOTTOM) return false;
        const int row = (event.y - LIST_TOP) / LIST_ROW_H;
        const int index = _page * LIST_ROWS + row;
        if (index >= static_cast<int>(_books.size())) return true;
        _shelf_page = _page;
        _selected_book = _books[index];
        if (event.type == TouchEventType::LongPress) {
            _book_menu_visible = true;
            draw_shelf();
            return true;
        }
        _resume_after_catalog = true;
        start_work(WorkKind::LoadCatalog, _selected_book);
        return true;
    }

    bool book_menu_touch(const TouchEvent &event)
    {
        if (event.type == TouchEventType::Back) {
            _book_menu_visible = false;
            draw_shelf();
            return true;
        }
        if (event.type != TouchEventType::Tap) return true;
        const int box_x = 32;
        const int box_w = M5.Display.width() - 64;
        const int button_y = 546;
        const int button_h = 104;
        if (event.y >= button_y && event.y < button_y + button_h) {
            const int split = M5.Display.width() / 2;
            _book_menu_visible = false;
            if (event.x < split && event.x >= box_x + 24) {
                _resume_after_catalog = false;
                start_work(WorkKind::LoadCatalog, _selected_book);
                return true;
            }
            if (event.x >= split && event.x < box_x + box_w - 24) {
                start_work(WorkKind::LoadReviews, _selected_book, 0, false, true, 3);
                return true;
            }
        }
        _book_menu_visible = false;
        draw_shelf();
        return true;
    }

    bool catalog_touch(const TouchEvent &event)
    {
        if (event.type == TouchEventType::SwipeLeft) { ++_page; draw_catalog(); return true; }
        if (event.type == TouchEventType::SwipeRight) { --_page; draw_catalog(); return true; }
        if (event.type == TouchEventType::Tap && event.y >= LIST_TOP && event.y < LIST_BOTTOM) {
            const int row = (event.y - LIST_TOP) / LIST_ROW_H;
            const int index = _page * LIST_ROWS + row;
            if (index >= 0 && index < static_cast<int>(_chapters.size())) {
                weread::store::save_manifest(_selected_book, _chapters, _format);
                start_work(WorkKind::LoadChapter, _selected_book, index);
            }
            return true;
        }
        return false;
    }

    bool reviews_touch(const TouchEvent &event)
    {
        if (event.type == TouchEventType::SwipeUp) { ++_page; draw_reviews(); return true; }
        if (event.type == TouchEventType::SwipeDown) { --_page; draw_reviews(); return true; }
        if (event.type != TouchEventType::Tap) return false;
        if (event.y >= REVIEW_TABS_TOP && event.y < REVIEW_TABS_TOP + REVIEW_TABS_H) {
            const int type = event.x < M5.Display.width() / 2 ? 3 : 2;
            if (type != _review_list_type) {
                start_work(WorkKind::LoadReviews, _selected_book, 0, false, true, type);
            }
            return true;
        }
        if (event.y >= REVIEW_LIST_TOP && event.y < LIST_BOTTOM) {
            const int row = (event.y - REVIEW_LIST_TOP) / REVIEW_ROW_H;
            const int index = _page * REVIEW_ROWS + row;
            if (index >= 0 && index < static_cast<int>(_reviews.size())) {
                _review_index = index;
                _review_detail_page = 0;
                _view = View::ReviewDetail;
                draw_review_detail();
            }
            return true;
        }
        return false;
    }

    bool review_detail_touch(const TouchEvent &event)
    {
        if (event.type == TouchEventType::SwipeUp) {
            ++_review_detail_page;
            draw_review_detail();
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            --_review_detail_page;
            draw_review_detail();
            return true;
        }
        return false;
    }

    static const char *work_loading_message(WorkKind kind)
    {
        using ui_locale::TextId;
        using ui_locale::text;
        switch (kind) {
        case WorkKind::LoadShelf: return text(TextId::LoadingShelf);
        case WorkKind::LoadCatalog: return text(TextId::LoadingChapters);
        case WorkKind::LoadChapter: return text(TextId::DownloadingChapter);
        case WorkKind::LoadReviews: return text(TextId::LoadingBookReviews);
        case WorkKind::LoadCovers: return text(TextId::LoadingCovers);
        }
        return text(TextId::Loading);
    }

    void start_work(WorkKind kind, const weread::Book &book, int chapter_index = 0,
                    bool resume_saved_position = false, bool show_loading = true,
                    int review_list_type = 3)
    {
        if (!_queue) return;
        bool cached_reader_open = false;
        if (kind == WorkKind::LoadCatalog && _resume_after_catalog) {
            std::vector<weread::Chapter> cached_chapters;
            std::string cached_format;
            cached_reader_open = weread::store::load_chapters(
                book.id, &cached_chapters, &cached_format) && !cached_chapters.empty();
        } else if (kind == WorkKind::LoadChapter && chapter_index >= 0 &&
                   chapter_index < static_cast<int>(_chapters.size())) {
            struct stat epub_info = {};
            cached_reader_open =
                (stat(weread::store::epub_payload_path(book.id).c_str(), &epub_info) == 0 &&
                 epub_info.st_size > 0) ||
                weread::store::chapter_cached(book.id, _chapters[static_cast<size_t>(chapter_index)]);
        }
        auto show_work_feedback = [&]() {
            if (cached_reader_open) {
                // One white acknowledgement spans cached catalog lookup,
                // resume selection and reader construction. No status redraw.
                if (!_reader_opening)
                    _reader_opening = std::make_unique<ReaderOpeningWhite>();
            } else if (show_loading) {
                draw_loading(work_loading_message(kind));
                M5.Display.waitDisplay();
            }
        };
        if (_task) {
            if (_cover_prefetch_active) {
                _cancelled = true;
                _pending_work.valid = true;
                _pending_work.kind = kind;
                _pending_work.book = book;
                _pending_work.chapter_index = chapter_index;
                _pending_work.resume_saved_position = resume_saved_position;
                _pending_work.review_list_type = review_list_type;
                _busy = true;
                _view = View::Loading;
                show_work_feedback();
            }
            return;
        }
        if (_busy) return;
        _busy = true;
        _cancelled = false;
        _exit_when_done = false;
        _view = View::Loading;
        show_work_feedback();

        auto *job = new WorkJob;
        job->kind = kind;
        job->book = book;
        job->chapter_index = chapter_index;
        job->resume_saved_position = resume_saved_position;
        job->review_list_type = review_list_type;
        if (kind == WorkKind::LoadChapter && chapter_index >= 0 &&
            chapter_index < static_cast<int>(_chapters.size())) {
            job->chapter = _chapters[static_cast<size_t>(chapter_index)];
        }
        job->result_queue = _queue;
        job->cancelled = &_cancelled;
        // Do not use an external-RAM task stack here. wifi_setting and the
        // WeRead session store access NVS; NVS flash reads temporarily disable
        // cache, which makes a PSRAM stack unusable and trips cache_utils' stack
        // sanity assertion. Large HTTP/content buffers are still allocated in
        // PSRAM by their owning components.
        job->external_stack = false;
        BaseType_t created = xTaskCreatePinnedToCore(
            weread_task, "weread_net", WEREAD_TASK_STACK_BYTES, job, 4, &_task,
            0);
        if (created != pdPASS) {
            delete job;
            _busy = false;
            _view = View::Error;
            _status = ui_locale::text(ui_locale::TextId::NetworkTaskStartFailed);
            draw_error();
        }
    }

    void schedule_cover_prefetch()
    {
        if (weread::offline_mode() || !_queue || _task || _busy || _cover_prefetch_disabled ||
            _view != View::Shelf || _books.empty()) return;
        const int total_pages = std::max(1, static_cast<int>((_books.size() + LIST_ROWS - 1) / LIST_ROWS));
        if (static_cast<int>(_cover_pages_attempted.size()) != total_pages) {
            _cover_pages_attempted.assign(static_cast<size_t>(total_pages), false);
        }

        int target_page = -1;
        std::vector<weread::Book> missing;
        for (int offset = 0; offset < total_pages; ++offset) {
            const int page = (_page + offset) % total_pages;
            if (_cover_pages_attempted[static_cast<size_t>(page)]) continue;
            _cover_pages_attempted[static_cast<size_t>(page)] = true;
            missing.clear();
            const int begin = page * LIST_ROWS;
            const int end = std::min(static_cast<int>(_books.size()), begin + LIST_ROWS);
            for (int index = begin; index < end; ++index) {
                const auto &book = _books[static_cast<size_t>(index)];
                if (book.cover_url.empty()) continue;
                const std::string cover = weread::store::cover_path(book.cover_url);
                struct stat info = {};
                if (stat(cover.c_str(), &info) != 0 || info.st_size <= 0) missing.push_back(book);
            }
            if (!missing.empty()) {
                target_page = page;
                break;
            }
        }
        if (target_page < 0 || missing.empty()) return;

        auto *job = new WorkJob;
        job->kind = WorkKind::LoadCovers;
        job->cover_page = target_page;
        job->cover_books = std::move(missing);
        job->result_queue = _queue;
        job->cancelled = &_cancelled;
        job->external_stack = false;
        const unsigned missing_count = static_cast<unsigned>(job->cover_books.size());
        _cancelled = false;
        _cover_prefetch_active = true;
        BaseType_t created = xTaskCreatePinnedToCore(
            weread_task, "weread_cover", WEREAD_TASK_STACK_BYTES, job, 2, &_task,
            0);
        if (created != pdPASS) {
            delete job;
            _task = nullptr;
            _cover_prefetch_active = false;
            ESP_LOGW(TAG, "failed to start shelf cover prefetch task");
            return;
        }
        ESP_LOGI(TAG, "shelf cover prefetch page=%d/%d books=%u",
                 target_page + 1, total_pages, missing_count);
    }

    void go_back()
    {
        if (_view == View::OfflineNotice) {
            ActivityManager::instance().pop();
            return;
        }
        if (_enter_offline_when_done) {
            _enter_offline_when_done = false;
            _exit_when_done = true;
            return;
        }
        if (_view == View::Error && weread::offline_mode()) {
            enter_offline();
            return;
        }
        if (_book_menu_visible) {
            _book_menu_visible = false;
            draw_shelf();
            return;
        }
        if (_view == View::Login && _task) {
            cancel_login_wait();
            return;
        }
        if (_busy) {
            _cancelled = true;
            _exit_when_done = true;
            draw_loading(ui_locale::text(ui_locale::TextId::Cancelling));
            return;
        }
        if (_cover_prefetch_active) {
            _cancelled = true;
            _exit_when_done = true;
            _busy = true;
            return;
        }
        if (_view == View::Catalog) {
            _view = View::Shelf;
            _page = _shelf_page;
            restore_selected_book_shelf_page();
            draw_shelf();
            return;
        }
        if (_view == View::ReviewDetail) {
            _view = View::Reviews;
            draw_reviews();
            return;
        }
        if (_view == View::Reviews) {
            _reviews.clear();
            _reviews.shrink_to_fit();
            _view = View::Shelf;
            _page = _shelf_page;
            restore_selected_book_shelf_page();
            draw_shelf();
            return;
        }
        ActivityManager::instance().pop();
    }

    void cancel_login_wait()
    {
        // The worker owns its HttpClient and WorkJob. Do not pop this
        // activity immediately: it would leave the worker with dangling
        // queue/cancellation pointers. The login wait checks this flag every
        // 100 ms and sends Cancelled before the activity is popped in loop().
        _cancelled = true;
        _exit_when_done = true;
        _view = View::Loading;
        draw_loading(ui_locale::text(ui_locale::TextId::CancellingLogin));
        ESP_LOGI(TAG, "QR login cancellation requested");
    }

    void restore_selected_book_shelf_page()
    {
        _page = _shelf_page;
        if (_selected_book.id.empty() || _books.empty()) return;
        const auto found = std::find_if(_books.begin(), _books.end(), [&](const auto &book) {
            return book.id == _selected_book.id;
        });
        if (found != _books.end()) {
            _page = static_cast<int>(std::distance(_books.begin(), found)) / LIST_ROWS;
            _shelf_page = _page;
            ESP_LOGI(TAG, "shelf position restored book=%s page=%d",
                     _selected_book.id.c_str(), _page + 1);
        }
    }
};

}  // namespace

void launch_weread()
{
    std::vector<weread::Book> cached;
    if (!weread::store::load_shelf(&cached) && !ensure_wifi_configured_or_show_guide()) return;
    ActivityManager::instance().push(new WereadActivity());
}
