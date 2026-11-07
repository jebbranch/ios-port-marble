#!/bin/bash
# Build the RAM-only boot image:
#   patched XNU kernel + ADSP firmware blob -> XNU payload -> shim -> boot.img
#
#   ADSP_FW_DIR=/path/to/adsp2-segments ./scripts/mkimage.sh [extra boot args ...]
#
# Inputs:
#   out/kernel.adsp          from scripts/fetch_kernel.sh
#   $ADSP_FW_DIR/adsp2.*     split firmware pulled from YOUR phone
#                            (scripts/extract_adsp_fw.sh), default
#                            $HOME/Desktop/marble-ios/work/adsp-fw
# Output:
#   images/shim-iosroot-adsp.img
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$ROOT/out"
K="${KERNEL:-$OUT/kernel.adsp}"
FW_DIR="${ADSP_FW_DIR:-$HOME/Desktop/marble-ios/work/adsp-fw}"

[ -f "$K" ] || { echo "missing $K; run scripts/fetch_kernel.sh first" >&2; exit 1; }
[ -f "$FW_DIR/adsp2.mdt" ] || {
  echo "missing $FW_DIR/adsp2.mdt; run scripts/extract_adsp_fw.sh" >&2; exit 1; }

python3 -I - "$K" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
n = d.count(bytes.fromhex('28e21bd5'))
if n:
    sys.exit('refusing: kernel still has %d unpatched MSR CNTP_CTL' % n)
PY

# ufsroot_* describe the HFS+ root image that lives in this phone's userdata
# partition.  Change them if your partition layout differs.
# Two base args are deliberately NOT listed, both provable no-ops that were
# eating the 607-byte boot-args field:
#   cs_debug=0    - bsd/kern/kern_cs.c:87 initialises int cs_debug = 0
#   sweetkeys=1   - xnu/patches/0060-sweet-keys...:sweet_keys_init() starts at
#                   int enabled = 1, so only sweetkeys=0 changes anything
CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 ufsroot=userdata ufsroot_lba=0x3603600 ufsroot_blocks=5243387 ufsrw=1 rootrw=1 wdt=-1 sweetusb=2 marbleusb=1 ufslogsecs=1 sweetsample=- cs_enforcement_disable=1 unentitled_ios_sim_launch=1 sweettouch=0 marble_touch=1 sweetfb_noconsole=1 sweetiolog=- sweetadsp=1 gicrstride=0x40000 sweetbatt=1 sweetglink=1 sweetreboot=1'
for a in "$@"; do CMDLINE="$CMDLINE $a"; done
# KEEPALIVE=1 builds an image that stays in iOS instead of resetting itself
# after boot.  Use it when a human needs to touch the GUI (e.g. to feel the
# haptics); log collection needs the default reset behaviour.
if [ "${KEEPALIVE:-0}" = 1 ]; then
  CMDLINE=$(printf '%s' "$CMDLINE" | sed 's/ *sweetreboot=1//')
fi
# CPUS=n replaces the single-core default; interactive testing needs the
# other cores available or every GPIO-bitbang or I2C scan stalls the UI.
if [ -n "${CPUS:-}" ]; then
  CMDLINE=$(printf '%s' "$CMDLINE" | sed "s/cpus=[0-9]*/cpus=$CPUS/")
fi
# QUIET=1 turns kernel debug output and the persistent SWEETLOG ring off
# (debug=0 + sweetlog=0).  The ring costs disk I/O; skip it on keepalive
# images where no log collection is planned.
if [ "${QUIET:-0}" = 1 ]; then
  CMDLINE=$(printf '%s' "$CMDLINE" | sed 's/debug=0xc/debug=0/')
  CMDLINE="$CMDLINE sweetlog=0"
fi
# Host wall clock at image build time; XNU sets its calendar from this.
CMDLINE="$CMDLINE sweettime=$(date +%s)"

# SWEETIOLOG=<procname[,procname]> turns on the userspace IOKit query trace
# that 0049-sweet-iokit-trace.patch already compiles into every kernel:
# every io_service_get_matching_services / _service / add_notification /
# registry-path lookup made by a named process is printed as
#   IOK <name>: matching <dict> -> <N found | class | none>
# capped at 400 lines per boot.  The base string carries sweetiolog=- (off), so
# the flag has to be substituted rather than appended - PE_parse_boot_argn
# returns the FIRST match, and the base copy comes earlier in the line.  This
# has never been enabled in any archived ring (no "IOK " line in logs/ring-*.txt
# at all), which makes it the cheapest answer we can get to "does anything in
# userspace even look for an audio class".
if [ -n "${SWEETIOLOG:-}" ]; then
  CMDLINE=$(printf '%s' "$CMDLINE" | sed "s/sweetiolog=-/sweetiolog=${SWEETIOLOG}/")
fi

# The boot-args field is 607 bytes: mkpayload.py puts CMDLINE at 0x48 and the
# segment table at 0x2a8, i.e. BOOT_LINE_LENGTH 608 (kernel + sysctl copy it
# with strlcpy(..., BOOT_LINE_LENGTH)).  Until now anything past byte 606 was
# sliced off in silence, which voided whole cells and corrupted one value:
# ring-ab lost sweetibatburst/sweetaudioprobe/sweetbufs (so its tone ran
# uncapped at 1200 buffers instead of 30), and ring-i4's "sweetbufs=30" was cut
# mid-value into "sweetbufs=3" and then looked like a valid measurement.
# Budget 590 so a future append cannot land exactly on the edge.
if [ "${#CMDLINE}" -gt 590 ]; then
  echo "::error::cmdline is ${#CMDLINE} bytes; the field holds 607 and byte 590 is the budget." >&2
  echo "   tail that would be lost: ${CMDLINE:560}" >&2
  exit 1
fi

mkdir -p "$OUT"

# The ADT needs Apple's D22AP device tree (product metadata).  It is not in
# this repo; extract it from YOUR iPhone X IPSW and point D22AP_ADT at it
# (default: research/ipsw-x/d22ap.adt).
BASE_ADT="${D22AP_ADT:-$ROOT/research/ipsw-x/d22ap.adt}"
[ -f "$BASE_ADT" ] || {
  echo "missing Apple base ADT: $BASE_ADT" >&2
  echo "set D22AP_ADT=/path/to/d22ap.adt (see scripts/mkadt.py header)" >&2
  exit 1; }
D22AP_ADT="$BASE_ADT" python3 -I "$ROOT/scripts/mkadt.py" "$OUT/marble.adt" --highmem

python3 -I "$ROOT/scripts/mkfwblob.py" "$FW_DIR" "$OUT/adsp2.fwblob"
python3 -I "$ROOT/scripts/mkpayload.py" "$K" "$OUT/marble.adt" \
  "$OUT/marble-adsp.payload" --cmdline "$CMDLINE" \
  --ramdisk "$OUT/adsp2.fwblob" | tail -2

( cd "$ROOT/shim" && ./build_xnu.sh "$OUT/marble-adsp.payload" 2>&1 | tail -2 )

mkdir -p "$ROOT/images"
cp -f "$ROOT/shim/out-xnu/shim-xnu.img" "$ROOT/images/shim-iosroot-adsp.img"
echo "cmdline: $CMDLINE"
ls -lh "$ROOT/images/shim-iosroot-adsp.img"
