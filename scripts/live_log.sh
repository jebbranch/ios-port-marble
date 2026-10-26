#!/bin/bash
# Live XNU console over USB Ethernet (the sweetusb gadget's ECM NIC).
#
#   ./scripts/live_log.sh [label]        # new file, stream until Ctrl-C
#   APPEND=1 ./scripts/live_log.sh [label]   # append to the same label
#   IFACE=en7 ./scripts/live_log.sh          # pin the interface if already up
#   MAX_MB=64 ./scripts/live_log.sh          # rotate at 64 MiB (default 16)
#
# Start it BEFORE `fastboot boot`, then boot. Needs sudo for ifconfig.
#
# Duplicate handling (the reason this script is not a bare `nc -ul`):
#   * the device keeps its own read position and only replays the last 64 KiB
#     ONCE, when its worker thread starts - so re-running the listener does not
#     re-send anything.  Duplicates would instead come from the host appending
#     a second boot into the same file, so:
#       - a fresh label gets a NEW file (name-2.txt, name-3.txt ... if taken);
#         appending requires APPEND=1 and says so in the header
#       - every session writes a header with the time and interface
#       - a boot boundary marker is inserted whenever the early-boot line
#         appears again, so two boots in one file cannot be mistaken for one
#       - the file rotates at MAX_MB, keeping one previous generation
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

LABEL="${1:-$(date +%H%M%S)}"
OUT="$ROOT/logs/live-$LABEL.txt"
PORT=5140
HOSTIP=192.168.7.1
NETMASK=255.255.255.0
MAX_MB="${MAX_MB:-16}"
APPEND="${APPEND:-0}"
mkdir -p "$ROOT/logs"

find_iface() {
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

if ! ifconfig "$ifc" | grep -q "inet $HOSTIP "; then
  echo "configuring $ifc $HOSTIP/$NETMASK (sudo)"
  sudo ifconfig "$ifc" "$HOSTIP" netmask "$NETMASK" up || {
    echo "!! ifconfig failed; run manually:" >&2
    echo "   sudo ifconfig $ifc $HOSTIP netmask $NETMASK up" >&2
    exit 1
  }
fi
ifconfig "$ifc" | sed -n '1,4p'

if [ "$APPEND" != 1 ] && [ -e "$OUT" ]; then
  n=2
  while [ -e "$ROOT/logs/live-$LABEL-$n.txt" ]; do n=$((n + 1)); done
  OUT="$ROOT/logs/live-$LABEL-$n.txt"
  echo "note: live-$LABEL.txt already exists; writing $OUT instead"
  echo "      (use APPEND=1 to deliberately continue an old file)"
fi

echo
echo "listening on UDP $PORT -> $OUT   (rotate at ${MAX_MB} MiB)"
echo "(Ctrl-C to stop; the phone keeps running)"
echo

exec python3 - "$PORT" "$OUT" "$ifc" "$APPEND" "$MAX_MB" <<'PY'
import os, socket, sys, time

port = int(sys.argv[1])
path = sys.argv[2]
iface = sys.argv[3]
append = sys.argv[4] == '1'
max_bytes = int(sys.argv[5]) * 1024 * 1024

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', port))

# Every boot replays from the start, so this line marks a new session.
BOOT_MARK = b'SWEET CPU: cpu=0 mpidr='

def header(tag):
    return (b"===== %s %s iface=%s port=%d =====\n"
            % (tag, time.strftime('%Y-%m-%d %H:%M:%S').encode(), iface.encode(), port))

total = 0

def open_out():
    global total
    if append and os.path.exists(path):
        f = open(path, 'ab', buffering=0)
        f.write(header(b"session (appended)"))
        total = os.path.getsize(path)
    else:
        f = open(path, 'wb', buffering=0)
        f.write(header(b"session start"))
        total = 0
    return f

f = open_out()
start = last = time.time()
nbytes = nl = 0
boots = 0       # how many boot markers seen in this session
last_boot = 0.0  # when the previous one arrived

while True:
    try:
        s.settimeout(5.0)
        d, a = s.recvfrom(65535)
    except socket.timeout:
        continue
    if not d:
        continue

    if BOOT_MARK in d:
        # The marker also sits inside ordinary chunks (a chunk can straddle it,
        # and the 64 KiB replay repeats the boot's early lines), so only a
        # marker that follows >60 s of silence starts a new boot.  Real boots
        # are minutes apart; this also keeps a split line from double counting.
        now_b = time.time()
        if now_b - last_boot > 60.0:
            boots += 1
            if boots > 1:
                d = (b"\n===== boot boundary (another replays from the start) =====\n" + d)
            last_boot = now_b

    f.write(d)
    total += len(d)
    nbytes += len(d)
    nl += d.count(b'\n')
    try:
        sys.stdout.write(d.decode('utf-8', 'replace'))
        sys.stdout.flush()
    except Exception:
        pass

    now = time.time()
    if now - last > 10:
        sys.stderr.write("[live_log] %.1fs, %d bytes, %d lines, boots=%d, file=%.1f MiB\n"
                         % (now - start, nbytes, nl, boots, total / 1048576.0))
        sys.stderr.flush()
        last = now

    if total >= max_bytes:
        f.close()
        prev = path + '.1'
        try:
            if os.path.exists(prev):
                os.unlink(prev)
            os.rename(path, prev)
        except Exception as e:
            sys.stderr.write("[live_log] rotate failed: %r\n" % (e,))
        f = open(path, 'wb', buffering=0)
        f.write(header(b"session rotated"))
        total = 0
        sys.stderr.write("[live_log] rotated, previous generation at %s\n" % prev)
PY
