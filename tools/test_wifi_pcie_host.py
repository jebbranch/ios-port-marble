#!/usr/bin/env python3
"""Run actual Wi-Fi phase-2 sequencing against an MMIO/RPMh/PCIe model."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT.parent / "chain-tree-wifi/bsd/dev/sweet_wifi.c"
PHY = ROOT.parent / "chain-tree-wifi/bsd/dev/sweet_wifi_phy.h"


def body(text):
    return "\n".join(
        line for line in text.splitlines()
        if not line.startswith(("#include", "#if", "#endif"))
    )


source = body(SOURCE.read_text())
prefix, remainder = source.split("struct sw_wifi_page {", 1)
_, remainder = remainder.split("static bool\nsw_wifi_wait", 1)
DECLARATIONS = prefix + r'''
static bool sw_wifi_rpmh_poisoned;
static uint32_t sw_wifi_read(uint32_t pa, bool *ok);
static void sw_wifi_write(uint32_t pa, uint32_t value, bool *ok);
static uint32_t sw_wifi_halted(bool *ok);
'''
PRODUCTION = "\nstatic bool\nsw_wifi_wait" + remainder

HEADERS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
'''

SUPPORT = r'''
struct cell {uint32_t pa,value;};
static struct cell cells[32768];
static unsigned cells_n,writes,votes;
static int boot_stage,log_syncs,complete_votes,link_ready,bad_vendor;
static char output[32768];
static size_t output_at;
static int test_printf(const char *format,...) {
 va_list ap;va_start(ap,format);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
 int n=vsnprintf(output+output_at,sizeof(output)-output_at,format,ap);
#pragma clang diagnostic pop
 va_end(ap);assert(n>=0 && output_at+(size_t)n<sizeof(output));
 output_at+=(size_t)n;return n;
}
#define printf test_printf
static struct cell *cell(uint32_t pa) {
 for(unsigned i=0;i<cells_n;i++)if(cells[i].pa==pa)return &cells[i];
 assert(cells_n<sizeof(cells)/sizeof(cells[0]));
 cells[cells_n]=(struct cell){pa,0};return &cells[cells_n++];
}
static void put(uint32_t pa,uint32_t value){cell(pa)->value=value;}
static uint32_t get(uint32_t pa){return cell(pa)->value;}
static int PE_parse_boot_argn(const char *name,void *value,int bytes) {
 assert(!strcmp(name,"sweetwifi") && bytes==4);memcpy(value,&boot_stage,4);return 1;
}
vm_offset_t ml_io_map(vm_offset_t pa,vm_size_t bytes){(void)pa;(void)bytes;return 1;}
void sweet_log_sync(void){log_syncs++;}
static void IOSleep(unsigned ms){(void)ms;}
static void IODelay(unsigned us){(void)us;}
static void delay(unsigned us) {
 (void)us;
 const uint32_t base=SW_WIFI_RSC_PA+0xd00u;
 if(complete_votes && (get(base+SW_WIFI_CONTROL)&SW_WIFI_TRIGGER)) {
  put(base+SW_WIFI_STATUS,1);
  put(base+SW_WIFI_CMD_STATUS,SW_WIFI_ISSUED|SW_WIFI_COMPLETE);
  put(SW_WIFI_RSC_PA+SW_WIFI_IRQ_STATUS,1);
 }
}
static uint32_t sw_wifi_read(uint32_t pa,bool *ok) {
 (void)ok;
 if(pa==SW_WIFI_DBI_PA+0x80u)return link_ready?1u<<29:0;
 return get(pa);
}
static uint32_t sw_wifi_halted(bool *ok) {
 uint32_t halted=0;
 for(unsigned i=0;i<sizeof(sw_wifi_branches)/sizeof(sw_wifi_branches[0]);i++)
  if(sw_wifi_read(SW_WIFI_GCC_PA+sw_wifi_branches[i],ok)&(1u<<31))halted|=1u<<i;
 return halted;
}
static void sw_wifi_write(uint32_t pa,uint32_t value,bool *ok) {
 (void)ok;writes++;
 const uint32_t rsc_base=SW_WIFI_RSC_PA+0xd00u;
 if(pa==SW_WIFI_RSC_PA+SW_WIFI_IRQ_CLEAR) {
  put(SW_WIFI_RSC_PA+SW_WIFI_IRQ_STATUS,get(SW_WIFI_RSC_PA+SW_WIFI_IRQ_STATUS)&~value);
  return;
 }
 if((pa==SW_WIFI_GCC_PA+0x7b064u || pa==SW_WIFI_GCC_PA+0x7b048u) && (value&1)) {
  put(pa,0);return;
 }
 if(pa==rsc_base+SW_WIFI_CONTROL && (value&SW_WIFI_TRIGGER)) {
  votes++;put(pa,value);put(rsc_base+SW_WIFI_STATUS,0);put(rsc_base+SW_WIFI_CMD_STATUS,0);return;
 }
 if(pa==SW_WIFI_CFG_PA+0x10u && value==0xffffffffu) {
  put(pa,0xffe00000u);return; /* 2 MiB BAR */
 }
 put(pa,value);
}
'''

TESTS = r'''
static void le16put(uint32_t pa,uint16_t value) {
 uint32_t base=pa&~3u,shift=(pa&3u)*8u,mask=0xffffu<<shift;
 put(base,(get(base)&~mask)|((uint32_t)value<<shift));
}
static void db_resource(unsigned index,const char *name,uint32_t address,const uint16_t *levels,unsigned count,unsigned *aux_at) {
 const uint32_t entry=SW_WIFI_CMD_DB_PA+SW_WIFI_DB_HEADER+24u*index;
 char id[8]={0};strncpy(id,name,8);
 uint32_t w0=0,w1=0;memcpy(&w0,id,4);memcpy(&w1,id+4,4);
 put(entry,w0);put(entry+4,w1);put(entry+16,address);
 le16put(entry+20,(uint16_t)(count*2));
 le16put(entry+22,(uint16_t)(*aux_at-(SW_WIFI_DB_HEADER+12u*24u)));
 for(unsigned i=0;i<count;i++)le16put(SW_WIFI_CMD_DB_PA+*aux_at+2*i,levels[i]);
 *aux_at+=2*count;
}
static void prepare_db(void) {
 put(SW_WIFI_CMD_DB_PA+4,SW_WIFI_CMD_MAGIC);
 le16put(SW_WIFI_CMD_DB_PA+8,4);
 le16put(SW_WIFI_CMD_DB_PA+10,0);
 le16put(SW_WIFI_CMD_DB_PA+12,12u*24u);
 le16put(SW_WIFI_CMD_DB_PA+14,12);
 unsigned aux=SW_WIFI_DB_HEADER+12u*24u;
 const char *names[]={"smpe2","smpb11","smpb10","smpc1","smpb12","ldoe7","ldob5","ldoc10","cx.lvl","mx.lvl","xo.lvl","SN7"};
 const uint16_t levels[]={0x10,0x100};
 for(unsigned i=0;i<12;i++)db_resource(i,names[i],0x40000u+0x100u*i,i>=8&&i<=10?levels:NULL,i>=8&&i<=10?2:0,&aux);
}
static void reset_case(void) {
 memset(cells,0,sizeof(cells));cells_n=writes=votes=0;boot_stage=2;log_syncs=0;
 complete_votes=1;link_ready=1;bad_vendor=0;output_at=0;output[0]=0;
 sw_wifi_rpmh_poisoned=false;
 prepare_db();
 const uint32_t rsc=SW_WIFI_RSC_PA+0xd00u;
 put(rsc+SW_WIFI_STATUS,1);put(rsc+SW_WIFI_CONTROL,0);put(rsc+SW_WIFI_ENABLE,0);put(rsc+SW_WIFI_WAIT,0);
 put(SW_WIFI_GCC_PA+SW_WIFI_GDSC,1u<<31);
 put(SW_WIFI_CFG_PA,bad_vendor?0x11031234u:0x110317cbu);
 put(SW_WIFI_CFG_PA+0x2c,0x010817cbu);put(SW_WIFI_CFG_PA+4,0);
 put(SW_WIFI_CFG_PA+0x10,0);
 put(0x60400000u+0x224u,0x400c1211u);
 put(0x60400000u+0x90010u,0x1016d0e1u);
}
int main(void) {
 reset_case();
 sweet_wifi_init();
 assert(votes==28 && writes>200);
 assert(strstr(output,"wifi pcie v1 stage=2 id=0x110317cb")!=NULL);
 assert(strstr(output,"soc=0x400c1211 sub=0x1016d0e1")!=NULL);

 reset_case();complete_votes=0;
 sweet_wifi_init();
 assert(votes==1 && sw_wifi_rpmh_poisoned);
 assert(strstr(output,"phase 2 failed errno=60 poisoned=1")!=NULL);

 reset_case();link_ready=0;
 sweet_wifi_init();
 assert(votes==28);
 assert(strstr(output,"phase 2 failed errno=60")!=NULL);
 assert(strstr(output,"wifi pcie v1")==NULL);

 reset_case();put(SW_WIFI_CFG_PA,0x11031234u);
 sweet_wifi_init();
 assert(votes==28);
 assert(strstr(output,"phase 2 failed errno=6")!=NULL);
 assert(strstr(output,"wifi pcie v1")==NULL);

 printf("{\"passed\":true,\"cases\":4,\"votes_on_success\":%u,"
        "\"actual_phase2\":true,\"hardware\":\"mock-only\"}\n",28u);
 fputs(output,stdout);
 return 0;
}
'''


def main():
    code = (HEADERS + "\n" + DECLARATIONS + "\n" + SUPPORT + "\n" + body(PHY.read_text()) +
            "\n" + PRODUCTION + "\n" + TESTS)
    with tempfile.TemporaryDirectory(prefix="wifi-pcie-") as name:
        cfile, exe = Path(name) / "test.c", Path(name) / "test"
        cfile.write_text(code)
        subprocess.run([
            "clang", "-std=gnu11", "-Wall", "-Wextra", "-Wformat=2",
            "-Werror", "-fsanitize=address,undefined", str(cfile),
            "-o", str(exe),
        ], check=True)
        run = subprocess.run([str(exe)], check=False, capture_output=True, text=True)
        if run.returncode:
            raise RuntimeError(
                f"wifi phase2 host failed {run.returncode}\n{run.stdout}\n{run.stderr}")
    result = json.loads(run.stdout.splitlines()[-1])
    result["checks"] = [
        "28 exact RPMh resource votes and endpoint identity",
        "first-vote timeout poisons lifecycle and stops",
        "PCIe link timeout prevents endpoint access",
        "wrong PCI vendor/device is rejected",
    ]
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
