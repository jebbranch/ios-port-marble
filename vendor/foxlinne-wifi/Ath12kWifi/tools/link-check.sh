#!/bin/bash
# Compile every source of the module and list the symbols still undefined across all objects.
cd "$(dirname "$0")/.."
bash tools/build-ath12k.sh >/dev/null 2>&1
bash tools/check-mhi.sh >/dev/null 2>&1
O=out/all; rm -rf $O; mkdir -p $O
CF="-c -nostdinc -isystem shim/libc -isystem /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/21/include -std=gnu11 -w -g0 -DCONFIG_ATH12K_DEBUG -include lx_pci.h -I shim/include -I include -I out/pinned -I src -I out/ath12k-src"
for f in src/*.c shim/src/*.c; do clang $CF -I out/mhi-src $f -o $O/$(basename $f .c).o 2>&1 | grep error | head -3; done
for f in init main pm boot; do clang $CF -I out/mhi-src out/mhi-src/$f.c -o $O/mhi_$f.o 2>&1 | grep error | head -3; done
for f in qmi_interface qmi_encdec; do clang $CF deps/linux/drivers/soc/qcom/$f.c -o $O/$f.o 2>&1 | grep error | head -3; done
cp out/ath12k-obj/*.o $O/
nm -g --defined-only $O/*.o 2>/dev/null | awk 'NF==3{print $3}' | sort -u > /tmp/def.txt
nm -u $O/*.o | grep -v ":$" | awk '{print $NF}' | sort -u > /tmp/und.txt
comm -13 /tmp/def.txt /tmp/und.txt | grep -v "^_memcpy$\|^_memmove$\|^_memset$\|^_memcmp$\|^_strlen$\|^_strcmp$\|^_strncmp$\|^_strncpy$\|^_snprintf$\|^_vsnprintf$\|^_strtoul$\|^_bzero$\|^___stack_chk"
