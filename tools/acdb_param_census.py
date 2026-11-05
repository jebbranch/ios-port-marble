#!/usr/bin/env python3
"""Census the ACDB's per-module cal table so the probe's records can be checked
against what the vendor actually registers, with no device and no build.

Why: the wire record of a SET_CFG is {module_instance_id, param_id, param_size,
bytes}.  Two different places in an ACDB look like that shape and it matters
which one a probe copies:

  * chain-level TLV records, whose first word is a small pool index (<= 0x20) and
    whose param ids belong to the subgraph header family (0x08001000..0x08001004);
  * the per-module table inside the 0x08001003 record, whose entries really are
    {module_instance_id, param_id, param_size, bytes}.

Boots H/J sent the first kind verbatim (iid=1 + 0x08001004), K1/L1/L2 send the
first kind's param id bolted onto real module ids, and C sent a different param
id pair that we labelled "invented" without ever checking it here.  This prints
what the file actually registers.

usage:
  python3 tools/acdb_param_census.py --file MTP_ukee
  python3 tools/acdb_param_census.py --file MTP_ukee --iid 0x428f,0x40bd,0x40be
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from acdb_module_tuples import SIG_HEAD, chain_records, module_tuples  # noqa: E402

CAL_TABLE = 0x08001003


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--acdb-dir", default=os.path.join(os.path.dirname(__file__),
                    "..", "..", "phone-acdb"))
    ap.add_argument("--file", default="", help="substring filter on the file name")
    ap.add_argument("--iid", default="", help="comma list of module ids to detail")
    args = ap.parse_args()

    wanted_iids = {int(x, 0) for x in args.iid.split(",") if x.strip()} if args.iid else set()
    paths = sorted(p for p in glob_paths(args.acdb_dir)
                   if not args.file or args.file in os.path.basename(p))
    if not paths:
        sys.exit(f"no acdb under {args.acdb_dir} matching {args.file!r}")

    for path in paths:
        buf = open(path, "rb").read()
        name = os.path.basename(path)
        triples = {}
        chain = {}
        tables = 0
        off = buf.find(SIG_HEAD)
        while off >= 0:
            for iid, pid, size, body in chain_records(buf, off):
                chain.setdefault((pid, size), [0, set()])
                chain[(pid, size)][0] += 1
                chain[(pid, size)][1].add(iid)
                if pid != CAL_TABLE:
                    continue
                tables += 1
                for miid, mpid, msize, data in module_tuples(size, body, buf):
                    key = (miid, mpid, msize)
                    triples.setdefault(key, []).append(data)
            off = buf.find(SIG_HEAD, off + 1)

        params = {}
        for (miid, mpid, msize), v in triples.items():
            params.setdefault((mpid, msize), [0, set()])
            params[(mpid, msize)][0] += len(v)
            params[(mpid, msize)][1].add(miid)

        print(f"### {name}: {len(buf)} B, {tables} per-module tables, "
              f"{len(triples)} distinct (iid,param,size) triples")
        print("  param_id  size  #tuples  #modules")
        for (mpid, msize), (n, mods) in sorted(params.items(), key=lambda kv: -kv[1][0])[:14]:
            print(f"  {mpid:#010x} {msize:5}  {n:7}  {len(mods):8}")

        if wanted_iids:
            print("  per-module detail for the ids the probes use:")
            for miid in sorted(wanted_iids):
                rows = sorted(((mpid, msize) for (i2, mpid, msize) in triples if i2 == miid))
                if not rows:
                    print(f"    iid {miid:#06x}: NOT registered in any per-module table")
                    continue
                for mpid, msize in rows:
                    sample = triples[(miid, mpid, msize)][0]
                    hexd = " ".join(f"{w:08x}" for w in sample)
                    print(f"    iid {miid:#06x} param {mpid:#010x} size {msize:3} bytes {hexd}")
        # Does a header-family param ever appear with a real module id?
        hybrid = [(i, p, s) for (i, p, s) in triples if p in
                  (0x08001000, 0x08001001, 0x08001002, 0x08001003, 0x08001004)]
        print(f"  header-family params appearing in the per-module table: {len(hybrid)}"
              + (f" -> {hybrid[:4]}" if hybrid else ""))

        # Chain-level records: this is where the probes' 0x08001004 lives, and its
        # first word is a pool index (<= 0x20), not a module instance id.
        print("  chain-level (param,size) -> count, the iid values seen there:")
        for (pid, size), (n, iids) in sorted(chain.items(), key=lambda kv: -kv[1][0])[:12]:
            print(f"    {pid:#010x} size {size:4}  n={n:6}  iid in "
                  f"{sorted(hex(i) for i in iids)[:8]}")
        for probe_pid in (0x08001004, 0x08001017, 0x0800101b, 0x08001015):
            at_chain = {k: v for k, v in chain.items() if k[0] == probe_pid}
            in_table = sorted({(p, s) for (i, p, s) in triples if p == probe_pid})
            print(f"    probe param {probe_pid:#010x}: chain-level sizes "
                  f"{sorted(k[1] for k in at_chain) or 'NONE'}"
                  f" | per-module-table sizes {in_table or 'NONE'}")


def glob_paths(d):
    for root, _, files in os.walk(d):
        for f in files:
            if f.endswith(".acdb"):
                yield os.path.join(root, f)


if __name__ == "__main__":
    main()
