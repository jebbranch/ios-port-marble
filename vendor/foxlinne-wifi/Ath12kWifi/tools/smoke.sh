#!/bin/bash
# Host smoke test of the whole stack against a register window that never answers.
cd "$(dirname "$0")/.."
bash tools/link-check.sh >/dev/null 2>&1
clang -std=gnu11 -w -g -include lx_pci.h -I shim/include -I include -I out/pinned -I src tests/stack_smoke.c out/all/*.o -o out/smoke 2>&1 | grep -E "error|undefined" | head
ASAN=${ASAN:-}
./out/smoke
