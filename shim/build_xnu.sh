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
-Wall -Wextra -Wno-unused-function -Wno-unused-variable \
-DGPU_PROBE_LEVEL=${GPU_PROBE_LEVEL:-0} \
-DGP_CBAR_BANK=${GP_CBAR_BANK:--1} \
-DGP_CBAR_WRITE=${GP_CBAR_WRITE:-0} \
-DGP_CBAR_COUNT=${GP_CBAR_COUNT:-0} \
-DGP_SCTLR_BANK=${GP_SCTLR_BANK:--1} \
-DGP_SCTLR_WRITE=${GP_SCTLR_WRITE:-0} \
-DGP_CB_REGS=${GP_CB_REGS:-0} \
-DGP_GR1_READ=${GP_GR1_READ:-0} \
-DGP_KGSL_SCAN=${GP_KGSL_SCAN:-0} \
-DGP_SURVEY=${GP_SURVEY:-0} \
-DGP_PWR=${GP_PWR:-0} \
-DGP_EARLY=${GP_EARLY:-0} \
-DGP_GX=${GP_GX:-0} \
-DGP_CBW=${GP_CBW:-0} \
-DGP_CBT=${GP_CBT:-0} \
-DGP_CMDB=${GP_CMDB:-0} \
-DGP_RAIL=${GP_RAIL:-0} \
-DGP_CLK=${GP_CLK:-0} \
-DGP_GMU=${GP_GMU:-0} \
-DGP_CLK_SEL=${GP_CLK_SEL:-6} \
-DGP_BR=${GP_BR:-0} \
-DGP_GCC=${GP_GCC:-0} \
-DGP_VOTE=${GP_VOTE:-0} \
-DGP_RSCC=${GP_RSCC:-0} \
-DGP_GMUBOOT=${GP_GMUBOOT:-0} \
-DGP_TCM=${GP_TCM:-0} \
-DGP_STAGE=${GP_STAGE:-0}"
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

# PCM handed to the DSP: raw 48 kHz / 16-bit LE / stereo.  PCM_SRC can point
# at any audio file (ffmpeg converts it); without one we build a 1 kHz sine.
# Length is what the kernel declares as the shared-memory region: 0x5000.
PCM_BYTES="${PCM_BYTES:-20480}"
PCM_SRC="${PCM_SRC:-}"
# Amplitude of the test clip.  The speaker graph only has to be proven to
# play, and the operator is often somewhere quiet, so default to -40 dB.
PCM_GAIN="${PCM_GAIN:--40dB}"
if [ -n "$PCM_SRC" ] && [ ! -f "$PCM_SRC" ]; then
  echo "error: PCM_SRC not found: $PCM_SRC" >&2
  exit 1
fi
if [ ! -f pcm.raw ] || [ -n "$PCM_SRC" ] || [ pcm.raw -ot splash.png ]; then
  if [ ! -f pcm.raw ] || [ -n "$PCM_SRC" ]; then
    tmp="pcm.tmp.s16le"
    if [ -n "$PCM_SRC" ]; then
      ffmpeg -y -loglevel error -i "$PCM_SRC" -af "volume=$PCM_GAIN" \
        -f s16le -acodec pcm_s16le -ac 2 -ar 48000 "$tmp"
    else
      ffmpeg -y -loglevel error -f lavfi \
        -i "sine=frequency=1000:sample_rate=48000:duration=30" \
        -af "volume=$PCM_GAIN" -f s16le -acodec pcm_s16le -ac 2 "$tmp"
    fi
    python3 - "$tmp" pcm.raw "$PCM_BYTES" <<'PYPCM'
import sys
src, out, n = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
n -= n % 4                       # whole 16-bit stereo frames (4 bytes)
d = open(src, 'rb').read()[:n]
if not d:
    d = b'\0' * n
d = (d * ((n // len(d)) + 1))[:n]
open(out, 'wb').write(d)
print('pcm.raw: %d bytes (%.2f s at 48k/16/stereo)' % (n, n / 192000.0))
PYPCM
    rm -f "$tmp"
  fi
fi

for f in start.S shim.c console.c mmu.c fdt.c loader.c splash.S pcm.S gmu_fw.S; do
  $CC $CFLAGS -DFB_STRIDE=$STRIDE -DPCM_REGION=$PCM_BYTES -c "$f" -o "$OUT/${f%.*}.o"
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
  "$OUT/fdt.o" "$OUT/loader.o" "$OUT/splash.o" "$OUT/pcm.o" \
  "$OUT/gmu_fw.o" -o "$OUT/shim.elf"

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
