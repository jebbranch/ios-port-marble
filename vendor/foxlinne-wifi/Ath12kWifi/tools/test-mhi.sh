#!/bin/bash
# Build the MHI host sources with the runtime and run them against the simulated boot ROM.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/fetch-sources.py --check
bash tools/prepare.sh
L=$PWD/deps/linux; O=out/mhi-src
CF="-std=gnu11 -g -w -I shim/include -I include -I src -I $O -I out/pinned"
mkdir -p out/t
for f in init main pm boot; do clang $CF -c $O/$f.c -o out/t/$f.o; done
clang $CF -c shim/src/lx_rt.c -o out/t/rt.o; clang $CF -c shim/src/mhi_hook.c -o out/t/hook.o
clang $CF -c src/nw_mhi.c -o out/t/nw.o; clang $CF -c tests/mhi_host_test.c -o out/t/t.o
clang out/t/init.o out/t/main.o out/t/pm.o out/t/boot.o out/t/rt.o out/t/hook.o out/t/nw.o out/t/t.o -o out/t/mhi_test
./out/t/mhi_test
