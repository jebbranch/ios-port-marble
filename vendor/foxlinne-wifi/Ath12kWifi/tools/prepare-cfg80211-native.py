#!/usr/bin/env python3
"""Reuse original cfg80211 validation/cleanup with the native control transport."""
import hashlib
import json
from pathlib import Path
import subprocess
from source_fragments import function

root = Path(__file__).resolve().parents[1]
subprocess.run(["python3", str(root / "tools/fetch-sources.py"), "--check"], check=True)
fragments = []
records = []
for file, names in (
    ("nl80211.c", ["cfg80211_check_station_change", "cfg80211_free_coalesce"]),
    ("pmsr.c", ["cfg80211_pmsr_process_abort", "cfg80211_pmsr_free_wk", "cfg80211_pmsr_wdev_down", "cfg80211_release_pmsr"]),
):
    source = root / "deps/linux/net/wireless" / file
    for name in names:
        code = function(source.read_text(), name)
        fragments.append(code)
        records.append({"source": str(source.relative_to(root)), "function": name, "sha256": hashlib.sha256(code.encode()).hexdigest()})
output = root / "out/cfg80211-native-protocol.c"
output.write_text('/* SPDX-License-Identifier: GPL-2.0-only */\n'
                  '/* Generated unchanged function bodies from locked Linux sources. */\n'
                  '#include "lx_pci.h"\n#include <net/cfg80211.h>\n'
                  '#include "cfg80211/core.h"\n#include "cfg80211/rdev-ops.h"\n'
                  '#include "cfg80211/nl80211.h"\n#include "cfg80211/trace.h"\n' + "\n".join(fragments))
(root / "out/cfg80211-native-protocol.json").write_text(json.dumps({
    "linux_commit": json.loads((root / "tools/source-lock.json").read_text())["linux"]["commit"],
    "functions": records, "output_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
    "transport": "native control; Linux netlink command parsing is not selected",
}, indent=2) + "\n")
print(f"staged {len(records)} original cfg80211 validation/lifetime functions: {output}")
fils_source = root / "deps/linux/net/mac80211/fils_aead.c"
fils = {name: function(fils_source.read_text(), name) for name in ("fils_encrypt_assoc_req", "fils_decrypt_assoc_resp")}
(root / "out/mac80211-fils-protocol.inc").write_text("/* Generated unchanged FILS protocol bodies. */\n" + "\n".join(fils.values()))
(root / "out/mac80211-fils-protocol.json").write_text(json.dumps({"source": str(fils_source.relative_to(root)),
    "functions": {name: hashlib.sha256(code.encode()).hexdigest() for name, code in fils.items()}}, indent=2) + "\n")
