#!/bin/bash
# Compile-check the MHI host sources against the shim headers (their trace points are replaced by empty ones).
cd "$(dirname "$0")/.."
L=$PWD/deps/linux; H=$L/drivers/bus/mhi/host; O=out/mhi-src
rm -rf $O; mkdir -p $O
for f in $H/*.c $H/internal.h; do ln -s $f $O/; done
cp shim/mhi/trace.h $O/trace.h
mkdir -p $O/../common; ln -sf $L/drivers/bus/mhi/common.h $O/../common.h
for f in init main pm boot; do
  echo "== $f"
  clang -fsyntax-only -std=gnu11 -Wall -Wno-unused-function -Wno-unused-variable -Wno-pointer-sign -I shim/include -I include -I $O -I out/pinned $O/$f.c 2>&1 | grep -E "error|fatal" | head -${N:-12}
done
