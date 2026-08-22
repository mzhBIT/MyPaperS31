#include "activities/WifiTransferActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "storage/storage_sdcard.h"
#include "ui/SystemFont.h"
#include "wifi/wifi_setting.h"

#include <M5Unified.h>
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
#include <nvs_flash.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const char *TAG = "WIFI_TRANSFER";
static const char *AP_PASSWORD = "12345678";
static const char *WIFI_CONFIG_PATH = "/sdcard/wifi.txt";
static const char *DEFAULT_STA_SSID = "TP-LINK_A027";
static const char *DEFAULT_STA_PASSWORD = "18557530408";
static const size_t UPLOAD_BUFFER_BYTES = 64 * 1024;
static const size_t FILE_WRITE_BUFFER_BYTES = 64 * 1024;
static const size_t UPLOAD_LOG_INTERVAL_BYTES = 1024 * 1024;

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MyPaperS3 Transfer</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:24px;max-width:720px;color:#111}"
    "h1{font-size:26px;margin:0 0 16px}.box{border:1px solid #bbb;padding:16px;margin:14px 0}"
    "label{display:block;margin:12px 0 6px;font-weight:600}select,input,button{font-size:16px;width:100%;box-sizing:border-box}"
    "button{margin-top:14px;padding:12px;background:#111;color:white;border:0}.muted{color:#666;font-size:14px}"
    "progress{width:100%;height:18px}pre{white-space:pre-wrap;background:#f3f3f3;padding:12px}"
    "</style></head><body>"
    "<h1>MyPaperS3 WiFi Transfer</h1>"
    "<div class='box'>"
    "<div class='muted'>Upload books, images, and fonts to the SD card.</div>"
    "<label>Target</label><select id='target'>"
    "<option value='books'>Books: /sdcard/books (.txt .epub)</option>"
    "<option value='images'>Images: /sdcard/images (.bmp .png .jpg .jpeg)</option>"
    "<option value='fonts'>Fonts: /sdcard/fonts (.ttf .otf .bin)</option>"
    "</select>"
    "<label>Files</label><input id='files' type='file' multiple>"
    "<button onclick='upload()'>Upload</button>"
    "<p><progress id='bar' value='0' max='100'></progress></p>"
    "<pre id='log'>Ready.</pre>"
    "</div>"
    "<script>"
    "const log=t=>document.getElementById('log').textContent=t;"
    "function sendFile(target,f,onp){return new Promise((resolve)=>{"
    " const xhr=new XMLHttpRequest();"
    " xhr.open('POST','/upload?target='+encodeURIComponent(target)+'&name='+encodeURIComponent(f.name));"
    " xhr.upload.onprogress=e=>{if(e.lengthComputable)onp(e.loaded,e.total);};"
    " xhr.onload=()=>resolve({ok:xhr.status>=200&&xhr.status<300,text:xhr.responseText,status:xhr.status});"
    " xhr.onerror=()=>resolve({ok:false,text:'Network error',status:0});"
    " xhr.send(f);"
    "});}"
    "async function upload(){"
    " const files=[...document.getElementById('files').files];"
    " const target=document.getElementById('target').value;"
    " const bar=document.getElementById('bar');"
    " if(!files.length){log('Choose at least one file.');return;}"
    " let out='';"
    " for(let i=0;i<files.length;i++){"
    "  const f=files[i]; bar.value=Math.round(i*100/files.length);"
    "  out+='Uploading '+f.name+'...\\n'; log(out);"
    "  const r=await sendFile(target,f,(loaded,total)=>{"
    "   const filePct=total?loaded/total:0;"
    "   bar.value=Math.round(((i+filePct)*100)/files.length);"
    "   log(out+'Progress '+Math.round(filePct*100)+'% ('+loaded+'/'+total+')');"
    "  });"
    "  out+=(r.ok?'OK: ':'ERR '+r.status+': ')+r.text+'\\n'; log(out);"
    " }"
    " bar.value=100;"
    "}"
    "</script></body></html>";

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

static std::string trim_copy(const std::string &s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start])) start++;
    size_t end = s.size();
    while (end > start && std::isspace((unsigned char)s[end - 1])) end--;
    return s.substr(start, end - start);
}

static bool load_wifi_config(std::string *ssid, std::string *password)
{
    if (!ssid || !password) return false;
    FILE *fp = std::fopen(WIFI_CONFIG_PATH, "rb");
    if (!fp) return false;

    std::string first_line;
    std::string second_line;
    char line[256];
    while (std::fgets(line, sizeof(line), fp)) {
        std::string s = trim_copy(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq != std::string::npos) {
            std::string key = trim_copy(s.substr(0, eq));
            std::string val = trim_copy(s.substr(eq + 1));
            if (key == "ssid") *ssid = val;
            else if (key == "password" || key == "pass") *password = val;
        } else if (first_line.empty()) {
            first_line = s;
        } else if (second_line.empty()) {
            second_line = s;
        }
    }
    std::fclose(fp);

    if (ssid->empty() && !first_line.empty()) *ssid = first_line;
    if (password->empty() && !second_line.empty()) *password = second_line;
    return !ssid->empty();
}

static const char *target_dir(const std::string &target)
{
    if (target == "fonts") return "/sdcard/fonts";
    if (target == "images") return "/sdcard/images";
    return "/sdcard/books";
}

static bool target_accepts(const std::string &target, const std::string &name)
{
    if (target == "fonts") {
        return has_ext_ci(name, ".ttf") || has_ext_ci(name, ".otf") || has_ext_ci(name, ".bin");
    }
    if (target == "images") {
        return has_ext_ci(name, ".bmp") || has_ext_ci(name, ".png") ||
               has_ext_ci(name, ".jpg") || has_ext_ci(name, ".jpeg");
    }
    return has_ext_ci(name, ".txt") || has_ext_ci(name, ".epub");
}

class WifiTransferActivity : public Activity {
public:
    ~WifiTransferActivity() override { stop(); }

    void on_create() override
    {
        _starting = true;
        draw();
        _running = start();
        _starting = false;
        vTaskDelay(pdMS_TO_TICKS(200));
        draw();
    }

    void on_hide() override { stop(); }

    void loop() override
    {
        if (_dirty) {
            _dirty = false;
            draw();
        }
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
        std::snprintf(_last_name, sizeof(_last_name), "%s", name.c_str());
        _last_bytes = bytes;
        _last_ok = ok;
        _dirty = true;
    }

private:
    bool _running = false;
    httpd_handle_t _server = nullptr;
    esp_netif_t *_wifi_netif = nullptr;
    bool _wifi_inited = false;
    bool _wifi_started = false;
    bool _ap_mode = true;
    std::string _network_name;
    char _ip[24] = "192.168.4.1";
    char _last_name[96] = "No uploads yet";
    size_t _last_bytes = 0;
    bool _last_ok = true;
    bool _starting = false;
    volatile bool _dirty = false;
    std::string _error_message;

    bool init_net()
    {
        esp_err_t ret = wifi_setting_deinit(false);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "wifi cleanup before transfer failed: %s", esp_err_to_name(ret));
        }

        ret = wifi_setting_c6_power_on();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "C6 power on failed: %s", esp_err_to_name(ret));
            return false;
        }

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
            _error_message = "SD card not mounted";
            ESP_LOGE(TAG, "sdcard mount point missing: errno=%d (%s)", errno, strerror(errno));
            return false;
        }

        if (!ensure_dir_logged("/sdcard/books") ||
            !ensure_dir_logged("/sdcard/images") ||
            !ensure_dir_logged("/sdcard/fonts")) {
            _error_message = "Create SD dirs failed";
            ESP_LOGE(TAG, "failed to create transfer dirs");
            return false;
        }

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.static_rx_buf_num = 8;
        cfg.dynamic_rx_buf_num = 32;
        cfg.tx_buf_type = 1;
        cfg.static_tx_buf_num = 0;
        cfg.dynamic_tx_buf_num = 32;
        cfg.cache_tx_buf_num = 16;
        cfg.csi_enable = 0;
        cfg.ampdu_rx_enable = 1;
        cfg.ampdu_tx_enable = 1;
        cfg.rx_ba_win = 6;
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

        if (try_sta(DEFAULT_STA_SSID, DEFAULT_STA_PASSWORD, "default")) {
            return start_http_server();
        }

        std::string router_ssid;
        std::string router_pass;
        if (load_wifi_config(&router_ssid, &router_pass) &&
            (router_ssid != DEFAULT_STA_SSID || router_pass != DEFAULT_STA_PASSWORD)) {
            if (try_sta(router_ssid, router_pass, WIFI_CONFIG_PATH)) {
                return start_http_server();
            }
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

        wifi_config_t sta_cfg = {};
        std::snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%s", ssid.c_str());
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
        http_cfg.max_uri_handlers = 4;
        http_cfg.stack_size = 8192;
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

        ESP_LOGI(TAG, "http server started url=http://%s/", _ip);
        return true;
    }

    void stop()
    {
        if (_server) {
            httpd_stop(_server);
            _server = nullptr;
        }
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
    }

    void draw()
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);

        int x = 24;
        int y = 12;
        int line_h = system_font::line_height() + 4;
        auto draw_line = [&](const std::string &text) {
            system_font::draw_left(M5.Display, system_font::ellipsize(text, M5.Display.width() - x * 2), x, y);
            y += line_h;
        };
        system_font::draw_center(M5.Display, "WiFi Transfer", M5.Display.width() / 2, y);
        y += line_h + 10;

        if (_starting) {
            draw_line("Starting WiFi...");
            draw_line("Please wait.");
            M5.Display.display();
            M5.Display.endWrite();
            return;
        }

        if (_running) {
            if (_ap_mode) {
                draw_line("Mode: AP hotspot");
                draw_line("Connect WiFi:");
                draw_line(_network_name);
                draw_line("Password: 12345678");
            } else {
                draw_line("Mode: Router WiFi");
                draw_line("Connected:");
                std::string show = _network_name;
                if (show.size() > 20) show = show.substr(0, 17) + "...";
                draw_line(show);
            }
            y += 8;
            draw_line("Open browser:");
            std::string url = std::string("http://") + _ip;
            draw_line(url);
            y += 8;
            draw_line("Targets: books images fonts");
            y += 8;
            draw_line(_last_ok ? "Last upload:" : "Upload error:");
            std::string last = _last_name;
            if (last.size() > 20) last = last.substr(0, 17) + "...";
            draw_line(last);
            char bytes[48];
            std::snprintf(bytes, sizeof(bytes), "%u bytes", (unsigned)_last_bytes);
            draw_line(bytes);
            system_font::draw_left(M5.Display, "BACK / OK: stop", x, M5.Display.height() - system_font::line_height() - 4);
        } else {
            draw_line(_error_message.empty() ? "Failed to start" : _error_message);
            draw_line("WiFi server.");
            y += line_h + 10;
            draw_line("BACK / OK: return");
        }
        M5.Display.display();
        M5.Display.endWrite();
    }

    static esp_err_t handle_index(httpd_req_t *req)
    {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    }

    static esp_err_t handle_upload(httpd_req_t *req)
    {
        WifiTransferActivity *self = (WifiTransferActivity *)req->user_ctx;
        char query[512] = {};
        std::string target = "books";
        std::string name = "upload.bin";
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char value[384] = {};
            if (httpd_query_key_value(query, "target", value, sizeof(value)) == ESP_OK) target = url_decode(value);
            if (httpd_query_key_value(query, "name", value, sizeof(value)) == ESP_OK) name = url_decode(value);
        }

        name = safe_filename(name);
        ESP_LOGI(TAG, "upload begin target=%s name=%s content_len=%d",
                 target.c_str(),
                 name.c_str(),
                 req->content_len);
        if (!target_accepts(target, name)) {
            ESP_LOGW(TAG, "upload rejected target=%s name=%s", target.c_str(), name.c_str());
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported file extension for target");
            if (self) self->record_upload(name, 0, false);
            return ESP_FAIL;
        }

        const char *dir = target_dir(target);
        ensure_dir(dir);
        std::string path = std::string(dir) + "/" + name;
        unlink(path.c_str());
        int64_t prealloc_t0 = esp_timer_get_time();
        esp_err_t prealloc_result = req->content_len > 0
                                      ? esp_vfs_fat_create_contiguous_file(STORAGE_SDCARD_MOUNT_POINT,
                                                                           path.c_str(),
                                                                           (uint64_t)req->content_len,
                                                                           true)
                                      : ESP_ERR_INVALID_SIZE;
        int64_t prealloc_us = esp_timer_get_time() - prealloc_t0;
        FILE *fp = std::fopen(path.c_str(), prealloc_result == ESP_OK ? "r+b" : "wb");
        if (!fp) {
            ESP_LOGE(TAG, "upload open failed path=%s errno=%d", path.c_str(), errno);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
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
        if (!file_buf || !file_buf_dma_compatible) {
            ESP_LOGE(TAG, "failed to allocate DMA-compatible SD write buffer");
            std::fclose(fp);
            if (file_buf) free(file_buf);
            unlink(path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD DMA buffer allocation failed");
            if (self) self->record_upload(name, 0, false);
            return ESP_FAIL;
        }
        if (setvbuf(fp, file_buf, _IOFBF, FILE_WRITE_BUFFER_BYTES) != 0) {
            ESP_LOGE(TAG, "SD stdio buffer setup failed");
            std::fclose(fp);
            free(file_buf);
            unlink(path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD write buffer setup failed");
            if (self) self->record_upload(name, 0, false);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "SD write buffer ptr=%p requested=%u allocated=%u dma_align=%u dma_ok=%d memory=%s",
                 file_buf,
                 (unsigned)FILE_WRITE_BUFFER_BYTES,
                 (unsigned)file_buf_allocated,
                 (unsigned)file_buf_dma_alignment,
                 file_buf_dma_compatible ? 1 : 0,
                 file_buf_in_psram ? "PSRAM" : "internal");

        char *buf = (char *)heap_caps_malloc(UPLOAD_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            buf = (char *)heap_caps_malloc(UPLOAD_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!buf) {
            std::fclose(fp);
            if (file_buf) free(file_buf);
            unlink(path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload buffer allocation failed");
            if (self) self->record_upload(name, 0, false);
            return ESP_FAIL;
        }

        int remaining = req->content_len;
        size_t written = 0;
        size_t next_log = UPLOAD_LOG_INTERVAL_BYTES;
        int64_t t0 = esp_timer_get_time();
        int64_t last_log_time = t0;
        size_t last_log_written = 0;
        int64_t recv_us = 0;
        int64_t write_us = 0;
        int64_t max_recv_us = 0;
        int64_t max_write_us = 0;
        uint32_t recv_calls = 0;
        uint32_t write_calls = 0;
        uint32_t recv_timeouts = 0;
        while (remaining > 0) {
            int want = std::min<int>(remaining, UPLOAD_BUFFER_BYTES);
            int64_t recv_t0 = esp_timer_get_time();
            int got = httpd_req_recv(req, buf, want);
            int64_t recv_elapsed = esp_timer_get_time() - recv_t0;
            recv_us += recv_elapsed;
            max_recv_us = std::max(max_recv_us, recv_elapsed);
            recv_calls++;
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                recv_timeouts++;
                continue;
            }
            if (got <= 0) {
                heap_caps_free(buf);
                std::fclose(fp);
                if (file_buf) free(file_buf);
                unlink(path.c_str());
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload receive failed");
                if (self) self->record_upload(name, written, false);
                return ESP_FAIL;
            }
            int64_t write_t0 = esp_timer_get_time();
            size_t n = std::fwrite(buf, 1, got, fp);
            int64_t write_elapsed = esp_timer_get_time() - write_t0;
            write_us += write_elapsed;
            max_write_us = std::max(max_write_us, write_elapsed);
            write_calls++;
            if (n != (size_t)got) {
                heap_caps_free(buf);
                std::fclose(fp);
                if (file_buf) free(file_buf);
                unlink(path.c_str());
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD write failed");
                if (self) self->record_upload(name, written, false);
                return ESP_FAIL;
            }
            written += n;
            remaining -= got;
            if (written >= next_log || remaining == 0) {
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
        if (file_buf) free(file_buf);
        if (close_result != 0) {
            ESP_LOGE(TAG, "upload final flush failed path=%s errno=%d (%s)", path.c_str(), errno, strerror(errno));
            unlink(path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD final flush failed");
            if (self) self->record_upload(name, written, false);
            return ESP_FAIL;
        }

        char msg[256];
        std::snprintf(msg, sizeof(msg), "Saved %s (%u bytes)", path.c_str(), (unsigned)written);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, msg);
        if (self) self->record_upload(name, written, true);
        int64_t elapsed_us = esp_timer_get_time() - t0;
        uint32_t speed_kb = transfer_speed_kb(written, elapsed_us);
        ESP_LOGI(TAG, "upload done path=%s bytes=%u time=%lld ms avg=%u KB/s recv=%lld ms write=%lld ms close=%lld ms recv_calls=%u write_calls=%u timeouts=%u",
                 path.c_str(),
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
    ActivityManager::instance().push(new WifiTransferActivity());
}
