#!/usr/bin/env python3
"""watch_mesen.py — watch a canonical tas.json play in the MesenCE window.

Unlike --testRunner oracle runs (headless), this launches the MesenCE GUI
with a playback-only Lua script: setInput in inputPolled, no dumps, and
after the TAS ends the script stops overriding input so you can take over
with the keyboard.

Usage:
  python3 tools/tas/watch_mesen.py [--tas tas/sdb_4976.tas.json] [--frames N]
      [--out tas/runs/watch_mesen] [--rom ROM] [--dry-run]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

PROJ = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ROM_DEFAULT = os.path.join(PROJ, "roms", "Super Dodge Ball (USA).nes")
MESEN_BIN = os.path.join(PROJ, "external", "mesence", "Mesen")
MESENCE_DIR = os.path.join(PROJ, "external", "mesence")


def emit_lua(frames, path):
    with open(path, "w") as f:
        f.write("return {" + ",".join(str(m) for m in frames) + "}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tas", default=os.path.join(PROJ, "tas", "sdb_4976.tas.json"))
    ap.add_argument("--frames", type=int, default=None,
                    help="play only the first N TAS frames (default: whole file)")
    ap.add_argument("--out", default=os.path.join(PROJ, "tas", "runs", "watch_mesen"))
    ap.add_argument("--rom", default=ROM_DEFAULT)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    for p in (a.tas, a.rom, MESEN_BIN):
        if not os.path.exists(p):
            sys.exit(f"missing: {p}")
    tas = json.load(open(a.tas))
    frames = tas["frames"][:a.frames] if a.frames else tas["frames"]
    os.makedirs(a.out, exist_ok=True)
    home = os.path.join(a.out, "_home")
    os.makedirs(home, exist_ok=True)

    tas_lua = os.path.join(a.out, "input.lua")
    emit_lua(frames, tas_lua)

    # Isolated Mesen home: same settings the oracle harness uses, but GUI
    # mode (EnableTestMode off) so the window opens.
    settings_src = os.path.join(MESENCE_DIR, "settings.json")
    raw = open(settings_src, encoding="utf-8-sig").read()
    raw = raw.replace('"EnableTestMode": true', '"EnableTestMode": false')
    open(os.path.join(home, "settings.json"), "w").write(raw)
    for fn in ("MesenCore.so", "libHarfBuzzSharp.so", "libSkiaSharp.so"):
        src = os.path.join(MESENCE_DIR, fn)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(home, fn))
    for cand in (os.path.join(home, "mesen.lock"),
                  os.path.join(MESENCE_DIR, "mesen.lock")):
        if os.path.exists(cand):
            os.remove(cand)

    env = dict(os.environ, HOME=os.path.abspath(home),
               OUT_DIR=os.path.abspath(a.out),
               TAS_LUA=os.path.abspath(tas_lua),
               MAX_FRAMES=str(len(frames)))
    cmd = [MESEN_BIN, os.path.abspath(a.rom),
           os.path.join(PROJ, "tools", "tas", "watch_mesen.lua")]
    print(f"[watch-mesen] tas frames={len(frames)}", file=sys.stderr)
    print("[watch-mesen] +", " ".join(cmd), file=sys.stderr)
    if a.dry_run:
        return
    r = subprocess.run(cmd, env=env, cwd=PROJ)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
