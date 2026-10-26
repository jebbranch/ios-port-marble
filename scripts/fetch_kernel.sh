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
DIGEST=$(printf '%s' "$INFO" | grep -o "sha256:[0-9a-f]\{64\}" | head -1 | cut -d: -f2)
if [ -z "${DIGEST:-}" ]; then
  echo "error: could not read the release digest (rate limited? use MARBLE_PROXY=...)" >&2
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
if [ "$GOT" != "$DIGEST" ]; then
  # Documented in the handoff notes: the release digest and the bytes the CDN
  # actually serves can disagree (and differ between attempts).  The build
  # marker inside the kernel is the trustworthy check, so fall back to it.
  MARK=$(LC_ALL=C grep -ao 'SWEET AUDIO: build v[0-9]*' "$RAW" 2>/dev/null | head -1)
  if [ -n "$MARK" ]; then
    echo "warning: digest mismatch, but the payload looks like a real kernel:" >&2
    echo "         got $GOT, want $DIGEST" >&2
    echo "         marker: $MARK" >&2
  else
    echo "error: sha256 $GOT != $DIGEST and no build marker in the payload" >&2
    exit 1
  fi
fi

cp "$RAW" "$OUT_DIR/kernel.adsp"
python3 - "$OUT_DIR/kernel.adsp" <<'PY'
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
