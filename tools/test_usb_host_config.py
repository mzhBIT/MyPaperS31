"""Compile the actual Host config for both boards against ISR placement rules."""
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
source=(root/'main/audio/usb_audio_output.c').read_text(encoding='utf-8')
start=source.index('    const usb_host_config_t config = {',source.index('static void host_event_task('))
end=source.index('\n    };',start)+len('\n    };')
config=source[start:end]
program=r'''
#include <cassert>
#include <cstddef>
constexpr int ESP_INTR_FLAG_LEVEL3=8,ESP_INTR_FLAG_IRAM=1024;
struct usb_host_config_t {
    bool skip_phy_setup,root_port_unpowered;
    int intr_flags;
    void *enum_filter_cb;
    struct {unsigned nptx_fifo_lines,ptx_fifo_lines,rx_fifo_lines;} fifo_settings_custom;
    unsigned peripheral_map;
};
'''
program+='#define CONFIG_IDF_TARGET_ESP32S31 1\nusb_host_config_t s31(){\n'+config+'\nreturn config;\n}\n'
program+='#undef CONFIG_IDF_TARGET_ESP32S31\n#define CONFIG_IDF_TARGET_ESP32S31 0\nusb_host_config_t p4(){\n'+config+'\nreturn config;\n}\n'
program+=r'''
// esp_intr_alloc rejects IRAM requests whose handler is in a cached region.
bool sdk_accepts(const usb_host_config_t &c,bool handler_in_flash){return !(c.intr_flags&ESP_INTR_FLAG_IRAM) || !handler_in_flash;}
int main(){
    const auto a=s31(),b=p4();
    assert(sdk_accepts(a,true)); // Official S31 Host 1.6.0 uses a Flash ISR.
    assert(!sdk_accepts(b,true) && sdk_accepts(b,false)); // P4's noflash linker mapping.
    assert(a.intr_flags==ESP_INTR_FLAG_LEVEL3);
    assert(b.intr_flags==(ESP_INTR_FLAG_LEVEL3|ESP_INTR_FLAG_IRAM));
    assert(!a.skip_phy_setup && !a.root_port_unpowered && a.peripheral_map==0);
}
'''
(out/'test.cpp').write_text(program,encoding='utf-8')
