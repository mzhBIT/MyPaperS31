#pragma once
#include <sdkconfig.h>
#include <string>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#if CONFIG_IDF_TARGET_ESP32S31
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_wifi_he.h>
#include <esp_private/esp_wifi_he_private.h>
#include <freertos/FreeRTOS.h>
#include <lwip/tcpip.h>

namespace transfer_diag {
// Static lifetime: a callback can safely finish after an HTTP request or the
// activity has gone away. Epochs keep an old callback out of a new measurement.
struct Probe {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    bool active=false, pending=false;
    uint32_t epoch=0, sent_epoch=0, count=0, rejected=0;
    int64_t sent=0, total=0, maximum=0;
    void begin() {
        portENTER_CRITICAL(&mux);
        ++epoch; active=true; count=rejected=0; total=maximum=0;
        portEXIT_CRITICAL(&mux);
    }
    static void callback(void* ctx) {
        auto& p=*static_cast<Probe*>(ctx);
        const int64_t now=esp_timer_get_time();
        portENTER_CRITICAL(&p.mux);
        if (p.active && p.sent_epoch==p.epoch) {
            const int64_t elapsed=now-p.sent;
            ++p.count; p.total+=elapsed; p.maximum=std::max(p.maximum,elapsed);
        }
        p.pending=false;
        portEXIT_CRITICAL(&p.mux);
    }
    void submit() {
        const int64_t now=esp_timer_get_time();
        portENTER_CRITICAL(&mux);
        if (!active || pending) { portEXIT_CRITICAL(&mux); return; }
        pending=true; sent=now; sent_epoch=epoch;
        portEXIT_CRITICAL(&mux);
        if (tcpip_try_callback(callback,this)!=ERR_OK) {
            portENTER_CRITICAL(&mux);
            pending=false; ++rejected;
            portEXIT_CRITICAL(&mux);
        }
    }
    void stop() {
        portENTER_CRITICAL(&mux); active=false; portEXIT_CRITICAL(&mux);
    }
};
inline Probe probe;
inline std::string quote(const std::string& s) {
    std::string out="\"";
    for (unsigned char c:s) {
        if (c=='"' || c=='\\') { out+='\\'; out+=char(c); }
        else if (c<32) { char v[7]; std::snprintf(v,sizeof(v),"\\u%04x",unsigned(c)); out+=v; }
        else out+=char(c);
    }
    return out+'"';
}
}
#endif

class TransferDiagnostics {
public:
    struct Run {
        TransferDiagnostics& owner;
        Run(TransferDiagnostics& d, const std::string& name, const char* direction,
            const char* protocol, bool stbc, bool ampdu, bool ap, int requested_power=-1) : owner(d) {
            owner.begin(name,direction,protocol,stbc,ampdu,ap,requested_power);
        }
        ~Run() { owner.finish(false); }
        void network(int64_t us, size_t bytes, bool timeout=false) { owner.network(us,bytes,timeout); }
        void storage(int64_t us) { owner.storage(us); }
        void flush(int64_t us) { owner.flush(us); }
        void finish(bool ok) { owner.finish(ok); }
        Run(const Run&)=delete;
        Run& operator=(const Run&)=delete;
    };
    unsigned generation() const { return generation_.load(); }
    std::string json() const { std::lock_guard<std::mutex> lock(mutex_); return report_; }
    std::string summary() const { std::lock_guard<std::mutex> lock(mutex_); return summary_; }
private:
    mutable std::mutex mutex_;
    std::atomic<unsigned> generation_{0};
    std::string report_="{\"available\":false,\"reason\":\"no completed transfer\"}";
    std::string summary_="--";
#if CONFIG_IDF_TARGET_ESP32S31
    bool active_=false;
    int64_t begin_=0, net_=0, io_=0, flush_=0, net_max_=0, io_max_=0, sampled_=0, gap_max_=0;
    uint64_t bytes_=0, calls_=0, timeouts_=0, hw_samples_=0, hw_failures_=0;
    esp_err_t rx_enable_=ESP_FAIL, tx_enable_=ESP_FAIL, hw_status_=ESP_FAIL;
    esp_err_t tx_clear_=ESP_FAIL, rx_clear_=ESP_FAIL;
    esp_err_t error_clear_=ESP_FAIL;
    esp_test_hw_rx_statistics_t previous_{};
    uint64_t fcs_=0, abort_=0, samebm_=0, mpdu_=0, data_=0, full_=0, fifo_=0;
    uint64_t abort_good_fcs_=0, ht_sig_=0, he_sig_=0, agc_=0, restart_=0;
    std::string name_, direction_, protocol_;
    bool stbc_=true, ampdu_=true, ap_=false;
    int requested_power_=-1;
    struct Link {
        esp_err_t ap_status=ESP_FAIL, peers_status=ESP_FAIL, channel_status=ESP_FAIL, power_status=ESP_FAIL;
        wifi_ap_record_t ap{};
        wifi_sta_list_t peers{};
        uint8_t channel=0;
        wifi_second_chan_t secondary=WIFI_SECOND_CHAN_NONE;
        int8_t power=0;
    } link_start_,link_end_;
    void capture_link(Link& link) {
        link={};
        if (ap_) link.peers_status=esp_wifi_ap_get_sta_list(&link.peers);
        else link.ap_status=esp_wifi_sta_get_ap_info(&link.ap);
        link.channel_status=esp_wifi_get_channel(&link.channel,&link.secondary);
        link.power_status=esp_wifi_get_max_tx_power(&link.power);
    }
    uint32_t internal_=0, largest_=0;
    void memory() {
        internal_=std::min(internal_, uint32_t(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        largest_=std::min(largest_, uint32_t(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    }
    void sample(bool force=false) {
        const int64_t now=esp_timer_get_time();
        if (!force && now-sampled_<500000) return;
        if (sampled_) gap_max_=std::max(gap_max_,now-sampled_);
        sampled_=now;
        esp_test_hw_rx_statistics_t hw{};
        hw_status_=esp_test_get_hw_rx_statistics(&hw);
        if (hw_status_==ESP_OK) {
            auto add=[](uint64_t& total, uint16_t value, uint16_t old) { total+=uint16_t(value-old); };
            add(fcs_,hw.rx_fcs_err,previous_.rx_fcs_err);
            add(abort_,hw.rx_abort,previous_.rx_abort);
            add(samebm_,hw.rx_samebm_errcnt,previous_.rx_samebm_errcnt);
            add(mpdu_,hw.rx_mpdu,previous_.rx_mpdu);
            add(data_,hw.rx_datasuc,previous_.rx_datasuc);
            add(full_,hw.rx_buf_fullcnt,previous_.rx_buf_fullcnt);
            add(fifo_,hw.rx_fifo_ovfcnt,previous_.rx_fifo_ovfcnt);
            add(abort_good_fcs_,hw.rx_abort_fcs_pass,previous_.rx_abort_fcs_pass);
            add(ht_sig_,hw.nrx_htsig_err,previous_.nrx_htsig_err);
            add(he_sig_,hw.nrx_hesiga_crc,previous_.nrx_hesiga_crc);
            add(agc_,hw.brx_err_agc,previous_.brx_err_agc);
            add(restart_,hw.nrx_err_restart,previous_.nrx_err_restart);
            previous_=hw; ++hw_samples_;
        } else ++hw_failures_;
        memory();
        transfer_diag::probe.submit();
    }
    void begin(const std::string& name,const char* direction,const char* protocol,bool stbc,bool ampdu,bool ap,int requested_power) {
        active_=true;
        name_=name; direction_=direction; protocol_=protocol; stbc_=stbc; ampdu_=ampdu; ap_=ap;
        requested_power_=requested_power;
        capture_link(link_start_);
        net_=io_=flush_=net_max_=io_max_=sampled_=gap_max_=0;
        bytes_=calls_=timeouts_=hw_samples_=hw_failures_=fcs_=abort_=samebm_=mpdu_=data_=full_=fifo_=0;
        abort_good_fcs_=ht_sig_=he_sig_=agc_=restart_=0;
        previous_={};
        internal_=uint32_t(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        largest_=uint32_t(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        rx_enable_=esp_wifi_enable_rx_statistics(true,false);
        tx_enable_=esp_wifi_enable_tx_statistics(ESP_WIFI_ACI_BE,true);
        rx_clear_=esp_wifi_clr_rx_statistics(8);
        tx_clear_=esp_wifi_clr_tx_statistics(ESP_WIFI_ACI_BE);
        error_clear_=esp_test_clr_rx_error_occurs();
        esp_test_clr_hw_statistics();
        transfer_diag::probe.begin();
        begin_=esp_timer_get_time();
        sample(true);
    }
    void network(int64_t us,size_t bytes,bool timeout) {
        net_+=us; net_max_=std::max(net_max_,us); bytes_+=bytes; ++calls_; timeouts_+=timeout;
        sample();
    }
    void storage(int64_t us) { io_+=us; io_max_=std::max(io_max_,us); }
    void flush(int64_t us) { flush_+=us; }
    void finish(bool ok) {
        if (!active_) return;
        sample(true);
        transfer_diag::probe.stop();
        active_=false;
        const int64_t elapsed=esp_timer_get_time()-begin_;
        capture_link(link_end_);
        esp_test_tx_statistics_t tx{};
        esp_test_tx_fail_statistics_t fail[TEST_TX_FAIL_MAX]{};
        const auto tx_status=esp_wifi_get_tx_statistics(ESP_WIFI_ACI_BE,&tx,fail);
        esp_test_rx_statistics_t rx0{},rx7{};
        const auto rx0_status=esp_wifi_get_rx_statistics(0,&rx0);
        const auto rx7_status=esp_wifi_get_rx_statistics(7,&rx7);
        esp_test_rx_error_occurs_t errors{};
        const auto error_status=esp_test_get_rx_error_occurs(&errors);
        esp_wifi_enable_rx_statistics(false,false);
        esp_wifi_enable_tx_statistics(ESP_WIFI_ACI_BE,false);
        uint32_t probe_count,probe_rejected; int64_t probe_total,probe_max; bool probe_pending;
        auto& p=transfer_diag::probe;
        portENTER_CRITICAL(&p.mux);
        probe_count=p.count; probe_rejected=p.rejected; probe_total=p.total; probe_max=p.maximum;
        probe_pending=p.pending;
        portEXIT_CRITICAL(&p.mux);
        std::string text="{\"available\":true,\"schema\":2";
        auto str=[&](const char* key,const std::string& value) { text+=",\""+std::string(key)+"\":"+transfer_diag::quote(value); };
        bool metric_available=true;
        auto num=[&](const char* key,int64_t value) { text+=",\""+std::string(key)+"\":"+(metric_available?std::to_string(value):"null"); };
        str("file",name_); str("direction",direction_); str("allowed_protocol",protocol_);
        str("firmware_build",std::string(__DATE__)+" "+__TIME__);
        str("mode",ap_?"AP":"STA");
        num("ok",ok); num("he_stbc_requested",stbc_); num("ampdu",ampdu_);
        str("tx_power_profile",requested_power_<0?"driver default":std::to_string(requested_power_/4)+" dBm limit");
        metric_available=requested_power_>=0;
        num("tx_power_requested_qdbm",requested_power_);
        metric_available=true;
        auto append_link=[&](const char* name,const Link& link) {
            text+=",\""+std::string(name)+"\":{";
            text+="\"power_read_status\":"+transfer_diag::quote(esp_err_to_name(link.power_status));
            text+=",\"tx_power_limit_qdbm\":"+(link.power_status==ESP_OK?std::to_string(int(link.power)):"null");
            text+=",\"channel_status\":"+transfer_diag::quote(esp_err_to_name(link.channel_status));
            text+=",\"primary_channel\":"+(link.channel_status==ESP_OK?std::to_string(link.channel):"null");
            text+=",\"secondary_channel_raw\":"+(link.channel_status==ESP_OK?std::to_string(int(link.secondary)):"null");
            if (ap_) {
                text+=",\"peers_status\":"+transfer_diag::quote(esp_err_to_name(link.peers_status));
                text+=",\"peer_rssi_dbm\":";
                if (link.peers_status!=ESP_OK) text+="null";
                else {
                    text+='[';
                    const size_t count=std::min(size_t(link.peers.num),sizeof(link.peers.sta)/sizeof(link.peers.sta[0]));
                    for (size_t i=0;i<count;++i) { if(i)text+=',';text+=std::to_string(link.peers.sta[i].rssi); }
                    text+=']';
                }
            } else {
                text+=",\"sta_status\":"+transfer_diag::quote(esp_err_to_name(link.ap_status));
                text+=",\"rssi_dbm\":"+(link.ap_status==ESP_OK?std::to_string(link.ap.rssi):"null");
                char bssid[24];
                std::snprintf(bssid,sizeof(bssid),"%02x:%02x:%02x:%02x:%02x:%02x",
                    link.ap.bssid[0],link.ap.bssid[1],link.ap.bssid[2],link.ap.bssid[3],link.ap.bssid[4],link.ap.bssid[5]);
                text+=",\"bssid\":"+(link.ap_status==ESP_OK?transfer_diag::quote(bssid):"null");
            }
            text+='}';
        };
        append_link("link_start",link_start_);
        append_link("link_end",link_end_);
        wifi_phy_mode_t phy{};
        const auto phy_status=esp_wifi_sta_get_negotiated_phymode(&phy);
        str("negotiated_phy",ap_?"not available for AP" : phy_status!=ESP_OK?"unknown":
            phy==WIFI_PHY_MODE_HT20?"HT20":phy==WIFI_PHY_MODE_HT40?"HT40":phy==WIFI_PHY_MODE_HE20?"HE20":
            phy==WIFI_PHY_MODE_11G?"11g":phy==WIFI_PHY_MODE_11B?"11b":"other");
        wifi_ps_type_t ps{}; const auto ps_status=esp_wifi_get_ps(&ps);
        str("power_save_status",esp_err_to_name(ps_status));
        num("power_save_mode",ps_status==ESP_OK?int(ps):-1);
        num("bytes",bytes_); num("elapsed_us",elapsed);
        num("network_wait_us",net_); num("network_call_max_us",net_max_);
        num("storage_us",io_); num("storage_call_max_us",io_max_); num("flush_us",flush_);
        num("network_calls",calls_); num("network_timeouts",timeouts_);
        num("internal_free_min_sampled",internal_); num("internal_largest_min_sampled",largest_);
        num("tcpip_probe_samples",probe_count); num("tcpip_probe_rejected",probe_rejected);
        num("tcpip_probe_pending_at_end",probe_pending);
        metric_available=probe_count>0;
        num("tcpip_queue_average_us",probe_count?probe_total/probe_count:0); num("tcpip_queue_max_us",probe_max);
        metric_available=true;
        str("tx_enable_status",esp_err_to_name(tx_enable_)); str("tx_read_status",esp_err_to_name(tx_status));
        str("tx_clear_status",esp_err_to_name(tx_clear_)); str("rx_clear_status",esp_err_to_name(rx_clear_));
        str("rx_error_clear_status",esp_err_to_name(error_clear_));
        str("rx_enable_status",esp_err_to_name(rx_enable_)); str("rx_tid0_status",esp_err_to_name(rx0_status));
        str("rx_tid7_status",esp_err_to_name(rx7_status)); str("hardware_read_status",esp_err_to_name(hw_status_));
        const bool tx_valid=tx_status==ESP_OK && tx_enable_==ESP_OK && tx_clear_==ESP_OK;
        const bool rx_valid=rx_enable_==ESP_OK && rx_clear_==ESP_OK && rx0_status==ESP_OK && rx7_status==ESP_OK;
        metric_available=tx_valid;
        num("tx_be_attempts",tx.tx_enable); num("tx_be_success",tx.tx_succ);
        num("tx_be_retries_edca",tx.retry_edca); num("tx_be_retries_tb",tx.retry_tb);
        num("tx_be_wait_ack_count",fail[TEST_TX_WAIT_ACK].count);
        num("tx_be_wait_cts_count",fail[TEST_TX_WAIT_CTS].count);
        num("tx_be_ack_timeout",fail[TEST_TX_WAIT_ACK].match[TEST_TX_WAIT_TIMEOUT][0]);
        num("tx_be_cts_timeout",fail[TEST_TX_WAIT_CTS].match[TEST_TX_WAIT_TIMEOUT][0]);
        num("tx_be_rtt_max_raw",tx.tx_max_rtt);
        text+=",\"tx_be_failure_details\":";
        if (!tx_valid) text+="null";
        else {
            static const char* stages[]={"SUCCESS","FAIL_RTS","WAIT_CTS","FAIL_CTS","FAIL_DATA","WAIT_ACK"};
            static const char* matches[]={"MATCH","NOT_TO_SELF","MISMATCH","TIMEOUT"};
            static const char* errors[]={"H00","H53","H63","H75","H41","H42","H47","H80","H5A","OTHER"};
            text+='[';
            for (int stage=0;stage<TEST_TX_FAIL_MAX;++stage) {
                if(stage)text+=',';
                text+="{\"stage\":"+transfer_diag::quote(stages[stage])+",\"count\":"+std::to_string(fail[stage].count)+",\"details\":[";
                bool first=true;
                for (int match=0;match<TEST_TX_WAIT_MAX;++match) for(int error=0;error<TEST_TX_FAIL_ERROR_MAX;++error) {
                    const auto count=fail[stage].match[match][error];
                    if (!count) continue;
                    if(!first)text+=',';
                    first=false;
                    text+="{\"match\":"+transfer_diag::quote(matches[match])+",\"count\":"+std::to_string(count);
                    // Hardware error codes are meaningful only for MATCH.
                    if(match==TEST_TX_WAIT_MATCH) text+=",\"error\":"+transfer_diag::quote(errors[error]);
                    text+='}';
                }
                text+="]}";
            }
            text+=']';
        }
        metric_available=rx_valid;
        num("rx_ht",uint64_t(rx0.ht)+rx7.ht); num("rx_ht_stbc",uint64_t(rx0.ht_stbc)+rx7.ht_stbc);
        num("rx_no_buffer",uint64_t(rx0.legacy_noeb)+rx7.legacy_noeb+rx0.ht_noeb+rx7.ht_noeb+rx0.su_noeb+rx7.su_noeb+rx0.ersu_noeb+rx7.ersu_noeb+rx0.mu_noeb+rx7.mu_noeb);
        num("rx_base_null",uint64_t(rx0.rx_base_null_cnt)+rx7.rx_base_null_cnt);
        num("rx_base_reset",uint64_t(rx0.rx_reset_rxbase_cnt)+rx7.rx_reset_rxbase_cnt);
        num("rx_retries_ht_su",uint64_t(rx0.ht_retry)+rx7.ht_retry+rx0.su_retry+rx7.su_retry);
        metric_available=true;
        num("hw_samples",hw_samples_); num("hw_read_failures",hw_failures_); num("hw_sample_gap_max_us",gap_max_);
        metric_available=hw_samples_>0;
        num("rx_fcs_errors",fcs_); num("rx_abort",abort_); num("rx_same_bitmap_errors",samebm_);
        num("rx_mpdu",mpdu_); num("rx_data_success",data_); num("rx_buffer_full",full_); num("rx_fifo_overflow",fifo_);
        num("rx_abort_good_fcs",abort_good_fcs_); num("rx_ht_signal_errors",ht_sig_); num("rx_he_signal_crc_errors",he_sig_);
        num("rx_agc_errors",agc_); num("rx_phy_restarts",restart_);
        num("rx_cfo_hz_last",previous_.rx_cfo_hz);
        str("rx_error_status",esp_err_to_name(error_status));
        metric_available=error_status==ESP_OK && error_clear_==ESP_OK;
        num("rx_error_occurrences",errors.tot);
        str("scope","radio counters include unrelated traffic; no packet loss percentage; 16-bit HW counters may wrap between samples; TCPIP probe measures callback queue latency, not TCP RTT");
        text+="}";
        const std::string line="FCS="+(hw_samples_?std::to_string(fcs_):"--")+
            " TXretry="+(tx_valid?std::to_string(uint64_t(tx.retry_edca)+tx.retry_tb):"--")+
            " TCPq="+(probe_count?std::to_string(probe_max/1000)+"ms":"--");
        std::lock_guard<std::mutex> lock(mutex_);
        report_=std::move(text); summary_=line; generation_.fetch_add(1);
    }
#else
    void begin(const std::string&,const char*,const char*,bool,bool,bool,int) {}
    void network(int64_t,size_t,bool) {}
    void storage(int64_t) {}
    void flush(int64_t) {}
    void finish(bool) {}
#endif
};
