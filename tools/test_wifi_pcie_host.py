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
static bool sw_wifi_started_once;
static unsigned sw_wifi_milestone;
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
static unsigned cells_n,writes,votes,case_count;
static uint32_t vote_addresses[32],vote_values[32],bar_mask;
static int boot_stage,log_syncs,complete_votes,link_ready,bad_vendor;
static int phy_ready,stale_complete,all_ones_status,cleanup_failed;
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
void IOSleep(unsigned ms){(void)ms;}
void IODelay(unsigned us){(void)us;}
static void delay(unsigned us) {
 (void)us;
 /* Fixed physical hardware layout, NOT derived from production offsets. */
 if(complete_votes && (get(0x17a20d14u)&(1u<<24))) {
  put(0x17a20d18u,1);
  put(0x17a20d3cu,0x10100);
  put(0x17a20d04u,1);
 }
}
static uint32_t sw_wifi_read(uint32_t pa,bool *ok) {
 if(!*ok)return 0;
 assert(pa!=0x17a20004u && pa!=0x17a20008u);
 if(all_ones_status && pa==0x17a20d3cu)return 0xffffffffu;
 if(pa==0x01c06214u)return phy_ready?0:1u<<6;
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
 if(!*ok)return;
 assert(pa<0x17a20000u || pa>=0x17a20010u); /* never overwrite RSC ID */
 writes++;
 if(pa==0x17a20d08u) {
  put(0x17a20d04u,get(0x17a20d04u)&~value);
  return;
 }
 if((pa==SW_WIFI_GCC_PA+0x7b064u || pa==SW_WIFI_GCC_PA+0x7b048u) && (value&1)) {
  put(pa,0);return;
 }
 if(pa==0x17a20d14u && (value&(1u<<24))) {
  assert(votes<32);
  vote_addresses[votes]=get(0x17a20d34u);
  vote_values[votes]=get(0x17a20d38u);
  votes++;put(pa,value);
  put(0x17a20d18u,stale_complete?1:0);
  put(0x17a20d3cu,stale_complete?0x10100u:0);return;
 }
 if(cleanup_failed && pa==0x17a20d1cu && value==0 && votes)return;
 if(pa==SW_WIFI_CFG_PA+0x10u && value==0xffffffffu) {
  put(pa,bar_mask);return;
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
 put(SW_WIFI_CMD_DB_PA,1);
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
 complete_votes=1;link_ready=phy_ready=1;bad_vendor=0;output_at=0;output[0]=0;
 stale_complete=all_ones_status=cleanup_failed=0;bar_mask=0xffe00000u;
 memset(vote_addresses,0,sizeof(vote_addresses));memset(vote_values,0,sizeof(vote_values));
 sw_wifi_rpmh_poisoned=false;
 sw_wifi_started_once=false;sw_wifi_milestone=0;
 prepare_db();
 put(0x17a20000u,0x20700u);
 put(0x17a20d18u,1);put(0x17a20d14u,0);put(0x17a20d1cu,0);put(0x17a20d10u,0);
 put(0x17a20d00u,0xa5u);
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
 assert(strstr(output,"wifi pcie v2 stage=2 id=0x110317cb")!=NULL);
 assert(strstr(output,"soc=0x400c1211 sub=0x1016d0e1")!=NULL);
 for(unsigned i=0;i<8;i++) {
  assert(vote_addresses[3*i]==0x40008u+0x100u*i && vote_values[3*i]==7);
  assert(vote_addresses[3*i+1]==0x40000u+0x100u*i);
  assert(vote_addresses[3*i+2]==0x40004u+0x100u*i && vote_values[3*i+2]==1);
 }
 assert(get(0x17a20d00u)==0xa5u && get(0x17a20d04u)==0);
 assert((get(SW_WIFI_CFG_PA+4)&6u)==2u);case_count++;
 unsigned success_writes=writes;sweet_wifi_init();assert(writes==success_writes);case_count++;

 reset_case();complete_votes=0;
 sweet_wifi_init();
 assert(votes==1 && sw_wifi_rpmh_poisoned);
 assert(strstr(output,"phase 2 failed errno=60 poisoned=1")!=NULL);
 unsigned before=writes;assert(sw_wifi_vote(0x40000u,1)==EIO && writes==before);
 case_count++;

 reset_case();link_ready=0;
 sweet_wifi_init();
 assert(votes==28);
 assert(strstr(output,"phase 2 failed errno=60")!=NULL);
 assert(strstr(output,"wifi pcie v2")==NULL);case_count++;

 reset_case();put(SW_WIFI_CFG_PA,0x11031234u);
 sweet_wifi_init();
 assert(votes==28);
 assert(strstr(output,"phase 2 failed errno=6")!=NULL);
 assert(strstr(output,"wifi pcie v2")==NULL);case_count++;

 reset_case();boot_stage=1;sweet_wifi_init();assert(writes==0 && votes==0);case_count++;
 reset_case();boot_stage=0;sweet_wifi_init();assert(writes==0 && votes==0 && !log_syncs);case_count++;
 reset_case();put(0x17a20d14u,0x10000);sweet_wifi_init();
 assert(writes==0 && !sw_wifi_rpmh_poisoned);case_count++;
 reset_case();put(0x17a20000u,0x30700);sweet_wifi_init();assert(writes==0);case_count++;
 reset_case();put(SW_WIFI_CMD_DB_PA+4,0);sweet_wifi_init();assert(writes==0);case_count++;
 reset_case();stale_complete=1;complete_votes=0;sweet_wifi_init();
 assert(votes==1 && sw_wifi_rpmh_poisoned);case_count++;
 reset_case();all_ones_status=1;sweet_wifi_init();assert(votes==1 && sw_wifi_rpmh_poisoned);case_count++;
 reset_case();cleanup_failed=1;sweet_wifi_init();assert(votes==1 && sw_wifi_rpmh_poisoned);case_count++;
 reset_case();phy_ready=0;sweet_wifi_init();assert(votes==28 && get(SW_WIFI_CFG_PA+4)==0);case_count++;
 reset_case();bar_mask=0xffffffffu;sweet_wifi_init();assert(strstr(output,"wifi pcie v2")==NULL);case_count++;
 reset_case();bar_mask=0xffe10000u;sweet_wifi_init();assert(strstr(output,"wifi pcie v2")==NULL);case_count++;

 printf("{\"passed\":true,\"cases\":%u,\"votes_on_success\":%u,"
        "\"actual_phase2\":true,\"hardware\":\"mock-only\"}\n",case_count,28u);
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
        "fixed physical TCS IRQ addresses independent of driver macros",
        "disabled/read-only phases perform zero writes",
        "busy slot and unknown RSC revision refused",
        "stale/all-ones completion and cleanup failure do not release ownership",
        "PHY timeout and invalid BAR geometry refused",
    ]
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
