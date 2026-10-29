#include "activities/WifiTransferActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/SettingActivity.h"
#include "activities/recent_books.h"
#include "audio/audio_file_formats.h"
#include "storage/app_paths.h"
#include "storage/storage_sdcard.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include "wifi/wifi_setting.h"
#include "wifi/S31WifiPolicy.h"
#include "wifi/TcpReceiveTest.h"
#include "wifi/TransferSessionGate.h"
#include "wifi/TransferDiagnostics.h"

#include <M5Unified.h>
#include <sdkconfig.h>
#include <dirent.h>
#include <errno.h>
#include <esp_dma_utils.h>
#include <esp_event.h>
#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_ip_addr.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#if CONFIG_IDF_TARGET_ESP32S31
#include <esp_private/esp_wifi_he_private.h>
#endif
#include <nvs_flash.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <atomic>
#include <mutex>

static const char *TAG = "WIFI_TRANSFER";
static const char *AP_PASSWORD = "12345678";
static const size_t UPLOAD_BUFFER_BYTES = 64 * 1024;
static const size_t FILE_WRITE_BUFFER_BYTES = 64 * 1024;
#if CONFIG_IDF_TARGET_ESP32S31
static constexpr bool REQUIRE_DIRECT_SD_DMA = false;
#else
static constexpr bool REQUIRE_DIRECT_SD_DMA = true;
#endif
static constexpr bool valid_sd_write_buffer(bool present, size_t capacity, bool dma) {
    return present && capacity >= FILE_WRITE_BUFFER_BYTES && (!REQUIRE_DIRECT_SD_DMA || dma);
}
static_assert(!valid_sd_write_buffer(false, FILE_WRITE_BUFFER_BYTES, true));
static_assert(!valid_sd_write_buffer(true, FILE_WRITE_BUFFER_BYTES - 1, true));
static_assert(valid_sd_write_buffer(true, FILE_WRITE_BUFFER_BYTES, true));
static_assert(valid_sd_write_buffer(true, FILE_WRITE_BUFFER_BYTES, false) == !REQUIRE_DIRECT_SD_DMA);
static const size_t UPLOAD_LOG_INTERVAL_BYTES = 1024 * 1024;
static constexpr int64_t UPLOAD_LOG_INTERVAL_US = 2 * 1000 * 1000;
static constexpr int64_t UPLOAD_IDLE_TIMEOUT_US = 30 * 1000 * 1000;
#if CONFIG_IDF_TARGET_ESP32S31
// Session-only trial: reopening transfer keeps the choice until reboot. Normal
// application Wi-Fi settings remain independent of this diagnostic selector.
static unsigned s_transfer_protocol = 2, s_transfer_power = 0;
static bool s_transfer_he_stbc = true, s_transfer_ampdu = true;
// Values map exactly to documented SDK power buckets (0.25 dBm units).
static constexpr int8_t TRANSFER_POWER_QDBM[] = {S31_WIFI_DEFAULT_TX_POWER_QDBM,56,80};
static const char* const TRANSFER_PROTOCOL_NAMES[] = {"b/g", "b/g/n", "b/g/n/ax"};
#endif

static void log_transfer_link(const char* stage) {
#if CONFIG_IDF_TARGET_ESP32S31
    wifi_ps_type_t ps{};
    const esp_err_t ps_err = esp_wifi_get_ps(&ps);
    wifi_ap_record_t ap{};
    const esp_err_t ap_err = esp_wifi_sta_get_ap_info(&ap);
    wifi_phy_mode_t phy{};
    const esp_err_t phy_err = esp_wifi_sta_get_negotiated_phymode(&phy);
    const char* phy_name = "unknown";
    if (phy_err == ESP_OK) {
        switch (phy) {
            case WIFI_PHY_MODE_LR: phy_name = "LR"; break;
            case WIFI_PHY_MODE_11B: phy_name = "11b"; break;
            case WIFI_PHY_MODE_11G: phy_name = "11g"; break;
            case WIFI_PHY_MODE_11A: phy_name = "11a"; break;
            case WIFI_PHY_MODE_HT20: phy_name = "HT20"; break;
            case WIFI_PHY_MODE_HT40: phy_name = "HT40"; break;
            case WIFI_PHY_MODE_HE20: phy_name = "HE20"; break;
            default: break;
        }
    }
    ESP_LOGI(TAG, "transfer link stage=%s STA=%s rssi=%d channel=%u phy=%s ps=%d ps_status=%s",
             stage, esp_err_to_name(ap_err), ap_err == ESP_OK ? ap.rssi : 0,
             ap_err == ESP_OK ? ap.primary : 0, phy_name,
             ps_err == ESP_OK ? int(ps) : -1, esp_err_to_name(ps_err));
#else
    (void)stage;
#endif
}


static const char FILE_MANAGER_HTML[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MyPaperP4 Transfer</title><style>
body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:20px auto;max-width:850px;padding:0 12px;color:#111}
h1{font-size:26px}.tabs{display:flex;gap:8px}.tabs button{flex:1}.box{border:1px solid #bbb;padding:16px;margin:14px 0}
button,input{font-size:15px;box-sizing:border-box}button{padding:10px;background:#111;color:#fff;border:0;border-radius:3px}
button.alt{background:#fff;color:#111;border:1px solid #777}.muted{color:#666;font-size:14px}progress{width:100%;height:18px}
pre{white-space:pre-wrap;background:#f3f3f3;padding:12px}.toolbar{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}
.toolbar button{width:auto}.path{font-family:monospace;overflow-wrap:anywhere}.row{display:grid;grid-template-columns:1fr auto;gap:8px;padding:10px 2px;border-top:1px solid #ddd;align-items:center}
.actions{display:flex;gap:6px}.actions button{padding:6px 9px}.hidden{display:none}.upload{padding:10px;background:#111;color:#fff;border-radius:3px;cursor:pointer}
</style></head><body><h1>MyPaperP4 <span data-i18n="WifiTransfer"></span></h1>
<div class="tabs"><button onclick="tab('smart')" data-i18n="WebSmartUpload"></button><button class="alt" onclick="tab('manager')" data-i18n="WebFileManager"></button></div>
<section id="smart" class="box"><div class="muted" data-i18n="WebSortHint"></div>
<p><input id="smartFiles" type="file" multiple></p><button onclick="uploadFiles('smartFiles','')" data-i18n="WebUploadSort"></button></section>
<section id="manager" class="box hidden"><div class="path" id="path">/</div><div class="toolbar">
<button onclick="up()" data-i18n="WebUp"></button><button onclick="reload()" data-i18n="WebRefresh"></button><button onclick="mkdirPrompt()" data-i18n="WebNewFolder"></button>
<label class="upload"><span data-i18n="WebUploadHere"></span><input id="directFiles" type="file" multiple class="hidden" onchange="uploadFiles('directFiles',currentPath)"></label></div>
<div id="entries" data-i18n="WebLoading"></div></section>
<section id="diagnostics" class="box hidden"><strong data-i18n="TransferDiagnostics"></strong>
<div class="toolbar"><button onclick="refreshDiagnostics()" data-i18n="WebRefresh"></button><button onclick="location.href='/api/diagnostics?download=1'" data-i18n="TransferDiagnosticsSave"></button></div>
<div class="muted" data-i18n="TransferDiagnosticsHint"></div><pre id="diagnosticResult">--</pre></section>
<p><progress id="bar" value="0" max="100"></progress></p><pre id="log" data-i18n="WebReady"></pre>
<script>
const translations=__UI_LOCALE__;const t=key=>translations[key]||key;
document.documentElement.lang=translations.lang;document.title='MyPaperP4 '+t('WifiTransfer');
document.querySelectorAll('[data-i18n]').forEach(el=>el.textContent=t(el.dataset.i18n));
let currentPath='/';const $=id=>document.getElementById(id);const log=t=>$('log').textContent=t;
const hasDiagnostics=__TRANSFER_DIAGNOSTICS__;
$('diagnostics').classList.toggle('hidden',!hasDiagnostics);
async function refreshDiagnostics(){if(!hasDiagnostics)return;try{const r=await fetch('/api/diagnostics',{cache:'no-store'});if(!r.ok)throw Error(r.status);$('diagnosticResult').textContent=JSON.stringify(await r.json(),null,2)}catch(e){$('diagnosticResult').textContent=t('WebNetworkError')+' '+e.message}}
if(hasDiagnostics)refreshDiagnostics();
function tab(id){$('smart').classList.toggle('hidden',id!=='smart');$('manager').classList.toggle('hidden',id!=='manager');if(id==='manager')reload()}
function request(method,url){return fetch(url,{method}).then(async r=>({ok:r.ok,status:r.status,text:await r.text()}))}
function sendFile(f,path,onp){return new Promise(resolve=>{const x=new XMLHttpRequest();let u='/upload?name='+encodeURIComponent(f.name);if(path)u+='&path='+encodeURIComponent(path);x.open('POST',u);x.upload.onprogress=e=>{if(e.lengthComputable)onp(e.loaded,e.total)};x.onload=()=>resolve({ok:x.status>=200&&x.status<300,text:x.responseText,status:x.status});x.onerror=()=>resolve({ok:false,text:t('WebNetworkError'),status:0});x.send(f)})}
async function uploadFiles(id,path){const files=[...$(id).files];if(!files.length)return;let out='';for(let i=0;i<files.length;i++){const f=files[i];out+=t('WebUploading')+f.name+'...\n';log(out);const r=await sendFile(f,path,(n,total)=>{$('bar').value=Math.round(((i+n/total)*100)/files.length)});out+=(r.ok?t('WebOk'):t('WebError')+' '+r.status+': ')+r.text+'\n';log(out);await refreshDiagnostics()}$('bar').value=100;$(id).value='';if(path)reload()}
function join(p,n){return p==='/'?'/'+n:p+'/'+n}function up(){if(currentPath==='/')return;currentPath=currentPath.slice(0,currentPath.lastIndexOf('/'))||'/';reload()}
function btn(text,fn,alt=true){const b=document.createElement('button');b.textContent=text;if(alt)b.className='alt';b.onclick=fn;return b}
async function reload(){const r=await fetch('/api/list?path='+encodeURIComponent(currentPath));if(!r.ok){log(await r.text());return}const data=await r.json();currentPath=data.path;$('path').textContent=currentPath;const box=$('entries');box.textContent='';for(const e of data.entries){const row=document.createElement('div');row.className='row';const name=document.createElement(e.dir?'button':'span');name.textContent=(e.dir?t('WebDir'):'')+e.name;if(e.dir){name.className='alt';name.onclick=()=>{currentPath=join(currentPath,e.name);reload()}}const acts=document.createElement('div');acts.className='actions';if(!e.dir)acts.appendChild(btn(t('WebDownload'),()=>location.href='/download?path='+encodeURIComponent(join(currentPath,e.name))));acts.appendChild(btn(t('Rename'),()=>renamePrompt(e.name)));acts.appendChild(btn(t('Delete'),()=>removeEntry(e.name)));row.append(name,acts);box.appendChild(row)}if(!data.entries.length)box.textContent=t('WebEmptyDirectory')}
async function action(op,path,name=''){let u='/api/action?op='+op+'&path='+encodeURIComponent(path);if(name)u+='&name='+encodeURIComponent(name);const r=await request('POST',u);log((r.ok?t('WebOk'):t('WebError')+': ')+r.text);if(r.ok)reload()}
function mkdirPrompt(){const n=prompt(t('WebFolderName'));if(n)action('mkdir',currentPath,n)}function renamePrompt(old){const n=prompt(t('WebNewName'),old);if(n&&n!==old)action('rename',join(currentPath,old),n)}function removeEntry(n){if(confirm(t('WebDeletePrefix')+n+'?'))action('delete',join(currentPath,n))}
</script></body></html>)HTML";

static bool has_ext_ci(const std::string &name, const char *ext)
{
    size_t ext_len = std::strlen(ext);
    if (name.size() < ext_len) return false;
    size_t start = name.size() - ext_len;
    for (size_t i = 0; i < ext_len; ++i) {
        char a = name[start + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static uint32_t transfer_speed_kb(size_t bytes, int64_t elapsed_us)
{
    if (elapsed_us <= 0) return 0;
    return (uint32_t)(((uint64_t)bytes * 1000000ULL) /
                      (uint64_t)elapsed_us / 1024ULL);
}

static char *allocate_sd_write_buffer(size_t requested_size,
                                      size_t *actual_size,
                                      bool *dma_compatible,
                                      bool *in_psram,
                                      size_t *dma_alignment)
{
    if (actual_size) *actual_size = 0;
    if (dma_compatible) *dma_compatible = false;
    if (in_psram) *in_psram = false;
    if (dma_alignment) *dma_alignment = 0;

#if CONFIG_IDF_TARGET_ESP32S31
    void* buffer = heap_caps_aligned_alloc(64, requested_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer) {
        if (in_psram) *in_psram = true;
    } else {
        buffer = heap_caps_aligned_alloc(64, requested_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buffer && actual_size) *actual_size = requested_size;
    return static_cast<char*>(buffer);
#else
    sdmmc_card_t *card = storage_sdcard_get_card();
    if (!card || !card->host.get_dma_info) return nullptr;

    esp_dma_mem_info_t dma_info = {};
    if (card->host.get_dma_info(card->host.slot, &dma_info) != ESP_OK) return nullptr;
    if (dma_alignment) *dma_alignment = dma_info.dma_alignment_bytes;

    void *buffer = nullptr;
    size_t allocated = 0;
#if SOC_SDMMC_PSRAM_DMA_CAPABLE
    esp_dma_mem_info_t psram_dma_info = dma_info;
    psram_dma_info.extra_heap_caps |= MALLOC_CAP_SPIRAM;
    if (esp_dma_capable_malloc(requested_size, &psram_dma_info, &buffer, &allocated) == ESP_OK) {
        bool valid = esp_dma_is_buffer_alignment_satisfied(buffer, requested_size, psram_dma_info);
        if (valid) {
            if (actual_size) *actual_size = allocated;
            if (dma_compatible) *dma_compatible = true;
            if (in_psram) *in_psram = true;
            return static_cast<char *>(buffer);
        }
        free(buffer);
        buffer = nullptr;
    }
#endif

    dma_info.extra_heap_caps |= MALLOC_CAP_INTERNAL;
    if (esp_dma_capable_malloc(requested_size, &dma_info, &buffer, &allocated) != ESP_OK) return nullptr;
    bool valid = esp_dma_is_buffer_alignment_satisfied(buffer, requested_size, dma_info);
    if (!valid) {
        free(buffer);
        return nullptr;
    }
    if (actual_size) *actual_size = allocated;
    if (dma_compatible) *dma_compatible = true;
    return static_cast<char *>(buffer);
#endif
}

static std::string url_decode(const char *s)
{
    std::string out;
    if (!s) return out;
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] == '%' && s[i + 1] && s[i + 2]) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            char *end = nullptr;
            long v = std::strtol(hex, &end, 16);
            if (end && *end == 0) {
                out.push_back((char)v);
                i += 2;
                continue;
            }
        }
        out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

static std::string safe_filename(const std::string &name)
{
    std::string base = name;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    std::string out;
    for (char c : base) {
        unsigned char u = (unsigned char)c;
        if (u < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    if (out.empty()) out = "upload.bin";
    return out;
}

static bool valid_path_component(const std::string &name)
{
    if (name.empty() || name == "." || name == ".." || name.size() > 255) return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    }
    return true;
}

static bool normalize_web_path(const std::string &input, std::string *web_path, std::string *sd_path)
{
    if (!web_path || !sd_path || input.find('\\') != std::string::npos) return false;
    std::string source = input.empty() ? "/" : input;
    if (source[0] != '/') source.insert(source.begin(), '/');
    std::string normalized;
    size_t pos = 1;
    while (pos <= source.size()) {
        size_t slash = source.find('/', pos);
        std::string part = source.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!part.empty() && part != ".") {
            if (!valid_path_component(part)) return false;
            normalized += "/" + part;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    if (normalized.empty()) normalized = "/";
    *web_path = normalized;
    *sd_path = normalized == "/" ? "/sdcard" : "/sdcard" + normalized;
    return true;
}

static std::string json_escape(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back((char)c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 0x20) out.push_back((char)c);
    }
    return out;
}

static bool ensure_dir(const char *path)
{
    struct stat st = {};
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path, 0775) == 0 || errno == EEXIST;
}

static bool ensure_dir_logged(const char *path)
{
    struct stat st = {};
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        ESP_LOGE(TAG, "path exists but is not a dir: %s", path);
        return false;
    }

    errno = 0;
    if (mkdir(path, 0775) == 0) {
        ESP_LOGI(TAG, "created dir: %s", path);
        return true;
    }
    int err = errno;
    if (err == EEXIST) return true;
    ESP_LOGE(TAG, "mkdir failed path=%s errno=%d (%s)", path, err, strerror(err));
    return false;
}

static const char *upload_dir_for_name(const std::string &name)
{
    if (strcasecmp(name.c_str(), "pinyin.bin") == 0) {
        return app_paths::ime_dir();
    }
    if (has_ext_ci(name, ".txt") || has_ext_ci(name, ".md") ||
        has_ext_ci(name, ".markdown") || has_ext_ci(name, ".epub") ||
        has_ext_ci(name, ".mobi") || has_ext_ci(name, ".prc") || has_ext_ci(name, ".azw") ||
        has_ext_ci(name, ".azw3") || has_ext_ci(name, ".fb2") || has_ext_ci(name, ".docx") ||
        has_ext_ci(name, ".zip") || has_ext_ci(name, ".cbz")) {
        return "/sdcard/books";
    }
    if (has_ext_ci(name, ".bmp") || has_ext_ci(name, ".png") ||
        has_ext_ci(name, ".jpg") || has_ext_ci(name, ".jpeg")) {
        return "/sdcard/images";
    }
    if (has_ext_ci(name, ".ttf") || has_ext_ci(name, ".otf") ||
        has_ext_ci(name, ".bin")) {
        return "/sdcard/fonts";
    }
    if (audio_file_formats::is_supported(name.c_str())) {
        return "/sdcard/music";
    }
    return nullptr;
}

static bool ensure_upload_dir(const char *path)
{
    if (!path) return false;
    if (std::strcmp(path, app_paths::ime_dir()) == 0) {
        return ensure_dir(app_paths::data_dir()) &&
               ensure_dir(app_paths::system_dir()) &&
               ensure_dir(app_paths::ime_dir());
    }
    return ensure_dir(path);
}

class WifiTransferActivity : public Activity {
public:
    explicit WifiTransferActivity(bool hotspot = false) : _force_hotspot(hotspot) {}
    bool can_standby() const override { return !_running; }
    ~WifiTransferActivity() override { stop(); }

    void on_create() override
    {
#if CONFIG_IDF_TARGET_ESP32S31
        _protocol = s_transfer_protocol;
        _power = s_transfer_power;
        _he_stbc = s_transfer_he_stbc;
        _ampdu = s_transfer_ampdu;
#endif
        _starting = true;
        draw();
        _running = start();
        if (!_running) stop();
        _starting = false;
        _session_gate.open();
#if CONFIG_IDF_TARGET_ESP32S31
        _link_stamp = connection_stamp();
        _next_link_check = esp_timer_get_time()+2000000;
#endif
        vTaskDelay(pdMS_TO_TICKS(200));
        draw();
    }

    void on_hide() override { stop(); }

    void loop() override
    {
#if CONFIG_IDF_TARGET_ESP32S31
        if ((_requested_protocol >= 0 || _requested_stbc >= 0 || _requested_ampdu >= 0 || _requested_power >= 0) && !M5.Display.displayBusy()) {
            const unsigned next = _requested_protocol >= 0 ? unsigned(_requested_protocol) : _protocol;
            const bool next_stbc = _requested_stbc >= 0 ? bool(_requested_stbc) : _he_stbc;
            const bool next_ampdu = _requested_ampdu >= 0 ? bool(_requested_ampdu) : _ampdu;
            const unsigned next_power = _requested_power >= 0 ? unsigned(_requested_power) : _power;
            _requested_protocol = _requested_stbc = _requested_ampdu = _requested_power = -1;
            if (!_session_gate.try_begin_switch()) {
                _notice = ui_locale::text(ui_locale::TextId::TransferProtocolBusy);
                _dirty = true;
            } else {
                stop(false);
                _running = false;
                _protocol = s_transfer_protocol = next;
                _power = s_transfer_power = next_power;
                _he_stbc = s_transfer_he_stbc = next_stbc;
                _ampdu = s_transfer_ampdu = next_ampdu;
                _notice.clear();
                _starting = true;
                draw();
                _running = start();
                if (!_running) stop(false);
                _starting = false;
                _session_gate.open();
                _link_stamp = connection_stamp();
                _next_link_check = esp_timer_get_time()+2000000;
                _dirty = true;
            }
        }
        if (_running && !M5.Display.displayBusy() && !_session_gate.busy() && esp_timer_get_time() >= _next_link_check) {
            _next_link_check = esp_timer_get_time()+2000000;
            const int stamp = connection_stamp();
            if (stamp != _link_stamp) { _link_stamp = stamp; _dirty = true; }
        }
#endif
        if (_diagnostics.generation() != _diagnostic_generation) {
            _diagnostic_generation = _diagnostics.generation();
            _dirty = true;
        }
        if (!M5.Display.displayBusy() && _dirty.exchange(false)) {
            draw();
        }
    }

    bool on_touch(const TouchEvent& event) override {
#if CONFIG_IDF_TARGET_ESP32S31
        if (event.type == TouchEventType::Tap && !_starting &&
            event.y >= _protocol_button_y && event.y < _protocol_button_y + _trial_line_h+20) {
            const int width = (M5.Display.width()-64)/3;
            for (unsigned i=0; i<3; ++i) {
                const int x=24+int(i)*(width+8);
                if (event.x >= x && event.x < x+width) {
                    if (i != _protocol || !_running) _requested_protocol = int(i);
                    return true;
                }
            }
        }
        if (event.type == TouchEventType::Tap && !_starting && event.x >= 24 && event.x < M5.Display.width()-24) {
            if (event.y >= _stbc_y && event.y < _stbc_y + _trial_line_h+8) {
                _requested_stbc = !_he_stbc; return true;
            }
            if (event.y >= _ampdu_y && event.y < _ampdu_y + _trial_line_h+8) {
                _requested_ampdu = !_ampdu; return true;
            }
            if (event.y >= _power_y && event.y < _power_y + _trial_line_h+8) {
                _requested_power = int((_power+1)%3); return true;
            }
        }
#else
        (void)event;
#endif
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK || key == APP_KEY_OK) {
            ActivityManager::instance().pop();
            return true;
        }
        return false;
    }

    void record_upload(const std::string &name, size_t bytes, bool ok)
    {
        std::lock_guard<std::mutex> lock(_stats_mutex);
        std::snprintf(_last_name, sizeof(_last_name), "%s", name.c_str());
        _last_bytes = bytes;
        _last_ok = ok;
#if CONFIG_IDF_TARGET_ESP32S31
        const int64_t elapsed = _upload_begin_us ? esp_timer_get_time()-_upload_begin_us : 0;
        _last_speed[_protocol][trial_flags()] = transfer_speed_kb(bytes, elapsed);
        _trial_done[_protocol][trial_flags()] = ok;
        _trial_failed[_protocol][trial_flags()] = !ok;
#endif
        _dirty = true;
    }

private:
    bool _force_hotspot = false;
    bool _running = false;
    httpd_handle_t _server = nullptr;
    TcpReceiveTest _tcp_receive_test;
    esp_netif_t *_wifi_netif = nullptr;
    bool _wifi_inited = false;
    bool _wifi_started = false;
    bool _c6_powered = false;
    bool _ap_mode = true;
    std::string _network_name;
    char _ip[24] = "192.168.4.1";
    char _last_name[96] = {};
    size_t _last_bytes = 0;
    bool _last_ok = true;
    bool _starting = false;
    std::atomic<bool> _dirty{false};
    std::mutex _stats_mutex;
    TransferSessionGate _session_gate;
    TransferDiagnostics _diagnostics;
    unsigned _diagnostic_generation = 0;
#if CONFIG_IDF_TARGET_ESP32S31
    unsigned _protocol = 0, _power = 0;
    int _requested_power = -1, _power_y = 0, _trial_line_h = 40;
    int _link_stamp = -1;
    int64_t _next_link_check = 0;
    int _requested_protocol = -1, _protocol_button_y = 0;
    bool _he_stbc = true, _ampdu = true;
    int _requested_stbc = -1, _requested_ampdu = -1;
    int _stbc_y = 0, _ampdu_y = 0;
    uint32_t _last_speed[3][12]{};
    bool _trial_done[3][12]{}, _trial_failed[3][12]{};
    unsigned trial_flags() const { return (unsigned(_he_stbc) | (unsigned(_ampdu) << 1)) + 4*_power; }
    int requested_tx_power() const { return TRANSFER_POWER_QDBM[_power]; }
    std::string power_label() const {
        return _power == 0 ? std::string(ui_locale::text(ui_locale::TextId::TransferPowerDefault))+" (5 dBm)" :
            std::to_string(requested_tx_power()/4)+" dBm";
    }
    bool apply_power() {
        const int request=requested_tx_power();
        const auto set_result=esp_wifi_set_max_tx_power(int8_t(request));
        int8_t actual=0;
        const auto read_result=esp_wifi_get_max_tx_power(&actual);
        ESP_LOGI(TAG,"TX power trial: requested_qdbm=%d set=%s read=%s actual_qdbm=%d",
            request,esp_err_to_name(set_result),esp_err_to_name(read_result),int(actual));
        if (set_result!=ESP_OK) {
            _error_message=std::string(ui_locale::text(ui_locale::TextId::FailedToStart))+": "+esp_err_to_name(set_result);
            return false;
        }
        return true;
    }
    void apply_stbc() {
        // Official IDF debug API controls HE STBC capabilities. Do not claim
        // that this verifies/disables HT STBC when the negotiated link is HT20.
        esp_wifi_enable_rx_stbc(_he_stbc);
        ESP_LOGI(TAG, "RX trial requested: HE_STBC=%d AMPDU_RX_TX=%d; HT STBC not verified",
                 int(_he_stbc), int(_ampdu));
    }
    int64_t _upload_begin_us = 0;
    std::string _notice;
    int connection_stamp() const {
        if (!_running) return -1;
        if (_ap_mode) {
            wifi_sta_list_t clients{};
            return esp_wifi_ap_get_sta_list(&clients) == ESP_OK ? clients.num : -1;
        }
        wifi_ap_record_t ap{};
        wifi_phy_mode_t phy{};
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return -1;
        return esp_wifi_sta_get_negotiated_phymode(&phy) == ESP_OK ? 100+int(phy) : 100;
    }
    esp_err_t apply_protocol(wifi_interface_t interface) {
        uint8_t mask = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G;
        if (_protocol >= 1) mask |= WIFI_PROTOCOL_11N;
        if (_protocol >= 2) mask |= WIFI_PROTOCOL_11AX;
        const esp_err_t result = esp_wifi_set_protocol(interface, mask);
        ESP_LOGI(TAG, "protocol trial: interface=%s selected=%s mask=0x%02x result=%s",
                 interface == WIFI_IF_AP ? "AP" : "STA", TRANSFER_PROTOCOL_NAMES[_protocol],
                 unsigned(mask), esp_err_to_name(result));
        if (result != ESP_OK) _error_message = std::string(ui_locale::text(ui_locale::TextId::FailedToStart))+": "+esp_err_to_name(result);
        return result;
    }
#endif
    std::string _error_message;

    bool init_net()
    {
        // Wi-Fi transfer owns the radio while switching between STA and AP,
        // so fully stop the shared station session and reset the C6 first.
        esp_err_t ret = wifi_setting_deinit(true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "wifi cleanup before transfer failed: %s", esp_err_to_name(ret));
        }

        ret = wifi_setting_c6_power_on();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "C6 power on failed: %s", esp_err_to_name(ret));
            return false;
        }
        _c6_powered = true;

        ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            ret = nvs_flash_init();
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(ret));
            return false;
        }

        ret = esp_netif_init();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
            return false;
        }
        ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(ret));
            return false;
        }
        return true;
    }

    bool start()
    {
        _error_message.clear();
        if (!init_net()) return false;

        struct stat sd_st = {};
        if (stat("/sdcard", &sd_st) != 0 || !S_ISDIR(sd_st.st_mode)) {
            _error_message = ui_locale::text(ui_locale::TextId::SdCardNotMounted);
            ESP_LOGE(TAG, "sdcard mount point missing: errno=%d (%s)", errno, strerror(errno));
            return false;
        }

        if (!ensure_dir_logged("/sdcard/books") ||
            !ensure_dir_logged("/sdcard/images") ||
            !ensure_dir_logged("/sdcard/fonts") ||
            !ensure_dir_logged("/sdcard/music")) {
            _error_message = ui_locale::text(ui_locale::TextId::CreateSdDirsFailed);
            ESP_LOGE(TAG, "failed to create transfer dirs");
            return false;
        }

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
#if !CONFIG_IDF_TARGET_ESP32S31
        cfg.static_rx_buf_num = 8;
        cfg.dynamic_rx_buf_num = 32;
        // P4 hosted transport keeps its existing overrides. Native S31 uses
        // WIFI_INIT_CONFIG_DEFAULT's TX policy (static with this PSRAM build).
        cfg.tx_buf_type = 1;
        cfg.static_tx_buf_num = 0;
        cfg.dynamic_tx_buf_num = 32;
        cfg.cache_tx_buf_num = 16;
#endif
        cfg.csi_enable = 0;
#if CONFIG_IDF_TARGET_ESP32S31
        cfg.ampdu_rx_enable = _ampdu;
        cfg.ampdu_tx_enable = _ampdu;
#else
        cfg.ampdu_rx_enable = 1;
        cfg.ampdu_tx_enable = 1;
#endif
#if !CONFIG_IDF_TARGET_ESP32S31
        cfg.rx_ba_win = 6;
#else
        // Use the same sdkconfig RX profile as normal native Wi-Fi. Do not
        // silently replace the S31 throughput trial with the P4 overrides.
        ESP_LOGI(TAG, "RX profile: static=%u dynamic=%u ba=%u tcp_window=%u tcp_mailbox=%u tcpip_mailbox=%u",
                 unsigned(cfg.static_rx_buf_num), unsigned(cfg.dynamic_rx_buf_num), unsigned(cfg.rx_ba_win),
                 unsigned(CONFIG_LWIP_TCP_WND_DEFAULT), unsigned(CONFIG_LWIP_TCP_RECVMBOX_SIZE),
                 unsigned(CONFIG_LWIP_TCPIP_RECVMBOX_SIZE));
#endif
        ESP_LOGI(TAG,
                 "heap before wifi free=%u internal=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
            return false;
        }
        _wifi_inited = true;
#if CONFIG_IDF_TARGET_ESP32S31
        err=esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err!=ESP_OK) {
            _error_message=std::string(ui_locale::text(ui_locale::TextId::FailedToStart))+": "+esp_err_to_name(err);
            return false;
        }
        apply_stbc();
        ESP_LOGI(TAG, "heap after wifi internal=%u largest=%u psram=%u",
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
#endif

        char router_ssid[WIFI_SETTING_SSID_MAX_LEN + 1] = {};
        char router_pass[WIFI_SETTING_PASSWORD_MAX_LEN + 1] = {};
        if (!_force_hotspot && wifi_setting_load_credentials(router_ssid, sizeof(router_ssid),
                                          router_pass, sizeof(router_pass))) {
            if (try_sta(router_ssid, router_pass, "NVS")) return start_http_server();
        }

        if (!start_ap()) return false;
        return start_http_server();
    }

    bool try_sta(const std::string &ssid, const std::string &password, const char *source)
    {
        ESP_LOGI(TAG, "trying STA from %s ssid=%s", source ? source : "config", ssid.c_str());
        if (start_sta(ssid, password)) return true;
        ESP_LOGW(TAG, "STA connect failed from %s, trying next mode", source ? source : "config");
        if (_wifi_started) {
            esp_wifi_stop();
            _wifi_started = false;
        }
        if (_wifi_netif) {
            esp_netif_destroy_default_wifi(_wifi_netif);
            _wifi_netif = nullptr;
        }
        return false;
    }

    bool start_ap()
    {
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        char ssid[32];
        std::snprintf(ssid, sizeof(ssid), "MyPaperS3-%02X%02X", mac[4], mac[5]);
        _network_name = ssid;
        std::snprintf(_ip, sizeof(_ip), "192.168.4.1");
        _ap_mode = true;

        _wifi_netif = esp_netif_create_default_wifi_ap();
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi AP mode failed: %s", esp_err_to_name(err));
            return false;
        }

#if CONFIG_IDF_TARGET_ESP32S31
        err = apply_protocol(WIFI_IF_AP);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "S31 Wi-Fi AP protocol failed: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "S31 Wi-Fi AP allowed protocols: %s", TRANSFER_PROTOCOL_NAMES[_protocol]);
#endif
        wifi_config_t ap_cfg = {};
        std::strncpy((char *)ap_cfg.ap.ssid, _network_name.c_str(), sizeof(ap_cfg.ap.ssid));
        std::strncpy((char *)ap_cfg.ap.password, AP_PASSWORD, sizeof(ap_cfg.ap.password));
        ap_cfg.ap.ssid_len = _network_name.size();
        ap_cfg.ap.channel = 6;
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        if (std::strlen(AP_PASSWORD) == 0) ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi config failed: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
            return false;
        }
        _wifi_started = true;
#if CONFIG_IDF_TARGET_ESP32S31
        if (!apply_power()) return false;
        apply_stbc();
#endif
        ESP_LOGI(TAG, "AP started ssid=%s password=%s url=http://%s/", _network_name.c_str(), AP_PASSWORD, _ip);
        return true;
    }

    bool start_sta(const std::string &ssid, const std::string &password)
    {
        _wifi_netif = esp_netif_create_default_wifi_sta();
        _network_name = ssid;
        _ap_mode = false;

        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi STA mode failed: %s", esp_err_to_name(err));
            return false;
        }

#if CONFIG_IDF_TARGET_ESP32S31
        err = apply_protocol(WIFI_IF_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "S31 Wi-Fi STA protocol failed: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "S31 Wi-Fi STA allowed protocols: %s", TRANSFER_PROTOCOL_NAMES[_protocol]);
#endif
        wifi_config_t sta_cfg = {};
        std::memcpy(sta_cfg.sta.ssid, ssid.data(),
                    std::min(ssid.size(), sizeof(sta_cfg.sta.ssid)));
        std::snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", password.c_str());
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi STA config failed: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi STA start failed: %s", esp_err_to_name(err));
            return false;
        }
        _wifi_started = true;
#if CONFIG_IDF_TARGET_ESP32S31
        if (!apply_power()) return false;
        apply_stbc();
#endif
#if CONFIG_IDF_TARGET_ESP32S31
        // This activity owns and later deinitializes its Wi-Fi driver. Avoid
        // native STA modem-sleep pauses during bulk uploads; P4 hosted behavior
        // is unchanged. A subsequent normal Wi-Fi init gets its own PS policy.
        const esp_err_t ps_result = esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "STA transfer power-save disable: %s", esp_err_to_name(ps_result));
#endif
        esp_wifi_connect();

        for (int i = 0; i < 80; ++i) {
            wifi_ap_record_t ap = {};
            esp_netif_ip_info_t ip_info = {};
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
                _wifi_netif &&
                esp_netif_get_ip_info(_wifi_netif, &ip_info) == ESP_OK &&
                ip_info.ip.addr != 0) {
                std::snprintf(_ip, sizeof(_ip), IPSTR, IP2STR(&ip_info.ip));
                ESP_LOGI(TAG, "STA connected ssid=%s ip=%s", ssid.c_str(), _ip);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        ESP_LOGW(TAG, "STA connect timeout ssid=%s", ssid.c_str());
        return false;
    }

    bool start_http_server()
    {
        httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
        http_cfg.server_port = 80;
        http_cfg.max_uri_handlers = 8;
        http_cfg.stack_size = 8192;
        http_cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        http_cfg.recv_wait_timeout = 10;
        esp_err_t err = httpd_start(&_server, &http_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
            return false;
        }

        httpd_uri_t index_uri = {};
        index_uri.uri = "/";
        index_uri.method = HTTP_GET;
        index_uri.handler = handle_index;
        index_uri.user_ctx = this;
        httpd_register_uri_handler(_server, &index_uri);

        httpd_uri_t upload_uri = {};
        upload_uri.uri = "/upload";
        upload_uri.method = HTTP_POST;
        upload_uri.handler = handle_upload;
        upload_uri.user_ctx = this;
        httpd_register_uri_handler(_server, &upload_uri);

        httpd_uri_t list_uri = {};
        list_uri.uri = "/api/list";
        list_uri.method = HTTP_GET;
        list_uri.handler = handle_list;
        list_uri.user_ctx = this;
        httpd_register_uri_handler(_server, &list_uri);

        httpd_uri_t action_uri = {};
        action_uri.uri = "/api/action";
        action_uri.method = HTTP_POST;
        action_uri.handler = handle_action;
        action_uri.user_ctx = this;
        httpd_register_uri_handler(_server, &action_uri);

        httpd_uri_t download_uri = {};
        download_uri.uri = "/download";
        download_uri.method = HTTP_GET;
        download_uri.handler = handle_download;
        download_uri.user_ctx = this;
        httpd_register_uri_handler(_server, &download_uri);

#if CONFIG_IDF_TARGET_ESP32S31
        httpd_uri_t diagnostics_uri{};
        diagnostics_uri.uri = "/api/diagnostics";
        diagnostics_uri.method = HTTP_GET;
        diagnostics_uri.handler = handle_diagnostics;
        diagnostics_uri.user_ctx = this;
        httpd_register_uri_handler(_server, &diagnostics_uri);
#endif
        ESP_LOGI(TAG, "http server started url=http://%s/", _ip);
        _tcp_receive_test.start();
        return true;
    }

    void stop(bool restore_wifi = true)
    {
        _session_gate.stop_accepting();
        _tcp_receive_test.stop();
        if (_server) {
            httpd_stop(_server);
            _server = nullptr;
        }
#if CONFIG_IDF_TARGET_ESP32S31
        if (_wifi_inited) esp_wifi_enable_rx_stbc(true);
        if (_wifi_started) {
            const auto restore_power=esp_wifi_set_max_tx_power(S31_WIFI_DEFAULT_TX_POWER_QDBM);
            if (restore_power!=ESP_OK) ESP_LOGW(TAG,"TX power default restore: %s",esp_err_to_name(restore_power));
        }
#endif
        if (_wifi_started) {
            esp_wifi_stop();
            _wifi_started = false;
        }
        if (_wifi_inited) {
            esp_wifi_deinit();
            _wifi_inited = false;
        }
        if (_wifi_netif) {
            esp_netif_destroy_default_wifi(_wifi_netif);
            _wifi_netif = nullptr;
        }
        if (_c6_powered) {
            esp_err_t ret = wifi_setting_deinit(true);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "C6 power down after transfer failed: %s", esp_err_to_name(ret));
            }
            _c6_powered = false;
        }
        // AP-mode transfer temporarily owns the C6. Restore the persistent
        // station connection after its server and radio have been released.
        _running = false;
        esp_err_t restore_ret = restore_wifi ? wifi_setting_start_always_on() : ESP_OK;
        if (restore_ret != ESP_OK) {
            ESP_LOGW(TAG, "restore always-on Wi-Fi failed: %s", esp_err_to_name(restore_ret));
        }
    }

    void draw()
    {
        std::lock_guard<std::mutex> lock(_stats_mutex);
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);

        int x = 24;
        int y = 12;
        int line_h = system_font::line_height() + 4;
        int text_pixels = system_font::pixel_size();
        auto draw_line = [&](const std::string &text) {
#if CONFIG_IDF_TARGET_ESP32S31
            // Preserve the system size unless the diagnostic controls would
            // overlap the footer. Measure using the actual content font.
            std::string shown=text;
            while (!shown.empty() && system_font::measure(shown,text_pixels)>M5.Display.width()-x*2) {
                size_t at=shown.size()-1;
                while (at && (static_cast<unsigned char>(shown[at])&0xc0)==0x80) --at;
                shown.resize(at);
            }
            system_font::draw_left_sized(M5.Display, shown, x, y, text_pixels);
#else
            system_font::draw_left_sized(M5.Display,system_font::ellipsize(text,M5.Display.width()-x*2),x,y,text_pixels);
#endif
            y += line_h;
        };
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::WifiTransfer), M5.Display.width() / 2, y);
        y += line_h + 10;

#if CONFIG_IDF_TARGET_ESP32S31
        using ui_locale::TextId;
        const int available = M5.Display.height()-system_font::line_height()-4-y;
        while (text_pixels>16 && 14*system_font::line_height(text_pixels)+116>available) --text_pixels;
        line_h=system_font::line_height(text_pixels)+4;
        _trial_line_h=line_h;
        auto center_content = [&](const std::string& text,int cx,int top) {
            system_font::draw_left_sized(M5.Display,text,cx-system_font::measure(text,text_pixels)/2,top,text_pixels);
        };
        _protocol_button_y = y;
        const int button_w = (M5.Display.width()-64)/3;
        const int button_h = line_h+20;
        for (unsigned i=0; i<3; ++i) {
            const int bx=24+int(i)*(button_w+8);
            if (i == _protocol) M5.Display.fillRoundRect(bx, y+8, 5, button_h-16, 2, TFT_BLACK);
            center_content(TRANSFER_PROTOCOL_NAMES[i], bx+button_w/2, y+12);
            M5.Display.drawFastHLine(bx, y+button_h-1, button_w, TFT_BLACK);
        }
        y += button_h+8;
        auto draw_toggle = [&](const char* label, bool on, int& hit_y) {
            hit_y = y;
            system_font::draw_left_sized(M5.Display, label, x+12, y+4, text_pixels);
            const std::string state = std::string(ui_locale::text(on ? TextId::On : TextId::Off))+" >";
            system_font::draw_right_sized(M5.Display, state, M5.Display.width()-x, y+4, text_pixels);
            M5.Display.drawFastHLine(x, y+line_h+3, M5.Display.width()-2*x, TFT_BLACK);
            y += line_h+8;
        };
        draw_toggle(ui_locale::text(TextId::TransferHeStbc), _he_stbc, _stbc_y);
        draw_toggle(ui_locale::text(TextId::TransferAmpdu), _ampdu, _ampdu_y);
        _power_y=y;
        system_font::draw_left_sized(M5.Display,ui_locale::text(TextId::TransferPowerLimit),x+12,y+4,text_pixels);
        system_font::draw_right_sized(M5.Display,power_label()+" >",M5.Display.width()-x,y+4,text_pixels);
        M5.Display.drawFastHLine(x,y+line_h+3,M5.Display.width()-2*x,TFT_BLACK);
        y+=line_h+8;
        draw_line(_notice.empty() ? ui_locale::text(TextId::TransferProtocolReconnect) : _notice);
        if (_starting) {
            draw_line(ui_locale::text(TextId::StartingWifi));
        } else if (_running) {
            draw_line(ui_locale::text(_ap_mode ? TextId::ModeApHotspot : TextId::ModeRouterWifi));
            draw_line(_network_name);
            if (_ap_mode) draw_line(ui_locale::text(TextId::ApPassword));
            draw_line(std::string("http://")+_ip);
            if (_ap_mode) {
                wifi_sta_list_t stations{};
                if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK) {
                    char state[80];
                    std::snprintf(state, sizeof(state), ui_locale::text(TextId::TransferClientsFormat), unsigned(stations.num));
                    draw_line(state);
                }
            } else {
                wifi_phy_mode_t phy{};
                wifi_ap_record_t ap{};
                const bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
                const esp_err_t mode_result = esp_wifi_sta_get_negotiated_phymode(&phy);
                const char* mode = mode_result != ESP_OK ? "--" :
                    phy == WIFI_PHY_MODE_11B ? "11b" : phy == WIFI_PHY_MODE_11G ? "11g" :
                    phy == WIFI_PHY_MODE_HT20 ? "HT20" : phy == WIFI_PHY_MODE_HT40 ? "HT40" :
                    phy == WIFI_PHY_MODE_HE20 ? "HE20" : "--";
                char state[96];
                std::snprintf(state, sizeof(state), "%s  %d dBm  CH%u", mode,
                    connected ? ap.rssi : 0, connected ? unsigned(ap.primary) : 0);
                draw_line(connected ? state : ui_locale::text(TextId::TransferDisconnected));
            }
            y += 8;
            draw_line(ui_locale::text(TextId::TransferSpeedResults));
            for (unsigned i=0; i<3; ++i) {
                const std::string result = _trial_done[i][trial_flags()] ? std::to_string(_last_speed[i][trial_flags()])+" KiB/s" :
                    _trial_failed[i][trial_flags()] ? ui_locale::text(TextId::UploadError) : "--";
                draw_line(std::string(TRANSFER_PROTOCOL_NAMES[i])+" : "+result);
            }
            if (y+line_h < M5.Display.height()-system_font::line_height()-4)
                draw_line(_diagnostics.generation() ? _diagnostics.summary() : ui_locale::text(TextId::TransferDiagnosticsHint));
        } else {
            draw_line(_error_message.empty() ? ui_locale::text(TextId::FailedToStart) : _error_message);
            draw_line(ui_locale::text(TextId::TransferProtocolRetry));
        }
        system_font::draw_left(M5.Display, ui_locale::text(TextId::BackOkStop),
            x, M5.Display.height()-system_font::line_height()-4);
        M5.Display.display();
        M5.Display.endWrite();
        return;
#endif

        if (_starting) {
            draw_line(ui_locale::text(ui_locale::TextId::StartingWifi));
            draw_line(ui_locale::text(ui_locale::TextId::PleaseWait));
            M5.Display.display();
            M5.Display.endWrite();
            return;
        }

        if (_running) {
            if (_ap_mode) {
                draw_line(ui_locale::text(ui_locale::TextId::ModeApHotspot));
                draw_line(ui_locale::text(ui_locale::TextId::ConnectWifi));
                draw_line(_network_name);
                draw_line(ui_locale::text(ui_locale::TextId::ApPassword));
            } else {
                draw_line(ui_locale::text(ui_locale::TextId::ModeRouterWifi));
                draw_line(ui_locale::text(ui_locale::TextId::ConnectedLabel));
                std::string show = _network_name;
                if (show.size() > 20) show = show.substr(0, 17) + "...";
                draw_line(show);
            }
            y += 8;
            draw_line(ui_locale::text(ui_locale::TextId::OpenBrowser));
            std::string url = std::string("http://") + _ip;
            draw_line(url);
            y += 8;
            draw_line(ui_locale::text(ui_locale::TextId::WebUploadManager));
            y += 8;
            draw_line(ui_locale::text(_last_ok ? ui_locale::TextId::LastUpload
                                               : ui_locale::TextId::UploadError));
            std::string last = _last_name[0] ? _last_name : ui_locale::text(ui_locale::TextId::NoUploadsYet);
            if (last.size() > 20) last = last.substr(0, 17) + "...";
            draw_line(last);
            char bytes[48];
            std::snprintf(bytes, sizeof(bytes), ui_locale::text(ui_locale::TextId::ByteCountFormat),
                          (unsigned)_last_bytes);
            draw_line(bytes);
            system_font::draw_left(M5.Display, ui_locale::text(ui_locale::TextId::BackOkStop),
                                   x, M5.Display.height() - system_font::line_height() - 4);
        } else {
            draw_line(_error_message.empty() ? ui_locale::text(ui_locale::TextId::FailedToStart)
                                             : _error_message);
            draw_line(ui_locale::text(ui_locale::TextId::WifiServer));
            y += line_h + 10;
            draw_line(ui_locale::text(ui_locale::TextId::BackOkReturn));
        }
        M5.Display.display();
        M5.Display.endWrite();
    }

    static esp_err_t handle_diagnostics(httpd_req_t* req) {
        auto* self=static_cast<WifiTransferActivity*>(req->user_ctx);
        TransferSessionGate::Request operation(self->_session_gate);
        if (!operation) return restarting_response(req);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        std::string download;
        if (query_value(req, "download", &download) && download == "1")
            httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"s31-wifi-diagnostic.json\"");
        const auto report=self->_diagnostics.json();
        return httpd_resp_send(req,report.c_str(),report.size());
    }

    static esp_err_t restarting_response(httpd_req_t* req) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Connection", "close");
        return httpd_resp_sendstr(req, ui_locale::text(ui_locale::TextId::TransferProtocolChanging));
    }

    static esp_err_t handle_index(httpd_req_t *req)
    {
        auto* gate_owner = static_cast<WifiTransferActivity*>(req->user_ctx);
        TransferSessionGate::Request operation(gate_owner->_session_gate);
        if (!operation) return restarting_response(req);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        using ui_locale::TextId;
        struct WebText { const char *key; TextId id; };
        static constexpr WebText labels[] = {
            {"WifiTransfer", TextId::WifiTransfer},
            {"TransferDiagnostics", TextId::TransferDiagnostics},
            {"TransferDiagnosticsSave", TextId::TransferDiagnosticsSave},
            {"TransferDiagnosticsHint", TextId::TransferDiagnosticsHint},
            {"WebSmartUpload", TextId::WebSmartUpload},
            {"WebFileManager", TextId::WebFileManager},
            {"WebSortHint", TextId::WebSortHint},
            {"WebUploadSort", TextId::WebUploadSort},
            {"WebUp", TextId::WebUp},
            {"WebRefresh", TextId::WebRefresh},
            {"WebNewFolder", TextId::WebNewFolder},
            {"WebUploadHere", TextId::WebUploadHere},
            {"WebLoading", TextId::WebLoading},
            {"WebReady", TextId::WebReady},
            {"WebNetworkError", TextId::WebNetworkError},
            {"WebUploading", TextId::WebUploading},
            {"WebOk", TextId::WebOk},
            {"WebError", TextId::WebError},
            {"WebDir", TextId::WebDir},
            {"WebDownload", TextId::WebDownload},
            {"WebEmptyDirectory", TextId::WebEmptyDirectory},
            {"WebFolderName", TextId::WebFolderName},
            {"WebNewName", TextId::WebNewName},
            {"WebDeletePrefix", TextId::WebDeletePrefix},
            {"Rename", TextId::Rename},
            {"Delete", TextId::Delete},
        };
        const char *language = ui_locale::get() == ui_locale::Language::SimplifiedChinese ? "zh-Hans"
                             : ui_locale::get() == ui_locale::Language::TraditionalChinese ? "zh-Hant" : "en";
        std::string translations = std::string("{\"lang\":\"") + language + "\"";
        for (const auto &label : labels) {
            translations += std::string(",\"") + label.key + "\":\"" +
                            json_escape(ui_locale::text(label.id)) + "\"";
        }
        translations += "}";
        std::string page = FILE_MANAGER_HTML;
        const size_t diagnostics_marker = page.find("__TRANSFER_DIAGNOSTICS__");
        if (diagnostics_marker != std::string::npos)
            page.replace(diagnostics_marker, std::strlen("__TRANSFER_DIAGNOSTICS__"),
#if CONFIG_IDF_TARGET_ESP32S31
                "true"
#else
                "false"
#endif
            );
        constexpr char marker[] = "__UI_LOCALE__";
        const size_t position = page.find(marker);
        if (position != std::string::npos) page.replace(position, sizeof(marker) - 1, translations);
        return httpd_resp_send(req, page.c_str(), page.size());
    }

    static bool query_value(httpd_req_t *req, const char *key, std::string *value)
    {
        if (!req || !key || !value) return false;
        char query[1024] = {};
        char encoded[768] = {};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, key, encoded, sizeof(encoded)) != ESP_OK) return false;
        *value = url_decode(encoded);
        return true;
    }

    static esp_err_t handle_list(httpd_req_t *req)
    {
        auto* gate_owner = static_cast<WifiTransferActivity*>(req->user_ctx);
        TransferSessionGate::Request operation(gate_owner->_session_gate);
        if (!operation) return restarting_response(req);
        std::string requested, web_path, sd_path;
        query_value(req, "path", &requested);
        if (!normalize_web_path(requested, &web_path, &sd_path)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ui_locale::text(ui_locale::TextId::InvalidPath));
        }
        DIR *dir = opendir(sd_path.c_str());
        if (!dir) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, ui_locale::text(ui_locale::TextId::DirectoryNotFound));

        httpd_resp_set_type(req, "application/json; charset=utf-8");
        std::string head = "{\"path\":\"" + json_escape(web_path) + "\",\"entries\":[";
        if (httpd_resp_send_chunk(req, head.c_str(), head.size()) != ESP_OK) {
            closedir(dir);
            return ESP_FAIL;
        }
        bool first = true;
        struct dirent *entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
            std::string full = sd_path + "/" + entry->d_name;
            struct stat st = {};
            if (stat(full.c_str(), &st) != 0) continue;
            std::string item = std::string(first ? "" : ",") + "{\"name\":\"" +
                               json_escape(entry->d_name) + "\",\"dir\":" +
                               (S_ISDIR(st.st_mode) ? "true" : "false") + ",\"size\":" +
                               std::to_string((unsigned long long)st.st_size) + "}";
            if (httpd_resp_send_chunk(req, item.c_str(), item.size()) != ESP_OK) {
                closedir(dir);
                return ESP_FAIL;
            }
            first = false;
        }
        closedir(dir);
        httpd_resp_send_chunk(req, "]}", 2);
        return httpd_resp_send_chunk(req, nullptr, 0);
    }

    static esp_err_t handle_action(httpd_req_t *req)
    {
        auto* gate_owner = static_cast<WifiTransferActivity*>(req->user_ctx);
        TransferSessionGate::Request operation(gate_owner->_session_gate);
        if (!operation) return restarting_response(req);
        std::string op, requested, name, web_path, sd_path;
        if (!query_value(req, "op", &op) || !query_value(req, "path", &requested) ||
            !normalize_web_path(requested, &web_path, &sd_path)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ui_locale::text(ui_locale::TextId::InvalidActionPath));
        }
        query_value(req, "name", &name);
        int result = -1;
        if (op == "mkdir") {
            if (!valid_path_component(name)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ui_locale::text(ui_locale::TextId::InvalidFolderName));
            result = mkdir((sd_path + "/" + name).c_str(), 0775);
        } else if (op == "rename") {
            if (web_path == "/" || !valid_path_component(name)) {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ui_locale::text(ui_locale::TextId::InvalidNewName));
            }
            size_t slash = sd_path.find_last_of('/');
            std::string destination = sd_path.substr(0, slash + 1) + name;
            struct stat st = {};
            if (stat(destination.c_str(), &st) == 0) {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ui_locale::text(ui_locale::TextId::DestinationExists));
            }
            result = std::rename(sd_path.c_str(), destination.c_str());
            if (result == 0) recent_books_rename_path(sd_path.c_str(), destination.c_str());
        } else if (op == "delete") {
            if (web_path == "/") return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ui_locale::text(ui_locale::TextId::CannotDeleteSdRoot));
            struct stat st = {};
            if (stat(sd_path.c_str(), &st) != 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, ui_locale::text(ui_locale::TextId::EntryNotFound));
            result = S_ISDIR(st.st_mode) ? rmdir(sd_path.c_str()) : unlink(sd_path.c_str());
        } else {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ui_locale::text(ui_locale::TextId::UnknownAction));
        }
        if (result != 0) {
            ESP_LOGW(TAG, "file action failed op=%s path=%s errno=%d", op.c_str(), sd_path.c_str(), errno);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       op == "delete" ? ui_locale::text(ui_locale::TextId::DeleteEmptyFolderHint) : ui_locale::text(ui_locale::TextId::FileOperationFailed));
        }
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, ui_locale::text(ui_locale::TextId::OperationComplete));
    }

    static esp_err_t handle_download(httpd_req_t *req)
    {
        auto* gate_owner = static_cast<WifiTransferActivity*>(req->user_ctx);
        TransferSessionGate::Request operation(gate_owner->_session_gate);
        if (!operation) return restarting_response(req);
        std::string requested, web_path, sd_path;
        if (!query_value(req, "path", &requested) ||
            !normalize_web_path(requested, &web_path, &sd_path) || web_path == "/") {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ui_locale::text(ui_locale::TextId::InvalidFilePath));
        }
        struct stat st = {};
        if (stat(sd_path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, ui_locale::text(ui_locale::TextId::FileNotFound));
        }
        FILE *fp = std::fopen(sd_path.c_str(), "rb");
        if (!fp) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::FileOpenFailed));
        char *buffer = static_cast<char *>(heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buffer) buffer = static_cast<char *>(heap_caps_malloc(16 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!buffer) {
            std::fclose(fp);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::DownloadBufferUnavailable));
        }
        httpd_resp_set_type(req, "application/octet-stream");
        size_t slash = web_path.find_last_of('/');
        std::string download_name = slash == std::string::npos ? web_path : web_path.substr(slash + 1);
        std::string disposition = "attachment; filename=\"" + safe_filename(download_name) + "\"";
        httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
        esp_err_t result = ESP_OK;
        size_t capacity = heap_caps_get_allocated_size(buffer);
#if CONFIG_IDF_TARGET_ESP32S31
        TransferDiagnostics::Run diagnostic(gate_owner->_diagnostics,download_name,"download",TRANSFER_PROTOCOL_NAMES[gate_owner->_protocol],gate_owner->_he_stbc,gate_owner->_ampdu,gate_owner->_ap_mode,gate_owner->requested_tx_power());
#endif
        while (true) {
            const int64_t read_begin=esp_timer_get_time();
            size_t count = std::fread(buffer, 1, capacity, fp);
#if CONFIG_IDF_TARGET_ESP32S31
            diagnostic.storage(esp_timer_get_time()-read_begin);
#endif
            if (count > 0) {
                const int64_t send_begin=esp_timer_get_time();
                result=httpd_resp_send_chunk(req,buffer,count);
#if CONFIG_IDF_TARGET_ESP32S31
                diagnostic.network(esp_timer_get_time()-send_begin,result==ESP_OK?count:0);
#endif
                if (result!=ESP_OK) break;
            }
            if (count < capacity) { if (std::ferror(fp)) result=ESP_FAIL; break; }
        }
        heap_caps_free(buffer);
        std::fclose(fp);
        if (result != ESP_OK) return result;
        result=httpd_resp_send_chunk(req, nullptr, 0);
#if CONFIG_IDF_TARGET_ESP32S31
        diagnostic.finish(result==ESP_OK);
#endif
        return result;
    }

    static esp_err_t handle_upload(httpd_req_t *req)
    {
        auto* gate_owner = static_cast<WifiTransferActivity*>(req->user_ctx);
        TransferSessionGate::Request operation(gate_owner->_session_gate);
        if (!operation) return restarting_response(req);
        WifiTransferActivity *self = (WifiTransferActivity *)req->user_ctx;
#if CONFIG_IDF_TARGET_ESP32S31
        if (self) self->_upload_begin_us = 0;
#endif
        std::string name = "upload.bin";
        query_value(req, "name", &name);

        name = safe_filename(name);
        std::string requested_dir, web_dir, direct_dir;
        bool direct = query_value(req, "path", &requested_dir);
        if (direct && !normalize_web_path(requested_dir, &web_dir, &direct_dir)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ui_locale::text(ui_locale::TextId::InvalidDestinationPath));
            return ESP_FAIL;
        }
        const char *dir = direct ? direct_dir.c_str() : upload_dir_for_name(name);
        ESP_LOGI(TAG, "upload begin dir=%s name=%s content_len=%d",
                 dir ? dir : "unsupported", name.c_str(), req->content_len);
        if (dir == nullptr) {
            ESP_LOGW(TAG, "upload rejected unsupported name=%s", name.c_str());
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ui_locale::text(ui_locale::TextId::UnsupportedFileType));
            if (self) self->record_upload(name, 0, false);
            return ESP_FAIL;
        }

        struct stat dir_stat = {};
        bool destination_ok = direct ? (stat(dir, &dir_stat) == 0 && S_ISDIR(dir_stat.st_mode))
                                     : ensure_upload_dir(dir);
        if (!destination_ok) {
            ESP_LOGE(TAG, "upload destination unavailable dir=%s", dir);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::DestinationUnavailable));
            if (self) self->record_upload(name, 0, false);
            return ESP_FAIL;
        }
        std::string final_path = std::string(dir) + "/" + name;
        std::string path = final_path + ".uploading";
        unlink(path.c_str());
        int64_t prealloc_t0 = esp_timer_get_time();
#if CONFIG_IDF_TARGET_ESP32S31
        // The IDF 6.1 FAT build disables f_expand. Use the existing streaming
        // allocation fallback rather than modifying the shared SDK sources.
        esp_err_t prealloc_result = ESP_ERR_NOT_SUPPORTED;
#else
        esp_err_t prealloc_result = req->content_len > 0
                                      ? esp_vfs_fat_create_contiguous_file(STORAGE_SDCARD_MOUNT_POINT,
                                                                           path.c_str(),
                                                                           (uint64_t)req->content_len,
                                                                           true)
                                      : ESP_ERR_INVALID_SIZE;
#endif
        int64_t prealloc_us = esp_timer_get_time() - prealloc_t0;
        FILE *fp = std::fopen(path.c_str(), prealloc_result == ESP_OK ? "r+b" : "wb");
        if (!fp) {
            ESP_LOGE(TAG, "upload open failed path=%s errno=%d", path.c_str(), errno);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::CreateFileFailed));
            if (self) self->record_upload(name, 0, false);
            return ESP_FAIL;
        }
        if (prealloc_result == ESP_OK) {
            ESP_LOGI(TAG, "upload preallocated path=%s bytes=%d time=%lld ms",
                     path.c_str(), req->content_len, prealloc_us / 1000);
        } else {
            ESP_LOGW(TAG, "upload preallocate unavailable path=%s err=%s time=%lld ms; using incremental allocation",
                     path.c_str(), esp_err_to_name(prealloc_result), prealloc_us / 1000);
        }

        size_t file_buf_allocated = 0;
        size_t file_buf_dma_alignment = 0;
        bool file_buf_dma_compatible = false;
        bool file_buf_in_psram = false;
        char *file_buf = allocate_sd_write_buffer(FILE_WRITE_BUFFER_BYTES,
                                                  &file_buf_allocated,
                                                  &file_buf_dma_compatible,
                                                  &file_buf_in_psram,
                                                  &file_buf_dma_alignment);
        if (!valid_sd_write_buffer(file_buf != nullptr, file_buf_allocated, file_buf_dma_compatible)) {
            ESP_LOGE(TAG, "SD write buffer unavailable: ptr=%p allocated=%u required=%u direct_dma_required=%d dma_ok=%d psram_free=%u psram_largest=%u",
                     file_buf, (unsigned)file_buf_allocated, (unsigned)FILE_WRITE_BUFFER_BYTES,
                     REQUIRE_DIRECT_SD_DMA, file_buf_dma_compatible,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            std::fclose(fp);
            if (file_buf) free(file_buf);
            unlink(path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(REQUIRE_DIRECT_SD_DMA
                ? ui_locale::TextId::SdDmaBufferFailed : ui_locale::TextId::SdWriteBufferFailed));
            if (self) self->record_upload(name, 0, false);
            return ESP_FAIL;
        }
        if (setvbuf(fp, file_buf, _IOFBF, FILE_WRITE_BUFFER_BYTES) != 0) {
            ESP_LOGE(TAG, "SD stdio buffer setup failed");
            std::fclose(fp);
            free(file_buf);
            unlink(path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::SdWriteBufferFailed));
            if (self) self->record_upload(name, 0, false);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "SD write buffer ptr=%p requested=%u allocated=%u dma_align=%u dma_ok=%d memory=%s path=%s",
                 file_buf,
                 (unsigned)FILE_WRITE_BUFFER_BYTES,
                 (unsigned)file_buf_allocated,
                 (unsigned)file_buf_dma_alignment,
                 file_buf_dma_compatible ? 1 : 0,
                 file_buf_in_psram ? "PSRAM" : "internal",
                 REQUIRE_DIRECT_SD_DMA ? "direct-dma" : "sdmmc-bounce");

        char *buf = (char *)heap_caps_malloc(UPLOAD_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            buf = (char *)heap_caps_malloc(UPLOAD_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!buf) {
            std::fclose(fp);
            if (file_buf) free(file_buf);
            unlink(path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::UploadBufferFailed));
            if (self) self->record_upload(name, 0, false);
            return ESP_FAIL;
        }

        int remaining = req->content_len;
        size_t written = 0;
        size_t next_log = UPLOAD_LOG_INTERVAL_BYTES;
        int64_t t0 = esp_timer_get_time();
#if CONFIG_IDF_TARGET_ESP32S31
        if (self) self->_upload_begin_us = t0;
#endif
#if CONFIG_IDF_TARGET_ESP32S31
        TransferDiagnostics::Run diagnostic(self->_diagnostics,name,"upload",TRANSFER_PROTOCOL_NAMES[self->_protocol],self->_he_stbc,self->_ampdu,self->_ap_mode,self->requested_tx_power());
#endif
        log_transfer_link("upload-begin");
        int64_t last_log_time = t0;
        size_t last_log_written = 0;
        int64_t recv_us = 0;
        int64_t write_us = 0;
        int64_t max_recv_us = 0;
        int64_t max_write_us = 0;
        uint32_t recv_calls = 0;
        uint32_t write_calls = 0;
        uint32_t recv_timeouts = 0;
        int64_t last_data_time = t0;
        while (remaining > 0) {
            int want = std::min<int>(remaining, UPLOAD_BUFFER_BYTES);
            int64_t recv_t0 = esp_timer_get_time();
            errno = 0;
            int got = httpd_req_recv(req, buf, want);
            const int recv_errno = errno;
            int64_t recv_elapsed = esp_timer_get_time() - recv_t0;
            recv_us += recv_elapsed;
            max_recv_us = std::max(max_recv_us, recv_elapsed);
            recv_calls++;
#if CONFIG_IDF_TARGET_ESP32S31
            diagnostic.network(recv_elapsed,got>0?size_t(got):0,got==HTTPD_SOCK_ERR_TIMEOUT);
#endif
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                recv_timeouts++;
                const int64_t idle_us = esp_timer_get_time() - last_data_time;
                ESP_LOGW(TAG, "upload receive timeout: written=%u/%d remaining=%d idle=%lld ms timeouts=%u recv=%lld ms write=%lld ms",
                         (unsigned)written, req->content_len, remaining, idle_us / 1000,
                         (unsigned)recv_timeouts, recv_us / 1000, write_us / 1000);
                log_transfer_link("receive-timeout");
                if (idle_us < UPLOAD_IDLE_TIMEOUT_US) continue;
            }
            if (got <= 0) {
                ESP_LOGE(TAG, "upload receive failed: got=%d errno=%d written=%u/%d remaining=%d elapsed=%lld ms recv_calls=%u timeouts=%u recv=%lld ms write=%lld ms recv_max=%lld ms write_max=%lld ms",
                         got, recv_errno, (unsigned)written, req->content_len, remaining,
                         (esp_timer_get_time() - t0) / 1000, (unsigned)recv_calls,
                         (unsigned)recv_timeouts, recv_us / 1000, write_us / 1000,
                         max_recv_us / 1000, max_write_us / 1000);
                wifi_ap_record_t ap{};
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                    ESP_LOGW(TAG, "upload STA link: rssi=%d channel=%u", ap.rssi, ap.primary);
                ESP_LOGW(TAG, "upload heap: internal=%u largest=%u psram=%u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                heap_caps_free(buf);
                std::fclose(fp);
                if (file_buf) free(file_buf);
                unlink(path.c_str());
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::UploadReceiveFailed));
                if (self) self->record_upload(name, written, false);
                return ESP_FAIL;
            }
            last_data_time = esp_timer_get_time();
            if (written == 0)
                ESP_LOGI(TAG, "upload first body data: bytes=%d wait=%lld ms", got, (last_data_time - t0) / 1000);
            int64_t write_t0 = esp_timer_get_time();
            size_t n = std::fwrite(buf, 1, got, fp);
            int64_t write_elapsed = esp_timer_get_time() - write_t0;
            write_us += write_elapsed;
            max_write_us = std::max(max_write_us, write_elapsed);
            write_calls++;
#if CONFIG_IDF_TARGET_ESP32S31
            diagnostic.storage(write_elapsed);
#endif
            if (n != (size_t)got) {
                heap_caps_free(buf);
                std::fclose(fp);
                if (file_buf) free(file_buf);
                unlink(path.c_str());
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::SdWriteFailed));
                if (self) self->record_upload(name, written, false);
                return ESP_FAIL;
            }
            written += n;
            remaining -= got;
            if (written >= next_log || remaining == 0 ||
                esp_timer_get_time() - last_log_time >= UPLOAD_LOG_INTERVAL_US) {
                int64_t now = esp_timer_get_time();
                uint32_t average_kb = transfer_speed_kb(written, now - t0);
                uint32_t interval_kb = transfer_speed_kb(written - last_log_written, now - last_log_time);
                ESP_LOGI(TAG, "upload progress name=%s written=%u/%d avg=%u KB/s inst=%u KB/s recv=%lld ms write=%lld ms recv_max=%lld ms write_max=%lld ms timeouts=%u",
                         name.c_str(),
                         (unsigned)written,
                         req->content_len,
                         (unsigned)average_kb,
                         (unsigned)interval_kb,
                         recv_us / 1000,
                         write_us / 1000,
                         max_recv_us / 1000,
                         max_write_us / 1000,
                         (unsigned)recv_timeouts);
                last_log_time = now;
                last_log_written = written;
                while (next_log <= written) next_log += UPLOAD_LOG_INTERVAL_BYTES;
            }
        }
        heap_caps_free(buf);
        int64_t close_t0 = esp_timer_get_time();
        int close_result = std::fclose(fp);
        int64_t close_us = esp_timer_get_time() - close_t0;
#if CONFIG_IDF_TARGET_ESP32S31
        diagnostic.flush(close_us);
#endif
        if (file_buf) free(file_buf);
        if (close_result != 0) {
            ESP_LOGE(TAG, "upload final flush failed path=%s errno=%d (%s)", path.c_str(), errno, strerror(errno));
            unlink(path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::SdFlushFailed));
            if (self) self->record_upload(name, written, false);
            return ESP_FAIL;
        }

        // Keep the previous file intact until the upload is completely
        // received and flushed to the SD card.
        std::string backup_path = final_path + ".upload.bak";
        unlink(backup_path.c_str());
        struct stat previous = {};
        bool had_previous = stat(final_path.c_str(), &previous) == 0;
        if (had_previous && std::rename(final_path.c_str(), backup_path.c_str()) != 0) {
            unlink(path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::PreserveFileFailed));
            if (self) self->record_upload(name, written, false);
            return ESP_FAIL;
        }
        if (std::rename(path.c_str(), final_path.c_str()) != 0) {
            ESP_LOGE(TAG, "upload commit failed temp=%s final=%s errno=%d",
                     path.c_str(), final_path.c_str(), errno);
            if (had_previous) std::rename(backup_path.c_str(), final_path.c_str());
            unlink(path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ui_locale::text(ui_locale::TextId::CommitUploadFailed));
            if (self) self->record_upload(name, written, false);
            return ESP_FAIL;
        }
        if (had_previous) unlink(backup_path.c_str());

#if CONFIG_IDF_TARGET_ESP32S31
        diagnostic.finish(true);
#endif
        char msg[256];
        std::snprintf(msg, sizeof(msg), ui_locale::text(ui_locale::TextId::UploadSavedFormat), final_path.c_str(), (unsigned)written);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, msg);
        if (self) self->record_upload(name, written, true);
        int64_t elapsed_us = esp_timer_get_time() - t0;
        uint32_t speed_kb = transfer_speed_kb(written, elapsed_us);
        ESP_LOGI(TAG, "upload done path=%s bytes=%u time=%lld ms avg=%u KB/s recv=%lld ms write=%lld ms close=%lld ms recv_calls=%u write_calls=%u timeouts=%u",
                 final_path.c_str(),
                 (unsigned)written,
                 elapsed_us / 1000,
                 (unsigned)speed_kb,
                 recv_us / 1000,
                 write_us / 1000,
                 close_us / 1000,
                 (unsigned)recv_calls,
                 (unsigned)write_calls,
                 (unsigned)recv_timeouts);
        return ESP_OK;
    }
};

void launch_wifi_transfer()
{
    if (!ensure_wifi_configured_or_show_guide()) return;
    ActivityManager::instance().push(new WifiTransferActivity());
}

void launch_hotspot_transfer()
{
    ActivityManager::instance().push(new WifiTransferActivity(true));
}
