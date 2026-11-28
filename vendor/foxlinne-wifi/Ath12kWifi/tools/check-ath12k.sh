#!/bin/bash
# Compile-check the ath12k sources (core, wifi7) against the shim headers; prints the first errors per file, or a count summary with SUM=1.
cd "$(dirname "$0")/.."
bash tools/pin-headers.sh
A=$PWD/deps/linux/drivers/net/wireless/ath/ath12k; O=out/ath12k-src
rm -rf $O; mkdir -p $O/wifi7
for f in $A/*.c $A/*.h; do ln -s $f $O/; done
for f in $A/wifi7/*.c $A/wifi7/*.h; do ln -s $f $O/wifi7/; done
rm -f $O/trace.h
{ echo '#ifndef LX_ATH12K_TRACE_H'; echo '#define LX_ATH12K_TRACE_H'; grep -ohE "trace_ath12k_[a-z_0-9]+" $A/*.c $A/*.h $A/wifi7/*.c | sort -u | sed 's/.*/#define & (...) ((void)0)/' | sed 's/ (\.\.\.)/(...)/'; echo '#endif'; } > $O/trace.h
files=${FILES:-"wifi7/ce wifi7/core wifi7/dp_mon wifi7/dp_rx wifi7/dp_tx wifi7/dp wifi7/hal_wcn7850 wifi7/hal_rx wifi7/hal_tx wifi7/hal wifi7/hw wifi7/mhi wifi7/pci wifi7/wmi core hal ce htc qmi mhi pci wmi mac dp dp_rx dp_tx dp_htt dp_mon dp_peer peer reg fw dbring p2p debug"}
for f in $files; do
  if [ -n "$SUM" ]; then echo -n "$f: "; clang -fsyntax-only -ferror-limit=0 -std=gnu11 -w -DCONFIG_ATH12K_DEBUG -include lx_pci.h -I shim/include -I include -I out/pinned -I $O -I $O/wifi7 $O/$f.c 2>&1 | grep -c "error"
  else echo "== $f"; clang -fsyntax-only -ferror-limit=0 -std=gnu11 -w -DCONFIG_ATH12K_DEBUG -include lx_pci.h -I shim/include -I include -I out/pinned -I $O -I $O/wifi7 $O/$f.c 2>&1 | grep -E "error" | head -${N:-8}; fi
done
