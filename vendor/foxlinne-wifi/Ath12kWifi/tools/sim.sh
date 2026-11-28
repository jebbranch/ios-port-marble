#!/bin/bash
# Host run of the whole stack against the simulated device (tests/stack_sim.c).
cd "$(dirname "$0")/.."
bash tools/link-check.sh >/dev/null 2>&1
clang -std=gnu11 -w -g -DCONFIG_ATH12K_DEBUG -include lx_pci.h -I shim/include -I include -I out/pinned -I src -I out/ath12k-src tests/stack_sim.c $(ls out/all/*.o | grep -v '/qmi.o') -o out/sim 2>&1 | grep -E "error|undefined|Undefined" | head
./out/sim
