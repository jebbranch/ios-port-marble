#!/bin/bash
# List the distinct undeclared names and missing members across the ath12k sources.
cd "$(dirname "$0")/.."
bash tools/check-ath12k.sh >/dev/null 2>&1
A=$PWD/out/ath12k-src
for f in $A/*.c $A/wifi7/*.c; do b=$(basename $f .c); case $b in debugfs*|testmode|ahb|trace|acpi) continue;; esac
  clang -fsyntax-only -ferror-limit=0 -std=gnu11 -w -DCONFIG_ATH12K_DEBUG -include lx_pci.h -I shim/include -I include -I out/pinned -I $A -I $A/wifi7 $f 2>&1 | grep "error"; done | sed -E "s#^[^ ]+ error: ##" | sed -E "s/'[^']*'/X/2" | sort | uniq -c | sort -rn | head -${N:-60}
