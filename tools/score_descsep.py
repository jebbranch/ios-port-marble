#!/usr/bin/env python3
"""Fail-closed image/ring gates for the registered 0289 paired audio tests.

Reads artifacts only. It neither boots a device nor treats ACK as audible sound.
    python3 -I tools/score_descsep.py IMAGE --value 0 --build-log LOG
    python3 -I tools/score_descsep.py IMAGE --value 1 --build-log LOG --ring LABEL
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parent.parent
MARKERS = (b'SWEET MARK: descsep v1 on=%d off=%#x len=%u',
           b'SWEET MARK: dcnv v2 on=%d bytes=%u start=%#x',
           b'SWEET MARK: rxall v1 ')
FLAGS = {'cpus': '1', 'sweetbufs': '120', 'sweetpcm': '1', 'sweets1': '1',
         'sweetiova': '1', 'sweets1va': '0x1FFE8000', 'sweetshmhi': '1',
         'sweets1ap': '3', 'sweetampfirst': '1', 'sweetvol': '16',
         'sweettdmvendor': '7', 'sweetvgraph7': '1', 'sweetrxlog': '1'}
FORBIDDEN = ('sweeteos', 'sweetvgraph8', 'sweetvgraph6', 'sweetvgraph',
             'sweetvcfg2', 'sweetnowait', 'sweetdbb', 'sweetcccfg', 'sweetoobcfg',
             'sweetshmgraph', 'sweetmfc', 'sweetep4308')


def payload_cmdline(image):
    candidates, offset = [], 0
    while True:
        pos = image.find(b'SWEETXNU', offset)
        if pos < 0:
            break
        offset = pos + 8
        if pos + 0x2a8 > len(image):
            continue
        version, segments = struct.unpack_from('<II', image, pos + 8)
        field = image[pos + 0x48:pos + 0x2a8]
        if version != 1 or not 1 <= segments <= 64 or b'\0' not in field:
            continue
        command = field.split(b'\0', 1)[0]
        if command.startswith(b'debug=0xc serial=3 cpus='):
            candidates.append((pos, command.decode('ascii')))
    if len(candidates) != 1:
        raise ValueError(f'expected one real payload header, got {len(candidates)}')
    return candidates[0]


def image_gate(path, value, build_log):
    data = path.read_bytes()
    offset, command = payload_cmdline(data)
    flags, duplicates = {}, []
    for token in command.split():
        if '=' not in token:
            continue
        key, val = token.split('=', 1)
        if key in flags:
            duplicates.append(key)
        flags[key] = val
    requirements = dict(FLAGS, sweetdescsep=str(value))
    checks = {
        'cmdline_budget': len(command.encode()) <= 590,
        'unique_keys': not duplicates,
        'expected_flags': all(flags.get(k) == v for k, v in requirements.items()),
        'no_other_experiment_flags': not any(k in flags for k in FORBIDDEN),
        'timestamp': bool(re.fullmatch(r'\d{10}', flags.get('sweettime', ''))),
        'markers': all(data.count(marker) == 1 for marker in MARKERS),
    }
    if build_log:
        lines = re.findall(r'^cmdline: (.+)$', build_log.read_text(), re.M)
        checks['build_log_matches_payload'] = lines == [command]
    return {'image': str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path),
            'sha256': hashlib.sha256(data).hexdigest(), 'payload_offset': offset,
            'cmdline': command, 'cmdline_bytes': len(command.encode()),
            'sweettime': flags.get('sweettime'), 'descsep': value,
            'image_checks': checks, 'image_valid': all(checks.values())}


def ring_gate(label, expected_time, value):
    raw_path = ROOT / 'logs/raw' / f'ring-{label}.bin'
    text_path = ROOT / 'logs' / f'ring-{label}.txt'
    raw, saved = raw_path.read_bytes(), text_path.read_bytes()
    if len(raw) < 4096 or raw[:8] != b'SWEETLOG':
        raise ValueError('raw ring lacks SWEETLOG header')
    version, size = struct.unpack_from('<II', raw, 8)
    total, seq = struct.unpack_from('<QQ', raw, 16)
    if version != 1 or size == 0 or len(raw) < 4096 + size:
        raise ValueError('invalid/truncated raw ring geometry')
    region = raw[4096:4096 + size]
    start = max(0, total - size)
    first = start % size
    n = total - start
    decoded = (region[first:] + region[:first])[:n]
    text = decoded.decode('utf-8', errors='replace')
    stamps = re.findall(r'\bsweettime=(\d+)', text)
    opens = re.findall(r'SWEET GLINK: APM (graph_open/[^\s]+)', text)
    witnesses = re.findall(r'SWEET MARK: descsep v1 on=(\d+) off=(\w+) len=(\d+)', text)
    done = len(re.findall(r'SWEET MARK: rxall v1 op=0x5001004\b', text))
    checks = {
        'decoded_window_matches_file': decoded == saved,
        'timestamp_matches_image': bool(stamps) and stamps[0] == expected_time,
        'one_expected_graph_no_fallback': opens == ['graph_open/dec-cnv'],
        'graph_witness': text.count('SWEET MARK: dcnv v2 on=1 bytes=856 start=0xb0000002') == 1,
        'descriptor_witness': witnesses == ([('1', '0x4000', '856')] if value else []),
        'not_refused': 'SWEET MARK: descsep v1 refused' not in text,
        'open_ok': bool(re.search(r'graph_open/dec-cnv rsp op=0x1001000 status=0\b', text)),
        'start_ok': bool(re.search(r'graph_start rsp op=0x1001002 status=0\b', text)),
        'started': text.count('SWEET GLINK: dec-cnv started') == 1,
        'tone_120': text.count('pcm tone done (120 buffers)') == 1,
        'timeout_zero': bool(re.search(r'dblat v2 tone n=120[^\n]*\btimeout=0\b', text)),
        'done_121': done == 121,
        'receive_positive_controls': 'rxall v1 op=0x2001005 ' in text and 'rxall v1 op=0x2001001 ' in text,
        'no_eos_sent': 'SWEET GLINK: APM wr_eos/last' not in text,
        'eos_disabled_witness': text.count('SWEET MARK: eos v2 on=0 dst=0x40bd policy=1') == 1,
        'tone_gain': 'SWEET MARK: tone gain g=16 peak=16384' in text,
        'normal_end': '===== done =====' in text and 'PSCI SYSTEM_RESET' in text,
        'no_panic': not bool(re.search(r'panic\(cpu|Kernel panic|Debugger called: <panic>', text)),
    }
    return {'ring': label, 'raw_sha256': hashlib.sha256(raw).hexdigest(),
            'version': version, 'size': size, 'total': total, 'seq': seq,
            'decoded_bytes': len(decoded), 'graph_opens': opens, 'done_count': done,
            'descriptor_witnesses': witnesses, 'ring_checks': checks,
            'ring_valid': all(checks.values()), 'audible_sound': 'unknown; requires human observation'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--value', type=int, choices=(0, 1), required=True)
    parser.add_argument('--build-log', type=Path)
    parser.add_argument('--ring')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = image_gate(args.image.resolve(), args.value, args.build_log)
    if args.ring:
        result.update(ring_gate(args.ring, result['sweettime'], args.value))
    result['valid'] = result['image_valid'] and result.get('ring_valid', True)
    output = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        args.output.write_text(output)
    print(output, end='')
    sys.exit(0 if result['valid'] else 1)


if __name__ == '__main__':
    main()
