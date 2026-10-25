#!/bin/bash
# Fetch the kernel-latest asset WITHOUT touching github.com.
#
# On some networks github.com / raw.githubusercontent.com are unreachable
# while api.github.com and objects.githubusercontent.com still work, which
# breaks fetch_kernel.sh (it polls the "expanded_assets" HTML page).  This
# variant uses the REST API for the digest and the asset API endpoint for
# the bytes; the latter redirects to objects.githubusercontent.com.
#
#   scripts/fetch_kernel_api.sh            # normal fetch
#   DIGEST_ONLY=1 scripts/fetch_kernel_api.sh   # just print the digest
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO="${GH_REPO:-jebbranch/ios-port-marble}"
TAG="${ASSET_TAG:-kernel-latest}"
NAME="${ASSET_NAME:-kernel.development.sweet}"
OUT_DIR="${OUT_DIR:-$ROOT/out}"
API="https://api.github.com/repos/$REPO/releases/tags/$TAG"

mkdir -p "$OUT_DIR"
INFO=$(curl -s --max-time 60 "$API")
[ -n "${INFO:-}" ] || { echo "error: empty API response" >&2; exit 1; }

DIGEST=$(printf '%s' "$INFO" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit("bad json")
for a in d.get("assets", []):
    if a.get("name") == sys.argv[1]:
        print((a.get("digest") or "").replace("sha256:", ""))
        break
' "$NAME")
[ -n "${DIGEST:-}" ] || { echo "error: no digest for $NAME" >&2; exit 1; }
echo "want sha256: $DIGEST"
if [ "${DIGEST_ONLY:-0}" = 1 ]; then exit 0; fi

URL=$(printf '%s' "$INFO" | python3 -c '
import json, sys
d = json.load(sys.stdin)
for a in d.get("assets", []):
    if a.get("name") == sys.argv[1]:
        print(a["url"])
        break
' "$NAME")
[ -n "${URL:-}" ] || { echo "error: no asset url for $NAME" >&2; exit 1; }

RAW="$OUT_DIR/$NAME.raw"
for try in 1 2 3; do
  curl -sL --max-time 900 -H "Accept: application/octet-stream" \
    -o "$RAW" "$URL" || true
  GOT=$(shasum -a 256 "$RAW" 2>/dev/null | awk '{print $1}')
  [ "$GOT" = "$DIGEST" ] && break
  echo "attempt $try: got ${GOT:-none}, want $DIGEST"
  sleep 20
done
GOT=$(shasum -a 256 "$RAW" | awk '{print $1}')
[ "$GOT" = "$DIGEST" ] || { echo "error: sha256 $GOT != $DIGEST" >&2; exit 1; }
cp "$RAW" "$OUT_DIR/kernel.adsp"
# Same post-processing step as fetch_kernel.sh: the guest must not write
# CNTP_CTL_EL0 (the host timer is not ours to touch).
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
echo "ok: $OUT_DIR/kernel.adsp"
