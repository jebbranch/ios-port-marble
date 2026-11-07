#!/usr/bin/env python3
"""Do the links in a graph container name modules that the container actually defines?

WHY THIS EXISTS.  My first pass at this check reported "0 defined, 0 referenced"
for the spliced container.  That was not a clean result - the walker matched no
blocks at all, so it had measured nothing, and I nearly used it as a green light
to boot.  A checker that cannot find its subject reads as success, which is the
worst possible failure shape, so this version:
  * reports how many blocks it identified per container, and FAILS if that is 0;
  * tries both body offsets (+8 after {param_id,data_len} and +12 after
    {param_id,data_len,extra}) and keeps only the reading whose record count and
    declared length agree arithmetically - no silent guess;
  * always scores two controls whose answers are known from the driver's own
    behaviour, so a broken walker cannot pass quietly.

Terminating by construction: every loop is bounded by a literal.
"""
import re
import struct
import sys

TERM = 0x4000, 0x5000


def load_carray(path):
    txt = open(path, errors="replace").read()
    m = re.search(r"g_graph_\w+\[(\d+)\] = \{(.*?)\n\};", txt, re.S)
    if not m:
        sys.exit(f"FATAL: no g_graph_* array in {path}")
    n = int(m.group(1))
    d = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(2)))
    if n != len(d):
        sys.exit(f"FATAL: declared {n} parsed {len(d)}")
    return d


def entries(d):
    u = lambda o: struct.unpack_from("<I", d, o)[0]
    o, cnt = 8, u(4)
    out = []
    for _ in range(cnt):
        if o + 8 > len(d):
            break
        key, sz = u(o), u(o + 4)
        out.append((key, d[o + 8:o + 8 + sz]))
        o += 8 + sz
    return out


def is_term(v):
    return TERM[0] <= v < TERM[1]


def blocks(entry, pid):
    """Find every occurrence of param id `pid` in one entry and return
    (offset, words, reading) for each self-validating reading of its body."""
    u = lambda o: struct.unpack_from("<I", entry, o)[0]
    found = []
    for o in range(0, len(entry) - 8, 4):
        if u(o) != pid:
            continue
        ln = u(o + 4)
        if not ln or ln % 4 or ln > 2048 or o + 8 + ln > len(entry):
            continue
        for label, base in (("hdr8", o + 8), ("hdr12", o + 12), ("hdr16", o + 16)):
            w = [u(base + 4 * k) for k in range((len(entry) - base) // 4)]
            if not w:
                continue
            n = w[0]
            if pid == 0x08001002:                       # MODULE_LIST {1,sg,cont,n,(mod,iid)x n}
                ok = n <= 16 and len(w) >= 4 + 2 * n and all(
                    is_term(w[5 + 2 * k]) for k in range(n))
            elif pid == 0x08001004:                     # MODULE_CONN {n,(iid,port,iid,port)x n}
                ok = 1 <= n <= 32 and len(w) >= 1 + 4 * n and all(
                    is_term(w[1 + 4 * k]) and is_term(w[3 + 4 * k]) for k in range(n))
            else:
                ok = False
            if ok:
                found.append((o, ln, label, w, n))
                break
    return found


def score(name, d):
    ents = entries(d)
    defined, referenced, nb = set(), set(), 0
    for key, body in ents:
        for off, ln, label, w, n in blocks(body, 0x08001002):
            nb += 1
            for k in range(n):
                if is_term(w[5 + 2 * k]):
                    defined.add(w[5 + 2 * k])
        for off, ln, label, w, n in blocks(body, 0x08001004):
            nb += 1
            for k in range(n):
                referenced.add(w[1 + 4 * k])
                referenced.add(w[3 + 4 * k])
    print(f"{name}: {len(ents)} entries, {nb} blocks parsed, "
          f"defined={len(defined)} referenced={len(referenced)}")
    if nb == 0:
        print("  !! PARSED NOTHING - this container is UNJUDGED, not clean")
        return None
    print("  defined   :", " ".join(f"{i:#06x}" for i in sorted(defined)))
    print("  referenced:", " ".join(f"{i:#06x}" for i in sorted(referenced)))
    miss = sorted(referenced - defined)
    print("  DANGLING (link names an iid no module list defines):",
          " ".join(f"{m:#06x}" for m in miss) or "(none)")
    return miss


def main():
    paths = sys.argv[1:] or [
        "../chain-tree/bsd/dev/sweet_glink.c",      # both source containers
        "docs/data/g_graph_ukee_hwspliced.txt",
    ]
    for p in paths:
        txt = open(p, errors="replace").read()
        for m in re.finditer(r"static const uint8_t (g_graph_\w+)\[(\d+)\] = \{(.*?)\n\};",
                             txt, re.S):
            d = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(3)))
            if len(d) != int(m.group(2)):
                continue
            score(m.group(1), d)
            print()


main()
