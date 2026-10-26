#!/bin/bash
# Live XNU console over USB Ethernet (the sweetusb gadget's ECM NIC).
#
#   ./scripts/live_log.sh [label]        # watch for the phone, then stream
#   IFACE=en7 ./scripts/live_log.sh      # pin the interface if it is already up
#
# The device side is already in the kernel: sweet_usb.c brings up ue0 with
# 192.168.7.2/24 and a worker sends every new console byte as a UDP datagram
# to 192.168.7.1:5140 (starting with the last 64 KiB, so the boot is included).
# This script is the host half: it finds the new en* interface that appears when
# the phone boots, gives it 192.168.7.1/24, and writes the stream to
# logs/live-<label>.txt while echoing it to the terminal.
#
# Start it BEFORE `fastboot boot`, then boot. Needs sudo for ifconfig.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

LABEL="${1:-$(date +%H%M%S)}"
OUT="$ROOT/logs/live-$LABEL.txt"
PORT=5140
HOSTIP=192.168.7.1
NETMASK=255.255.255.0
mkdir -p "$ROOT/logs"

find_iface() {
  # The gadget NIC appears as an en* that was not in the list before we started.
  local before="$1"
  for ifc in $(ifconfig -l); do
    case "$ifc" in en*) ;; *) continue ;; esac
    case " $before " in *" $ifc "*) continue ;; esac
    printf '%s\n' "$ifc"
    return 0
  done
  return 1
}

if [ -n "${IFACE:-}" ]; then
  ifc="$IFACE"
  echo "using $ifc (IFACE override)"
else
  before=" $(ifconfig -l) "
  echo "waiting for the phone's USB Ethernet interface (up to 5 min)..."
  ifc=""
  for _ in $(seq 1 300); do
    if ifc=$(find_iface "$before"); then break; fi
    sleep 1
  done
  if [ -z "$ifc" ]; then
    echo "!! no new en* interface appeared." >&2
    echo "   Is the phone running XNU (not Android/TWRP) and plugged in?" >&2
    echo "   If the interface already existed, rerun with IFACE=enN." >&2
    exit 1
  fi
  echo "found $ifc"
fi

# macOS wants an address on that link; the device is 192.168.7.2/24.
if ! ifconfig "$ifc" | grep -q "inet $HOSTIP "; then
  echo "configuring $ifc $HOSTIP/$NETMASK (sudo)"
  sudo ifconfig "$ifc" "$HOSTIP" netmask "$NETMASK" up || {
    echo "!! ifconfig failed; run manually:" >&2
    echo "   sudo ifconfig $ifc $HOSTIP netmask $NETMASK up" >&2
    exit 1
  }
fi
ifconfig "$ifc" | sed -n '1,4p'

echo
echo "listening on UDP $PORT -> $OUT"
echo "(Ctrl-C to stop; the phone keeps running)"
echo

# python3 is always present on macOS; SO_REUSEADDR lets a stale socket go away.
exec python3 - "$PORT" "$OUT" <<'PY'
import socket, sys, time

port = int(sys.argv[1])
out = sys.argv[2]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', port))

f = open(out, 'ab', buffering=0)
start = time.time()
last = start
nbytes = 0
nl = 0
while True:
    try:
        s.settimeout(5.0)
        d, a = s.recvfrom(65535)
    except socket.timeout:
        continue
    if not d:
        continue
    f.write(d)
    nbytes += len(d)
    nl += d.count(b'\n')
    try:
        sys.stdout.write(d.decode('utf-8', 'replace'))
        sys.stdout.flush()
    except Exception:
        pass
    now = time.time()
    if now - last > 10:
        sys.stderr.write("[live_log] %.1fs, %d bytes, %d lines, last from %s\n"
                         % (now - start, nbytes, nl, a[0]))
        sys.stderr.flush()
        last = now
PY
