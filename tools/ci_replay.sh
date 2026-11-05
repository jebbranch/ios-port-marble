#!/usr/bin/env bash
# CI-equivalent offline test for the patch chain: replay every xnu/patches/*.patch onto
# the base revision the way dxb does (git apply --check, then git apply, skipping on
# failure), and require the result to equal the chain-tree HEAD byte-for-byte.
#
#   tools/ci_replay.sh                       # base e33e15e vs HEAD, sweet_glink.c
#   BASE=<rev> HEAD_REV=<rev> FILE=... tools/ci_replay.sh
#
# This is the test that would have caught run 387 before pushing: a patch whose context
# comes from a chain-tree commit that never landed as a .patch gets skipped in CI, and
# the published kernel is then the previous round's.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"                       # work/xnu-gh
CHAIN="${CHAIN:-$ROOT/../chain-tree}"
BASE="${BASE:-e33e15e}"                              # "base" commit of the chain tree
HEAD_REV="${HEAD_REV:-HEAD}"
FILE="${FILE:-bsd/dev/sweet_glink.c}"
WORK="${WORK:-/tmp/ci-replay-$$}"

[ -d "$CHAIN/.git" ] || { echo "missing chain tree at $CHAIN" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK/head"
git -C "$CHAIN" archive "$BASE" | tar -x -C "$WORK"
git -C "$CHAIN" archive "$HEAD_REV" | tar -x -C "$WORK/head"

cd "$WORK"
applied=0
skipped=""
for p in "$ROOT"/xnu/patches/*.patch; do
  if git apply --check -p1 --include="$FILE" "$p" 2>/dev/null; then
    git apply -p1 --include="$FILE" "$p" && applied=$((applied + 1))
  elif git apply --check -p1 "$p" >/dev/null 2>&1 || grep -q "$(basename "$FILE")" "$p"; then
    skipped="$skipped $(basename "$p")"
  fi
done

echo "replayed onto $BASE: applied=$applied skipped_touching_file=$(echo $skipped | wc -w | tr -d ' ')"
[ -n "$skipped" ] && echo "skipped:$skipped"
if diff -q "$FILE" "head/$FILE" >/dev/null; then
  echo "OK: CI post-image == chain-tree $HEAD_REV for $FILE"
else
  echo "FAIL: replay differs from $HEAD_REV - CI would publish a different file" >&2
  diff "$FILE" "head/$FILE" | head -20 >&2
  exit 1
fi
if [ -n "${EXPECT:-}" ]; then
  printf 'marker %-30s %s\n' "$EXPECT" "$(LC_ALL=C grep -acF "$EXPECT" "$FILE")"
fi
echo "work kept in $WORK (rm -rf when done)"
