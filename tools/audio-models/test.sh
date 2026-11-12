#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TEST_DIR="$(mktemp -d /tmp/marble-periods-test.XXXXXX)"
clang -std=gnu11 -Wall -Wextra -Wformat=2 -Werror \
  -fsanitize=address,undefined -I"$HERE" \
  "$HERE/marble_periods.c" "$HERE/test_periods.c" -o "$TEST_DIR/test"
"$TEST_DIR/test"
