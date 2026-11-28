#!/bin/bash
# The station code against a fake driver and access point (tests/sta_sim.c).
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/fetch-sources.py --check
bash tools/pin-headers.sh
clang -std=gnu11 -w -g -DCONFIG_ATH12K_DEBUG -include lx_pci.h \
  -I shim/include -I include -I out/pinned -I src \
  tests/sta_sim.c shim/src/lx_rt.c src/nw_sta.c src/nw_crypto.c \
  src/nw_mac80211.c src/nw_net.c src/nw_netdev.c src/nw_events.c deps/linux/lib/rbtree.c -o out/sta_sim
./out/sta_sim
