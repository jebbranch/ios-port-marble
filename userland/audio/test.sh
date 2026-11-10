#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
mkdir -p "$OUT"

clang -std=gnu11 -O1 -g -Wall -Wextra -Wformat=2 -Werror \
  -Wno-format-nonliteral -fsanitize=address,undefined \
  -framework CoreFoundation -I"$HERE" "$HERE/test_host.c" \
  -o "$OUT/test_host"
"$OUT/test_host"
echo "MarbleAudio host contract: PASS"
