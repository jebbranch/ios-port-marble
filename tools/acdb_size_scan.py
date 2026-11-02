#!/usr/bin/env python3
"""Look for the SET_CFG payload sizes inside the vendor ACDB.

Why: the only thing we know about the missing 18 KB APM SET_CFG body is its
size (2712 for the aw882xx acf packet, 18128 for the APM packet).  If AGM
hands the DSP a *slice of the ACDB*, the blob length must appear as a size
field in that file's section tables.  2712/2672 are the calibration points:
whatever structural pattern wraps those numbers tells us how to read the
neighbourhood, and the same pattern carrying 18128/18088 names the slice.
"""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "dumps/audio/Forte_acdb_cal.acdb"
blob = open(path, "rb").read()
print(f"file={path} size={len(blob)}")

WANT = [18128, 18088, 18112, 20480, 2712, 2672, 2696, 18144, 18168, 20440, 20448]
words = struct.unpack("<%dI" % (len(blob) // 4), blob[: len(blob) // 4 * 4])

hits = {}
for i, v in enumerate(words):
    if v in WANT:
        hits.setdefault(v, []).append(i * 4)

for w in WANT:
    h = hits.get(w, [])
    print(f"u32=={w:<6} count={len(h):<5} first={[hex(x) for x in h[:12]]}")

print("\n-- header (first 64 bytes as u32) --")
print([f"{v:#x}" for v in words[:16]])

# print a window around every 2712/2672 hit: that is the acf packet, we know
# what it is, so its framing teaches us the table layout.
for probe in (2712, 2672):
    for off in hits.get(probe, [])[:8]:
        lo = max(0, off - 32)
        print(f"\n== {probe} @{off:#x} window")
        row = []
        for j in range(lo, min(len(words), off + 40), 4):
            mark = " <<" if j == off else ""
            row.append(f"{j:#x}:{words[j // 4]:#010x}{mark}")
        print("  " + " ".join(row))
