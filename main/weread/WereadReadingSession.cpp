#include "weread/WereadReadingSession.h"
#include "weread/WereadContentSource.h"
#include "weread/WereadHttpClient.h"
#include "weread/WereadOffline.h"
#include "weread/WereadReadingTime.h"
#include "wifi/wifi_setting.h"
#include <M5Unified.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <new>

namespace weread {
namespace {
constexpr const char *TAG="WEREAD_LIVE";
constexpr int64_t interval_us=60LL*1000000;
std::mutex transaction_mutex;
std::atomic<unsigned> active_workers{0};
bool connected() { return !offline_mode() && wifi_setting_is_connected(); }
}
struct ReadingSession::State {
    std::mutex mutex;
    store::PendingProgress position;
    uint64_t active_us=0, live_us=0;
    bool finish=false;
    std::atomic<bool> io_allowed{false};
    std::atomic<int64_t> ready_since{0};
};
std::mutex &reading_transaction_mutex() { return transaction_mutex; }
bool reading_sessions_idle() { return active_workers.load() == 0; }

namespace {
// Called only with the transaction lock. Unknown reports and offline seconds
// leave the active slot but remain durably recoverable in progress_*.bin.held.
bool hold_current(const std::string &account,const std::string &book) {
    store::PendingProgress old;
    if(!store::load_pending_progress(account,book,&old)) {
        std::vector<store::PendingProgress> check;
        return store::list_pending_progress(account,&check); // distinguish read failure
    }
    if(!store::hold_pending_progress(old)) return false;
    ESP_LOGI(TAG,"progress held: book=%s seconds=%u submitted=%u state=%u (not replayed)",
             book.c_str(),unsigned(old.active_seconds),unsigned(old.receipt.seconds),unsigned(old.receipt.state));
    return true;
}

void run_session(const std::shared_ptr<ReadingSession::State> &state) {
    std::unique_ptr<HttpClient> client;
    std::unique_ptr<Api> api;
    uint64_t saved_total=0, consumed_live=0;
    int64_t last_flush=esp_timer_get_time(), retry_after=0;
    unsigned attempts=0;
    bool was_connected=false;
    for(;;) {
        store::PendingProgress sample;
        uint64_t active_us,live_us; bool finish;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            sample=state->position; active_us=state->active_us;
            live_us=state->live_us; finish=state->finish;
        }
        const int64_t now=esp_timer_get_time();
        const bool online=connected();
        if(!online) {
            state->ready_since=0;
            api.reset(); client.reset();
            consumed_live=live_us/1000000;
        }
        if(online&&!was_connected) { attempts=0; retry_after=0; }
        was_connected=online;
        const bool idle=state->io_allowed.load()&&!M5.Display.displayBusy();
        // Exit must always persist, even if the display is refreshing. Network
        // finalization waits for EPD idle below; the UI never waits for us.
        if(!api&&online&&!finish&&idle&&now>=retry_after&&attempts<3) {
            std::unique_lock<std::mutex> operation(transaction_mutex,std::try_to_lock);
            if(operation.owns_lock()) {
                ++attempts;
                consumed_live=live_us/1000000;
                client=std::make_unique<HttpClient>();
                api=std::make_unique<Api>(client.get());
                uint64_t offset=0; const char *why="ok"; std::string error;
                const bool owner=client->session().valid()&&client->session().vid==sample.account_vid;
                const bool positioned=owner&&resolve_progress_position(sample.book_id,sample.chapter,sample.summary,&offset,&why);
                bool still_reading;
                { std::lock_guard<std::mutex> lock(state->mutex); still_reading=!state->finish; }
                if(!positioned||!still_reading||!api->begin_reading(sample.book_id,sample.chapter,sample.percent,offset,sample.summary,&error)) {
                    ESP_LOGW(TAG,"session deferred: book=%s attempt=%u position=%s error=%s",
                             sample.book_id.c_str(),attempts,why,error.c_str());
                    api.reset(); client.reset(); retry_after=now+30LL*1000000;
                } else {
                    // Only time genuinely read after this acknowledgement is
                    // eligible. Local/offline time before it is never backdated.
                    state->ready_since=esp_timer_get_time();
                    ESP_LOGI(TAG,"session ready: book=%s interval=60 s",sample.book_id.c_str());
                    client->close(); // keep ps/pc/token, release TLS heap
                }
            }
        }
        if((finish||now-last_flush>=interval_us)&&(finish||idle)) {
            std::unique_lock<std::mutex> operation(transaction_mutex,std::try_to_lock);
            if(!operation.owns_lock()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
            // A prior accepted-but-unconfirmed transaction must never occupy
            // this book's active slot forever, nor be sent again with new time.
            if(!hold_current(sample.account_vid,sample.book_id)) {
                ESP_LOGE(TAG,"cannot preserve old checkpoint; live flush deferred");
                vTaskDelay(pdMS_TO_TICKS(1000)); continue;
            }
            const auto slice=reading_time::due(active_us,live_us,saved_total,consumed_live,api&&online);
            const uint32_t live=slice.live;
            if(slice.total||finish) {
                sample.active_seconds=uint32_t(slice.total);
                sample.receipt={}; sample.position_verified=false;
                if(!store::save_pending_progress(sample)) {
                    ESP_LOGE(TAG,"checkpoint save failed; retaining worker counters");
                    vTaskDelay(pdMS_TO_TICKS(1000)); continue;
                }
                saved_total+=sample.active_seconds;
                consumed_live+=live;
                store::PendingProgress pending;
                if(!store::load_pending_progress(sample.account_vid,sample.book_id,&pending)) {
                    ESP_LOGE(TAG,"checkpoint reload failed; report not sent");
                } else if(api&&online&&(live||finish)) {
                    // On exit, io_allowed may be false because no reader loop
                    // remains. Waiting here does not block Activity destruction.
                    while(M5.Display.displayBusy()) vTaskDelay(pdMS_TO_TICKS(20));
                    Session owner;
                    const bool owner_matches=load_session(&owner)&&owner.vid==sample.account_vid;
                    const char *why="ok"; std::string error;
                    const bool positioned=owner_matches&&resolve_progress_position(pending.book_id,pending.chapter,pending.summary,&pending.chapter_offset,&why);
                    if(positioned) {
                        pending.receipt.revision=pending.revision;
                        const bool verified=api->upload_progress(pending.book_id,pending.chapter,pending.percent,live,
                            pending.chapter_offset,pending.summary,&error,&pending.receipt,
                            [&](const ReportReceipt &r){return store::checkpoint_progress(pending,r);});
                        if(verified&&store::acknowledge_progress(pending))
                            ESP_LOGI(TAG,"live server-confirmed: book=%s active=%u s",pending.book_id.c_str(),unsigned(live));
                        else ESP_LOGW(TAG,"live retained: book=%s active=%u state=%u reason=%s",
                                      pending.book_id.c_str(),unsigned(live),unsigned(pending.receipt.state),error.c_str());
                        // After any failed/ambiguous report, do not grow rt in
                        // the old context. Its submitted seconds are held.
                        if(!verified) { state->ready_since=0; api.reset(); retry_after=esp_timer_get_time()+30LL*1000000; }
                    } else {
                        ESP_LOGW(TAG,"live position unavailable: %s",why);
                    }
                    if(client) client->close();
                }
                if(!hold_current(sample.account_vid,sample.book_id))
                    ESP_LOGW(TAG,"checkpoint remains in active file; will not replay without live context");
            }
            last_flush=now;
            if(finish) break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    state->ready_since=0;
}
void session_task(void *argument) {
    {
        std::unique_ptr<std::shared_ptr<ReadingSession::State>> owner(
            static_cast<std::shared_ptr<ReadingSession::State> *>(argument));
        run_session(*owner);
    } // Destroy all C++ owners before vTaskDelete (no automatic unwinding).
    --active_workers;
    vTaskDelete(nullptr);
}
} // namespace

ReadingSession::~ReadingSession() {
    if(_state) { std::lock_guard<std::mutex> lock(_state->mutex); _state->finish=true; }
}
bool ReadingSession::start(const store::PendingProgress &initial) {
    if(_state||initial.account_vid.empty()||initial.book_id.empty()) return false;
    auto state=std::make_shared<State>(); state->position=initial;
    auto *owner=new(std::nothrow) std::shared_ptr<State>(state);
    if(!owner) return false;
    unsigned count=active_workers.load();
    do {
        if(count>=2) { delete owner; return false; }
    } while(!active_workers.compare_exchange_weak(count,count+1));
    // NVS/flash APIs require an internal task stack. Low priority/core0 leaves
    // the panel's producer and scanner ahead of background TLS work.
    if(xTaskCreatePinnedToCore(session_task,"weread_live",8192,owner,1,nullptr,0)!=pdPASS) { --active_workers; delete owner; return false; }
    _state=std::move(state); return true;
}
bool ReadingSession::publish(const store::PendingProgress &position,uint64_t active_us,uint64_t live_us,bool finish) {
    if(!_state) return false;
    std::lock_guard<std::mutex> lock(_state->mutex);
    if(_state->finish) return false;
    _state->position=position; _state->active_us=active_us; _state->live_us=live_us;
    _state->finish=finish; return true;
}
void ReadingSession::allow_io(bool allowed) { if(_state) _state->io_allowed=allowed; }
int64_t ReadingSession::ready_since_us() const {
    return _state&&connected()?_state->ready_since.load():0;
}
} // namespace weread
