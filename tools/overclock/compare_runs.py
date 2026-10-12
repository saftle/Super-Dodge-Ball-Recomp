#!/usr/bin/env python3
"""Compare two overclock oracle runs produced by tools/overclock/run_overclock.sh.

Answers the only question that matters for docs/research.md section 23: did
PpuExtraScanlinesBeforeNmi make the guest actually run faster, or not?

Usage:
    python3 tools/overclock/compare_runs.py tmp/overclock_run_stock/m.csv \
                                     tmp/overclock_run_oc300/m.csv

Both CSVs come from overclock_probe.lua:
    frame,cpu_cycles_delta,<11 comma-joined ints>
where those ints are RAM bytes: 0100 0106 0107 06B1 06EA 069E 067A 067B 0100
followed by an object-block fingerprint and an OAM fingerprint.

$069E is the game's per-NMI tick counter (research.md section 19.6), so a
positive (oc - stock) delta means the overclocked guest completed more of its
own work per frame. A constant offset would be a phase shift; a delta that
GROWS is the guest genuinely doing more.
"""
import csv
import sys

STATE = ["0100", "0106", "0107", "06B1", "06EA", "069E", "067A", "067B", "0100"]
IDX_069E = STATE.index("069E")
NTSC_CYCLES = 29781  # 341 * 262 / 3, one stock NTSC frame in CPU cycles


def load(path):
    out = {}
    with open(path, newline="") as fh:
        for row in csv.reader(fh):
            if len(row) < 13:
                continue
            out[int(row[0])] = {
                "cycles": int(row[1]),
                "state": [int(x) for x in row[2:13]],
            }
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    stock = load(sys.argv[1])
    oc = load(sys.argv[2])
    common = sorted(set(stock) & set(oc))
    if not common:
        sys.exit("no overlapping frames between the two runs")

    sc = sum(stock[f]["cycles"] for f in common[10:]) / max(1, len(common) - 10)
    oc_c = sum(oc[f]["cycles"] for f in common[10:]) / max(1, len(common) - 10)

    print(f"frames compared     : {len(common)}")
    print(f"cpu cycles/frame    : stock {sc:.0f}   overclocked {oc_c:.0f}"
          f"   (+{oc_c - sc:.0f})")
    if sc < NTSC_CYCLES * 0.9:
        print("  WARNING: stock run does not look like stock NTSC timing.")

    print()
    print("mean $069E lead (overclocked - stock), by window:")
    window = max(1, len(common) // 6)
    for i in range(0, len(common), window):
        chunk = common[i:i + window]
        if not chunk:
            continue
        lead = sum((oc[f]["state"][IDX_069E] - stock[f]["state"][IDX_069E]) & 0xFF
                   for f in chunk) / len(chunk)
        print(f"  frames {chunk[0]:>5}-{chunk[-1]:<5}  {lead:+7.1f}")

    total = sum((oc[f]["state"][IDX_069E] - stock[f]["state"][IDX_069E]) & 0xFF
                for f in common)
    print()
    print(f"cumulative extra $069E ticks : {total}")
    print(f"mean extra laps per frame    : {total / len(common):+.1f}")

    hist = {}
    for f in common:
        d = (oc[f]["state"][IDX_069E] - stock[f]["state"][IDX_069E]) & 0xFF
        hist[d] = hist.get(d, 0) + 1
    print()
    print("$069E delta distribution (top 6):")
    for d in sorted(hist, key=lambda k: -hist[k])[:6]:
        print(f"  {d:+4d}: {hist[d]:>5} frames")

    growing = (sum((oc[f]["state"][IDX_069E] - stock[f]["state"][IDX_069E]) & 0xFF
                   for f in common[-window:])
               > sum((oc[f]["state"][IDX_069E] - stock[f]["state"][IDX_069E]) & 0xFF
                     for f in common[:window]))
    print()
    print("verdict:", "lead GROWS over time -> guest does more work per frame "
                     "(real speedup)" if growing else
                     "lead does not grow -> phase shift or no effect")


if __name__ == "__main__":
    main()