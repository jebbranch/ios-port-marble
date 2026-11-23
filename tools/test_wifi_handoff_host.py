#!/usr/bin/env python3
"""Compile the actual 0299 read-only Wi-Fi survey against an MMIO model."""
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT.parent / "chain-tree-wifi/bsd/dev/sweet_wifi.c"
PHY = ROOT.parent / "chain-tree-wifi/bsd/dev/sweet_wifi_phy.h"


def body(text):
    lines = []
    for line in text.splitlines():
        if line.startswith("#include") or line.startswith("#if") or line.startswith("#endif"):
            continue
        lines.append(line)
    return "\n".join(lines)


SUPPORT = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
static uint32_t gcc_words[0x100000/4];
static uint32_t tlmm_words[0x100000/4];
static int boot_stage,map_failure,map_calls,log_syncs;
static char output[4096];
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
static int PE_parse_boot_argn(const char *name,void *value,int bytes) {
 assert(!strcmp(name,"sweetwifi") && bytes==4);
 memcpy(value,&boot_stage,4);return boot_stage!=0;
}
static vm_offset_t ml_io_map(vm_offset_t physical,vm_size_t bytes) {
 map_calls++;assert(bytes==0x1000);
 if(map_failure)return 0;
 if(physical>=0x00100000u && physical<0x00200000u)
  return (vm_offset_t)(uintptr_t)((uint8_t *)gcc_words+physical-0x00100000u);
 if(physical>=0x0f100000u && physical<0x0f200000u)
  return (vm_offset_t)(uintptr_t)((uint8_t *)tlmm_words+physical-0x0f100000u);
 assert(0);return 0;
}
static void sweet_log_sync(void){log_syncs++;}
void IOSleep(unsigned ms){(void)ms;}
void IODelay(unsigned us){(void)us;}
static void delay(unsigned us){(void)us;}
'''


TESTS = r'''
static void reset_case(int stage) {
 memset(gcc_words,0,sizeof(gcc_words));memset(tlmm_words,0,sizeof(tlmm_words));
 memset(sw_wifi_pages,0,sizeof(sw_wifi_pages));
 sw_wifi_started_once=false;sw_wifi_milestone=0;
 boot_stage=stage;map_failure=map_calls=log_syncs=0;output_at=0;output[0]=0;
}
int main(void) {
 reset_case(0);sweet_wifi_init();
 assert(map_calls==0 && log_syncs==0 && output_at==0);

 reset_case(1);
 gcc_words[SW_WIFI_GDSC/4]=0xf81f0001u;
 gcc_words[sw_wifi_branches[0]/4]=1u<<31;
 gcc_words[sw_wifi_branches[3]/4]=1u<<31;
 tlmm_words[(SW_WIFI_WLAN_GPIO*SW_WIFI_GPIO_STRIDE+SW_WIFI_GPIO_IO)/4]=2;
 tlmm_words[(SW_WIFI_PERST_GPIO*SW_WIFI_GPIO_STRIDE+SW_WIFI_GPIO_IO)/4]=0;
 sweet_wifi_init();
 assert(map_calls==4 && log_syncs==1);
 assert(strstr(output,"readonly=1 stage=1")!=NULL);
 assert(strstr(output,"gdsc=0xf81f0001")!=NULL);
 assert(strstr(output,"halted=0x9")!=NULL);
 assert(strstr(output,"wlan=0x2 perst=0")!=NULL);

 reset_case(3);sweet_wifi_init();
 assert(map_calls==0 && log_syncs==1);
 assert(strstr(output,"invalid phase 3")!=NULL);

 reset_case(1);map_failure=1;sweet_wifi_init();
 assert(map_calls>=1 && log_syncs==1);
 assert(strstr(output,"readonly page mapping failed")!=NULL);

 printf("{\"passed\":true,\"cases\":4,\"readonly\":true,"
        "\"source\":\"actual sweet_wifi.c\",\"device_access\":false}\n");
 fputs(output,stdout);
 return 0;
}
'''


def main():
    source = SOURCE.read_text()
    readonly_prefix = source.split("static void\nsw_wifi_write", 1)[0]
    forbidden = (
        "sw_wifi_write", "sweet_rpmh_vreg_enable", "kmem_alloc",
        "kmem_alloc_contig", "kernel_thread_start",
    )
    if any(item in readonly_prefix for item in forbidden):
        raise ValueError("read-only phase contains a mutation/allocation entry")
    marker = (
        "SWEET MARK: wifi handoff v1 readonly=1 stage=%d gdsc=%#x "
        "halted=%#x wlan=%#x perst=%#x"
    )
    if source.count(marker) != 1:
        raise ValueError("0299 marker must be unique")
    code = SUPPORT + "\n" + body(PHY.read_text()) + "\n" + body(source) + "\n" + TESTS
    with tempfile.TemporaryDirectory(prefix="wifi-handoff-") as name:
        cfile = Path(name) / "test.c"
        exe = Path(name) / "test"
        cfile.write_text(code)
        subprocess.run([
            "clang", "-std=gnu11", "-Wall", "-Wextra", "-Wformat=2",
            "-Werror", "-fsanitize=address,undefined", str(cfile),
            "-o", str(exe),
        ], check=True)
        run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    result = json.loads(run.stdout.splitlines()[-1])
    result["checks"] = {
        "disabled_zero_effect": True,
        "readonly_values": True,
        "unsupported_stage_refused": True,
        "mapping_failure_safe": True,
        "forbidden_mutators_absent": True,
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
