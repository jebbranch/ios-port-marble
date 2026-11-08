#!/usr/bin/env python3
"""0290 PA-guard image/ring checks, separate from actual sound or PLL readiness."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location('score_descsep', ROOT / 'tools/score_descsep.py')
base = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(base)
MARKER = b'SWEET MARK: ampguard v1 pass=%u skip=%d init=%d profiles=%d rstA=%d rstB=%d'


def amp_gate(text):
    guards = [tuple(map(int, row)) for row in re.findall(
        r'SWEET MARK: ampguard v1 pass=(\d+) skip=(-?\d+) init=(\d+) profiles=(\d+) rstA=(-?\d+) rstB=(-?\d+)', text)]
    readback = []
    for row in re.findall(r'SWEET MARK: ampread post-init addr=(0x[0-9a-f]+) reg=(0x[0-9a-f]+|0) r=(-?\d+) val=(0x[0-9a-f]+|0)', text):
        addr, reg, status, value = row
        readback.append({'addr': int(addr, 0), 'reg': int(reg, 0), 'status': int(status), 'value': int(value, 0)})
    geometry = {(row['addr'], row['reg']) for row in readback}
    expected = {(addr, reg) for addr in (0x34, 0x35) for reg in (0, 3, 4, 5)}
    checks = {
        'early_guard': len(guards) == 2 and guards[0][:4] == (1, 1, 0, 0),
        'late_guard': len(guards) == 2 and guards[1] == (2, 1, 1, 2, 1, 1),
        'bitbang_skip_two': text.count('SWEET MARK: bitbang skipped') == 2,
        'no_bitbang_scan': not any(s in text for s in ('SWEET I2C: released scan', 'SWEET I2C: spksw-high scan', 'SWEET I2C: asserted scan', 'SWEET I2C: sda driven', 'SWEET I2C: scl driven')),
        'chip_ids': all(text.count(f'SWEET I2C: geni amp {addr:#x} r=0 id=0x2032') == 1 for addr in (0x34, 0x35)),
        'profile_batches': all(text.count(f'SWEET I2C: geni amp {addr:#x} profile 44/44') == 1 for addr in (0x34, 0x35)),
        'readback_geometry': len(readback) == 8 and geometry == expected,
        'readback_success': len(readback) == 8 and all(row['status'] == 0 for row in readback),
        'readback_ids': len(readback) == 8 and all(row['status'] == 0 and row['value'] == 0x2032 for row in readback if row['reg'] == 0),
    }
    states = []
    for addr in (0x34, 0x35):
        rows = {row['reg']: row for row in readback if row['addr'] == addr}
        known = all(reg in rows and rows[reg]['status'] == 0 for reg in (4, 5))
        state = {'addr': addr, 'known': known}
        if known:
            sysctrl, sysctrl2 = rows[4]['value'], rows[5]['value']
            state.update(sysctrl=sysctrl, sysctrl2=sysctrl2,
                         pwdn=sysctrl & 1, amppd=(sysctrl >> 1) & 1,
                         hmute=(sysctrl2 >> 4) & 1)
        states.append(state)
    clear = all(row['known'] and row['pwdn'] == row['amppd'] == row['hmute'] == 0 for row in states)
    return {'amp_checks': checks, 'amp_guard_valid': all(checks.values()),
            'amp_guards': guards, 'post_init_readback': readback,
            'post_init_states': states, 'power_mute_bits_clear': clear,
            'state_scope': 'Post-init bits only; not PLL/I2S lock, rendering or audible output.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--build-log', type=Path)
    parser.add_argument('--ring')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = base.image_gate(args.image.resolve(), 1, args.build_log)
    result['image_checks']['ampguard_marker'] = args.image.read_bytes().count(MARKER) == 1
    result['image_valid'] = all(result['image_checks'].values())
    if args.ring:
        result.update(base.ring_gate(args.ring, result['sweettime'], 1))
        text = (ROOT / 'logs' / f'ring-{args.ring}.txt').read_text(errors='replace')
        result.update(amp_gate(text))
    result['valid'] = result['image_valid'] and result.get('ring_valid', True) and result.get('amp_guard_valid', True)
    output = json.dumps(result, indent=2, ensure_ascii=False) + '\n'
    if args.output:
        args.output.write_text(output)
    print(output, end='')
    sys.exit(0 if result['valid'] else 1)


if __name__ == '__main__':
    main()
