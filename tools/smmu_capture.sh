#!/bin/bash
# Capture what the *vendor* Android kernel programs into apps-smmu.
#
#   ./tools/smmu_capture.sh setup          # probe availability, arm what exists
#   ./tools/smmu_capture.sh run -- <cmd>   # trace while <cmd> runs on device
#   ./tools/smmu_capture.sh pull           # fetch + summarise
#   ./tools/smmu_capture.sh install-boot  # arm from /data/adb/post-fs-data.d
#   ./tools/smmu_capture.sh remove-boot
#   ./tools/smmu_capture.sh clean
#
# install-boot is the only way to catch the msm-audio-ion IOMMU attach: it
# happens at ~13 s uptime, long before adb is reachable.  The script also
# enables the iommu tracepoints (attach/io_page_fault/map/unmap).
#
# Why: on XNU we hand-build an S1 translation for the ADSP's audio SID and the
# DSP still dies the moment it reads AP memory.  The Android kernel on the same
# hardware does the same job successfully, so the question is not "is it
# allowed" but "what exactly does it program".  Two ways to find out:
#
#   1. bpftrace + BTF  -> read arm_smmu_device fields directly (best)
#   2. kprobe_events   -> no BTF needed, but only gives (smmu, idx) unless a
#                         *_write helper exists as a real symbol, in which case
#                         we also get (register offset, value) - which is all we
#                         actually need, because SMR/S2CR/CBAR/SCTLR addresses
#                         are fixed offsets from the SMMU base.
#
# Known DT facts for this phone (marble, SM7475):
#   iommus                   = <&apps_smmu 0x1801 0x0>
#   qcom,smmu-sid-mask       = <0xf>        (so 0x1800..0x180f)
#   qcom,iommu-dma-addr-pool = <0x10000000 0x10000000>
#   audio buffers come from /dev/dma_heap/qcom,adsp
#
# apps-smmu base 0x15000000; register map (SMMU-500):
#   SMR(n)     0x800  + 8n      S2CR(n)  0xC00 + 8n
#   CBAR(n)    0x80000 + 0x1000n   SCTLR   +0x000   TCR2 +0x010
#   TTBR0      +0x020   TTBR1   +0x030   MAIR  +0x040   FSR +0x058
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SERIAL="${SERIAL:-2aa26095}"
OUTDIR="${OUTDIR:-$ROOT/logs}"
ADB="adb -s $SERIAL"
DEVSH=/data/local/tmp/smmu_capture_dev.sh

PROBES=(
	arm_smmu_write_smr
	arm_smmu_write_s2cr
	arm_smmu_write_context_bank
	arm_smmu_gr0_write
	arm_smmu_gr1_write
	arm_smmu_cb_write
)

devscript() {
	cat <<'EOS'
#!/system/bin/sh
# device half of tools/smmu_capture.sh
set -u
T=/sys/kernel/tracing
[ -e "$T/kprobe_events" ] || T=/sys/kernel/debug/tracing
mkdir -p /sys/kernel/debug 2>/dev/null
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null

have_sym() { grep -q " $1\$" /proc/kallsyms 2>/dev/null || grep -q " $1\$" /proc/modules 2>/dev/null; }

case "$1" in
setup)
	echo 0 > "$T/tracing_on"
	echo > "$T/kprobe_events" 2>/dev/null
	n=0
	# kallsyms line: "addr t symbol<TAB>[module]" - match the 3rd field.
	has() { awk '{print $3}' /proc/kallsyms 2>/dev/null | grep -qx "$1"; }
	arm() {   # arm <event> <symbol> <fields>
		if echo "p:$1 $2 $3" >> "$T/kprobe_events" 2>/dev/null; then
			n=$((n+1)); echo "  armed $2"
		elif echo "p:$1 arm_smmu:$2 $3" >> "$T/kprobe_events" 2>/dev/null; then
			n=$((n+1)); echo "  armed arm_smmu:$2"
		else
			echo "  FAILED $2"
		fi
	}
	for f in arm_smmu_write_smr arm_smmu_write_s2cr \
	         arm_smmu_write_context_bank arm_smmu_gr0_write \
	         arm_smmu_gr1_write arm_smmu_cb_write; do
		has "$f" && arm "smmu_$f" "$f" "a0=%x0 a1=%x1 a2=%x2"
	done
	# Attach path: SMR/S2CR are usually written there, not on every map.
	for f in $(awk '{print $3}' /proc/kallsyms 2>/dev/null | \
	    grep -E "^(arm_smmu_(attach_dev|domain_add_master|probe_device))" | \
	    sed "s/\$[0-9a-f]*$//" | sort -u | head -4); do
		[ -n "$f" ] && arm "smmu_$(echo $f | tr -c 'a-zA-Z0-9_' '_')" "$f" "a0=%x0 a1=%x1"
	done
	echo 8192 > "$T/buffer_size_kb"
	for e in "$T"/events/kprobes/*/enable; do echo 1 > "$e"; done 2>/dev/null
	echo "armed $n probe(s)"
	[ -f /sys/kernel/btf/vmlinux ] && echo "BTF present: yes" || echo "BTF present: no"
	command -v bpftrace >/dev/null && echo "bpftrace: yes" || echo "bpftrace: no"
	;;
start) echo 1 > "$T/tracing_on" ;;
stop)  echo 0 > "$T/tracing_on" ;;
clean)
	echo 0 > "$T/tracing_on"
	echo > "$T/kprobe_events" 2>/dev/null
	echo "probes removed"
	;;
esac
EOS
}

case "${1:-}" in
setup)
	devscript > /tmp/smmu_capture_dev.sh
	$ADB push /tmp/smmu_capture_dev.sh "$DEVSH" >/dev/null
	$ADB shell "su -c 'chmod 755 $DEVSH'"
	$ADB shell "su -c 'sh $DEVSH setup'"
	;;
run)
	shift
	[ "${1:-}" = "--" ] && shift
	[ $# -gt 0 ] || { echo "usage: $0 run -- <device command>" >&2; exit 2; }
	$ADB shell "su -c 'sh $DEVSH start; $*; echo rc=\$?; sh $DEVSH stop'"
	;;
pull)
	mkdir -p "$OUTDIR"
	f="$OUTDIR/smmu-$(date +%Y%m%d-%H%M%S).txt"
	$ADB shell "su -c 'cat /sys/kernel/tracing/trace'" > "$f" 2>/dev/null
	[ -s "$f" ] || $ADB shell "su -c 'cat /sys/kernel/debug/tracing/trace'" > "$f"
	echo "trace: $f"
	python3 - "$f" <<'PY'
import re, sys, collections
p = sys.argv[1]
rows = collections.Counter()
samples = {}
for line in open(p, errors='replace'):
    m = re.search(r'smmu_(\S+):.*?a0=(\d+)\s+a1=(\d+)(?:\s+a2=(\d+))?', line)
    if not m:
        continue
    fn, a0, a1, a2 = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
    key = (fn, a0, a1, int(a2) if a2 is not None else None)
    rows[key] += 1
print(f"{len(rows)} distinct (fn, arg0, arg1, arg2) tuples")
def reg_name(off):
    if 0x800 <= off < 0xC00: return f"SMR({(off-0x800)//8})"
    if 0xC00 <= off < 0x1000: return f"S2CR({(off-0xC00)//8})"
    if off >= 0x80000:
        n, r = divmod(off - 0x80000, 0x1000)
        return f"CB{n}+{r:#x}"
    return f"0x{off:x}"
for (fn, a0, a1, a2), n in rows.most_common(60):
    if a2 is not None and a1 < 0x100000:
        print(f"  {fn:32s} {reg_name(a1):16s} = 0x{a2:08x}  (x{n})")
    else:
        print(f"  {fn:32s} a0=0x{a0:x} a1={a1} x{n}")
PY
	;;
install-boot)
	cat > /tmp/99smmu-probe.sh <<'EOB'
#!/system/bin/sh
T=/sys/kernel/tracing
mkdir -p "$T" 2>/dev/null
mount -t tracefs tracefs "$T" 2>/dev/null
[ -e "$T/kprobe_events" ] || exit 0
i=0
while [ $i -lt 90 ]; do
	awk '{print $3}' /proc/kallsyms 2>/dev/null | grep -qx arm_smmu_write_smr && break
	sleep 1; i=$((i+1))
done
echo 0 > "$T/tracing_on"
echo > "$T/kprobe_events" 2>/dev/null
for f in arm_smmu_write_smr arm_smmu_write_s2cr arm_smmu_write_context_bank; do
	echo "p:smmu_$f $f a0=%x0 a1=%x1 a2=%x2" >> "$T/kprobe_events" 2>/dev/null
done
for e in attach_device_to_domain detach_device_from_domain io_page_fault map unmap; do
	echo 1 > "$T/events/iommu/$e/enable" 2>/dev/null
done
echo 32768 > "$T/buffer_size_kb"
for e in "$T"/events/kprobes/*/enable; do [ -e "$e" ] && echo 1 > "$e"; done
echo 1 > "$T/tracing_on"
echo "armed at $(cat /proc/uptime)" > /data/local/tmp/smmu-probe-ready.txt
EOB
	$ADB push /tmp/99smmu-probe.sh /data/local/tmp/99smmu-probe.sh >/dev/null
	$ADB shell "su -c 'cp /data/local/tmp/99smmu-probe.sh /data/adb/post-fs-data.d/99smmu-probe.sh && chmod 755 /data/adb/post-fs-data.d/99smmu-probe.sh'"
	echo "installed; reboot to capture"
	;;
remove-boot)
	$ADB shell "su -c 'rm -f /data/adb/post-fs-data.d/99smmu-probe.sh'"
	echo "removed"
	;;
clean)
	$ADB shell "su -c 'for e in /sys/kernel/tracing/events/kprobes/*/enable; do [ -e \"\$e\" ] && echo 0 > \"\$e\"; done; sleep 1; echo > /sys/kernel/tracing/kprobe_events'" 2>/dev/null || true
	echo "probes removed"
	;;
*)
	sed -n '2,40p' "$0"
	exit 2
	;;
esac
