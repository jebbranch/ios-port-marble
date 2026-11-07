#!/bin/bash
# Download the kernel that GitHub Actions published as the rolling
# `kernel-latest` release, verify its digest, and apply the one local patch
# the phone needs (CNTP_CTL -> NOP, see docs/audio-status.md).
#
#   GH_REPO=jebbranch/ios-port-marble ./scripts/fetch_kernel.sh
#
# Output: out/kernel.adsp  (patched; this is what mkimage.sh consumes)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
GH_REPO="${GH_REPO:-jebbranch/ios-port-marble}"
TAG="${ASSET_TAG:-kernel-latest}"
NAME="${ASSET_NAME:-kernel.development.sweet}"
OUT_DIR="${OUT_DIR:-$ROOT/out}"
PROXY="${MARBLE_PROXY:-}"

mkdir -p "$OUT_DIR"
PAGE="https://github.com/$GH_REPO/releases/expanded_assets/$TAG"
# macOS ships bash 3.2, where ${arr[@]} on an empty array is an "unbound
# variable" error under `set -u`.  Use a function instead of an array.
curlx() { if [ -n "$PROXY" ]; then curl -x "$PROXY" "$@"; else curl "$@"; fi; }

echo "release page: $PAGE"
INFO=$(curlx -s --max-time 60 "$PAGE?x=$(date +%s%N)")
# The release carries a stale build-error.txt next to the kernel, and that asset is
# listed first, so "the first sha256 on the page" is the wrong kernel's digest.  Take
# the digest from the element whose clipboard label names the asset we want.
DIGEST=$(printf '%s' "$INFO" | tr '>' '\n' \
    | grep -F "aria-label=\"Copy to clipboard digest for $NAME\"" \
    | grep -o 'value="sha256:[0-9a-f]\{64\}"' | head -1 \
    | sed 's/.*sha256://; s/"//')
if [ -z "${DIGEST:-}" ]; then
  echo "error: no digest listed for $NAME (rate limited? use MARBLE_PROXY=...)" >&2
  exit 1
fi
echo "want sha256: $DIGEST"

RAW="$OUT_DIR/$NAME.raw"
URL="https://github.com/$GH_REPO/releases/download/$TAG/$NAME"
for try in 1 2 3 4 5; do
  curlx -sL --max-time 900 -o "$RAW" "$URL?cb=$(date +%s%N)" || true
  GOT=$(shasum -a 256 "$RAW" 2>/dev/null | awk '{print $1}')
  [ "$GOT" = "$DIGEST" ] && break
  echo "attempt $try: got ${GOT:-none}, want $DIGEST (CDN stale?)"
  sleep 30
done
GOT=$(shasum -a 256 "$RAW" | awk '{print $1}')
# EXPECT_MARKER is the only freshness proof that distinguishes rounds: the digest
# can be served stale, and every kernel ever built carries "SWEET AUDIO: build v",
# so a generic marker accepted the previous round's bytes and nearly booted them.
if [ -n "${EXPECT_MARKER:-}" ] && ! LC_ALL=C grep -aqF "$EXPECT_MARKER" "$RAW"; then
  echo "error: $NAME payload does not contain EXPECT_MARKER=$EXPECT_MARKER" >&2
  echo "       (sha $GOT) - this is not the build you are waiting for" >&2
  exit 1
fi
if [ "$GOT" != "$DIGEST" ]; then
  # Documented in the handoff notes: the release digest and the bytes the CDN
  # actually serves can disagree (and differ between attempts).  Refuse rather
  # than warn: with EXPECT_MARKER already proven above the bytes are from the
  # right round, but a mismatched digest still means we cannot name the build.
  if [ -n "${ALLOW_DIGEST_MISMATCH:-}" ]; then
    echo "warning: digest mismatch accepted because ALLOW_DIGEST_MISMATCH is set:" >&2
    echo "         got $GOT, want $DIGEST" >&2
  else
    echo "error: sha256 $GOT != $DIGEST after 5 attempts" >&2
    echo "       re-run, or set ALLOW_DIGEST_MISMATCH=1 with EXPECT_MARKER set" >&2
    exit 1
  fi
fi

cp "$RAW" "$OUT_DIR/kernel.adsp"
python3 -I - "$OUT_DIR/kernel.adsp" <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
pat = bytes.fromhex('28e21bd5')          # msr CNTP_CTL_EL0, x8
repl = bytes.fromhex('1f2003d5')         # nop
offs = []
i = 0
while True:
    j = d.find(pat, i)
    if j < 0:
        break
    offs.append(j)
    i = j + 1
print('CNTP_CTL occurrences:', [hex(o) for o in offs])
assert len(offs) == 1, 'expected exactly one CNTP_CTL site'
d[offs[0]:offs[0] + 4] = repl
open(p, 'wb').write(d)
print('patched CNTP_CTL at', hex(offs[0]))
PY
echo "ready: $OUT_DIR/kernel.adsp"
