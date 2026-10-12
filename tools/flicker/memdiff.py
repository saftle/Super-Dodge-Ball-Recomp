#!/usr/bin/env python3
"""memdiff.py — diff two --mem-out dumps section by section."""
import sys
from collections import Counter


SECTIONS = {"ram", "ciram", "oam", "palette"}


def load(p):
    t = open(p).read().split()
    out = {}
    sec = None
    i = 0
    while i < len(t):
        tok = t[i]
        if tok == "cycles":
            out["cycles"] = t[i + 1]
            i += 2
            continue
        if tok in SECTIONS:
            sec = tok
            i += 1
            continue
        if tok.endswith(":") and sec:
            base = int(tok[:-1], 16)
            i += 1
            d = out.setdefault(sec, {})
            while i < len(t) and t[i] not in SECTIONS and not t[i].endswith(":") \
                    and t[i] not in ("cycles", "frame"):
                d[base] = int(t[i], 16)
                base += 1
                i += 1
            continue
        i += 1
    return out


def main(a_path, b_path):
    a, b = load(a_path), load(b_path)
    for k in ("cycles",):
        if k in a or k in b:
            print(f"{k}: {a.get(k)} vs {b.get(k)}"
                  + ("   SAME" if a.get(k) == b.get(k) else "   DIFFER"))
    for sec in sorted(set(a) | set(b)):
        if sec in ("cycles",):
            continue
        da, db = a.get(sec, {}), b.get(sec, {})
        diff = sorted(k for k in set(da) | set(db) if da.get(k) != db.get(k))
        print(f"{sec}: {len(diff)} differing bytes of {len(da)}")
        if diff:
            pages = Counter(k >> 8 for k in diff)
            print("   pages:", {hex(p): n for p, n in sorted(pages.items())})
            for k in diff[:24]:
                print(f"   ${k:04X}: {da.get(k, 0):02X} -> {db.get(k, 0):02X}")
            if len(diff) > 24:
                print(f"   ... {len(diff) - 24} more")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])