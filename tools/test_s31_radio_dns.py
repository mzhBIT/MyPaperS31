"""Generate a native regression for the actual radio fallback DNS setup."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
source = (root / 'main/audio/audio_radio_s31.cpp').read_text(encoding='utf-8')
start = source.index('void network_prepare_dns()')
end = source.index('\nvoid network_acquire_wifi()', start)
program = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_NETIF_DNS_FALLBACK=2,ESP_IPADDR_TYPE_V4=0;
#define ESP_IP4TOADDR(a,b,c,d) ((uint32_t(a)<<24)|(uint32_t(b)<<16)|(uint32_t(c)<<8)|uint32_t(d))
struct Ip {unsigned type=0;struct {struct {uint32_t addr=0;} ip4;struct {unsigned addr=0;} ip6;} u_addr;};
bool any(const Ip& ip){return ip.type==0 ? ip.u_addr.ip4.addr==0 : ip.u_addr.ip6.addr==0;}
#define ESP_IP_IS_ANY(ip) any(ip)
struct esp_netif_dns_info_t {Ip ip;};
struct esp_netif_t {} wifi,other;
esp_netif_t *route=&wifi,*sta=&wifi;
esp_netif_dns_info_t servers[3];
int writes=0,read_result=ESP_OK,write_result=ESP_OK;
esp_netif_t* esp_netif_get_handle_from_ifkey(const char*){return sta;}
esp_netif_t* esp_netif_get_default_netif(){return route;}
int esp_netif_get_dns_info(esp_netif_t* netif,int type,esp_netif_dns_info_t* dns){
    assert(netif==&wifi && type==2);*dns=servers[type];return read_result;
}
int esp_netif_set_dns_info(esp_netif_t* netif,int type,esp_netif_dns_info_t* dns){
    assert(netif==&wifi && type==2);++writes;if(write_result==ESP_OK)servers[type]=*dns;return write_result;
}
'''
program += source[start:end]
program += r'''
int main(){
    servers[0].ip.u_addr.ip4.addr=11;servers[1].ip.u_addr.ip4.addr=22;
    network_prepare_dns();
    assert(writes==1 && servers[2].ip.u_addr.ip4.addr==ESP_IP4TOADDR(223,5,5,5));
    network_prepare_dns();assert(writes==1);
    servers[2].ip.u_addr.ip4.addr=33;network_prepare_dns();assert(writes==1);
    servers[2].ip.type=6;servers[2].ip.u_addr.ip6.addr=44;
    network_prepare_dns();assert(writes==1 && servers[2].ip.u_addr.ip6.addr==44);
    servers[2]={};route=&other;network_prepare_dns();assert(writes==1);
    route=&wifi;sta=nullptr;network_prepare_dns();assert(writes==1);
    sta=&wifi;read_result=ESP_FAIL;network_prepare_dns();assert(writes==1);
    read_result=ESP_OK;write_result=ESP_FAIL;network_prepare_dns();
    assert(writes==2 && servers[2].ip.u_addr.ip4.addr==0);
    assert(servers[0].ip.u_addr.ip4.addr==11 && servers[1].ip.u_addr.ip4.addr==22);
    puts("S31 radio DNS: preserve DHCP/configured fallback, empty-slot setup, route guard, failures PASS");
}
'''
(out / 'test.cpp').write_text(program, encoding='utf-8')
