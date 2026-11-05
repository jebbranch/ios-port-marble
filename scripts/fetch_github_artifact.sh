#!/bin/bash
# Download one GitHub Actions artifact without printing or persisting the token.
#
#   ./scripts/fetch_github_artifact.sh <run-id> [artifact-name] [output.zip]
#
# Uses the system Keychain through git-credential-osxkeychain. Override the
# repository or proxy with GH_REPO / GITHUB_PROXY.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
RUN_ID="${1:?usage: fetch_github_artifact.sh <run-id> [artifact-name] [output.zip]}"
ARTIFACT_NAME="${2:-xnu-kernel}"
OUTPUT="${3:-$ROOT/out/github-$RUN_ID-$ARTIFACT_NAME.zip}"
REPO="${GH_REPO:-jebbranch/ios-port-marble}"
PROXY="${GITHUB_PROXY:-http://127.0.0.1:7890}"

CREDENTIALS="$(printf 'protocol=https\nhost=github.com\n\n' | git credential fill)"
TOKEN="$(printf '%s\n' "$CREDENTIALS" | sed -n 's/^password=//p')"
[ -n "$TOKEN" ] || {
	echo "error: no GitHub credential in Keychain" >&2
	exit 1
}

API="https://api.github.com/repos/$REPO/actions/runs/$RUN_ID/artifacts"
INFO="$(curl -fsS --max-time 60 -x "$PROXY" \
	-H "Authorization: Bearer $TOKEN" \
	-H "Accept: application/vnd.github+json" "$API")"

URL="$(printf '%s' "$INFO" | python3 -c '
import json, sys
d = json.load(sys.stdin)
for item in d.get("artifacts", []):
    if item.get("name") == sys.argv[1] and not item.get("expired"):
        print(item["archive_download_url"])
        break
' "$ARTIFACT_NAME")"
[ -n "$URL" ] || {
	echo "error: artifact $ARTIFACT_NAME not found or expired in run $RUN_ID" >&2
	exit 1
}

mkdir -p "$(dirname "$OUTPUT")"
TMP="$OUTPUT.part"
curl -fsSL --max-time 900 -x "$PROXY" \
	-H "Authorization: Bearer $TOKEN" \
	-H "Accept: application/vnd.github+json" \
	-o "$TMP" "$URL"
mv "$TMP" "$OUTPUT"
echo "$OUTPUT"
