#!/usr/bin/env python3
"""Walk a multi-record apm_module_param_data container out of the source, byte-exactly.

Written after I mis-described g_graph_open_payload in the docs ("ACDB chain, 8-byte
header, no iid") and then had to re-derive it.  The walker either closes to the declared
array length or prints where it breaks, so the next person does not read it by eye.

  tools/walk_param_records.py                       # the 344 B GRAPH_OPEN body
  tools/walk_param_records.py g_graph_vendor_payload
"""
import pathlib
import re
import sys

import os
import pathlib
import re
import sys

DEFAULT = pathlib.Path(__file__).resolve().parents[2] / "chain-tree" / "bsd" / "dev" / "sweet_glink.c"
NAME = sys.argv[1] if len(sys.argv) > 1 else "g_graph_open_payload"
PATH = pathlib.Path(os.environ.get("SRC", str(DEFAULT)))
src = PATH.read_text()
m = re.search(rf"static const uint8_t {NAME}\[(\d+)\] = \{{(.*?)\n\}};", src, re.S)
if not m:
    sys.exit(f"no uint8_t array named {NAME} in {PATH}")
declared = int(m.group(1))
by = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2)))
assert len(by) == declared, f"parsed {len(by)} bytes, declared {declared}"
u32 = lambda o: int.from_bytes(by[o:o + 4], "little")   # noqa: E731

names = {"0x08001000": "CONTAINER_CONFIG", "0x08001001": "SUB_GRAPH_CONFIG",
         "0x08001002": "MODULES_LIST", "0x08001003": "MODULE_PROP",
         "0x08001004": "MODULE_CONN", "0x08001015": "PORT_INFO",
         "0x08001010": "SCENARIO_ID", "0x08001174": "HEAP_ID"}
print(f"{NAME}: {len(by)} bytes, source {PATH}")
off = 0
while off < len(by):
    iid, param, size, err = u32(off), u32(off + 4), u32(off + 8), u32(off + 12)
    if iid != 1 or (param >> 16) not in (0x0800, 0x0400):
        print(f"  walk breaks at {off:#x}: {{{iid:#x}, {param:#x}, {size:#x}, {err:#x}}}")
        break
    pad = (size + 7) & ~7
    print(f"  @{off:#05x} iid={iid:#x} param={param:#010x} "
          f"{names.get(hex(param), '?'):16} size={size} err={err:#x} "
          f"body={off+16:#x}..{off+16+size:#x} next={off+16+pad:#x}")
    for w in range(off + 16, off + 16 + size, 4):
        v = u32(w)
        hint = names.get(hex(v), "")
        print(f"      {w:#05x}: {v:#010x} {hint}")
    off += 16 + pad
print(f"  closed at {off:#x} == {len(by)}: {off == len(by)}")
