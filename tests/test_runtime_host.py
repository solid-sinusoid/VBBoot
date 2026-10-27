"""Execute production update/manifest validation against process-local simulated Flash."""
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    pos = text.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def test_update_manifest_and_flash_crc(tmp_path):
    app = (ROOT/'App/app.c').read_text()
    runtime = (ROOT/'App/bootloader_runtime.c').read_text()
    for name in ('main.h', 'stm32g4xx_hal.h'):
        (tmp_path/name).write_text('#pragma once\ntypedef int HAL_StatusTypeDef;\ntypedef int FDCAN_HandleTypeDef;\n')
    source = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#include "app.h"
#include "bootloader_runtime.h"
#define CRC32_POLY 0xEDB88320UL
static BootSession boot_session;
BootSession* get_boot_session(void){return &boot_session;}
static unsigned erases;
bool boot_prepare_flash(uint32_t size){
 if(size==0 || size>APP_END_ADDR-APP_START_ADDR)return false;
 ++erases;memset((void*)APP_START_ADDR,255,size);return true;
}
bool boot_write_chunk(uint32_t offset,const uint8_t* bytes,uint8_t size,uint32_t total){
 if(offset+size>total)return false;
 memcpy((void*)(uintptr_t)(APP_START_ADDR+offset),bytes,size);return true;
}
'''
    source += function(runtime, 'uint32_t crc32_update(')
    for signature in ('bool is_application_valid(', 'void boot_on_start(', 'bool boot_on_data(', 'bool boot_on_done('):
        source += '\n'+function(app, signature)
    source += r'''
int main(void){
 assert(mmap((void*)APP_START_ADDR,0x1D000,PROT_READ|PROT_WRITE,
             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=(void*)-1);
 const uint32_t size=APP_MANIFEST_ADDR-APP_START_ADDR+32;
 static uint8_t image[0x1D000];memset(image,255,sizeof(image));
 uint32_t vectors[]={0x20008000,0x08003101,0x08003111,0x08003121};
 memcpy(image,vectors,sizeof(vectors));
 BootApplicationManifest manifest={0x50414256,1,32,0x31444256,0x44AAAC06,1,
     0x08003000,0x0801F800,0};
 memcpy(image+APP_MANIFEST_ADDR-APP_START_ADDR,&manifest,sizeof(manifest));
 memcpy((void*)APP_START_ADDR,image,size);
 assert(is_application_valid());
 const uint32_t crc=crc32_update(0xFFFFFFFF,image,size)^0xFFFFFFFF;
 boot_on_start(size,crc);assert(erases==1 && boot_session.state==BootStateReceiving);
 for(uint32_t offset=0;offset<size;offset+=64){
   const uint8_t length=size-offset<64 ? size-offset : 64;
   assert(boot_on_data(offset,image+offset,length));
 }
 assert(boot_on_done());
 // An ACK over received bytes is not enough: independently corrupt programmed Flash.
 boot_session.state=BootStateReceiving;
 ((uint8_t*)APP_START_ADDR)[100]^=1;
 assert(!boot_on_done() && boot_session.state==BootStateError);
 memcpy((void*)APP_START_ADDR,image,size);
 ((BootApplicationManifest*)APP_MANIFEST_ADDR)->config_abi=0x44AAABFF;
 assert(!is_application_valid());
 boot_on_start(0,0);assert(erases==1 && boot_session.state==BootStateError);
 boot_on_start(UINT32_MAX,0);assert(erases==1 && boot_session.state==BootStateError);
 assert(munmap((void*)APP_START_ADDR,0x1D000)==0);
}
'''
    (tmp_path/'runtime.c').write_text(source)
    library = ROOT.parent/'Drivers/libvoltbro'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-I'+str(tmp_path),
                    '-I'+str(ROOT/'App'),'-I'+str(library),str(tmp_path/'runtime.c'),
                    '-o',str(tmp_path/'runtime')],check=True)
    subprocess.run([str(tmp_path/'runtime')],check=True)


def test_current_eeprom_transport_wins_over_stale_recovery_metadata(tmp_path):
    app=(ROOT/'App/app.c').read_text()
    source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct {uint32_t can_id;uint8_t nominal_prescaler,data_prescaler;bool fd_mode,bitrate_switch;} BootTransportConfig;
static BootTransportConfig g_transport_config;
static bool valid_eeprom=true,valid_request=false,valid_metadata=true;
void transport_config_load_defaults(void){g_transport_config=(BootTransportConfig){.can_id=0x444};}
bool transport_config_load_eeprom(BootTransportConfig* out){if(valid_eeprom)out->can_id=11;return valid_eeprom;}
bool boot_request_read(BootTransportConfig* out,bool clear){(void)clear;if(valid_request)out->can_id=12;return valid_request;}
bool transport_config_is_valid(const BootTransportConfig* config){return config->can_id!=0;}
uint32_t boot_request_pack(const BootTransportConfig* config){return config->can_id;}
bool boot_metadata_store_transport(uint32_t packed){(void)packed;return true;}
bool boot_metadata_read_transport(uint32_t* out){*out=7;return valid_metadata;}
void boot_request_unpack(uint32_t value,BootTransportConfig* out){out->can_id=value;}
void transport_config_force_fd_brs(BootTransportConfig* out){out->fd_mode=true;out->bitrate_switch=true;}
''' + function(app,'void transport_config_load(void)') + r'''
int main(void){
 transport_config_load();assert(g_transport_config.can_id==11);
 valid_request=true;transport_config_load();assert(g_transport_config.can_id==12);
 valid_request=false;valid_eeprom=false;transport_config_load();assert(g_transport_config.can_id==7);
 valid_metadata=false;transport_config_load();assert(g_transport_config.can_id==0x444);
}
'''
    (tmp_path/'transport.c').write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(tmp_path/'transport.c'),'-o',str(tmp_path/'transport')],check=True)
    subprocess.run([str(tmp_path/'transport')],check=True)
