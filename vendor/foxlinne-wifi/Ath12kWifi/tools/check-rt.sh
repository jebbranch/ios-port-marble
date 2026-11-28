#!/bin/bash
cd "$(dirname "$0")/.."
clang -std=gnu11 -Wall -Wno-unused-function -I shim/include -I include -c shim/src/lx_rt.c -o out/lx_rt.o 2>&1 | grep -E "error|warning" | head -20
