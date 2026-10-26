#!/system/bin/sh
# Find the AudioReach graph blob in a running Android's audio HAL memory.
#
#   adb push tools/find_agm_graph.sh /data/local/tmp/
#   adb shell su -c "sh /data/local/tmp/find_agm_graph.sh [PID]"
#
# v1 read every rw mapping end to end and appeared to hang; this one scans
# only the audio HAL, only mappings that could hold a graph (anonymous or
# dma-heap, 4 KiB..4 MiB), and at most 256 KiB of each, printing a line per
# mapping so progress is visible.
#
# Signatures (little endian):
#   02 00 00 b0                 device-graph subgraph id 0xb0000002
#   d6 00 00 b0                 stream-graph subgraph id 0xb00000d6
#   01 10 00 08 30 00 00 00     AGM module-param header every graph starts with
#
# Hits are saved to /data/local/tmp/agm-<pid>-<off>.bin (8 KiB centred).
set -u

OUTDIR=/data/local/tmp
MAXMAP=$((4 * 1024 * 1024))
MAXREAD=$((256 * 1024))

log() { echo "$@"; }

if [ "$(id -u)" != 0 ]; then
	log "need root: run with su -c"
	exit 1
fi

PID="${1:-}"
if [ -z "$PID" ]; then
	for p in $(ls /proc | grep -E '^[0-9]+$'); do
		c=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
		case "$c" in
		*android.hardware.audio.service*|*audio.service*) PID=$p; break ;;
		esac
	done
fi
if [ -z "$PID" ] || [ ! -r "/proc/$PID/mem" ]; then
	log "no audio HAL pid (pass one as the first argument)"
	log "candidates:"
	for p in $(ls /proc | grep -E '^[0-9]+$'); do
		c=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
		case "$c" in *audio*|*agm*) log "  $p  $c" ;; esac
	done
	exit 1
fi
log "scanning pid $PID  ($(tr '\0' ' ' < /proc/$PID/cmdline 2>/dev/null))"

hits=0
scanned=0
while read -r line; do
	range=$(echo "$line" | awk '{print $1}')
	perms=$(echo "$line" | awk '{print $2}')
	path=$(echo "$line" | awk '{print $6}')
	case "$perms" in rw*) ;; *) continue ;; esac
	start=$((0x${range%-*}))
	end=$((0x${range#*-}))
	len=$((end - start))
	[ "$len" -ge 4096 ] || continue
	[ "$len" -le "$MAXMAP" ] || continue
	# Skip ordinary file mappings (apex/apk/system libs); a graph lives in
	# anonymous memory or an ion/dma-heap buffer.
	case "$path" in
	/*.so|/*.jar|/*.apk|/*.odex|/*.oat|/*.vdex|/*/system/*|/*/apex/*) continue ;;
	esac
	rd=$len
	[ "$rd" -gt "$MAXREAD" ] && rd=$MAXREAD
	scanned=$((scanned + rd))
	log "  scan $range len=0x$(printf %x $len) rd=0x$(printf %x $rd) ${path:-anon}"
	for pat in '\x02\x00\x00\xb0' '\xd6\x00\x00\xb0' '\x01\x10\x00\x08\x30\x00\x00\x00'; do
		hit=$(dd if="/proc/$PID/mem" bs=4096 skip=$((start / 4096)) \
			count=$(((rd + 4095) / 4096)) 2>/dev/null | \
			LC_ALL=C grep -abo "$(printf "$pat")" -m1)
		[ -n "$hit" ] || continue
		off=$(echo "$hit" | cut -d: -f1)
		abs=$((start + off))
		hits=$((hits + 1))
		f="$OUTDIR/agm-$PID-$(printf %x $abs).bin"
		from=$((abs - 4096))
		[ "$from" -lt "$start" ] && from=$start
		dd if="/proc/$PID/mem" bs=4096 skip=$((from / 4096)) count=2 \
			2>/dev/null > "$f"
		log "HIT pat=$pat off=0x$(printf %x $abs) -> $f"
	done
done < "/proc/$PID/maps"

log "== done: $hits hit(s), scanned 0x$(printf %x $scanned) bytes =="
if [ "$hits" = 0 ]; then
	log "Play a sound first (the HAL builds the graph when a stream starts),"
	log "then rerun.  If it still finds nothing, try the AGM service pid."
fi
