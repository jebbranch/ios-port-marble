#!/bin/bash
# Install and verify the exact source versions in tools/source-lock.json.
set -e
cd "$(dirname "$0")/.."
exec python3 tools/fetch-sources.py "$@"
