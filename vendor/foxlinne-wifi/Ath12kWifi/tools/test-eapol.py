#!/usr/bin/env python3
"""Check the actual ath12k encrypted-EAPOL backport with production skb/DMA code."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from source_fragments import function

root = Path(__file__).resolve().parents[1]
out = root / "out/eapol-test"
out.mkdir(parents=True, exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
subprocess.run(["bash", str(root / "tools/prepare.sh")], cwd=root, check=True)
prepared = root / "out/ath12k-src"
fragments = {}
for path, names in (
    (prepared / "dp_tx.c", ["ath12k_dp_tx_get_encrypt_type", "ath12k_dp_tx_crypto_iv_len", "ath12k_dp_tx_crypto_icv_len"]),
    (prepared / "wifi7/dp_tx.c", ["ath12k_wifi7_dp_tx_encap_eapol"]),
    (root / "deps/linux/net/wireless/util.c", ["ieee80211_hdrlen"]),
):
    for name in names: fragments[name] = function(path.read_text(), name)
source = out / "eapol-test.c"
source.write_text('#include "lx_pci.h"\n#include "core.h"\n#include "dp_tx.h"\n#include "wifi7/hal_tx.h"\n' +
                  "\n".join(fragments.values()) + '\n#include "eapol_tx_test.c"\n')
compiler = shutil.which("clang")
if not compiler: raise SystemExit("clang is required")
command = [compiler, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           "-fno-strict-aliasing", "-fwrapv", "-Wno-incompatible-pointer-types", "-Wno-macro-redefined",
           "-ffunction-sections", "-Wl,-dead_strip", "-include", "lx_pci.h"]
for directory in ("shim/include", "include", "out/pinned", "out/ath12k-src", "out/ath12k-src/wifi7", "tests"):
    command += ["-I", str(root / directory)]
sources = [source, *[root / path for path in ("src/nw_net.c", "shim/src/lx_rt.c", "deps/linux/lib/rbtree.c")]]
subprocess.run([*command, *map(str, sources), "-o", str(out / "eapol-test")], cwd=root, check=True)
result = subprocess.run([str(out / "eapol-test")], text=True, capture_output=True)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end="")
    raise SystemExit(result.returncode)
(out / "report.json").write_text(json.dumps({
    "passed": True, "sanitizers": ["address", "undefined"], "hardware_verified": False,
    "upstream_backport": "1e33f8acd837420160ea088160d8648a3db54c3b",
    "function_sha256": {name: hashlib.sha256(code.encode()).hexdigest() for name, code in fragments.items()},
    "source_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources[1:] + [root / "tests/eapol_tx_test.c"]},
    "output": result.stdout,
}, indent=2) + "\n")
