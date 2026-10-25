#!/bin/bash
# Boot the shim image with `fastboot boot` — RAM ONLY.
# Nothing is written to the phone: boot/vendor_boot/userdata/GPT stay untouched.
#
#   SERIAL=2aa26095 ./scripts/boot_ram.sh [image]
#
# Default image: images/shim-iosroot-adsp.img
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
IMAGE="${1:-$ROOT/images/shim-iosroot-adsp.img}"
SERIAL="${SERIAL:-}"

[ -f "$IMAGE" ] || { echo "missing image: $IMAGE" >&2; exit 1; }

if [ -z "$SERIAL" ]; then
  SERIAL=$(adb devices 2>/dev/null | awk '$2=="device"||$2=="recovery"{print $1; exit}')
fi
if [ -z "$SERIAL" ]; then
  SERIAL=$(fastboot devices 2>/dev/null | awk 'NF{print $1; exit}')
fi
[ -n "$SERIAL" ] || { echo "no device in adb/fastboot" >&2; exit 1; }
echo "device: $SERIAL"

in_fastboot() {
  fastboot devices 2>/dev/null | awk '{print $1}' | grep -qx "$SERIAL"
}

# Get into the bootloader.  "fastboot getvar" blocks forever when no device is
# present, so poll "fastboot devices" instead.  TWRP sometimes ignores
# "adb reboot bootloader"; retry through its root shell.  Recovery can take
# well over 30 s to come back up in fastboot (run66 was lost that way), so
# keep polling for 120 s and keep retrying while recovery still answers adb.
in_fastboot || adb -s "$SERIAL" reboot bootloader >/dev/null 2>&1 || true
for _ in $(seq 1 120); do
  in_fastboot && break
  if adb -s "$SERIAL" get-state 2>/dev/null | grep -q recovery; then
    adb -s "$SERIAL" shell reboot bootloader >/dev/null 2>&1 || true
  fi
  sleep 1
done
if ! in_fastboot; then
  echo "device $SERIAL is not in fastboot." >&2
  echo "hold Power+VolDown until the bootloader screen appears, then retry." >&2
  exit 1
fi

echo "booting $IMAGE (RAM-only; nothing is flashed)"
fastboot -s "$SERIAL" boot "$IMAGE"
echo "kernel running.  It resets itself when done; pull logs with scripts/pull_logs.sh"
