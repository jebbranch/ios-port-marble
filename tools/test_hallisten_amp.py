#!/usr/bin/env python3
"""Compile the actual AW88266 listen state machine against a register model."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT.parent / "chain-tree-i2c/bsd/dev/sweet_i2c.c"


def masked(source):
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                  lambda match: re.sub(r"[^\n]", " ", match.group()),
                  source, flags=re.S)


def functions(source, names):
    clean = masked(source)
    out = []
    for name in names:
        match = re.search(
            r"^(?:static\s+)?(?:int|void)\s*\n" + re.escape(name) +
            r"\([^;]+?\)\s*\{", clean, re.M)
        if match is None:
            raise ValueError("missing actual function " + name)
        begin = clean.index("{", match.start())
        depth = 0
        for end in range(begin, len(clean)):
            depth += (clean[end] == "{") - (clean[end] == "}")
            if depth == 0:
                out.append(source[match.start():end + 1])
                break
        else:
            raise ValueError("unclosed " + name)
    return "\n".join(out)


SUPPORT = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SWEET_MARKER(...)
#define SW_AMP_SPKSW_PA 0x0f134000u
#define SW_AMP_RST_A_PA 0x0f101000u
#define SW_AMP_RST_B_PA 0x0f178000u
static uint16_t regs[2][256];
static int writes[2][256], reads[2][256], sequence[4096], sequence_n;
static int init_fail, fail_addr, fail_reg, fail_kind, pll_fail, status_fault;
static int tdm_pins, spksw, powerdown_writes, first_unmute_seq[2], both_healthy_seq;
static int reset_a,reset_b;
static int amp_index(uint8_t addr){return addr==0x34?0:addr==0x35?1:-1;}
static void delay(int us){(void)us;}
static void sweet_log_sync(void){}
static bool se15_init(void){return !init_fail;}
static void se15_tdm_pins(void){tdm_pins++;}
static void sw_gpio_drive(uint32_t pa,bool high){
 if(pa==SW_AMP_SPKSW_PA)spksw=high?1:0;
 else if(pa==SW_AMP_RST_A_PA)reset_a=high?1:0;
 else if(pa==SW_AMP_RST_B_PA)reset_b=high?1:0;
 else assert(0);
}
static int sw_gpio_read_level(uint32_t pa){
 if(pa==SW_AMP_SPKSW_PA)return spksw;
 if(pa==SW_AMP_RST_A_PA)return reset_a;
 if(pa==SW_AMP_RST_B_PA)return reset_b;
 assert(0);return -1;
}
static int se15_aw_read(uint8_t addr,uint8_t reg,uint16_t *out){
 int a=amp_index(addr);assert(a>=0);reads[a][reg]++;
 if(fail_kind==1 && addr==fail_addr && reg==fail_reg)return -1;
 if(reg==AW_SYSST) {
  if(pll_fail==addr)*out=0;
  else *out=(uint16_t)(AW_PLL_OK | (status_fault==addr?AW_NOCLKS:0));
 } else *out=regs[a][reg];
 return 0;
}
static int se15_aw_write(uint8_t addr,uint8_t reg,uint16_t value){
 int a=amp_index(addr);assert(a>=0);writes[a][reg]++;
 sequence[sequence_n++]=(addr<<16)|(reg<<8)|(value&0xff);
 if(fail_kind==2 && addr==fail_addr && reg==fail_reg)return -1;
 if(reg==0 && value==0x55aa) {
  uint16_t id=regs[a][0];memset(regs[a],0,sizeof(regs[a]));regs[a][0]=id;
 } else regs[a][reg]=value;
 if(reg==AW_SYSCTRL) {
  uint16_t off=AW_PWDN|AW_AMPPD|AW_HMUTE|AW_ULS_HMUTE;
  if((value&off)==0 && !first_unmute_seq[a])first_unmute_seq[a]=sequence_n;
  if((value&(AW_PWDN|AW_AMPPD))==(AW_PWDN|AW_AMPPD))powerdown_writes++;
 }
 return 0;
}
'''

TESTS = r'''
static void reset_case(void){
 memset(regs,0,sizeof(regs));memset(writes,0,sizeof(writes));memset(reads,0,sizeof(reads));
 regs[0][0]=regs[1][0]=0x2032;sequence_n=0;init_fail=fail_addr=fail_reg=fail_kind=0;
 pll_fail=status_fault=0;tdm_pins=0;spksw=1;powerdown_writes=0;
 first_unmute_seq[0]=first_unmute_seq[1]=both_healthy_seq=0;
 reset_a=reset_b=1;
 listen_ready=listen_started=0;listen_i2sctrl3[0]=listen_i2sctrl3[1]=0;
}
static void assert_off(void){
 uint16_t off=AW_PWDN|AW_AMPPD|AW_HMUTE|AW_ULS_HMUTE;
 assert((regs[0][AW_SYSCTRL]&off)==off);
 assert((regs[1][AW_SYSCTRL]&off)==off);
 assert(!listen_started);
}
static void success(void){
 reset_case();assert(sweet_i2c_listen_start()==0);assert(listen_started);
 assert(spksw==0 && tdm_pins==1);
 assert(regs[0][0x05]==0x05a9 && regs[1][0x05]==0x05a9);
 assert(first_unmute_seq[0] && first_unmute_seq[1]);
 assert(reads[0][AW_SYSST]>=3 && reads[1][AW_SYSST]>=3);
 sweet_i2c_listen_stop();assert_off();assert(powerdown_writes>=2);
}
static void fail_pll(int addr){
 reset_case();pll_fail=addr;assert(sweet_i2c_listen_start()!=0);assert_off();
 assert(first_unmute_seq[0]==0 && first_unmute_seq[1]==0);
}
static void fail_status(int addr){
 reset_case();status_fault=addr;assert(sweet_i2c_listen_start()!=0);assert_off();
 assert(first_unmute_seq[0]==0 && first_unmute_seq[1]==0);
}
int main(void){
 success();
 fail_pll(0x34);fail_pll(0x35);fail_status(0x34);fail_status(0x35);
 reset_case();regs[1][0]=0xffff;assert(sweet_i2c_listen_start()!=0);assert_off();
 reset_case();init_fail=1;assert(sweet_i2c_listen_start()!=0);
 assert(reset_a==0 && reset_b==0);
 reset_case();fail_kind=2;fail_addr=0x35;fail_reg=0x05;assert(sweet_i2c_listen_start()!=0);assert_off();
 reset_case();fail_kind=1;fail_addr=0x34;fail_reg=AW_SYSCTRL;assert(sweet_i2c_listen_start()!=0);assert_off();
 assert(reset_a==0 && reset_b==0);
 reset_case();spksw=1;assert(sweet_i2c_listen_start()==0);assert(spksw==0);sweet_i2c_listen_stop();
 printf("{\"passed\":true,\"cases\":10,\"chip\":\"AW88266\","
   "\"speaker_switch\":0,\"volume_reg\":\"0x05a9\","
   "\"failure_paths_power_down_both\":true}\n");
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    source = SOURCE.read_text()
    table = re.search(
        r"static const struct \{.*?\} aw882xx_music_a\[44\] = \{.*?\n\};",
        source, re.S)
    block = re.search(
        r"#define AW_SYSST.*?static uint16_t listen_i2sctrl3\[2\];",
        source, re.S)
    if table is None or block is None:
        raise ValueError("listen constants or exact 44-register table missing")
    names = (
        "se15_aw_ctrl", "sweet_i2c_listen_powerdown", "se15_aw_init_safe",
        "sweet_i2c_listen_start", "sweet_i2c_listen_stop",
    )
    # Mocks refer to constants, while production functions follow the mocks.
    code = (
        "#include <stdint.h>\n#include <stdbool.h>\n#include <stdio.h>\n"
        "#include <string.h>\n#include <assert.h>\n" +
        table.group() + "\n" + block.group() + "\n" + SUPPORT +
        functions(source, names) + "\n" + TESTS)
    with tempfile.TemporaryDirectory(prefix="hallisten-amp-") as folder:
        cfile, exe = Path(folder) / "test.c", Path(folder) / "test"
        cfile.write_text(code)
        flags = [
            "-std=gnu11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
            "-fsanitize=address,undefined",
        ]
        subprocess.run(["clang", *flags, str(cfile), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    result = json.loads(run.stdout.splitlines()[-1])
    result["scope"] = "actual listen state functions and exact 44-register table; I2C/MMIO mocked"
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "amp-host.log").write_text(run.stdout)
        (args.out_dir / "amp-host.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
