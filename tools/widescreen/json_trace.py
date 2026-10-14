#!/usr/bin/env python3
"""json_trace.py - read the SDB_TRACE_JSON stream.

    SDB_TRACE_JSON=600-1200 ./build-cycle/nes_game ROM --frames 1200 \\
        --input tas/vectors/sdb_sweep.input --ram-init zeros --widescreen 16:9 \\
        --overscan --present-out tmp/jt/p_ --present-every 1 \\
        > /dev/null 2> tmp/jt/trace.log
    python3 tools/widescreen/json_trace.py summary tmp/jt/trace.log
    python3 tools/widescreen/json_trace.py flicker tmp/jt/trace.log
    python3 tools/widescreen/json_trace.py margin  tmp/jt/trace.log --objects 2,8

WHY THIS AND NOT margin_track.py
--------------------------------
`margin_track.py` reads the human `sdbw` trace, and two of the seven bugs in
research.md 26 were that interface lying rather than the feature misbehaving: a
missing newline made its regex match nothing, and a `part` check that only
fired for wholly off-screen objects let the seventh bug through. Both were
parsing and ordering faults.

`SDB_TRACE_JSON` emits one JSON object per line with an explicit per-part
outcome, so this reader uses `json.loads` and cannot care where a closer was
printed. It is NOT a replacement for margin_track's temporal checks -- `gone`,
`shift`, `ghost` across frames are still margin_track's job -- but it does not
need a second regex to do it.

THE THREE RECORD KINDS
----------------------
  {"k":"f",...}  at frame begin: the flicker mod's counters and every live OAM
                 entry for THAT frame.
  {"k":"m",...}  at composite: what this presented frame painted, per object.
  {"k":"r",...}  once at exit: run totals.

f and m describe different instants and do not share a frame -- see the
comments on sdb_json_frame/sdb_json_margin. Join them on "f" knowing that.

`flicker` IS PLAN ITEM 14
-------------------------
The open Bean Ball bug asks one question: does the game emit fewer sprites
there, or does the mod drop them? `pub` is the size of the list the mod
presented; `oamn` is how many live sprites are in OAM in the same frame.

    oamn >> pub, pub collapsing as the player runs   ->  ours. The game emitted
                                                       them; the mod lost them.
                                                       Go to the list bookkeeping
                                                       (sdb_restart/sdb_publish/
                                                       sdb_present).
    oamn tracks pub down                              ->  the game's. It is not
                                                       emitting them in this mode;
                                                       the mod is faithful and the
                                                       honest outcome is a recorded
                                                       finding, not a fix.

`oamn` counts every live slot, including the ones the mod does not capture
(persistent sprites, slot 0), so a small constant offset above `pub` is normal
and only the TREND is informative.
"""
import argparse
import collections
import json
import sys

STATUSES = ("drawn", "machine", "ghost", "clip", "oob")


def read(path):
    """(frames, margins, run) from a trace log. Lines that are not JSON objects
    are ignored, so this can be pointed at a log that also carries the human
    trace lines."""
    frames, margins, run = {}, {}, None
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line.startswith("{"):
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as e:
                raise SystemExit(f"{path}:{lineno}: not valid JSON ({e}). "
                                 "The producer is emitting malformed lines; do "
                                 "not paper over it here.")
            kind = rec.get("k")
            if kind == "f":
                frames[rec["f"]] = rec
            elif kind == "m":
                margins[rec["f"]] = rec
            elif kind == "r":
                run = rec
            else:
                raise SystemExit(f"{path}:{lineno}: unknown record kind {kind!r}")
    if not frames and not margins:
        raise SystemExit(f"{path}: no SDB_TRACE_JSON records. Is "
                         "SDB_TRACE_JSON=<from>-<to> set for the run?")
    return frames, margins, run


def cmd_summary(frames, margins, run, args):
    if run:
        print("run totals:")
        for k in ("frames", "composed", "pillarboxed", "diff",
                  "mcap", "moff", "mdrawn", "mpeak", "mlost"):
            if k in run:
                print(f"  {k:12s} {run[k]}")
    print(f"\nrecords: {len(frames)} frame, {len(margins)} margin")
    both = sorted(set(frames) & set(margins))
    print(f"frames carrying both kinds: {len(both)}")
    if frames:
        fo = sorted(frames)
        print(f"frame range: {fo[0]}..{fo[-1]}")
    st = collections.Counter()
    stale = 0
    for r in margins.values():
        for o in r["obj"]:
            stale += o.get("stale", 0)
            for p in o["parts"]:
                st[p[3]] += 1
    print("\nmargin part outcomes:")
    for s in STATUSES:
        print(f"  {s:8s} {st.get(s, 0)}")
    print(f"  {'stale':8s} {stale}   (parts in recycled slots; the draw site "
          f"skips the object before placing them)")
    return 0


def cmd_flicker(frames, margins, run, args):
    if not frames:
        print("no frame records", file=sys.stderr)
        return 1
    keys = sorted(frames)
    print("frame  oamn  pub   peak  ovf  lost  delta(oamn-pub)")
    prev = None
    for k in keys:
        r = frames[k]
        d = r["oamn"] - r["pub"]
        flag = ""
        if prev is not None:
            dd = d - prev
            if dd <= -3:
                flag = f"  <- delta fell {dd}"
            elif dd >= 3:
                flag = f"  <- delta rose {dd}"
        print(f"{k:5d}  {r['oamn']:4d}  {r['pub']:3d}  {r['peak']:4d}  "
              f"{r['ovf']:3d}  {r['lost']:4d}  {d:+4d}{flag}")
        prev = d
    p = [frames[k]["pub"] for k in keys]
    o = [frames[k]["oamn"] for k in keys]
    print(f"\noamn: min {min(o)} max {max(o)} first {o[0]} last {o[-1]}")
    print(f"pub : min {min(p)} max {max(p)} first {p[0]} last {p[-1]}")
    # Only speak up on a real collapse. `pub` jitters by a few sprites frame to
    # frame as sprites enter and leave the list, so any decline at all is not
    # the Bean Ball signature and saying so on every run would train the reader
    # to ignore the line. Plan item 14 measured 53 -> 10; 30% is well clear of
    # the jitter and well short of that.
    drop = 1.0 - (p[-1] / p[0]) if p and p[0] else 0.0
    if drop >= 0.30:
        print(f"\n  pub fell {drop*100:.0f}% ({p[0]} -> {p[-1]}). That is the "
              f"collapse shape, not jitter. Read the delta column:")
        print("    oamn holds up while pub falls  ->  ours; the mod lost them.")
        print("    oamn falls with it             ->  the game is not emitting them.")
    elif drop > 0:
        print(f"\n  pub fell {drop*100:.0f}% ({p[0]} -> {p[-1]}) -- within "
              f"frame-to-frame jitter for a normal run, not the collapse.")
    return 0


def cmd_margin(frames, margins, run, args):
    if not margins:
        print("no margin records (the compositor only runs when the picture is "
              "presented: --present-out or a window)", file=sys.stderr)
        return 1
    keys = sorted(margins)
    want = set(args.objects.split(",")) if args.objects else None
    print(f"margin records {keys[0]}..{keys[-1]} ({len(keys)} frames)\n")
    print("obj  drawn  frames  parts(min/max)  live-frames  ghost  clip  oob  machine")
    rows = []
    for oi in sorted({o["i"] for r in margins.values() for o in r["obj"]}):
        if want is not None and str(oi) not in want:
            continue
        c = collections.Counter()
        nparts, live_frames, drawn_frames = [], 0, 0
        for k in keys:
            ob = next((o for o in margins[k]["obj"] if o["i"] == oi), None)
            if ob is None:
                continue
            if not ob.get("stale") and ob["live"]:
                live_frames += 1
            n = len(ob["parts"])
            if n:
                nparts.append(n)
            got = [p for p in ob["parts"] if p[3] == "drawn"]
            if got:
                drawn_frames += 1
            for p in ob["parts"]:
                c[p[3]] += 1
        rows.append((oi, c, len(keys), nparts, live_frames, drawn_frames))
        nmin = min(nparts) if nparts else 0
        nmax = max(nparts) if nparts else 0
        print(f"{oi:3d}  {c['drawn']:5d}  {drawn_frames:6d}  {nmin:5d}/{nmax:<5d}  "
              f"{live_frames:10d}  {c['ghost']:5d}  {c['clip']:4d}  {c['oob']:3d}  "
              f"{c['machine']:7d}")

    # The seventh bug's shape: a part standing in the revealed band that OAM
    # never held. Research.md 26 measured the legitimate rate at ~0.2/frame, so
    # report the rate rather than treating any ghost as a failure.
    g = [(k, o["i"], p) for k in keys for o in margins[k]["obj"]
         for p in o["parts"] if p[3] == "ghost"]
    print(f"\nghost parts: {len(g)} over {len(keys)} frames "
          f"({len(g)/max(len(keys),1):.2f}/frame; ~0.2 is the measured norm)")
    for k, oi, p in g[:args.show]:
        print(f"  frame {k} obj {oi}: sx={p[0]} y={p[1]} tile=${p[2]:02X}")
    if len(g) > args.show:
        print(f"  ... {len(g) - args.show} more")
    return 1 if (g and args.ghost_fails) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("summary", "flicker", "margin"))
    ap.add_argument("trace")
    ap.add_argument("--objects", metavar="LIST",
                    help="margin: restrict to these object indices, e.g. 2,8")
    ap.add_argument("--show", type=int, default=20,
                    help="margin: how many ghost parts to list (default 20)")
    ap.add_argument("--ghost-fails", action="store_true",
                    help="margin: exit 1 if any ghost part is reported")
    args = ap.parse_args()
    frames, margins, run = read(args.trace)
    return {"summary": cmd_summary, "flicker": cmd_flicker,
            "margin": cmd_margin}[args.command](frames, margins, run, args)


if __name__ == "__main__":
    sys.exit(main())