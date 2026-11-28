#!/usr/bin/env python3
"""Reuse locked Linux notification bodies and their private encoding helpers."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
from source_fragments import function

root = Path(__file__).resolve().parents[1]
subprocess.run(["python3", str(root / "tools/fetch-sources.py"), "--check"], check=True)
source = root / "deps/linux/net/wireless/nl80211.c"
text = source.read_text()
names = """
nl80211hdr_put
cfg80211_assoc_comeback cfg80211_bss_color_notify cfg80211_ch_switch_notify cfg80211_ch_switch_started_notify
cfg80211_cqm_beacon_loss_notify cfg80211_cqm_pktloss_notify cfg80211_cqm_rssi_notify cfg80211_cqm_rssi_notify_work
cfg80211_del_sta_sinfo cfg80211_epcs_changed cfg80211_gtk_rekey_notify cfg80211_links_removed cfg80211_mgmt_tx_status_ext
cfg80211_nan_func_terminated cfg80211_nan_match cfg80211_new_sta cfg80211_probe_status cfg80211_ready_on_channel
cfg80211_remain_on_channel_expired cfg80211_report_obss_beacon_khz cfg80211_rx_spurious_frame
cfg80211_rx_unexpected_4addr_frame cfg80211_rx_unprot_mlme_mgmt cfg80211_schedule_channels_check
cfg80211_sta_opmode_change_notify cfg80211_tdls_oper_request cfg80211_tx_mgmt_expired
nl80211_common_reg_change_event nl80211_michael_mic_failure nl80211_mlo_reconf_add_done nl80211_radar_notify
nl80211_send_ap_stopped nl80211_send_beacon_hint_event nl80211_send_connect_result nl80211_send_ibss_bssid
nl80211_send_mgmt nl80211_send_port_authorized nl80211_send_roamed nl80211_send_sched_scan
""".split()
fragments = {}
def visit(name):
    if name in fragments: return
    try: code = function(text, name)
    except ValueError: return  # Implemented by another original cfg80211 unit.
    fragments[name] = code
    for child in re.findall(r"\b((?:__)?(?:nl80211|cfg80211)_[a-zA-Z0-9_]+)\s*\(", code):
        visit(child)
for name in names: visit(name)
types = []
for name in sorted(set(re.findall(r"\bstruct (nl80211_[a-zA-Z0-9_]+)\b", "\n".join(fragments.values())))):
    match = re.search(r"\bstruct " + name + r"\s*\{", text)
    if not match: continue
    end = text.index("};", match.end()) + 2
    types.append(text[match.start():end] + "\n")
output = root / "out/cfg80211-native-events.c"
prefix = '''/* SPDX-License-Identifier: GPL-2.0-only */
/* Generated original Linux notification bodies; native event transport only. */
#include "lx_pci.h"
#include <net/genetlink.h>
#include <net/cfg80211.h>
#include <linux/nospec.h>
#include "cfg80211/core.h"
#include "cfg80211/rdev-ops.h"
#include "cfg80211/nl80211.h"
#include "cfg80211/reg.h"
#include "cfg80211/trace.h"
/* Encoding identity only; the XNU socket owns subscription and delivery. */
static struct genl_family nl80211_fam = { .id = 0x13, .version = 1 };
enum nl80211_multicast_groups { NL80211_MCGRP_CONFIG, NL80211_MCGRP_SCAN,
 NL80211_MCGRP_REGULATORY, NL80211_MCGRP_MLME, NL80211_MCGRP_VENDOR, NL80211_MCGRP_NAN,
 NL80211_MCGRP_TESTMODE };
'''
prototypes = [code[:code.index("{")].strip() + ";\n" for code in fragments.values()]
output.write_text(prefix + "\n".join(types + prototypes + list(fragments.values())))
(root / "out/cfg80211-native-events.json").write_text(json.dumps({
    "linux_commit": json.loads((root / "tools/source-lock.json").read_text())["linux"]["commit"],
    "source": str(source.relative_to(root)), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "functions": {name: hashlib.sha256(code.encode()).hexdigest() for name, code in fragments.items()},
    "output_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
    "request_dispatcher_selected": False,
}, indent=2) + "\n")
print(f"staged {len(fragments)} unchanged notification/encoding functions: {output}")
