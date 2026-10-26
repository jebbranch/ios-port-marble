#!/system/bin/sh
# Find the AudioReach graph blobs in a running Android's userspace memory.
#
#   adb push tools/find_agm_graph.sh /data/local/tmp/
#   adb shell su -c "sh /data/local/tmp/find_agm_graph.sh"
#
# Why: our own merged graph is the last structural difference from the vendor
# (vendor opens a stream graph + a device graph and connects them across
# graphs; we merged both into one).  The vendor's device graph subgraph id is
# 0xb0000002, which appears in the payload as the little-endian bytes
# 02 00 00 b0 - so we can locate the exact bytes the HAL handed to the DSP.
#
# Output: /data/local/tmp/agm-<pid>-<off>.bin, 8 KiB centred on the hit, plus
# a one-line summary per hit.  Nothing is written outside /data/local/tmp.
set -u

OUTDIR=/data/local/tmp
PATTERN='\x02\x00\x00\xb0'          # 0xb0000002, LE
ALSO='\xd6\x00\x00\xb0'             # 0xb00000d6, LE (stream graph)
MAXSCAN=$((64 * 1024 * 1024))       # skip mappings larger than this

log() { echo "$@"; }

if [ "$(id -u)" != 0 ]; then
	log "need root: run as  su -c \"sh $0\""
	exit 1
fi

log "== processes that plausibly build AudioReach graphs =="
pids=""
for pid in $(ls /proc | grep -E '^[0-9]+$'); do
	cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
	case "$cmd" in
	*audio*|*agm*|*AudioReach*|*audioreach*|*hal*audio*)
		log "  $pid  $cmd"
		pids="$pids $pid"
		;;
	esac
done

# Fall back to every process if the names did not match (some ROMs rename them)
if [ -z "$pids" ]; then
	log "  (no name match; scanning all readable processes)"
	for pid in $(ls /proc | grep -E '^[0-9]+$'); do
		[ -r "/proc/$pid/maps" ] && pids="$pids $pid"
	done
fi

hits=0
for pid in $pids; do
	[ -r "/proc/$pid/mem" ] || continue
	maps="/proc/$pid/maps"
	[ -r "$maps" ] || continue
	while read -r line; do
		# start-end perms offset dev inode pathname
		range=$(echo "$line" | awk '{print $1}')
		perms=$(echo "$line" | awk '{print $2}')
		path=$(echo "$line" | awk '{print $6}')
		case "$perms" in rw*) ;; *) continue ;; esac
		start=${range%-*}
		end=${range#*-}
		len=$((0x$end - 0x$start))
		[ "$len" -gt 0 ] || continue
		[ "$len" -le "$MAXSCAN" ] || continue
		# Only anonymous, heap-ish or dma-heap mappings can hold the blob.
		case "$path" in
		*audio*|*agm*|*dma_heap*|*ion*|""|"[anon:dalvik-"*) ;;
		*) continue ;;
		esac
		for pat in "$PATTERN" "$ALSO"; do
			hitchunk=$(dd if="/proc/$pid/mem" bs=4096 skip=$((0x$start / 4096)) \
				count=$(( (len + 4095) / 4096 )) 2>/dev/null | \
				LC_ALL=C grep -abo "$(printf "$pat")" -m1)
			[ -n "$hitchunk" ] || continue
			off=$(echo "$hitchunk" | cut -d: -f1)
			abs=$((0x$start + off))
			hits=$((hits + 1))
			log "HIT pid=$pid off=0x$(printf %x $abs) map=$range path=${path:-anon}"
			from=$((abs - 4096))
			[ "$from" -lt "$((0x$start))" ] && from=$((0x$start))
			dd if="/proc/$pid/mem" bs=4096 skip=$((from / 4096)) count=2 \
				2>/dev/null > "$OUTDIR/agm-$pid-$(printf %x $abs).bin"
			log "     saved $OUTDIR/agm-$pid-$(printf %x $abs).bin"
		done
	done < "$maps"
done

log "== done: $hits hit(s) =="
if [ "$hits" = 0 ]; then
	log "No graph signature found.  Play a sound first (the HAL builds the"
	log "graph when a stream starts), then run this again."
fi
