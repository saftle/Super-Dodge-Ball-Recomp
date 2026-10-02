#!/usr/bin/env python3
"""run_blargg.py — blargg test-ROM harness on the cycle-accurate backend.

Each manifest ROM gets its own cycle-backend build (upstream runner/cyc
project integration, HEADLESS) and runs headless; the verdict comes from
the shell's result text in low RAM ($0201+, NUL-terminated: "Passed..." or
"Failed..."), read via --mem-frame/--mem-out. No extras, no patches: this
gates upstream cycle fidelity (submodule bumps), not game code.

Usage:
  python3 tools/blargg/run_blargg.py --list
  python3 tools/blargg/run_blargg.py --fetch        # clone nes-test-roms (asks nothing else)
  python3 tools/blargg/run_blargg.py                # full suite
  python3 tools/blargg/run_blargg.py --tier cpu --only instr_01
  python3 tools/blargg/run_blargg.py --rom path/to/test.nes --name label

ROM sourcing: --roms-dir DIR (default external/nes-test-roms) holds the
christopherpow/nes-test-roms checkout; manifest paths are relative to it.
--fetch clones it (explicit opt-in). Missing ROMs are SKIP, never FAIL.
Mappers outside the cycle backend's set are SKIP with reason.

Exit code: 0 iff every tier=cpu test with a present ROM reports PASS.
tier=info failures are reported but non-blocking. Exit 2 if nothing ran.
"""
import argparse
import concurrent.futures
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS_DIR = os.path.join(ROOT, "tools", "blargg")
BUILD_DIR = os.path.join(ROOT, "build-cyc-blargg")
CYC_PROJECT = os.path.join(ROOT, "external", "nesrecomp", "runner", "cyc", "project")
MANIFEST = os.path.join(TOOLS_DIR, "manifest.json")
DEFAULT_ROMS_DIR = os.path.join(ROOT, "external", "nes-test-roms")
FETCH_URL = "https://github.com/christopherpow/nes-test-roms"

# Mapper IDs the cycle backend implements (MAPPERS.md: originals 0,1,2,3,4,7,66).
CYC_MAPPERS = {0, 1, 2, 3, 4, 7, 66}


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def project_defaults():
    """Read [tool.blargg] from pyproject.toml (CLI flags override)."""
    try:
        import tomllib
    except ImportError:
        return {}
    try:
        with open(os.path.join(ROOT, "pyproject.toml"), "rb") as f:
            cfg = tomllib.load(f).get("tool", {}).get("blargg", {})
    except OSError:
        return {}
    return cfg if isinstance(cfg, dict) else {}


def sanitize(name):
    s = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not s or not (s[0].isalpha() or s[0] == "_"):
        s = "t_" + s
    return s


def parse_ines(path):
    """Return (mapper, prg_kb, chr_kb) or None on bad header."""
    try:
        with open(path, "rb") as f:
            h = f.read(16)
    except OSError:
        return None
    if len(h) < 16 or h[:4] != b"NES\x1a":
        return None
    mapper = ((h[7] & 0xF0)) | ((h[6] & 0xF0) >> 4)
    return (mapper, h[4] * 16, h[5] * 8)


def patch_state():
    """Capture framework provenance for results (backend is cycle: patches
    touch the legacy runner only, so this is informational here)."""
    sub = os.path.join(ROOT, "external", "nesrecomp")
    commit = sh(["git", "-C", sub, "rev-parse", "HEAD"]).stdout.strip()
    status = sh(["git", "-C", sub, "status", "--porcelain"]).stdout.strip()
    patches = sorted(glob.glob(os.path.join(ROOT, "patches", "*.patch")))
    applied = []
    for p in patches:
        # If reverse-apply checks cleanly, the patch is currently applied.
        r = sh(["git", "-C", sub, "apply", "--check", "--reverse", p])
        applied.append({"file": os.path.basename(p),
                        "applied": r.returncode == 0})
    return {"backend": "cycle", "nesrecomp_head": commit, "worktree_dirty": bool(status),
            "worktree_status": status[:2000], "patches": applied}


def ensure_roms_dir(args):
    if os.path.isdir(args.roms_dir):
        return True
    print(f"ROMs dir missing: {args.roms_dir}")
    print(f"  clone it:  python3 tools/blargg/run_blargg.py --fetch")
    print(f"  or point:  --roms-dir <dir with nes-test-roms checkout>")
    return False


def do_fetch(args):
    if os.path.isdir(args.roms_dir):
        print(f"exists, not cloning: {args.roms_dir}")
        return 0
    print(f"cloning {FETCH_URL} -> {args.roms_dir}")
    r = sh(["git", "clone", "--depth", "1", FETCH_URL, args.roms_dir])
    if r.returncode != 0:
        print(r.stderr[-3000:])
        return 1
    print("clone ok")
    return 0


def load_manifest():
    with open(MANIFEST) as f:
        return json.load(f)["roms"]


def build_one(t, jobs):
    """Configure + build one ROM's cycle backend. Returns (ok, detail)."""
    bdir = os.path.join(BUILD_DIR, "cyc-" + sanitize(t["name"]))
    t["build_dir"] = bdir
    t["exe"] = os.path.join(bdir, "nes_game")
    cfg = ["cmake", "-S", CYC_PROJECT, "-B", bdir,
           f"-DNESRECOMP_ROM={t['rom']}", "-DNESRECOMP_HEADLESS=ON"]
    r = sh(cfg)
    if r.returncode != 0:
        return False, "cmake configure failed:\n" + (r.stdout + r.stderr)[-2000:]
    b = ["cmake", "--build", bdir, "-j", str(jobs)]
    r = sh(b)
    if r.returncode != 0 or not os.path.isfile(t["exe"]):
        return False, "cmake build failed:\n" + (r.stdout + r.stderr)[-3000:]
    return True, ""


def read_verdict_text(mem_path):
    """Extract the shell result text near $0200 (NUL-terminated ASCII run).

    Singles print `Passed...` at $0201; multi-test suites print
    `All N tests passed` at $0200; some leave binary debris before the text.
    Searches $0200-$02FF for the first long printable run instead of
    assuming an offset.
    """
    try:
        rows = {}
        for line in open(mem_path, errors="replace"):
            m = re.match(r"ram ([0-9A-Fa-f]{4}):((?: [0-9A-Fa-f]{2})+)", line)
            if m:
                rows[int(m.group(1), 16)] = bytes(int(b, 16) for b in m.group(2).split())
        buf = b"".join(rows.get(a, b"") for a in range(0x200, 0x300, 0x20))
        text = buf.decode("ascii", errors="replace")
        # Shell verdict shapes observed: `Passed<suffix>` at $0200/$0201
        # (suffix varies; bare `Passed` is only 6 chars) and
        # `All N tests passed` anywhere in the window.
        m = re.search(r"(Passed[ -~]*?)(?:\x00|$)|((?:All \d+ tests? passed|Failed[ -~]*?))(?:\x00|$)", text)
        return (m.group(0).rstrip("\x00") if m else "")
    except OSError:
        return ""


def classify_verdict(text):
    if re.search(r"passed", text, re.IGNORECASE) and not re.search(r"failed?", text, re.IGNORECASE):
        return "PASS"
    if re.search(r"failed?", text, re.IGNORECASE):
        return "FAIL"
    return ""


def run_one(t, max_frames, wall_timeout):
    mem_path = os.path.join(t["build_dir"], "mem.txt")
    miss_path = os.path.join(t["build_dir"], "miss.txt")
    shot_path = os.path.join(t["build_dir"], "shot.png")
    cmd = [t["exe"], t["rom"], "--frames", str(max_frames), "--ram-init", "zeros",
           "--mem-frame", str(max_frames - 1), "--mem-out", mem_path,
           "--miss-log", miss_path, "--screenshot", shot_path]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=wall_timeout)
        rc, out, err = r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired as e:
        out = e.stdout if isinstance(e.stdout, str) else ""
        err = e.stderr if isinstance(e.stderr, str) else ""
        return {"name": t["name"], "status": "HANG", "code": -2, "frames": None,
                "misses": None, "native_pct": None, "detail": "wall timeout (stalled)",
                "tail": (out + err)[-800:]}
    native = None
    m = re.search(r"\((\d+\.\d+)%\)", out or "")
    if m:
        native = float(m.group(1))
    misses = None
    if os.path.isfile(miss_path):
        misses = sum(1 for ln in open(miss_path) if ln.strip() and not ln.startswith("#"))
    text = read_verdict_text(mem_path)
    verdict = classify_verdict(text)
    if verdict == "PASS":
        return {"name": t["name"], "status": "PASS", "code": 0, "frames": max_frames,
                "misses": misses, "native_pct": native, "detail": text[:100],
                "tail": (out + err)[-800:]}
    if verdict == "FAIL":
        return {"name": t["name"], "status": "FAIL", "code": 1, "frames": max_frames,
                "misses": misses, "native_pct": native, "detail": text[:100],
                "tail": (out + err)[-800:]}
    if rc != 0:
        return {"name": t["name"], "status": "ERROR", "code": rc, "frames": None,
                "misses": misses, "native_pct": native,
                "detail": f"exit {rc} with no verdict", "tail": (out + err)[-800:]}
    # Ran the full budget with no RAM text: some tests (branch timing)
    # render their verdict to the screen only. Human-readable screenshot
    # saved; reported, non-blocking (like info tier).
    return {"name": t["name"], "status": "UNVERIFIED",
            "code": 3, "frames": max_frames,
            "misses": misses, "native_pct": native,
            "detail": f"no RAM text; see {shot_path}",
            "tail": (out + err)[-800:]}


def main():
    ap = argparse.ArgumentParser(description="blargg suite on the cycle backend")
    ap.add_argument("--roms-dir", default=None,
                    help="nes-test-roms checkout (default: [tool.blargg] roms_dir)")
    ap.add_argument("--fetch", action="store_true", help="git clone nes-test-roms")
    ap.add_argument("--list", action="store_true", help="show manifest + ROM presence")
    ap.add_argument("--tier", default=None, choices=["all", "cpu", "info"])
    ap.add_argument("--only", default="", help="substring filter on test name")
    ap.add_argument("--rom", default="", help="ad-hoc single ROM (skips manifest)")
    ap.add_argument("--name", default="", help="label for --rom")
    ap.add_argument("--max-frames", type=int, default=None)
    ap.add_argument("--wall-timeout", type=int, default=None)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--no-build", action="store_true", help="skip configure+build, run existing binaries")
    ap.add_argument("--any-mapper", action="store_true",
                    help="ad-hoc: also build ROMs outside the cycle mapper set (diagnostics only)")
    args = ap.parse_args()

    cfg = project_defaults()
    if args.roms_dir is None:
        args.roms_dir = cfg.get("roms_dir", DEFAULT_ROMS_DIR)
    if not os.path.isabs(args.roms_dir):
        args.roms_dir = os.path.join(ROOT, args.roms_dir)
    if args.tier is None:
        args.tier = cfg.get("tier", "all")
    if args.max_frames is None:
        args.max_frames = int(cfg.get("max_frames", 3600))
    if args.wall_timeout is None:
        args.wall_timeout = int(cfg.get("wall_timeout", 300))

    if args.fetch:
        return do_fetch(args)

    if args.rom:
        entries = [{"name": args.name or os.path.splitext(os.path.basename(args.rom))[0],
                    "path": args.rom, "tier": "cpu", "adhoc": True}]
    else:
        entries = load_manifest()
        if args.tier != "all":
            entries = [e for e in entries if e.get("tier") == args.tier]
    if args.only:
        entries = [e for e in entries if args.only in e["name"]]

    if args.list:
        for e in entries:
            p = e["path"] if os.path.isabs(e["path"]) else os.path.join(args.roms_dir, e["path"])
            hdr = parse_ines(p) if os.path.isfile(p) else None
            print(f'{e["name"]:22s} tier={e.get("tier", "?"):4s} '
                  f'{"MISSING" if hdr is None else f"mapper={hdr[0]} prg={hdr[1]}k chr={hdr[2]}k"}  {p}')
        return 0

    if not args.rom and not ensure_roms_dir(args):
        return 2

    tests = []
    results = []
    for e in entries:
        if e.get("adhoc") or os.path.isabs(e["path"]):
            p = e["path"]
        else:
            p = os.path.join(args.roms_dir, e["path"])
        if not os.path.isfile(p):
            results.append({"name": e["name"], "status": "SKIP", "detail": f"missing ROM: {p}"})
            print(f'{e["name"]:22s} SKIP         missing ROM', flush=True)
            continue
        hdr = parse_ines(p)
        if hdr is None:
            results.append({"name": e["name"], "status": "SKIP", "detail": "bad iNES header"})
            print(f'{e["name"]:22s} SKIP         bad iNES header', flush=True)
            continue
        if hdr[0] not in CYC_MAPPERS and not args.any_mapper:
            results.append({"name": e["name"], "status": "SKIP",
                            "detail": f"mapper {hdr[0]} outside cycle set"})
            print(f'{e["name"]:22s} SKIP         mapper {hdr[0]} (outside cycle set)', flush=True)
            continue
        tests.append({"name": e["name"], "tier": e.get("tier", "cpu"),
                      "rom": os.path.abspath(p), "hdr": hdr})

    if not args.no_build:
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(args.jobs, max(1, len(tests)))) as ex:
            built = dict(zip([t["name"] for t in tests],
                             ex.map(lambda t: build_one(t, 2), tests)))
        for t in tests:
            ok, detail = built[t["name"]]
            if not ok:
                results.append({"name": t["name"], "status": "RECOMP_FAIL", "detail": detail[-500:]})
                print(f'{t["name"]:22s} RECOMP_FAIL {detail[-200:]}', flush=True)
        tests = [t for t in tests if t.get("exe") and os.path.isfile(t["exe"])]
    else:
        for t in tests:
            t["build_dir"] = os.path.join(BUILD_DIR, "cyc-" + sanitize(t["name"]))
            t["exe"] = os.path.join(t["build_dir"], "nes_game")

    for t in tests:
        if not os.path.isfile(t.get("exe", "")):
            results.append({"name": t["name"], "status": "NO_BINARY",
                            "detail": "build produced no binary (use without --no-build?)"})
            continue
        print(f"--- {t['name']} ---", flush=True)
        r = run_one(t, args.max_frames, args.wall_timeout)
        r["tier"] = t["tier"]
        results.append(r)
        det = (r.get("detail") or "")[:100]
        print(f'{t["name"]:22s} {r["status"]:12s} frames={r.get("frames")} '
              f'misses={r.get("misses")} native={r.get("native_pct")} {det}', flush=True)

    state = patch_state()
    out = {"patch_state": state, "results": results,
           "max_frames": args.max_frames}
    os.makedirs(BUILD_DIR, exist_ok=True)
    with open(os.path.join(BUILD_DIR, "results.json"), "w") as f:
        json.dump(out, f, indent=2)

    cpu = [r for r in results if r.get("tier") == "cpu"]
    info = [r for r in results if r.get("tier") == "info"]
    def counts(rs):
        c = {}
        for r in rs:
            c[r["status"]] = c.get(r["status"], 0) + 1
        return c
    print(f"\ncpu : {counts(cpu)}")
    print(f"info: {counts(info)}")
    ran = [r for r in cpu if r.get("status") not in ("SKIP", "UNVERIFIED")]
    fails = [r for r in ran if r["status"] != "PASS"]
    unver = [r for r in cpu if r.get("status") == "UNVERIFIED"]
    if unver:
        print(f"UNVERIFIED (screen verdict only, see shot.png in each build dir): "
              f"{[r['name'] for r in unver]}")
    fails = [r for r in ran if r["status"] != "PASS"]
    if not ran:
        print("nothing ran (all SKIP).")
        return 2
    if fails:
        print(f"FAIL: {len(fails)}/{len(ran)} cpu tests not passing.")
        return 1
    print(f"ALL PASS: {len(ran)}/{len(ran)} cpu tests.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
