#!/usr/bin/env bash
# CI-equivalent apply check for 0292's three touched kernel files.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WORK_ROOT="$(cd "$ROOT/.." && pwd)"
TMP="$(mktemp -d /tmp/halbridge-replay.XXXXXX)"
PATCH="$ROOT/xnu/patches/0292-sweet-hal-pcm-capture.patch"

mkdir -p "$TMP/bsd/dev" "$TMP/bsd/conf"
git -C "$WORK_ROOT/chain-tree-ufs" show HEAD^:bsd/dev/sweet_ufs.c \
  > "$TMP/bsd/dev/sweet_ufs.c"
git -C "$WORK_ROOT/chain-tree-conf" show 970d4eb:bsd/conf/files \
  > "$TMP/bsd/conf/files"
git -C "$TMP" init -q
git -C "$TMP" add bsd/dev/sweet_ufs.c bsd/conf/files
git -C "$TMP" commit -q -m baseline
git -C "$TMP" apply --check -p1 "$PATCH"
git -C "$TMP" apply -p1 "$PATCH"

diff -q "$TMP/bsd/dev/sweet_ufs.c" \
  "$WORK_ROOT/chain-tree-ufs/bsd/dev/sweet_ufs.c"
diff -q "$TMP/bsd/dev/sweet_audio_bridge.c" \
  <(git -C "$WORK_ROOT/chain-tree-halbridge" show b0a29db:bsd/dev/sweet_audio_bridge.c)
diff -q "$TMP/bsd/conf/files" \
  <(git -C "$WORK_ROOT/chain-tree-conf" show af6ead7:bsd/conf/files)
echo "HAL bridge three-file replay: PASS ($TMP)"
