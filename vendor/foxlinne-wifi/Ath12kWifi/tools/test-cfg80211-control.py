#!/usr/bin/env python3
"""Check native command mapping with explicit radio/MLME test doubles."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
root = Path(__file__).resolve().parents[1]
out = root / "out/cfg80211-control-test"
out.mkdir(exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
subprocess.run(["python3", str(root / "tools/prepare-mac80211.py"), "--subsystem", "cfg80211"], check=True)
sources = [root / path for path in ("tests/cfg80211_control_test.c", "src/nw_net.c", "src/nw_netdev.c",
    "src/nw_cfg80211_os.c", "src/nw_crc32.c", "src/nw_control.c", "src/nw_control_wire.c", "shim/src/lx_rt.c",
    "deps/linux/lib/rbtree.c", "out/cfg80211/util.c")]
command = [shutil.which("clang"), "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    "-DCONFIG_CFG80211=1", "-DCONFIG_MAC80211=1", "-fno-strict-aliasing", "-fwrapv", "-ffunction-sections",
    "-Wl,-dead_strip", "-Wno-incompatible-pointer-types", "-Wno-pointer-sign", "-Wno-macro-redefined", "-include", "lx_pci.h"]
for directory in ("shim/include", "include", "src", "out/pinned", "out", "out/cfg80211"):
    command += ["-I", str(root / directory)]
command += [str(path) for path in sources] + ["-o", str(out / "cfg80211-control-test")]
result = subprocess.run(command, cwd=root, capture_output=True, text=True)
(out / "build.log").write_text(result.stdout + result.stderr)
if result.returncode:
    print("\n".join(line for line in result.stderr.splitlines() if "error:" in line or "undefined" in line))
    raise SystemExit(f"native mapping compile failed; see {out / 'build.log'}")
result = subprocess.run([str(out / "cfg80211-control-test")], capture_output=True, text=True, timeout=15)
print(result.stdout, end=""); print(result.stderr, end="")
if result.returncode: raise SystemExit(result.returncode)
(out / "report.json").write_text(json.dumps({"passed": True, "radio_and_cfg80211_mlme_simulated": True,
    "upstream_key_validator": True, "hardware_verified": False, "sanitizers": ["address", "undefined"],
    "output": result.stdout, "source_sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
    for p in [*sources, root / "src/nw_cfg80211_control.c"]}}, indent=2) + "\n")
