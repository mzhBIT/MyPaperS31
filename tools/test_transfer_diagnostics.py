"""Generate native SDK-stub tests of the actual transfer diagnostics header.

Usage: python tools/test_transfer_diagnostics.py output-directory
Compile output-directory/test.cpp with that directory and main as include paths.
"""
from pathlib import Path
import sys

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
stub = r'''
#pragma once
#include <cstdint>
#include <cstddef>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ERR_OK=0;
inline const char* esp_err_to_name(int value) { return value==0?"ESP_OK":"ESP_FAIL"; }
inline int64_t clock_us=1000000;
inline int64_t esp_timer_get_time() { return clock_us; }
constexpr int MALLOC_CAP_INTERNAL=1;
inline size_t heap_caps_get_free_size(int) { return 120000; }
inline size_t heap_caps_get_largest_free_block(int) { return 60000; }
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
using wifi_phy_mode_t=int;
using wifi_ps_type_t=int;
constexpr int WIFI_PHY_MODE_HT20=1,WIFI_PHY_MODE_HT40=2,WIFI_PHY_MODE_HE20=3,
 WIFI_PHY_MODE_11G=4,WIFI_PHY_MODE_11B=5,ESP_WIFI_ACI_BE=2;
inline int esp_wifi_sta_get_negotiated_phymode(int* p) { *p=WIFI_PHY_MODE_HT20;return 0; }
inline int esp_wifi_get_ps(int* p) { *p=0;return 0; }
enum {TEST_TX_WAIT_ACK=5,TEST_TX_WAIT_CTS=2,TEST_TX_FAIL_MAX=6,TEST_TX_WAIT_TIMEOUT=3,
 TEST_TX_WAIT_MAX=4,TEST_TX_FAIL_ERROR_MAX=10,TEST_TX_WAIT_MATCH=0};
using wifi_second_chan_t=int;
constexpr int WIFI_SECOND_CHAN_NONE=0;
struct wifi_ap_record_t { int8_t rssi=-33;uint8_t bssid[6]{1,2,3,4,5,6}; };
struct wifi_sta_list_t { int num=1;struct { int8_t rssi=-45; } sta[2]; };
struct esp_test_hw_rx_statistics_t {
 uint16_t rx_fcs_err=0,rx_abort=0,rx_samebm_errcnt=0,rx_mpdu=0,rx_datasuc=0,
 rx_buf_fullcnt=0,rx_fifo_ovfcnt=0,rx_abort_fcs_pass=0,nrx_htsig_err=0,nrx_hesiga_crc=0,brx_err_agc=0,nrx_err_restart=0;
 int16_t rx_cfo_hz=0;
};
struct esp_test_tx_statistics_t { uint32_t tx_enable=0,tx_succ=0,retry_edca=0,retry_tb=0,tx_max_rtt=0; };
struct esp_test_tx_fail_statistics_t { uint32_t match[4][10]{},count=0; };
struct esp_test_rx_statistics_t {
 uint32_t ht=0,ht_stbc=0,legacy_noeb=0,ht_noeb=0,su_noeb=0,ht_retry=0,su_retry=0,
 ersu_noeb=0,mu_noeb=0,rx_base_null_cnt=0,rx_reset_rxbase_cnt=0;
};
struct esp_test_rx_error_occurs_t { uint32_t tot=0; };
inline bool mock_failure=false, reject_callback=false;
inline int8_t mock_power=80;
inline int esp_wifi_get_max_tx_power(int8_t* value) { *value=mock_power;return mock_failure?-1:0; }
inline int esp_wifi_get_channel(uint8_t* primary,int* secondary) { *primary=6;*secondary=0;return mock_failure?-1:0; }
inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap) { *ap={};return mock_failure?-1:0; }
inline int esp_wifi_ap_get_sta_list(wifi_sta_list_t* clients) { *clients={};return mock_failure?-1:0; }
inline esp_test_hw_rx_statistics_t hardware;
inline int esp_wifi_enable_rx_statistics(bool,bool) { return mock_failure?-1:0; }
inline int esp_wifi_enable_tx_statistics(int,bool) { return mock_failure?-1:0; }
inline int esp_wifi_clr_rx_statistics(int) { return mock_failure?-1:0; }
inline int esp_wifi_clr_tx_statistics(int) { return mock_failure?-1:0; }
inline int esp_test_clr_rx_error_occurs() { return 0; }
inline void esp_test_clr_hw_statistics() { hardware={}; }
inline int esp_test_get_hw_rx_statistics(esp_test_hw_rx_statistics_t* p) { *p=hardware;return mock_failure?-1:0; }
inline int esp_test_get_rx_error_occurs(esp_test_rx_error_occurs_t* p) { p->tot=12;return mock_failure?-1:0; }
inline int esp_wifi_get_tx_statistics(int,esp_test_tx_statistics_t* p,esp_test_tx_fail_statistics_t* f) {
 p->retry_edca=7;f[TEST_TX_WAIT_ACK].match[TEST_TX_WAIT_TIMEOUT][0]=9;
 f[TEST_TX_WAIT_ACK].match[TEST_TX_WAIT_MATCH][1]=2;f[TEST_TX_WAIT_ACK].count=11;return mock_failure?-1:0;
}
inline int esp_wifi_get_rx_statistics(int,esp_test_rx_statistics_t* p) { p->ht=5;p->ht_stbc=2;return mock_failure?-1:0; }
inline void (*queued)(void*)=nullptr;
inline void* queued_context=nullptr;
inline int tcpip_try_callback(void (*fn)(void*),void* ctx) {
 if(reject_callback)return -1;queued=fn;queued_context=ctx;return 0;
}
inline void run_callback() {
 auto fn=queued;queued=nullptr;if(fn)fn(queued_context);
}
'''
(out / 'test_sdk.h').write_text(stub)
(out / 'sdkconfig.h').write_text('#pragma once\n#define CONFIG_IDF_TARGET_ESP32S31 1\n')
for name in ['esp_timer.h', 'esp_heap_caps.h', 'esp_wifi.h', 'esp_wifi_he.h',
             'esp_private/esp_wifi_he_private.h', 'freertos/FreeRTOS.h', 'lwip/tcpip.h']:
    path = out / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#pragma once\n#include "test_sdk.h"\n')
(out / 'test.cpp').write_text(r'''
#include "wifi/TransferDiagnostics.h"
#include <cassert>
#include <iostream>
int main() {
 TransferDiagnostics d;
 {
  TransferDiagnostics::Run r(d,"quote\"\\\n","upload","b/g/n",true,true,true);
  clock_us+=500000; hardware.rx_fcs_err=65530;
  r.network(50000,100);run_callback();
  clock_us+=500000;hardware.rx_fcs_err=4;
  r.network(70000,200,true);
  clock_us+=25000;run_callback();
  r.storage(2000);r.flush(3000);r.finish(true);
 }
 assert(d.generation()==1);
 std::cout<<d.json()<<"\n";
 // A pending old callback is harmless after its activity/report is destroyed.
 {
  TransferDiagnostics old;
  TransferDiagnostics::Run r(old,"old","upload","b/g",true,true,false);
 }
 // Start a new epoch before that old callback returns. It must not enter the
 // new epoch's counts; rejected queue submissions must not remain pending.
 {
  mock_power=56;
  TransferDiagnostics::Run r(d,"new","download","b/g/n",true,false,false,56);
  clock_us+=900000;run_callback();
  reject_callback=true;clock_us+=500000;r.network(1000,10);
  reject_callback=false;clock_us+=500000;r.network(2000,20);
  clock_us+=9000;run_callback();
  mock_failure=true;r.finish(false);mock_failure=false;
 }
 assert(d.generation()==2);
 std::cout<<d.json()<<"\n";
 run_callback();
}
''', encoding='utf-8')
