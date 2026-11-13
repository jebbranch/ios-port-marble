#!/usr/bin/env python3
"""Compile the real PA probe against host GPIO/controller mocks.

Includes the pre-0290 source as a failing-state control. No device access.
    python3 -I tools/stub_ampguard.py --output-dir verify/ampguard-0290
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / 'chain-tree-i2c'
BASE = '1079cf21c9274f45e9f96f75853e76f60dafb3ff'


def extract(source):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda m: re.sub(r'[^\n]', ' ', m.group()), source, flags=re.S)
    match = re.search(r'^void\nsweet_i2c_probe_amps\(void\)\s*\{', masked, re.M)
    if match is None:
        raise ValueError('missing actual PA probe')
    depth, begin = 0, masked.index('{', match.start())
    for end in range(begin, len(masked)):
        depth += (masked[end] == '{') - (masked[end] == '}')
        if depth == 0:
            return source[match.start():end + 1]
    raise ValueError('unclosed PA probe')


def definitions(source):
    names = ('SW_I2C_TLMM_PA', 'SW_I2C_TLMM_SZ', 'SW_I2C_SCL', 'SW_I2C_SDA',
             'SW_AMP_RST_A_PA', 'SW_AMP_RST_B_PA', 'SW_AMP_SPKSW_PA',
             'SW_GPIO_CFG', 'SW_GPIO_IO')
    result = []
    for name in names:
        match = re.search(r'^#define\s+' + name + r'(?:\([^\n]*?\))?\s+[^\n]+', source, re.M)
        if match is None:
            raise ValueError('missing source constant: ' + name)
        result.append(match.group())
    return '\n'.join(result)


SUPPORT = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#define __asm__(...)
typedef uintptr_t vm_offset_t;
static vm_offset_t sw_i2c_gpio;
static uint32_t pins[1026], spksw_pad[2];
static int se15_probe_calls, sw_i2c_ok;
static int rst_a, rst_b, boot_nobb, disabled, map_fail, init_fail, bad_id, profile_fail;
static int scans, drive_low_after_profile, profile_attempts, init_calls, snapshots;
static int scan_success, halstream, maps;
int PE_parse_boot_argn(const char *name, void *out, unsigned long len) {
    int value;
    if(!strcmp(name,"sweethalstream") && halstream) value=halstream;
    else if(!strcmp(name,"sweetnobb") && boot_nobb >= 0) value=boot_nobb;
    else if(!strcmp(name,"sweeti2c") && disabled) value=0;
    else return 0;
    if(len!=sizeof(value)) abort(); memcpy(out,&value,len); return 1;
}
vm_offset_t ml_io_map(vm_offset_t address, unsigned long size) {
    (void)address; (void)size; maps++; return map_fail ? 0 : (vm_offset_t)pins;
}
vm_offset_t sw_pad_map(uint32_t address) {
    return address == SW_AMP_SPKSW_PA ? (vm_offset_t)spksw_pad : 0;
}
uint32_t sw_rd(uint32_t offset) {
    if(offset/4 >= 1026) abort(); return pins[offset/4];
}
void sw_wr(uint32_t offset, uint32_t value) {
    if(offset/4 >= 1026) abort(); pins[offset/4]=value;
}
void sw_gpio_drive(uint32_t address, bool high) {
    if(address==SW_AMP_RST_A_PA || address==SW_AMP_RST_B_PA) {
        if(!high && profile_attempts) drive_low_after_profile++;
        if(address==SW_AMP_RST_A_PA) rst_a=high; else rst_b=high;
    } else if(address==SW_AMP_SPKSW_PA) {
        spksw_pad[0]=0x200; spksw_pad[1]=high ? 3 : 0;
    }
}
int sw_gpio_read_level(uint32_t address) {
    if(address==SW_AMP_RST_A_PA) return rst_a;
    if(address==SW_AMP_RST_B_PA) return rst_b;
    if(address==SW_AMP_SPKSW_PA) return spksw_pad[1]&1u;
    return 1;
}
void sw_gpio_cfg(unsigned pin, unsigned pull, bool output) {
    pins[SW_GPIO_CFG(pin)/4]=(output ? 0x200u : 0u)+pull;
}
void delay(int usecs) { (void)usecs; }
void sweet_log_sync(void) {}
void sw_scl(int high) { pins[1]=high ? 3 : 0; }
void sw_i2c_delay(void) {}
void sw_i2c_stop(void) {}
void sw_i2c_pins_up(void) { pins[0]=pins[1024]=0x200; }
void sw_i2c_pins_down(void) { pins[0]=pins[1024]=0x100; }
int sw_sda_read(void) { return 1; }
int sw_i2c_scan(const char *tag) { (void)tag; scans++; return scan_success; }
int sw_aw_read(uint8_t addr, uint8_t reg, uint16_t *out) {
    (void)addr; (void)reg; (void)out; return -1;
}
int sw_aw_init(uint8_t addr) { (void)addr; return 0; }
int se15_init(void) {
    init_calls++; if(init_fail) return 0;
    sw_gpio_drive(SW_AMP_RST_A_PA,false); sw_gpio_drive(SW_AMP_RST_A_PA,true);
    sw_gpio_drive(SW_AMP_RST_B_PA,false); sw_gpio_drive(SW_AMP_RST_B_PA,true);
    pins[0]=pins[1024]=0x87; pins[1]=3; pins[1025]=1; return 1;
}
void se15_tdm_pins(void) {}
int se15_aw_read(uint8_t addr, uint8_t reg, uint16_t *out) {
    (void)reg; *out=(bad_id && addr==0x35) ? 0xffff : 0x2032; return 0;
}
int se15_aw_init(uint8_t addr) {
    profile_attempts++; return profile_fail && addr==0x35 ? -1 : 0;
}
void sweet_i2c_amp_readback(const char *tag) {
    if(!strcmp(tag,"post-init")) snapshots++;
}
'''
MAIN = r'''
void reset_case(void) {
    memset(pins,0,sizeof(pins)); pins[0]=pins[1024]=1; pins[1]=pins[1025]=1;
    spksw_pad[0]=0x21; spksw_pad[1]=0;
    rst_a=rst_b=0; boot_nobb=-1; disabled=map_fail=init_fail=bad_id=profile_fail=0;
    scans=drive_low_after_profile=profile_attempts=init_calls=snapshots=0;
    se15_probe_calls=1; sw_i2c_ok=0; scan_success=0;
    halstream=maps=0;
}
void report(const char *label) {
    printf("CASE {\"label\":\"%s\",\"scans\":%d,\"post_profile_reset_low\":%d,"
        "\"rst_a\":%d,\"rst_b\":%d,\"profile_attempts\":%d,\"init_calls\":%d,"
        "\"snapshots\":%d,\"maps\":%d,\"scl_cfg\":%u,\"sda_cfg\":%u,\"scl_io\":%u,"
        "\"sda_io\":%u,\"spksw_cfg\":%u,\"spksw_io\":%u}\n",label,
        scans,drive_low_after_profile,rst_a,rst_b,profile_attempts,init_calls,
        snapshots,maps,pins[0],pins[1024],pins[1],pins[1025],spksw_pad[0],spksw_pad[1]);
}
int main(void) {
    reset_case(); sweet_i2c_probe_amps(); report("default_late");
    reset_case(); se15_probe_calls=0; sweet_i2c_probe_amps(); report("default_early");
    reset_case(); boot_nobb=1; sweet_i2c_probe_amps(); report("explicit_skip");
    reset_case(); init_fail=1; sweet_i2c_probe_amps(); report("init_failure");
    reset_case(); bad_id=1; sweet_i2c_probe_amps(); report("wrong_chip_id");
    reset_case(); profile_fail=1; sweet_i2c_probe_amps(); report("profile_failure");
    reset_case(); boot_nobb=0; sweet_i2c_probe_amps(); report("diagnostic_no_devices");
    reset_case(); boot_nobb=0; scan_success=1; sweet_i2c_probe_amps(); report("diagnostic_device_found");
    reset_case(); boot_nobb=0; se15_probe_calls=0; sweet_i2c_probe_amps(); report("diagnostic_early");
    reset_case(); disabled=1; sweet_i2c_probe_amps(); report("disabled");
    reset_case(); map_fail=1; sweet_i2c_probe_amps(); report("mapping_failure");
    reset_case(); halstream=1; sweet_i2c_probe_amps(); report("halstream_bypass");
    return 0;
}
'''


def execute(source, directory, label):
    directory.mkdir(parents=True, exist_ok=True)
    cpath = directory / (label + '.c')
    exe = directory / (label + '.host')
    # Constants are inserted before mocks because the mocks refer to GPIO IDs.
    split = SUPPORT.index('int PE_parse_boot_argn')
    code = SUPPORT[:split] + definitions(source) + '\n' + SUPPORT[split:] + extract(source) + '\n' + MAIN
    cpath.write_text(code)
    flags = ['-std=gnu11', '-Wall', '-Wextra', '-Wformat=2', '-Werror']
    compiled = subprocess.run(['clang', *flags, str(cpath), '-o', str(exe)], capture_output=True, text=True)
    if compiled.returncode:
        raise RuntimeError(compiled.stderr)
    subprocess.run(['clang', *flags, '-fsyntax-only', str(cpath)], check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    rows, guard = [], None
    for line in run.stdout.splitlines():
        match = re.search(r'ampguard v1 pass=(\d+) skip=(-?\d+) init=(\d+) profiles=(\d+) rstA=(-?\d+) rstB=(-?\d+)', line)
        if match:
            guard = dict(zip(('pass', 'skip', 'init', 'profiles', 'rst_a', 'rst_b'),
                             map(int, match.groups())))
        if line.startswith('CASE '):
            row = json.loads(line[5:])
            row['guard'] = guard
            rows.append(row)
            guard = None
    if len(rows) != 12:
        raise ValueError('not all probe paths were exercised')
    return code, run.stdout, {row['label']: row for row in rows}


def check(new, old):
    bypass = new['halstream_bypass']
    assert bypass['maps'] == bypass['profile_attempts'] == bypass['init_calls'] == 0
    assert bypass['scans'] == bypass['snapshots'] == 0
    assert (bypass['rst_a'], bypass['rst_b']) == (0, 0)
    assert bypass['guard'] is None
    assert old['default_late']['post_profile_reset_low'] == 2
    assert (old['default_late']['rst_a'], old['default_late']['rst_b']) == (0, 0)
    for name in ('default_late', 'explicit_skip', 'wrong_chip_id', 'profile_failure'):
        row = new[name]
        assert row['scans'] == row['post_profile_reset_low'] == 0
        assert (row['rst_a'], row['rst_b']) == (1, 1)
        assert row['snapshots'] == 1
        assert row['scl_cfg'] == row['sda_cfg'] == 0x87
    assert new['default_late']['guard']['skip'] == 1
    assert new['default_late']['guard']['init'] == 1
    assert new['default_late']['guard']['profiles'] == 2
    assert new['wrong_chip_id']['guard']['profiles'] == 1
    assert new['profile_failure']['guard']['profiles'] == 1
    assert new['init_failure']['guard']['init'] == 0
    assert new['default_early']['guard']['pass'] == 1
    assert new['default_late']['guard']['pass'] == 2
    assert new['wrong_chip_id']['profile_attempts'] == 1
    assert new['profile_failure']['profile_attempts'] == 2
    for name in ('default_early', 'init_failure', 'disabled', 'mapping_failure'):
        row = new[name]
        assert row['scans'] == row['profile_attempts'] == row['post_profile_reset_low'] == row['snapshots'] == 0
    for name, scans in (('diagnostic_no_devices', 2), ('diagnostic_device_found', 1)):
        row = new[name]
        assert row['scans'] == scans and row['post_profile_reset_low'] == 0
        assert (row['rst_a'], row['rst_b']) == (1, 1)
        assert (row['scl_cfg'], row['sda_cfg'], row['scl_io'], row['sda_io']) == (0x87, 0x87, 3, 1)
        assert (row['spksw_cfg'], row['spksw_io']) == (0x21, 0)
    assert new['diagnostic_early']['scans'] == 2
    assert new['diagnostic_early']['scl_cfg'] == new['diagnostic_early']['sda_cfg'] == 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=CHAIN / 'bsd/dev/sweet_i2c.c')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    current = args.source.read_text()
    old = subprocess.check_output(['git', '-C', str(CHAIN), 'show', BASE + ':bsd/dev/sweet_i2c.c'], text=True)
    with tempfile.TemporaryDirectory(prefix='ampguard-test-') as temp:
        folder = Path(temp)
        new_code, new_log, new_rows = execute(current, folder, 'new')
        old_code, old_log, old_rows = execute(old, folder, 'old')
        check(new_rows, old_rows)
        result = {'passed': True, 'cases': len(new_rows), 'old_default_late': old_rows['default_late'],
                  'new': list(new_rows.values()),
                  'scope': 'Host GPIO/controller mocks only; no real PA state or sound claim.'}
        if args.output_dir:
            args.output_dir.mkdir(parents=True, exist_ok=True)
            (args.output_dir / 'new.c').write_text(new_code)
            (args.output_dir / 'old.c').write_text(old_code)
            (args.output_dir / 'host.log').write_text(new_log + '\n--- baseline ---\n' + old_log)
            (args.output_dir / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
