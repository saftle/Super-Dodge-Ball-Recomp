#!/usr/bin/env python3
"""run_blargg.py — blargg test-ROM harness for NESRecomp runner patches.

Recompiles each manifest ROM with NESRecomp, builds it against the current
runner working tree (including whatever is in patches/*.patch), runs it
headless, and reports the blargg $6000 verdict per ROM.

Usage:
  python3 tools/blargg/run_blargg.py --list
  python3 tools/blargg/run_blargg.py --fetch        # clone nes-test-roms (asks nothing else)
  python3 tools/blargg/run_blargg.py                # full suite
  python3 tools/blargg/run_blargg.py --tier cpu --only instr_01
  python3 tools/blargg/run_blargg.py --rom "roms/Super Dodge Ball (USA).nes" --name surrogate

ROM sourcing: --roms-dir DIR (default external/nes-test-roms) holds the
christopherpow/nes-test-roms checkout; manifest paths are relative to it.
--fetch clones it (explicit opt-in). Missing ROMs are SKIP, never FAIL.

Exit code: 0 iff every tier=cpu test with a present ROM reports PASS.
tier=info failures are reported but non-blocking. Exit 2 if nothing ran.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS_DIR = os.path.join(ROOT, "tools", "blargg")
GEN_DIR = os.path.join(TOOLS_DIR, "generated")
TOML_DIR = os.path.join(TOOLS_DIR, "toml")
BUILD_DIR = os.path.join(ROOT, "build-blargg")
NESRECOMP_BIN = os.path.join(ROOT, "external", "nesrecomp", "recompiler", "build", "NESRecomp")
MANIFEST = os.path.join(TOOLS_DIR, "manifest.json")
DEFAULT_ROMS_DIR = os.path.join(ROOT, "external", "nes-test-roms")
FETCH_URL = "https://github.com/christopherpow/nes-test-roms"

CMAKE_TEMPLATE = """cmake_minimum_required(VERSION 3.20)
project(blargg_harness C CXX)
set(CMAKE_C_STANDARD 11)
set(NESRECOMP_ROOT "{nesrecomp_root}")
include(${{NESRECOMP_ROOT}}/runner/runner.cmake)
include_directories("{third_party_sdl}")
include_directories(${{NESRECOMP_RUNNER_INCLUDE_DIRS}})
include_directories("{bundled_sdl}")
set(SDL2_LIBRARY {sdl2_lib})
{targets}
"""

TARGET_TEMPLATE = """add_executable({exe}
    ${{NESRECOMP_RUNNER_SOURCES}}
    "{extras}"
    {sources}
)
target_include_directories({exe} PRIVATE
    ${{NESRECOMP_RUNNER_INCLUDE_DIRS}}
    "{bundled_sdl}"
    "{third_party_sdl}"
)
target_link_libraries({exe} PRIVATE ${{SDL2_LIBRARY}} m pthread)
"""


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
    """Capture which runner patches are active for result provenance."""
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
    return {"nesrecomp_head": commit, "worktree_dirty": bool(status),
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


def recompile(entry_path, prefix):
    """Run NESRecomp on one ROM. Returns (ok, log, gen_files)."""
    os.makedirs(GEN_DIR, exist_ok=True)
    os.makedirs(TOML_DIR, exist_ok=True)
    toml_path = os.path.join(TOML_DIR, prefix + ".toml")
    with open(toml_path, "w") as f:
        f.write(f'[game]\noutput_prefix = "{prefix}"\n')
    # Scrub stale outputs so a failed regen can't reuse last run's C.
    for stale in glob.glob(os.path.join(GEN_DIR, prefix + "*.c")):
        os.remove(stale)
    r = sh([NESRECOMP_BIN, entry_path, "--game", toml_path,
            "--output-prefix", prefix], cwd=TOOLS_DIR)
    log = (r.stdout + "\n" + r.stderr)[-4000:]
    # The umbrella <prefix>_full.c already contains all bank parts (same as
    # src/CMakeLists.txt); compiling the per-bank splits too would
    # double-define every function. Fall back to splits only if no umbrella.
    umbrella = os.path.join(GEN_DIR, prefix + "_full.c")
    dispatch = os.path.join(GEN_DIR, prefix + "_dispatch.c")
    if os.path.isfile(umbrella):
        gen = [umbrella] + ([dispatch] if os.path.isfile(dispatch) else [])
    else:
        gen = sorted(glob.glob(os.path.join(GEN_DIR, prefix + "*.c")))
    ok = r.returncode == 0 and gen and "TOML parse error" not in log
    return ok, log, gen


def write_cmakelists(tests):
    nesrecomp_root = os.path.join(ROOT, "external", "nesrecomp")
    third_party = os.path.join(ROOT, "src", "third_party", "SDL2")
    bundled = os.path.join(nesrecomp_root, "runner", "external", "SDL2", "include")
    sdl2_lib = "/usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0"
    extras = os.path.join(TOOLS_DIR, "blargg_extras.c")
    blocks = []
    for t in tests:
        srcs = "\n    ".join(f'"{g}"' for g in t["gen_files"])
        blocks.append(TARGET_TEMPLATE.format(exe=t["exe"], extras=extras, sources=srcs,
                                             bundled_sdl=bundled, third_party_sdl=third_party))
    content = CMAKE_TEMPLATE.format(nesrecomp_root=nesrecomp_root, third_party_sdl=third_party,
                                    bundled_sdl=bundled, sdl2_lib=sdl2_lib,
                                    targets="\n".join(blocks))
    os.makedirs(BUILD_DIR, exist_ok=True)
    with open(os.path.join(BUILD_DIR, "CMakeLists.txt"), "w") as f:
        f.write(content)


def build_all(jobs):
    r = sh(["cmake", "-S", BUILD_DIR, "-B", BUILD_DIR, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release"])
    if r.returncode != 0:
        return False, "cmake configure failed:\n" + (r.stdout + r.stderr)[-3000:]
    b = ["cmake", "--build", BUILD_DIR, "-j", str(jobs)]
    r = sh(b)
    if r.returncode != 0:
        return False, "cmake build failed:\n" + (r.stdout + r.stderr)[-6000:]
    return True, ""


def run_one(exe_path, rom_path, name, max_frames, wall_timeout):
    env = dict(os.environ)
    env["BLARGG_TEST_NAME"] = name
    env["BLARGG_MAX_FRAMES"] = str(max_frames)
    cmd = [exe_path, rom_path, "--smoke", str(max_frames),
           "--smoke-interval", str(max_frames)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=env,
                           timeout=wall_timeout)
        rc, out, err = r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "") if isinstance(e.stdout, str) else ""
        err = (e.stderr or "") if isinstance(e.stderr, str) else ""
        return {"name": name, "status": "WALL_TIMEOUT", "code": -1, "frames": None,
                "misses": None, "brks": None, "reset_seen": None, "detail": "",
                "tail": (out + err)[-800:]}
    verdict = None
    for line in out.splitlines():
        if line.startswith("[BLARGG]"):
            try:
                verdict = json.loads(line[len("[BLARGG]"):])
            except json.JSONDecodeError:
                continue
    if verdict and verdict.get("status") not in ("START",):
        verdict["tail"] = (out + err)[-800:]
        return verdict
    if "[HANG]" in err or "[HANG]" in out:
        return {"name": name, "status": "HANG", "code": -2, "frames": None,
                "misses": None, "brks": None, "reset_seen": None,
                "detail": "hang watchdog fired", "tail": (out + err)[-800:]}
    if rc != 0:
        return {"name": name, "status": "ERROR", "code": rc, "frames": None,
                "misses": None, "brks": None, "reset_seen": None,
                "detail": f"exit {rc} with no verdict", "tail": (out + err)[-800:]}
    return {"name": name, "status": "TIMEOUT", "code": 2, "frames": max_frames,
            "misses": None, "brks": None, "reset_seen": None,
            "detail": "no $6000 verdict before smoke exit", "tail": (out + err)[-800:]}


def main():
    ap = argparse.ArgumentParser(description="blargg suite for NESRecomp runner patches")
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
    ap.add_argument("--no-build", action="store_true", help="skip recompile+build, run existing binaries")
    ap.add_argument("--any-mapper", action="store_true",
                    help="ad-hoc: build ROMs of any mapper (minimal game.toml has no "
                         "bank_switch, so banked ROMs may miscompile — diagnostics only)")
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
        args.wall_timeout = int(cfg.get("wall_timeout", 120))

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
        if hdr[0] != 0 and not args.any_mapper:
            results.append({"name": e["name"], "status": "SKIP",
                            "detail": f"mapper {hdr[0]} unsupported in v1 (NROM only)"})
            print(f'{e["name"]:22s} SKIP         mapper {hdr[0]} (NROM-only v1)', flush=True)
            continue
        prefix = "blargg_" + sanitize(e["name"])
        tests.append({"name": e["name"], "tier": e.get("tier", "cpu"), "rom": os.path.abspath(p),
                      "prefix": prefix, "exe": "blargg_" + sanitize(e["name"]),
                      "hdr": hdr})

    if not args.no_build:
        for t in tests:
            ok, log, gen = recompile(t["rom"], t["prefix"])
            t["gen_files"] = gen
            if not ok:
                results.append({"name": t["name"], "status": "RECOMP_FAIL", "detail": log[-500:]})
                print(f'{t["name"]:22s} RECOMP_FAIL {log[-500:]}', flush=True)
        tests = [t for t in tests if t.get("gen_files")]
        if tests:
            write_cmakelists(tests)
            ok, msg = build_all(args.jobs)
            if not ok:
                print(msg)
                return 1
    else:
        for t in tests:
            t["gen_files"] = []

    for t in tests:
        exe_path = os.path.join(BUILD_DIR, t["exe"])
        if not os.path.isfile(exe_path):
            results.append({"name": t["name"], "status": "NO_BINARY",
                            "detail": "build produced no binary (use without --no-build?)"})
            continue
        print(f"--- {t['name']} ---", flush=True)
        r = run_one(exe_path, t["rom"], t["name"], args.max_frames, args.wall_timeout)
        r["tier"] = t["tier"]
        results.append(r)
        det = (r.get("detail") or "")[:100]
        print(f'{t["name"]:22s} {r["status"]:12s} frames={r.get("frames")} '
              f'misses={r.get("misses")} brks={r.get("brks")} {det}', flush=True)

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
    ran = [r for r in cpu if r["status"] not in ("SKIP",)]
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
