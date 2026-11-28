#!/usr/bin/env python3
"""Run the production wireless runtime with original Linux core and simulated radio."""
import hashlib
import json
from pathlib import Path
import shutil
import shlex
import subprocess
from concurrent.futures import ThreadPoolExecutor
root = Path(__file__).resolve().parents[1]
out = root / "out/mainline-runtime-test"
out.mkdir(exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
subprocess.run(["bash", str(root / "tools/prepare.sh")], check=True)
compiler = shutil.which("clang")
flags = ["-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    "-DCONFIG_CFG80211=1", "-DCONFIG_MAC80211=1", "-DCONFIG_MAC80211_STA_HASH_MAX_SIZE=0",
    '-DCONFIG_MAC80211_RC_DEFAULT=""', "-fno-strict-aliasing", "-fwrapv", "-ffunction-sections",
    "-Wno-incompatible-pointer-types", "-Wno-pointer-sign", "-Wno-macro-redefined", "-include", "lx_pci.h"]
for directory in ("shim/include", "include", "src", "out/pinned", "out"):
    flags += ["-I", str(root / directory)]
excluded = {"src/nw_wifi.c", "src/nw_mainline.c", "src/nw_debug.c", "src/nw_mhi.c", "src/nw_pci.c", "src/nw_qrtr.c",
    "src/nw_qrtr_mhi.c", "shim/src/mhi_hook.c"}
jobs = []
for line in ["tests/mainline_runtime_test.c", *(root / "tools/sources.txt").read_text().splitlines()]:
    line = line.strip()
    if not line or line.startswith("#") or line in excluded: continue
    tree, _, name = line.partition(":") if ":" in line else ("", "", line)
    if tree in ("ath12k", "mhi", "qmi"): continue
    source = root / "out" / tree / name if tree in ("mac80211", "cfg80211", "crypto") else root / "out" / name if tree == "generated" else root / name
    extra = []
    if tree in ("mac80211", "cfg80211"): extra += ["-I", str(root / "out" / tree)]
    if tree == "crypto" or name == "src/nw_crypto_mac80211.c": extra += ["-I", str(root / "shim/crypto"), "-I", str(root / "out/crypto")]
    if name in ("src/nw_fils.c", "src/nw_crypto_mac80211.c"): extra += ["-I", str(root / "out/mac80211")]
    if name.startswith("src/nw_cfg80211_"): extra += ["-I", str(root / "out/cfg80211")]
    if tree == "crypto" and name == "aes-siv.c": extra += ["-Daes_siv_encrypt=nw_aes_siv_encrypt", "-Daes_siv_decrypt=nw_aes_siv_decrypt"]
    destination = out / ((tree + "_" if tree else "") + source.stem + ".o")
    jobs.append((source, destination, extra))
def compile(job):
    source, destination, extra = job
    dependency = destination.with_suffix('.d')
    identity = destination.with_suffix('.inputs.json')
    command = [compiler, "-c", *extra, *flags, '-MD', '-MF', str(dependency), str(source), "-o", str(destination)]
    if identity.exists() and destination.exists():
        record = json.loads(identity.read_text())
        if record['command'] == command and record['object_sha256'] == hashlib.sha256(destination.read_bytes()).hexdigest() and all(
            Path(name).is_file() and hashlib.sha256(Path(name).read_bytes()).hexdigest() == digest
            for name, digest in record['dependencies'].items()): return
    result = subprocess.run(command, capture_output=True, text=True)
    destination.with_suffix(".log").write_text(result.stdout + result.stderr)
    if result.returncode: raise RuntimeError(f"{source}: {result.stderr[-3500:]}")
    names = shlex.split(dependency.read_text().replace('\\\n', '').partition(':')[2])
    identity.write_text(json.dumps({'command': command, 'object_sha256': hashlib.sha256(destination.read_bytes()).hexdigest(),
        'dependencies': {str(Path(name).absolute()): hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in names}}) + '\n')
with ThreadPoolExecutor(8) as pool: list(pool.map(compile, jobs))
result = subprocess.run([compiler, "-fsanitize=address,undefined", "-Wl,-dead_strip", *[str(p) for _, p, _ in jobs], "-o", str(out / "runtime-test")], capture_output=True, text=True)
if result.returncode: raise SystemExit(result.stderr)
result = subprocess.run([str(out / "runtime-test")], capture_output=True, text=True, timeout=30,
    env={**__import__('os').environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
print(result.stdout, end=""); print(result.stderr, end="")
if result.returncode: raise SystemExit(result.returncode)
(out / "report.json").write_text(json.dumps({"passed": True, "original_core": True, "radio_simulated": True,
    "hardware_verified": False, "output": result.stdout, "sanitizers": ["address", "undefined"],
    "source_sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [root / 'src/nw_mainline.c', *[source for source, _, _ in jobs]]}}, indent=2) + "\n")
