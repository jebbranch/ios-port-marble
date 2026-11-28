#!/usr/bin/env python3
"""Compile production buffer/runtime code and exercise DMA ownership on the host."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "out/buffer-test"
out.mkdir(parents=True, exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
subprocess.run(["python3", str(root / "tools/prepare-mac80211.py"), "--subsystem", "cfg80211"], check=True)
subprocess.run(["python3", str(root / "tools/prepare-cfg80211-native.py")], check=True)
compiler = shutil.which("clang")
if not compiler:
    raise SystemExit("clang is required")
sources = [root / path for path in ("tests/buffer_test.c", "src/nw_net.c", "src/nw_netdev.c", "src/nw_crc32.c", "src/nw_cfg80211_os.c", "shim/src/lx_rt.c", "deps/linux/lib/rbtree.c", "out/cfg80211/util.c", "out/cfg80211-native-protocol.c")]
command = [compiler, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
           "-DCONFIG_CFG80211=1", "-DCONFIG_MAC80211=1", "-fno-strict-aliasing", "-fwrapv", "-ffunction-sections", "-Wl,-dead_strip",
           "-fno-omit-frame-pointer", "-Wno-incompatible-pointer-types", "-Wno-pointer-sign",
           "-Wno-macro-redefined", "-include", "lx_pci.h"]
for directory in ("shim/include", "include", "src", "out/pinned", "out/cfg80211"):
    command += ["-I", str(root / directory)]
command += [str(path) for path in sources] + ["-o", str(out / "buffer-test")]
subprocess.run(command, cwd=root, check=True)
result = subprocess.run([str(out / "buffer-test")], cwd=root, text=True, capture_output=True)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end="")
    raise SystemExit(result.returncode)
(out / "report.json").write_text(json.dumps({
    "passed": True, "sanitizers": ["address", "undefined"],
    "production_code": [str(path.relative_to(root)) for path in sources[1:]],
    "source_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
    "output": result.stdout, "hardware_verified": False,
}, indent=2) + "\n")
