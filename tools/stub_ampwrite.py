#!/usr/bin/env python3
"""Host transaction-model checks for AW882xx register WRITE framing.

Uses the actual read/write/init functions. No device or real PA-state claim.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CHAIN = ROOT.parent / 'chain-tree-i2c'
BASE = '90562565206a2f39092c22b68b7aa22dbb026d53'


def function(source, name):
    clean = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                   lambda m: re.sub(r'[^\n]', ' ', m.group()), source, flags=re.S)
    match = re.search(r'^static int\s*\n' + name + r'\([^;]+?\)\s*\{', clean, re.M)
    if not match:
        raise ValueError('missing actual function ' + name)
    begin, depth = clean.index('{', match.start()), 0
    for end in range(begin, len(clean)):
        depth += (clean[end] == '{') - (clean[end] == '}')
        if depth == 0:
            return source[match.start():end + 1]
    raise ValueError('unclosed ' + name)


SUPPORT = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
static uint16_t regs[256];
static uint8_t pointer_reg, last_tx[3];
static unsigned calls, last_nt, last_nr;
static int last_stretch, force_error;
static void delay(int us) { (void)us; }
void sweet_log_sync(void) {}
static int se15_msg(uint8_t addr, const uint8_t *tx, unsigned nt,
    uint8_t *rx, unsigned nr, bool stretch) {
    (void)addr; calls++; last_nt=nt; last_nr=nr; last_stretch=stretch;
    memset(last_tx,0,sizeof(last_tx));
    if(nt>3 || (nt && nr)) abort();
    if(nt) memcpy(last_tx,tx,nt);
    if(force_error) return force_error;
    if(nt) {
        pointer_reg=tx[0];
        if(nt==3) regs[tx[0]]=(uint16_t)((tx[1]<<8)|tx[2]);
    } else if(nr==2) {
        rx[0]=(uint8_t)(regs[pointer_reg]>>8); rx[1]=(uint8_t)regs[pointer_reg];
    }
    return 0;
}
static void reset_model(void) {
    memset(regs,0,sizeof(regs)); regs[0]=0x2032; regs[4]=0x4003; regs[5]=0x38;
    pointer_reg=0; calls=last_nt=last_nr=0; force_error=0;
}
'''
MAIN = r'''
int main(void) {
    reset_model();
    int rc=se15_aw_write(0x34,4,0xb440);
    printf("ROW {\"kind\":\"write\",\"rc\":%d,\"calls\":%u,\"nt\":%u,"
        "\"nr\":%u,\"stretch\":%d,\"tx\":[%u,%u,%u],\"readback\":%u}\n",
        rc,calls,last_nt,last_nr,last_stretch,last_tx[0],last_tx[1],last_tx[2],regs[4]);
    reset_model(); uint16_t out=0; rc=se15_aw_read(0x35,0,&out);
    printf("ROW {\"kind\":\"read\",\"rc\":%d,\"calls\":%u,\"value\":%u}\n",rc,calls,out);
    reset_model(); rc=se15_aw_init(0x34);
    printf("ROW {\"kind\":\"profile\",\"rc\":%d,\"calls\":%u,"
        "\"sysctrl\":%u,\"sysctrl2\":%u}\n",rc,calls,regs[4],regs[5]);
    reset_model(); force_error=-2; rc=se15_aw_write(0x34,4,0xb440);
    printf("ROW {\"kind\":\"error\",\"rc\":%d,\"calls\":%u,\"readback\":%u}\n",rc,calls,regs[4]);
    return 0;
}
'''


def execute(source, folder, label):
    table = re.search(r'static const struct \{.*?\} aw882xx_music_a\[44\] = \{.*?\n\};', source, re.S)
    if not table:
        raise ValueError('missing real profile table')
    code = SUPPORT + table.group() + '\n' + '\n'.join(function(source, n) for n in
        ('se15_aw_read', 'se15_aw_write', 'se15_aw_init')) + '\n' + MAIN
    path, exe = folder / (label + '.c'), folder / label
    path.write_text(code)
    flags = ['-std=gnu11', '-Wall', '-Wextra', '-Wformat=2', '-Werror']
    subprocess.run(['clang', *flags, str(path), '-o', str(exe)], check=True)
    subprocess.run(['clang', *flags, '-fsyntax-only', str(path)], check=True)
    output = subprocess.check_output([str(exe)], text=True)
    rows = [json.loads(line[4:]) for line in output.splitlines() if line.startswith('ROW ')]
    if len(rows) != 4:
        raise ValueError('not all transaction cases exercised')
    return code, output, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    new = (CHAIN / 'bsd/dev/sweet_i2c.c').read_text()
    old = subprocess.check_output(['git', '-C', str(CHAIN), 'show', BASE + ':bsd/dev/sweet_i2c.c'], text=True)
    with tempfile.TemporaryDirectory(prefix='ampwrite-test-') as name:
        folder = Path(name)
        nc, nl, nr = execute(new, folder, 'new')
        oc, ol, old_rows = execute(old, folder, 'old')
        assert nr[0]['calls'] == 1 and nr[0]['nt'] == 3 and nr[0]['nr'] == 0 and nr[0]['stretch'] == 0
        assert nr[0]['tx'] == [4, 0xb4, 0x40] and nr[0]['readback'] == 0xb440
        assert old_rows[0]['calls'] == 2 and old_rows[0]['readback'] == 0x4003
        assert nr[1] == old_rows[1] == {'kind': 'read', 'rc': 0, 'calls': 2, 'value': 0x2032}
        assert nr[2]['rc'] == 0 and nr[2]['calls'] == 45 and nr[2]['sysctrl'] == 0xb440 and nr[2]['sysctrl2'] == 0x05a9
        assert old_rows[2]['rc'] == 0 and old_rows[2]['calls'] == 90 and old_rows[2]['sysctrl'] == 0x4003
        assert nr[3]['rc'] == -1 and nr[3]['calls'] == 1 and nr[3]['readback'] == 0x4003
        result = {'passed': True, 'new': nr, 'old': old_rows,
                  'scope': 'Each host I2C message is a frame per vendor writer; model is not real hardware proof.'}
        if args.output_dir:
            args.output_dir.mkdir(parents=True, exist_ok=True)
            (args.output_dir / 'new.c').write_text(nc)
            (args.output_dir / 'old.c').write_text(oc)
            (args.output_dir / 'host.log').write_text(nl + '\n--- baseline ---\n' + ol)
            (args.output_dir / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
