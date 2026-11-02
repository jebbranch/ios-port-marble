#!/system/bin/sh
# TEMPORARY boot-time probe armer for the 18 KB APM SET_CFG hunt.
# Installed at /data/adb/post-fs-data.d/99-apmharvest.sh by
# scripts/apm_bootarm.sh and MUST be removed after the capture
# (rm -f /data/adb/post-fs-data.d/99-apmharvest.sh).
#
# Why a boot hook: the ~18 KB APM-level SET_CFG is sent once per boot at the
# first speaker-device open, so arming after the fact misses it (verified
# 2026-11-02: alarm ring #1 produced size=18504, alarm ring #2 in a later boot
# produced no SHM_MAP at all => the config had already been pushed).
#
# Deliberately avoids the gsl_write hot path: the phone self-rebooted once
# inside a window where it fired 12295 times in 100 s (cause not proven).
set -u
T=/sys/kernel/tracing
D=/data/local/tmp
[ -d "$T" ] || exit 0
[ -f "$D/apm_blob_dev.sh" ] || exit 0

PROBES="umap:0x33580 umeta:0x33588 ucald:0x33400 usetcal:0x33390 \
usetcfg:0x33398 ugrcal:0x334c8 ugrcfg:0x33498 uioctl:0x333c8 ushmal:0x33558"

# kprobe on gpr_send_pkt (96 u32 words = the whole GPR header + apm_cmd_hdr)
sh "$D/gpr_capture_dev.sh" setup 384 >/dev/null 2>&1
# gsl uprobes (cold set only)
PROBES="$PROBES" sh "$D/apm_blob_dev.sh" arm >/dev/null 2>&1
echo 8192 > "$T/buffer_size_kb" 2>/dev/null
echo 1 > "$T/events/kprobes/gprsend/enable" 2>/dev/null
echo > "$T/trace" 2>/dev/null
echo 1 > "$T/tracing_on" 2>/dev/null
echo "APMHARVEST: armed at post-fs-data" > "$D/harvest-armed.flag"

# Timing is everything here: the 18504-byte APM config is written into AGM's
# 32 KB /dmabuf: cmd region at t~63 s (before any gsl_write: the first gsl_write
# of the boot is t~135 s), and PCM traffic then overwrites it.  So we keep numbered
# snapshots with the boot time in the name, covering the whole 0..~180 s window.
# ~120 x 32 KB = 3.8 MB, and the loop stops by itself.
(
	i=0
	while [ $i -lt 160 ]; do
		up=$(cut -d. -f1 /proc/uptime)
		for p in $(pidof vendor.qti.hardware.AGMIPC@1.0-service); do
			"$D/apm_dmabuf_pull" "$p" "$D/w-$up" >/dev/null 2>&1
		done
		i=$((i+1))
		sleep 1
	done
	echo "APMHARVEST: watch done i=$i" > "$D/harvest-watch.flag"
) &
