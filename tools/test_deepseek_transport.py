"""Prepare native regression checks for the production DeepSeek request flow."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv)>1 else '.codex-build-cache/deepseek-transport')
out.mkdir(parents=True, exist_ok=True)
source = (root/'main/deepseek/DeepSeekClient.cpp').read_text(encoding='utf-8')
source = re.sub(r'^#include <(?:esp_[^>]+|freertos/[^>]+)>\n', '', source, flags=re.M)
source = source.replace('#include "wifi/wifi_setting.h"', '')
config = (root/'main/deepseek/DeepSeekConfig.cpp').read_text(encoding='utf-8')
begin = config.index('const char *deepseek_model_id(')
end = config.index('\n}', begin)+2
model_id = config[begin:end]
stubs = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=1,ESP_ERR_HTTP_CONNECT=2,ESP_ERR_HTTP_WRITE_DATA=3;
constexpr int CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS=1000;
#define CONFIG_IDF_TARGET_ESP32S31 1
#define CONFIG_MBEDTLS_SSL_ALPN 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
constexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_SPIRAM=2;
size_t heap_caps_get_free_size(int){return 200000;}
size_t heap_caps_get_largest_free_block(int){return 64000;}
int64_t clock_us=0;
int64_t esp_timer_get_time(){return ++clock_us;}
void vTaskDelay(int ms){clock_us+=ms*1000;}
const char* esp_err_to_name(int e){return e==0?"ESP_OK":"ESP_FAIL";}
enum wifi_ps_type_t {WIFI_PS_NONE=0,WIFI_PS_MIN_MODEM=1};
wifi_ps_type_t power_save=WIFI_PS_MIN_MODEM;
struct wifi_ap_record_t {int rssi=-35;};
int esp_wifi_get_ps(wifi_ps_type_t* out){*out=power_save;return 0;}
int esp_wifi_set_ps(wifi_ps_type_t ps){power_save=ps;return 0;}
int esp_wifi_sta_get_ap_info(wifi_ap_record_t*){return 0;}
int wifi_setting_connect_saved(int){return 0;}
int wifi_setting_release(){return 0;}
bool wifi_setting_is_connected(){return true;}
void esp_crt_bundle_attach(){}
enum esp_http_client_method_t {HTTP_METHOD_GET,HTTP_METHOD_POST};
constexpr int HTTP_ADDR_TYPE_INET=1,ESP_HTTP_CLIENT_TLS_VER_TLS_1_2=12,HTTP_EVENT_ON_CONNECTED=10;
struct Client;
using esp_http_client_handle_t=Client*;
struct esp_http_client_event_t {int event_id;Client* client;void* user_data;};
struct esp_http_client_config_t {
    const char* url=nullptr;esp_http_client_method_t method=HTTP_METHOD_GET;
    int timeout_ms=0;void(*crt_bundle_attach)()=nullptr;
    const char* user_agent=nullptr;int addr_type=0,tls_version=0;
    const char** alpn_protos=nullptr;int buffer_size=0,buffer_size_tx=0;
    int(*event_handler)(esp_http_client_event_t*)=nullptr;void* user_data=nullptr;
};
enum Scenario {Normal,OneEof,TrustFailure,PersistentEof,CancelAfterEof,BodyFailure,HeaderFailure,ReadFailure,TruncatedModels,Unauthorized};
Scenario scenario=Normal;
int opens=0,closes=0,writes=0;
volatile bool* cancel_flag=nullptr;
std::string models=R"({"data":[{"id":"deepseek-flash"},{"id":"deepseek-v4-pro"}]})";
std::vector<std::string> posted;
struct Client {esp_http_client_config_t config;size_t offset=0;int tls=0,flags=0;std::string body;};
Client* esp_http_client_init(const esp_http_client_config_t* cfg){
    assert(cfg->addr_type==HTTP_ADDR_TYPE_INET&&cfg->tls_version==ESP_HTTP_CLIENT_TLS_VER_TLS_1_2);
    assert(cfg->crt_bundle_attach&&cfg->alpn_protos&&!strcmp(cfg->alpn_protos[0],"http/1.1"));
    assert(power_save==WIFI_PS_NONE);
    auto c=new Client{*cfg};
    c->body=cfg->method==HTTP_METHOD_GET?models:"data: {\"choices\":[{\"delta\":{\"content\":\"OK\"}}]}\n\ndata: [DONE]\n";
    if(scenario==Unauthorized)c->body=R"({"error":{"message":"bad API key"}})";
    return c;
}
int esp_http_client_set_header(Client*,const char*,const char*){return 0;}
int esp_http_client_open(Client* c,int){
    ++opens;
    if(scenario==TrustFailure){c->tls=-0x2700;c->flags=1;return ESP_ERR_HTTP_CONNECT;}
    if(scenario==PersistentEof||scenario==CancelAfterEof||(scenario==OneEof&&opens==1)){
        c->tls=-0x7280;return ESP_ERR_HTTP_CONNECT;
    }
    esp_http_client_event_t event{HTTP_EVENT_ON_CONNECTED,c,c->config.user_data};
    c->config.event_handler(&event);return 0;
}
int esp_http_client_get_errno(Client*){return 0;}
int esp_http_client_get_and_clear_last_tls_error(Client* c,int* tls,int* flags){*tls=c->tls;*flags=c->flags;return c->tls?5:0;}
int esp_http_client_write(Client*,const char* bytes,int n){++writes;if(scenario==BodyFailure)return -1;posted.emplace_back(bytes,n);return n;}
int esp_http_client_fetch_headers(Client*){return scenario==HeaderFailure?-1:0;}
int esp_http_client_get_status_code(Client*){return scenario==Unauthorized?401:200;}
int esp_http_client_read(Client* c,char* out,int size){
    if(scenario==ReadFailure)return -1;
    size_t n=std::min(size_t(size),c->body.size()-c->offset);
    memcpy(out,c->body.data()+c->offset,n);c->offset+=n;return int(n);
}
bool esp_http_client_is_complete_data_received(Client* c){return scenario!=TruncatedModels&&scenario!=ReadFailure&&c->offset==c->body.size();}
int esp_http_client_close(Client*){return 0;}
int esp_http_client_cleanup(Client* c){
    ++closes;delete c;if(scenario==CancelAfterEof&&cancel_flag)*cancel_flag=true;return 0;
}
'''
tests = r'''
void reset(Scenario s){scenario=s;opens=closes=writes=0;posted.clear();power_save=WIFI_PS_MIN_MODEM;cancel_flag=nullptr;}
DeepSeekClientResult chat(const volatile bool* cancel=nullptr){
    return DeepSeekClient::chat({"not-a-real-key",DeepSeekModel::Flash},DeepSeekModel::Flash,false,{{"user","hello",{}}},cancel);
}
int main(){
    DeepSeekConfig config{"not-a-real-key",DeepSeekModel::Flash};
    reset(Normal);auto r=DeepSeekClient::validate(config);assert(r.success&&opens==1&&closes==1&&power_save==WIFI_PS_MIN_MODEM);
    models=R"({"data":[{"id":"deepseek-v4-flash"},{"id":"deepseek-v4-pro"}]})";
    reset(Normal);assert(DeepSeekClient::validate(config).success);
    models=R"({"data":[{"id":"deepseek-flash"},{"id":"deepseek-v4-pro"}]})";
    reset(OneEof);r=DeepSeekClient::validate(config);assert(r.success&&r.error.empty()&&opens==2&&closes==2);
    reset(TrustFailure);r=chat();assert(!r.success&&opens==1&&writes==0&&closes==1);
    reset(PersistentEof);r=chat();assert(!r.success&&opens==2&&writes==0&&closes==2);
    reset(OneEof);r=chat();assert(r.success&&r.content=="OK"&&r.error.empty()&&opens==2&&writes==1&&closes==2);
    assert(posted[0].find("\"model\":\"deepseek-flash\"")!=std::string::npos);
    reset(BodyFailure);r=chat();assert(!r.success&&opens==1&&writes==1&&closes==1);
    reset(HeaderFailure);r=chat();assert(!r.success&&opens==1&&writes==1&&closes==1);
    reset(ReadFailure);r=chat();assert(!r.success&&opens==1&&writes==1&&closes==1);
    reset(TruncatedModels);r=DeepSeekClient::validate(config);assert(!r.success&&r.error=="DeepSeek response was interrupted");
    reset(Unauthorized);r=DeepSeekClient::validate(config);assert(!r.success&&r.http_status==401&&r.error=="bad API key"&&opens==1);
    volatile bool cancel=false;reset(CancelAfterEof);cancel_flag=&cancel;r=chat(&cancel);
    assert(r.canceled&&!r.success&&opens==1&&writes==0&&closes==1);
    assert(power_save==WIFI_PS_MIN_MODEM);
    cancel=true;reset(Normal);r=chat(&cancel);assert(r.canceled&&opens==0);
    puts("DeepSeek production transport PASS: new/old models, one pre-body reconnect, no certificate retries or chat replay, cancel, truncated models, HTTP auth and Wi-Fi power restoration");
}
'''
(out/'test.cpp').write_text(stubs+'\n'+source+'\n'+model_id+'\n'+tests, encoding='utf-8')
print(f'Native DeepSeek transport check ready: {out / "test.cpp"}')
