#!/bin/bash
# Link the public Linux headers that are used as they are into out/pinned (everything else comes from shim/include).
cd "$(dirname "$0")/.."
L=$PWD/deps/linux/include
rm -rf out/pinned; mkdir -p out/pinned/linux/soc/qcom out/pinned/net
for h in linux/mhi.h linux/ieee80211.h linux/soc/qcom/qmi.h linux/jhash.h linux/netdev_features.h linux/kconfig.h \
  linux/rbtree.h linux/rbtree_types.h linux/rbtree_augmented.h linux/once_lite.h linux/fips.h \
  linux/if_arp.h uapi/linux/if_arp.h kunit/visibility.h kunit/static_stub.h crypto/arc4.h crypto/aes.h linux/acpi_amd_wbrf.h \
  linux/mpls.h uapi/linux/mpls.h linux/device/faux.h linux/units.h net/cfg80211-wext.h \
  net/cfg80211.h net/regulatory.h net/codel.h net/codel_impl.h net/ieee80211_radiotap.h \
  net/fq.h net/fq_impl.h net/dropreason.h net/dropreason-core.h; do
  mkdir -p out/pinned/$(dirname $h); ln -s $L/$h out/pinned/$h
done
for f in $L/linux/ieee80211-*.h; do ln -s $f out/pinned/linux/$(basename $f); done
ln -s $L/uapi/linux/nl80211.h out/pinned/linux/nl80211.h
ln -s $L/uapi/linux/rfkill.h out/pinned/linux/rfkill.h
mkdir -p out/pinned/uapi/linux; ln -s $L/uapi/linux/rfkill.h out/pinned/uapi/linux/rfkill.h
ln -s $L/net/mac80211.h out/pinned/net/mac80211.real.h
python3 - "$L/linux/netdevice.h" out/pinned/linux/netdevice_enums.h <<'PY'
from pathlib import Path
import re
import sys
source = Path(sys.argv[1]).read_text()
blocks = []
for name in ('netdev_priv_flags', 'netdev_stat_type', 'netdev_cmd'):
    match = re.search(r'enum ' + name + r'\s*\{.*?\};', source, re.S)
    if not match:
        raise SystemExit('missing upstream netdevice enum: ' + name)
    blocks.append(match.group())
Path(sys.argv[2]).write_text('/* SPDX-License-Identifier: GPL-2.0 */\n#ifndef NW_NETDEVICE_ENUMS_H\n#define NW_NETDEVICE_ENUMS_H\n' + '\n'.join(blocks) + '\n#endif\n')
PY
