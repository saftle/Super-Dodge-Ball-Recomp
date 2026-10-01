#!/usr/bin/env python3
"""diff_tas.py — first-divergence report for a TAS run dir.

Usage:
  python3 tools/tas/diff_tas.py tas/runs/r400 [--image-threshold 90]

Layers (stops at first red):
  1. liveness: recomp rc, frames_run == requested, [HANG] scan, dispatch misses
  2. cosim chain: first f where any recomp sub-hash changes run-over-run
     (A-vs-A determinism) — cross-side sub-hash compare needs mesen-side
     hashing (future); today reports recomp chain + WRAM/PPU deltas
  3. WRAM/PPUMEM delta streams: first frame with changes, top addrs
  4. images: mesen_*.ppm vs recomp shots/frame_*.png (stride-aware join),
     palette-index structural match via compare_frames.image_to_palette_indices

Exit 0 iff liveness green; image/chain findings are reported, not gated (yet).
"""
import glob
import json
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from compare_frames import image_to_palette_indices  # noqa: E402
import numpy as np
from PIL import Image


def load_jsonl(path):
    rows = []
    if os.path.exists(path):
        with open(path) as f:
            for lineno, line in enumerate(f, 1):
                line = line.strip()
                if line.startswith("{"):
                    try:
                        rows.append(json.loads(line))
                    except json.JSONDecodeError:
                        print(f"NOTE {path}:{lineno}: skipping unparseable line")
                # else: header/comment lines (e.g. apu.jsonl "cycle,addr,val")
    return rows


def replay_deltas(path, size):
    """Reconstruct per-frame full images from a WRAM/PPUMEM delta trace.

    Returns {frame: bytearray}. Frame rows carry full baseline; later rows
    only changed bytes (applied cumulatively)."""
    if not os.path.exists(path):
        return {}
    cur = bytearray(size)
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            a = int(r["adr"], 16)
            if 0 <= a < size:
                cur[a] = int(r["val"], 16)
            out[int(r["f"])] = bytearray(cur)
    return out


def diff_state(rundir):
    """Renderer-independent cross-side gate: guest memory byte-compare.

    Recomp WRAM/PPUMEM delta traces are replayed to full per-vframe images
    and compared against Mesen's per-stride bin dumps. Joins on video-frame
    index (recomp g_cosim_vframe / mesen f), never wall-clock. Stack page
    ($0100-$01FF) masked: native JSR pushes nothing cross-impl (expected-diff).
    Returns True iff all compared frames match (or nothing comparable)."""
    out_r = os.path.join(rundir, "recomp")
    out_m = os.path.join(rundir, "mesen")
    wram = replay_deltas(os.path.join(out_r, "wram.jsonl"), 0x800)
    ppu = replay_deltas(os.path.join(out_r, "ppu.jsonl"), 0xA00)
    if not wram and not ppu:
        print("state: no recomp traces — skipped")
        return True
    bins = sorted(glob.glob(os.path.join(out_m, "mesen_*.ram.bin")))
    if not bins:
        print("state: no mesen dumps — skipped")
        return True
    ok, ncmp = True, 0
    for bp in bins:
        f = int(os.path.basename(bp).split("_")[1].split(".")[0])
        if f in wram:
            a = wram[f]
            b = open(bp, "rb").read()
            # stack page masked (expected cross-impl diff)
            diff = [ad for ad in range(0x800)
                    if not (0x100 <= ad < 0x200) and a[ad] != b[ad]]
            ncmp += 1
            if diff:
                ok = False
                show = " ".join(f"${ad:04X}(r={a[ad]:02X},m={b[ad]:02X})" for ad in diff[:8])
                print(f"  state RAM f={f}: {len(diff)} bytes differ (stack masked): {show}")
        if f in ppu:
            p = ppu[f]
            oam = open(bp.replace(".ram.bin", ".oam.bin"), "rb").read()
            pal = open(bp.replace(".ram.bin", ".pal.bin"), "rb").read()
            ncmp += 1
            do = sum(1 for i in range(0x100) if p[i] != oam[i])
            dp = sum(1 for i in range(0x20) if p[0x100 + i] != pal[i])
            if do or dp:
                ok = False
                print(f"  state PPU f={f}: OAM {do}/256 differ, pal {dp}/32 differ")
    print(f"state: {ncmp} frame-surfaces compared — {'MATCH' if ok else 'DIVERGE'}")
    return ok


def main():
    rundir = sys.argv[1]
    thr = 90.0
    if "--image-threshold" in sys.argv:
        thr = float(sys.argv[sys.argv.index("--image-threshold") + 1])
    run = json.load(open(os.path.join(rundir, "run.json")))
    out_r = os.path.join(rundir, "recomp")
    out_m = os.path.join(rundir, "mesen")
    print(f"TAS frames={run['tas_frames']} mesen_rc={run['mesen_rc']} recomp_rc={run['recomp_rc']}")

    # 1. liveness
    ok = True
    smoke = {}
    sp = os.path.join(out_r, "smoke.json")
    if os.path.exists(sp):
        smoke = json.load(open(sp))
        print(f"smoke: frames_run={smoke.get('frames_run')} "
              f"misses={smoke.get('dispatch_miss_count')} unique={smoke.get('dispatch_miss_unique')} "
              f"miss_keys={smoke.get('dispatch_miss_keys')}")
        if smoke.get("frames_run") != run["tas_frames"]:
            print(f"RED liveness: frames_run {smoke.get('frames_run')} != {run['tas_frames']}")
            ok = False
        if smoke.get("dispatch_miss_count"):
            print("RED liveness: dispatch misses nonzero")
            ok = False
    else:
        print("RED liveness: no smoke.json (hang/crash — see recomp/recomp.log)")
        ok = False
    logp = os.path.join(out_r, "recomp.log")
    if os.path.exists(logp):
        log = open(logp).read()
        for sig in ("[HANG]", "[TUNNEL]", "BRK"):
            n = log.count(sig)
            if n:
                print(f"log: {sig} x{n}")
        if "[HANG]" in log:
            ok = False

    # 2/3. recomp trace streams
    for name in ("cosim.jsonl", "wram.jsonl", "ppu.jsonl", "apu.jsonl"):
        p = os.path.join(out_r, name)
        rows = load_jsonl(p)
        print(f"recomp/{name}: {len(rows)} rows" + (f" first_f={rows[0].get('f')}" if rows else ""))

    # mesen side
    mstate = load_jsonl(os.path.join(out_m, "mesen_state.jsonl"))
    print(f"mesen_state.jsonl: {len(mstate)} rows")
    if mstate and len(mstate) != run["tas_frames"]:
        print(f"NOTE mesen rows {len(mstate)} != {run['tas_frames']} (early emu.stop?)")

    # 4. renderer-independent state gate (primary cross-side signal)
    if not diff_state(rundir):
        ok = False

    # 5. images, secondary: cross-renderer RGB always differs by palette
    # emulation; same-index only, informational (see state gate above)
    ppms = sorted(glob.glob(os.path.join(out_m, "mesen_*.ppm")))
    print(f"image pairs: {len(ppms)} mesen dumps, threshold {thr}% (informational)")
    worst = None
    for ppm in ppms:
        base = os.path.basename(ppm)
        f = int(base.split("_")[1].split(".")[0])
        rp = os.path.join(out_r, "shots", f"frame_{f:04d}.png")
        if not os.path.exists(rp):
            print(f"  f={f}: missing recomp shot")
            continue
        ref = np.array(Image.open(ppm).convert("RGB"))
        rec = np.array(Image.open(rp).convert("RGB"))
        if ref.shape != rec.shape:
            print(f"  f={f}: shape {ref.shape} vs {rec.shape} — skipped")
            continue
        ri, _ = image_to_palette_indices(ref)
        ci, _ = image_to_palette_indices(rec)
        pct = 100.0 * np.sum(ri == ci) / ri.size
        flag = "" if pct >= thr else "  <-- BELOW THRESHOLD"
        if worst is None or pct < worst[1]:
            worst = (f, pct)
        if flag or f < 3 or f % 50 == 0:
            print(f"  f={f}: palette_idx {pct:5.1f}%{flag}")
    if worst:
        print(f"worst frame: f={worst[0]} {worst[1]:.1f}%")
    print("GREEN" if ok else "RED")


if __name__ == "__main__":
    main()
