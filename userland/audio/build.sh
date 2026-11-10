#!/usr/bin/env bash
# Build the iOS 16 AudioServerPlugIn used by the marble HAL bridge.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC="$(xcrun --sdk iphoneos --find clang)"

mkdir -p "$OUT/MarbleAudio.driver"
"$CC" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 \
  -Wall -Wextra -Wformat=2 -Werror \
  -bundle -framework CoreFoundation \
  -I"$HERE" "$HERE/marble_audio_plugin.c" \
  -o "$OUT/MarbleAudio.driver/MarbleAudio"
cp "$HERE/Info.plist" "$OUT/MarbleAudio.driver/Info.plist"
codesign --force --sign - "$OUT/MarbleAudio.driver"

file "$OUT/MarbleAudio.driver/MarbleAudio"
otool -hv "$OUT/MarbleAudio.driver/MarbleAudio"
nm -gU "$OUT/MarbleAudio.driver/MarbleAudio" | grep -q '_MarbleAudio_Create$'
plutil -lint "$OUT/MarbleAudio.driver/Info.plist"
shasum -a 256 "$OUT/MarbleAudio.driver/MarbleAudio" \
  "$OUT/MarbleAudio.driver/Info.plist" > "$OUT/SHA256SUMS"
ls -lh "$OUT/MarbleAudio.driver/MarbleAudio" "$OUT/MarbleAudio.driver/Info.plist"
