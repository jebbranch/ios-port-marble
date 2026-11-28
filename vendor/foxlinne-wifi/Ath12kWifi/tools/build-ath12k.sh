#!/bin/bash
# Compile the ath12k sources to objects (host build, for checking what the runtime still has to provide).
cd "$(dirname "$0")/.."
bash tools/check-ath12k.sh >/dev/null 2>&1
A=$PWD/out/ath12k-src; O=out/ath12k-obj; rm -rf $O; mkdir -p $O
for f in $A/*.c $A/wifi7/*.c; do b=$(basename $f .c); d=$(dirname $f | xargs basename); case $b in debugfs*|testmode|ahb|trace|acpi|coredump|wow|debug) continue;; esac
  [ $d = wifi7 ] && b=w7_$b
  clang -c -nostdinc -isystem shim/libc -isystem /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/21/include -std=gnu11 -w -DCONFIG_ATH12K_DEBUG -g0 -include lx_pci.h -I shim/include -I include -I out/pinned -I $A -I $A/wifi7 $f -o $O/$b.o 2>&1 | grep error | head -3; done
ls $O | wc -l
