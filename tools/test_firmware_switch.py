"""Native regression for the production image identity and boot-selection gates."""
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
source=(root/'main/boot/firmware_switch.cpp').read_text(encoding='utf-8')
source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
program=r'''
#include <cassert>
#include <cstring>
#include <cstdio>
#define CONFIG_IDF_TARGET_ESP32S31 1
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_NOT_FOUND=-1,ESP_ERR_INVALID_SIZE=-2,ESP_ERR_INVALID_VERSION=-3,
    ESP_ERR_NOT_SUPPORTED=-4,ESP_ERR_OTA_VALIDATE_FAILED=-5;
constexpr int ESP_PARTITION_TYPE_APP=0,ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_SUBTYPE_APP_OTA_0=16,
    ESP_PARTITION_SUBTYPE_APP_FACTORY=0,ESP_PARTITION_SUBTYPE_DATA_OTA=0;
struct esp_partition_t {int type,subtype;unsigned address,size;const char*label;};
struct esp_app_desc_t {char project_name[32];};
esp_partition_t system_part{0,0,0x10000,0x800000,"factory"},emulator{0,16,0x810000,0x400000,"emulator"},
    otadata{1,0,0xc10000,0x2000,"otadata"};
bool emulator_exists=true,ota_exists=true,wrong_name=false,corrupt_image=false;
int header_error=0,selection_calls=0;
const esp_partition_t* selected=&system_part;
const esp_partition_t* esp_partition_find_first(int type,int subtype,const char*label){
    for(auto* part:{&system_part,&emulator,&otadata}){
        if(part==&emulator&&!emulator_exists)continue;
        if(part==&otadata&&!ota_exists)continue;
        if(part->type==type&&part->subtype==subtype&&!strcmp(label,part->label))return part;
    }return nullptr;
}
int esp_ota_get_partition_description(const esp_partition_t*part,esp_app_desc_t*desc){
    if(header_error)return header_error;
    strcpy(desc->project_name,wrong_name ? "UnrelatedApp" : part==&emulator ? "MyPaperEmulatorS31" : "MyPaperS31");
    return 0;
}
int esp_ota_set_boot_partition(const esp_partition_t*part){
    ++selection_calls;if(corrupt_image)return ESP_ERR_OTA_VALIDATE_FAILED;selected=part;return 0;
}
''' + source + r'''
int main(){
    using namespace firmware_switch;
    emulator_exists=false;assert(check_emulator()==ESP_ERR_NOT_FOUND);assert(select_emulator()==ESP_ERR_NOT_FOUND);
    emulator_exists=true;wrong_name=true;assert(select_emulator()==ESP_ERR_INVALID_VERSION && selection_calls==0);
    wrong_name=false;emulator.address=0x10000;assert(select_emulator()==ESP_ERR_INVALID_SIZE);
    emulator.address=0x810000;ota_exists=false;assert(select_emulator()==ESP_ERR_NOT_FOUND);
    ota_exists=true;header_error=-12;assert(select_emulator()==-12 && selected==&system_part);
    header_error=0;assert(check_emulator()==0 && selection_calls==0);
    corrupt_image=true;assert(select_emulator()==ESP_ERR_OTA_VALIDATE_FAILED && selected==&system_part);
    corrupt_image=false;assert(select_emulator()==0 && selected==&emulator);
    assert(select_system()==0 && selected==&system_part); // One-shot return arming.
    wrong_name=true;assert(select_system()==ESP_ERR_INVALID_VERSION && selected==&system_part);
    puts("Firmware switch: missing/layout/identity/header gates, SDK full validation, emulator selection and system return PASS");
}
'''
program=program.replace('#include <cstdio>','#include <cstdio>\n#include <initializer_list>')
(out/'test.cpp').write_text(program,encoding='utf-8')
