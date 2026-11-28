#!/usr/bin/env python3
"""Audit the exact production objects and their imports against an XNU kernel."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-report', type=Path, required=True)
parser.add_argument('--kernel', type=Path, required=True)
args = parser.parse_args()
out = root / 'out/mainline-link'
out.mkdir(exist_ok=True)
(out / 'report.json').unlink(missing_ok=True)
report = json.loads(args.build_report.read_text())
assert report['runtime'] == 'mainline' and not report['legacy_station_linked']
nm = shutil.which('llvm-nm') or str(Path(subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin/llvm-nm')
def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def symbols(path):
    result = []
    for line in subprocess.check_output([nm, '-g', str(path)], text=True).splitlines():
        parts = line.split()
        if len(parts) >= 2 and len(parts[-2]) == 1: result.append((parts[-2], parts[-1]))
    return result
definitions, references = {}, {}
for unit in report['objects']:
    path, source = Path(unit['path']), Path(unit['source'])
    assert source.name not in ('nw_sta.c', 'nw_mac80211.c', 'nw_crypto.c'), source
    assert digest(path) == unit['object_sha256'] and digest(source) == unit['source_sha256'], path
    raw = path.parent / 'raw' / path.name
    identity = json.loads(raw.with_suffix('.inputs.json').read_text())
    assert identity['schema'] == 1 and digest(raw) == identity['object_sha256'], raw
    assert identity['dependencies'] and all(Path(p).is_file() and digest(p) == sha for p, sha in identity['dependencies'].items()), source
    for kind, name in symbols(path):
        (references if kind == 'U' else definitions).setdefault(name, []).append(str(path))
pending = sorted(set(references) - set(definitions))
duplicates = {name: paths for name, paths in definitions.items() if len(paths) > 1}
kernel_symbols = {name for kind, name in symbols(args.kernel) if kind != 'U'}
missing = sorted(set(pending) - kernel_symbols)
result = {'passed': not missing and not duplicates, 'runtime': 'mainline',
    'objects': len(report['objects']), 'legacy_station_linked': False,
    'imports': pending, 'imports_missing_from_xnu': missing, 'duplicate_definitions': duplicates,
    'build_report': str(args.build_report.resolve()), 'build_report_sha256': digest(args.build_report),
    'kernel': str(args.kernel.resolve()), 'kernel_sha256': digest(args.kernel), 'hardware_verified': False}
(out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
print(f"production mainline: {result['objects']} objects; {len(pending)} XNU imports; {len(missing)} missing imports; {len(duplicates)} duplicate definitions")
if missing: print(missing)
raise SystemExit(0 if result['passed'] else 1)
