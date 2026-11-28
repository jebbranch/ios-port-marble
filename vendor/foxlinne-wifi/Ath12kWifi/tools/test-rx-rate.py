#!/usr/bin/env python3
"""Exercise the prepared ath12k RX parser, with unchanged upstream rate helpers."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from source_fragments import function

root = Path(__file__).resolve().parents[1]
out = root / "out/rx-rate-test"
out.mkdir(exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
subprocess.run(["bash", str(root / "tools/prepare.sh")], cwd=root, check=True)

original = root / "deps/linux/drivers/net/wireless/ath/ath12k"
prepared = root / "out/ath12k-src"
helpers = ["ath12k_mac_bitrate_is_cck", "ath12k_mac_hw_rate_to_idx",
           "ath12k_mac_bw_to_mac80211_bw", "ath12k_mac_eht_gi_to_nl80211_eht_gi"]
fragments = {name: function((original / "mac.c").read_text(), name) for name in helpers}
old_parser = function((original / "dp_rx.c").read_text(), "ath12k_dp_rx_h_rate")
new_parser = function((prepared / "dp_rx.c").read_text(), "ath12k_dp_rx_h_rate")
code = '#include "lx_pci.h"\n#include "core.h"\n#include "mac.h"\n#include "dp_rx.h"\n#include "debug.h"\n'
code += "\n".join(fragments.values())
code += old_parser.replace("ath12k_dp_rx_h_rate(", "ath12k_dp_rx_h_rate_before(", 1) + new_parser
code += '#include "rx_rate_test.c"\n'
source = out / "rx-rate-test.c"; source.write_text(code)
compiler = shutil.which("clang")
command = [compiler, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           "-fno-strict-aliasing", "-Wno-incompatible-pointer-types", "-Wno-macro-redefined"]
for directory in ("shim/include", "include", "out/pinned", "out/ath12k-src", "out/ath12k-src/wifi7", "tests"):
    command += ["-I", str(root / directory)]
subprocess.run([*command, str(source), "-o", str(out / "rx-rate-test")], cwd=root, check=True)
result = subprocess.run([str(out / "rx-rate-test")], check=True, text=True, capture_output=True)
print(result.stdout, end="")
(out / "report.json").write_text(json.dumps({
    "passed": True, "sanitizers": ["address", "undefined"],
    "original_parser_sha256": hashlib.sha256(old_parser.encode()).hexdigest(),
    "prepared_parser_sha256": hashlib.sha256(new_parser.encode()).hexdigest(),
    "upstream_helpers_sha256": {name: hashlib.sha256(code.encode()).hexdigest() for name, code in fragments.items()},
    "fixture_sha256": hashlib.sha256((root / "tests/rx_rate_test.c").read_bytes()).hexdigest(),
    "output": result.stdout, "hardware_verified": False,
}, indent=2) + "\n")
