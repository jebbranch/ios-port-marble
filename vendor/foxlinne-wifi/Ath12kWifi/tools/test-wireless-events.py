#!/usr/bin/env python3
"""Check the native decoder against the pinned supplicant UAPI and event ABI."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
root = Path(__file__).resolve().parents[1]
out = root / 'out/wireless-event-test'
out.mkdir(exist_ok=True)
(out / 'report.json').unlink(missing_ok=True)
if not (root / 'out/wpa-src/src/drivers/nl80211_copy.h').is_file():
    subprocess.run(['python3', root / 'tools/prepare-supplicant.py'], check=True)
sources = [root / name for name in ('tests/wireless_event_test.c', 'userspace/nw_wireless_event.c', 'src/nw_control_wire.c')]
flags = ['-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
for name in ('userspace', 'include', 'out/wpa-src/src', 'out/wpa-src/src/utils', 'out/wpa-src/src/drivers'):
    flags += ['-I', str(root / name)]
subprocess.run([shutil.which('clang'), *flags, *map(str, sources), '-o', str(out / 'wireless-events')], check=True)
result = subprocess.run([str(out / 'wireless-events')], text=True, capture_output=True,
    env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
print(result.stdout, end=''); print(result.stderr, end='')
if result.returncode: raise SystemExit(result.returncode)
(out / 'report.json').write_text(json.dumps({'passed': True, 'hardware_verified': False,
    'sanitizers': ['address', 'undefined'], 'output': result.stdout,
    'source_sha256': {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}}, indent=2) + '\n')
