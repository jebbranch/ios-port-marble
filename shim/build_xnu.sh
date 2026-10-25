#!/bin/bash
# Build the marble X1 shim: shim + loader + an embedded SWEETXNU payload,
# wrapped in an Android boot image v4 for `fastboot boot` (RAM-only).
# Usage: ./build_xnu.sh [PAYLOAD]
set -euo pipefail
cd "$(dirname "$0")"

NDK="${ANDROID_NDK_HOME:-/opt/homebrew/Caskroom/android-ndk/28/AndroidNDK13004108.app/Contents/NDK}"
export PATH="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH"

CC="clang --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -fno-jump-tables \
-mgeneral-regs-only -mstrict-align -fno-stack-protector \
-Wall -Wextra -Wno-unused-function -Wno-unused-variable"
STRIDE=4320
OUT="out-xnu"
PAYLOAD="${1:-../xnu/out/marble.payload}"

if [ ! -f "$PAYLOAD" ]; then
  echo "error: payload not found: $PAYLOAD" >&2
  echo "usage: $0 [PAYLOAD]" >&2
  exit 1
fi

mkdir -p "$OUT"

# Boot splash: convert splash.png to raw BGRA (1080x2400) for .incbin.
if [ ! -f splash.bgra ] || [ splash.png -nt splash.bgra ]; then
  python3 - <<'PY'
from PIL import Image
import numpy as np
a = np.asarray(Image.open('splash.png').convert('RGB'))
h, w, _ = a.shape
out = np.empty((h, w, 4), dtype=np.uint8)
out[..., 0] = a[..., 2]   # B
out[..., 1] = a[..., 1]   # G
out[..., 2] = a[..., 0]   # R
out[..., 3] = 0xff        # X
out.tofile('splash.bgra')
print('splash.bgra: %dx%d, %d bytes' % (w, h, out.size))
PY
fi

for f in start.S shim.c console.c mmu.c fdt.c loader.c splash.S; do
  $CC $CFLAGS -DFB_STRIDE=$STRIDE -c "$f" -o "$OUT/${f%.*}.o"
done

# Only start.o may carry absolute addresses (it is linked at 0).
abs=$(llvm-readelf -r "$OUT/shim.o" "$OUT/console.o" "$OUT/mmu.o" \
        "$OUT/fdt.o" "$OUT/loader.o" | grep -c ABS64 || true)
if [ "$abs" != 0 ]; then
  echo "error: $abs absolute relocations in C objects" >&2
  exit 1
fi

$CC $CFLAGS -fuse-ld=lld -static -T shim.ld -Wl,--build-id=none -Wl,-N \
  "$OUT/start.o" "$OUT/shim.o" "$OUT/console.o" "$OUT/mmu.o" \
  "$OUT/fdt.o" "$OUT/loader.o" "$OUT/splash.o" -o "$OUT/shim.elf"

llvm-objcopy -O binary "$OUT/shim.elf" "$OUT/Image"
SIZE_HEX="$(llvm-nm "$OUT/shim.elf" | awk '$3=="_end"{print $1}')"
if [ -z "$SIZE_HEX" ]; then
  echo "error: _end symbol not found" >&2
  exit 1
fi
SIZE=$((16#$SIZE_HEX))
truncate -s "$SIZE" "$OUT/Image"

# loader.c expects the payload at the first 4K boundary after _end, and the
# ARM64 Image header's image_size (offset 16) must grow to include it.
truncate -s $(( (SIZE + 0xfff) & ~0xfff )) "$OUT/Image"
cat "$PAYLOAD" >> "$OUT/Image"
python3 - "$OUT/Image" <<'PY'
import os, struct, sys
p = sys.argv[1]
with open(p, 'r+b') as f:
    f.seek(16)
    f.write(struct.pack('<Q', os.path.getsize(p)))
PY

# ABL dislikes a zero-size ramdisk; an empty newc cpio is enough.
( cd "$OUT" && rm -rf rd && mkdir rd && cd rd && \
  find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin )

python3 mkbootimg_v4.py --kernel "$OUT/Image" --ramdisk "$OUT/ramdisk.bin" \
  --cmdline "console=null" -o "$OUT/shim-xnu.img"

ls -l "$OUT/Image" "$OUT/ramdisk.bin" "$OUT/shim-xnu.img"
