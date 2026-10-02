#!/usr/bin/env python3
"""diff_tas.py — first-divergence report for a TAS run dir (cycle backend).

Usage:
  python3 tools/tas/diff_tas.py tas/runs/r400 [--image-threshold 90]

Layers (stops at first red):
  1. liveness: cyc rc 0, logged frames == requested
  2. state (primary cross-side signal): CYCFRAME snapshots (CPU RAM, CIRAM,
     palette indices, OAM) byte-compared against Mesen bins, joined on
     video-frame index. No stack masking: the cycle CPU pushes real return
     addresses, so the stack page must match too.
  3. images (informational): cyc shot_*.png vs mesen mesen_*.ppm —
     cross-renderer RGB always differs by palette emulation; same-index only.

Exit 0 iff liveness green; state/image findings are reported, not gated (yet).
"""
import glob
import json
import os
import struct
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from tools.compare_frames import image_to_palette_indices  # noqa: E402
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
    return rows


def parse_frame_log(path):
    """Parse CYCFRAME v2 log -> {frame: {ram, ciram, pal, oam}}.

    Header: "CYCFRAME" + u32 version(2); per frame: u32 frame, u32 cycles_lo,
    u32 cycles_hi, u32 lengths[6] (ram, ciram, cart, chr, picture, fds-audio),
    u8 pal[32], u8 oam[256], then blobs (ram 0x800, ciram, cart?, chr?,
    picture u16[], audio?). Picture/cart blobs are skipped (lengths known).
    """
    out = {}
    try:
        data = open(path, "rb").read()
    except OSError:
        return out
    if data[:8] != b"CYCFRAME":
        print(f"NOTE {path}: bad magic — skipped")
        return out
    (ver,) = struct.unpack_from("<I", data, 8)
    if ver != 2:
        print(f"NOTE {path}: version {ver} != 2 — skipped")
        return out
    pos = 12
    while pos + 36 + 32 + 256 <= len(data):
        (fr, _, _, lr, lc, lcart, lchr, lpict, laud) = struct.unpack_from("<9I", data, pos)
        pos += 36
        pal = data[pos:pos + 32]
        pos += 32
        oam = data[pos:pos + 256]
        pos += 256
        ram = data[pos:pos + 0x800]
        pos += 0x800
        ciram = data[pos:pos + lc]
        pos += lc
        pos += lcart + lchr + lpict + laud
        if len(ram) < 0x800 or len(ciram) < lc:
            break
        out[fr] = {"ram": ram, "ciram": ciram, "pal": pal, "oam": oam}
    return out


def diff_state(rundir):
    """Renderer-independent cross-side gate: guest memory byte-compare.

    Cycle snapshots (real CPU state, stack included) vs Mesen bins, joined
    on video-frame index. Returns True iff all compared frames match
    (or nothing comparable).
    """
    out_c = os.path.join(rundir, "cyc")
    out_m = os.path.join(rundir, "mesen")
    cyc = parse_frame_log(os.path.join(out_c, "frames.bin"))
    if not cyc:
        print("state: no cycle snapshots — skipped")
        return True
    bins = sorted(glob.glob(os.path.join(out_m, "mesen_*.ram.bin")))
    if not bins:
        print("state: no mesen dumps — skipped")
        return True
    ok, ncmp = True, 0
    for bp in bins:
        f = int(os.path.basename(bp).split("_")[1].split(".")[0])
        if f not in cyc:
            continue
        s = cyc[f]
        b = open(bp, "rb").read()
        ncmp += 1
        diff = [ad for ad in range(min(0x800, len(b))) if s["ram"][ad] != b[ad]]
        if diff:
            ok = False
            show = " ".join(f"${ad:04X}(c={s['ram'][ad]:02X},m={b[ad]:02X})" for ad in diff[:8])
            print(f"  state RAM f={f}: {len(diff)} bytes differ: {show}")
        oam = open(bp.replace(".ram.bin", ".oam.bin"), "rb").read()
        pal = open(bp.replace(".ram.bin", ".pal.bin"), "rb").read()
        cir = open(bp.replace(".ram.bin", ".ciram.bin"), "rb").read()
        do = sum(1 for i in range(min(0x100, len(oam))) if s["oam"][i] != oam[i])
        # Sprite entry-0 bytes ($3F10/$3F14/$3F18/$3F1C) are the same physical
        # byte as the backdrop ($3F00) on hardware; Mesen exposes the mirror,
        # the cycle palette RAM keeps stale power-on bytes there (hw_ppu.c
        # writes index (vbus & 0x0F) only). Masked; index 0 must still match.
        MIRRORS = {16, 20, 24, 28}
        dp = sum(1 for i in range(min(0x20, len(pal)))
                 if i not in MIRRORS and s["pal"][i] != pal[i])
        n = min(len(cir), len(s["ciram"]))
        dc = sum(1 for i in range(n) if s["ciram"][i] != cir[i])
        if do or dp or dc:
            ok = False
            print(f"  state PPU f={f}: OAM {do}/256 differ, pal {dp}/32 differ, ciram {dc}/{n} differ")
    print(f"state: {ncmp} frames compared — {'MATCH' if ok else 'DIVERGE'}")
    return ok


def main():
    rundir = sys.argv[1]
    thr = 90.0
    if "--image-threshold" in sys.argv:
        thr = float(sys.argv[sys.argv.index("--image-threshold") + 1])
    run = json.load(open(os.path.join(rundir, "run.json")))
    out_c = os.path.join(rundir, "cyc")
    out_m = os.path.join(rundir, "mesen")
    cyc = run.get("cyc") or {}
    print(f"TAS frames={run['tas_frames']} mesen_rc={run['mesen_rc']} cyc={cyc}")

    # 1. liveness
    ok = True
    if cyc.get("rc") != 0:
        print(f"RED liveness: cycle run rc={cyc.get('rc')}")
        ok = False
    if cyc.get("frames") != run["tas_frames"]:
        print(f"RED liveness: cycle frames {cyc.get('frames')} != {run['tas_frames']}")
        ok = False
    print(f"cycle native: {cyc.get('native_pct')}%")
    logp = os.path.join(out_c, "cyc.log")
    if os.path.exists(logp):
        log = open(logp).read()
        if "[HANG]" in log:
            print("RED liveness: [HANG] in cyc log")
            ok = False
    for name in ("miss.txt",):
        p = os.path.join(out_c, name)
        if os.path.exists(p):
            n = sum(1 for ln in open(p) if ln.strip() and not ln.startswith("#"))
            print(f"cyc/{name}: {n} uncompiled-seed lines")

    # mesen side
    mstate = load_jsonl(os.path.join(out_m, "mesen_state.jsonl"))
    print(f"mesen_state.jsonl: {len(mstate)} rows")
    if mstate and len(mstate) != run["tas_frames"]:
        print(f"NOTE mesen rows {len(mstate)} != {run['tas_frames']} (early emu.stop?)")

    # 2. state gate (primary cross-side signal)
    if not diff_state(rundir):
        ok = False

    # 3. images, secondary: cross-renderer RGB always differs by palette
    # emulation; same-index only, informational
    shots = sorted(glob.glob(os.path.join(out_c, "shots", "shot_*.png")))
    print(f"image pairs: {len(shots)} cyc shots, threshold {thr}% (informational)")
    worst = None
    for sp in shots:
        base = os.path.basename(sp)
        try:
            f = int(base.split("_")[1].split(".")[0])
        except (IndexError, ValueError):
            continue
        mp = os.path.join(out_m, f"mesen_{f:05d}.ppm")
        if not os.path.exists(mp):
            continue
        ref = np.array(Image.open(mp).convert("RGB"))
        rec = np.array(Image.open(sp).convert("RGB"))
        if ref.shape != rec.shape:
            print(f"  f={f}: shape {ref.shape} vs {rec.shape} — skipped")
            continue
        ri, _ = image_to_palette_indices(ref)
        ci, _ = image_to_palette_indices(rec)
        pct = 100.0 * np.sum(ri == ci) / ri.size
        flag = "" if pct >= thr else "  <-- BELOW THRESHOLD"
        if worst is None or pct < worst[1]:
            worst = (f, pct)
        if flag or f < 2:
            print(f"  f={f}: palette_idx {pct:5.1f}%{flag}")
    if worst:
        print(f"worst frame: f={worst[0]} {worst[1]:.1f}%")
    print("GREEN" if ok else "RED")


if __name__ == "__main__":
    main()
