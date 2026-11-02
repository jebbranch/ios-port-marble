#!/usr/bin/env python3
"""Decode a trace produced by tools/apm_blob_capture.sh.

Two jobs:

1. Find the APM-level SET_CFG packets the vendor sent (gprsend kprobe events;
   header layout documented in tools/gpr_capture.sh: p5 = opcode, p8 =
   mem_map_handle, p9 = payload_size) and report the size we are hunting.
2. Find the gsl uprobe hits (ugwr/ugrw/udpw/umap/umeta = gsl_write,
   gsl_graph_write, gsl_dp_write, gsl_shmem_map_allocation,
   gsl_shmem_get_metadata) and turn every plausible (pointer, length) pair into
   a ready-to-run dump command, because the *heap* copy is readable even though
   the /dmabuf: shm mapping is not (VM_PFNMAP -> EIO on /proc/pid/mem and on
   PTRACE_PEEKDATA).

Usage: parse_apm_blob.py <trace-file> [--want 18128] [--tol 400]
"""
import re
import sys

SET_CFG = 16781318  # 0x01001006
SHM_MAP = 16781324  # 0x0100100C
UNMAP = 16781325    # 0x0100100D
OPS = {SET_CFG: "APM_CMD_SET_CFG", SHM_MAP: "APM_CMD_SHM_MAP",
       UNMAP: "APM_CMD_SHM_UNMAP"}
UPR = ("ugwr", "ugrw", "udpw", "umap", "umeta")

args = [a for a in sys.argv[1:]]
want = 18128
tol = 400
if "--want" in args:
    i = args.index("--want")
    want = int(args[i + 1])
    del args[i:i + 2]
if "--tol" in args:
    i = args.index("--tol")
    tol = int(args[i + 1])
    del args[i:i + 2]
if not args:
    print(__doc__)
    sys.exit(2)

path = args[0]
fields = re.compile(r"\b(p\d+|a\d|common_pid|pid)=(\d+)")

setcfg, shmmap, uprobe_hits = [], [], []
for line in open(path, "r", errors="replace"):
    if "gprsend" in line:
        d = {k: int(v) for k, v in fields.findall(line)}
        op = d.get("p5")
        if op in OPS:
            setcfg.append((d.get("common_pid", d.get("pid", 0)), OPS[op],
                           d.get("p3"), d.get("p4"), op, d.get("p8"),
                           d.get("p9")))
    for name in UPR:
        if name + ":" in line or name + " " in line:
            d = {k: int(v) for k, v in fields.findall(line)}
            uprobe_hits.append((name, d))
            break

print(f"== APM commands seen: {len(setcfg)}")
for pid, name, dst, tok, op, handle, size in setcfg[:40]:
    mark = "  <== TARGET" if (size and abs(size - want) <= tol) else ""
    print(f"  pid={pid:<7} {name:<16} dst={dst:<6} tok={tok:<5} "
          f"op={op:#010x} handle={handle:#010x} size={size}{mark}")

print(f"\n== uprobe hits: {len(uprobe_hits)}")
for name, d in uprobe_hits[:40]:
    print(f"  {name}: " + " ".join(f"a{i}={d.get('a' + str(i), 0):#x}"
                                   for i in range(6)))

print(f"\n== candidate dumps (size within {tol} of {want})")
found = 0
for name, d in uprobe_hits:
    pid = d.get("common_pid", d.get("pid", 0))
    for i in range(6):
        v = d.get("a" + str(i), 0)
        if abs(v - want) <= tol and v < 0x100000:
            ptrs = [d.get("a" + str(j), 0) for j in range(6)
                    if 0x1000 < d.get("a" + str(j), 0) < 0x800000000000]
            for p in ptrs:
                found += 1
                print(f"  {name} len={v} -> /data/local/tmp/apm_shm_read "
                      f"{pid} 0x{p:x} {v} /data/local/tmp/blob-{name}-{p:x}.bin")
if not found:
    print("  none: no uprobe arg pair looked like (ptr, ~18k). Either no gsl_"
          "write happened in the window (no real playback) or the blob is "
          "assembled field-by-field straight into the shm mapping.")
print(f"\nIf the SET_CFG itself is missing from the trace, the vendor never "
      f"opened the device in this window: trigger a *real* playback "
      f"(video/music/ringtone preview), not tinyplay (ASoC delegates to the "
      f"userspace service and shuts the substream down immediately).")
