#!/usr/bin/env python3
"""Stage and archive a release build of Super Dodge Ball Recomp.

The published archive carries NO ROM and NO player data. The launcher asks the
player for their own copy of the cartridge at first launch; what ships here is
the recompiled executable, the launcher assets, the mod packages (declarations
only), the cheat file, and the licences.

    tools/release/package.py --build-dir build-release --platform linux

Exit codes:
    0  the archive was written
    2  nothing was packaged (no build, no assets) -- the failure mode this
       project keeps hitting, where a gate passes over zero examined files
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

# Player data and ROMs must never reach a release archive. The engine's own
# Windows packaging recipe refuses a stage that holds any of these; the same
# list, applied on every platform.
FORBIDDEN_SUFFIXES = {".nes", ".fds", ".qd", ".sav", ".srm", ".state", ".cycstate", ".log", ".dmp"}
FORBIDDEN_NAMES = {"rom.cfg", "config.ini", "keybinds.ini", "debug.ini", "state.toml",
                   "launcher-window.ini", "mesen.lock"}

# Licences that must travel with the binary, wherever each one lives.
LICENCE_SOURCES = [
    ("Super-Dodge-Ball-Recomp-LICENSE.md", "LICENSE.md"),
    ("Super-Dodge-Ball-Recomp-NOTICE.md", "NOTICE.md"),
    ("NESRecomp-LICENSE.txt", "external/nesrecomp/LICENSE"),
    ("SDL2-COPYING.txt", "external/nesrecomp/runner/external/SDL2/COPYING.txt"),
    ("ImGui-LICENSE.txt", "external/recomp-ui/src/third_party/imgui/LICENSE.txt"),
]

README_TEMPLATE = """\
Super Dodge Ball Recomp {version} - {platform} ({arch})

A cycle-accurate recompilation of Super Dodge Ball (USA). The game's 6502 code
is recompiled ahead of time; the picture, sound and music are the original
cartridge's, so you need your own copy of the ROM to play.

  1. Run {exe}.
  2. The launcher opens. Point it at your own copy of the ROM
     (Super Dodge Ball (USA).nes) when it asks.
  3. Play.

No ROM is included in this archive and none is needed to install it. The
launcher checks the copy you select, and reads the cartridge's graphics and
music out of it while the game runs -- which is why the archive is not a
substitute for the original.

If the launcher says the ROM is "not recognized", check that you have the
right file before anything else:
    CRC32  689971F9
    SHA-1  42f954e9bd3256c011aba14c7e5b400abe35fde3

Controls
  Arrow keys  D-pad          Z  A            X  B
  Enter      Start          \\  Select       Esc  in-game menu
  Hold Tab   fast-forward   F8 / F9  save / load a cycle state
  F11        fullscreen     F12  screenshot

Gamepads work too; the launcher (Controls) remaps them. Settings live in
config.ini next to the executable -- which, for the AppImage and the Flatpak,
is inside the writable copy the launcher makes, not the read-only one.

Mods
  The Mods screen in the launcher toggles the three features this port adds:
  character-flicker removal, widescreen with margin sprites, and PPU
  overclocking. All three ship OFF; the game is stock until you enable one.

Cheats
  The Escape menu has a Cheats section, one switch per entry. The file is
  cheats/Super Dodge Ball (USA).cht and every entry ships disabled.

Licences are in the licenses/ directory. The game itself is not ours to
licence: see licenses/Super-Dodge-Ball-Recomp-NOTICE.md for who owns it.

This build was made from revision {revision}.
"""


def die(message: str, code: int = 2) -> "None":
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(code)


def find_executable(build_dir: Path, platform: str, exe_name: str) -> Path:
    name = f"{exe_name}.exe" if platform == "windows" else exe_name
    candidates = [
        build_dir / name,                                   # single-config generators
        build_dir / "Release" / name,                       # multi-config generators
        build_dir / "RelWithDebInfo" / name,
        build_dir / "MinSizeRel" / name,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    die(f"no {name} under {build_dir} (looked in "
        f"{', '.join(str(c) for c in candidates)}). Build it first.")


def copy_tree(src: Path, dst: Path, skip_names: set[str] | None = None) -> int:
    """Copy a directory tree, skipping named files. Returns the file count."""
    skip_names = skip_names or set()
    count = 0
    for path in sorted(src.rglob("*")):
        if path.is_dir():
            continue
        if path.name in skip_names:
            continue
        target = dst / path.relative_to(src)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        count += 1
    return count


def guard(stage: Path) -> list[str]:
    """Return every player-data or ROM file found in the stage."""
    found = []
    for path in stage.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() in FORBIDDEN_SUFFIXES or path.name in FORBIDDEN_NAMES:
            found.append(str(path.relative_to(stage)))
    return found


def epoch() -> int:
    """Archive member timestamp: SOURCE_DATE_EPOCH when set, else now."""
    value = os.environ.get("SOURCE_DATE_EPOCH")
    if value and value.isdigit():
        return int(value)
    return int(time.time())


def git_revision(root: Path) -> str:
    """The short commit the build came from, and whether the tree was dirty.

    Reading .git/HEAD is not enough: it holds "ref: refs/heads/master", and
    a release that says it was built from "ref: refs/he" is worse than one
    that says nothing. Resolve the ref, and say so plainly when the working
    tree carried changes the commit does not -- a release built from an
    uncommitted tree must not claim to be the commit.
    """
    try:
        out = subprocess.run(["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=30)
        if out.returncode == 0 and out.stdout.strip():
            sha = out.stdout.strip()
            dirty = subprocess.run(["git", "-C", str(root), "status", "--porcelain"],
                                   capture_output=True, text=True, timeout=30)
            if dirty.returncode == 0 and dirty.stdout.strip():
                return f"{sha} (with uncommitted changes)"
            return sha
    except (OSError, subprocess.SubprocessError):
        pass
    # No git: read the refs by hand.
    git_dir = root / ".git"
    if git_dir.is_file():
        try:
            git_dir = Path(git_dir.read_text().split("gitdir:", 1)[1].strip())
        except (OSError, IndexError):
            return ""
    head = git_dir / "HEAD"
    if not head.is_file():
        return ""
    text = head.read_text().strip()
    if text.startswith("ref:"):
        ref = text.split(":", 1)[1].strip()
        loose = git_dir / ref
        if loose.is_file():
            text = loose.read_text().strip()
        else:
            packed = git_dir / "packed-refs"
            if packed.is_file():
                for line in packed.read_text().splitlines():
                    parts = line.split()
                    if len(parts) == 2 and parts[1] == ref:
                        text = parts[0]
                        break
    return text[:12] if text else ""


def make_tar(stage: Path, out: Path) -> None:
    stamp = epoch()
    with tarfile.open(out, "w:gz", format=tarfile.PAX_FORMAT) as tar:
        for path in sorted(stage.rglob("*")):
            if path.is_dir():
                continue
            info = tar.gettarinfo(str(path), arcname=str(path.relative_to(stage.parent)))
            info.mtime = stamp
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            with path.open("rb") as handle:
                tar.addfile(info, handle)


def make_zip(stage: Path, out: Path) -> None:
    stamp = epoch()
    # A fixed timestamp keeps two builds of the same commit byte-identical.
    date_time = time.gmtime(stamp)[:6]
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(stage.rglob("*")):
            if path.is_dir():
                continue
            info = zipfile.ZipInfo(str(path.relative_to(stage.parent)), date_time)
            info.compress_type = zipfile.ZIP_DEFLATED
            # 0o755 for anything that was executable in the build tree.
            mode = path.stat().st_mode
            info.external_attr = (stat.S_IMODE(mode) & 0o777) << 16
            zf.writestr(info, path.read_bytes())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build-release", type=Path)
    parser.add_argument("--platform", required=True, choices=["linux", "windows", "macos"])
    parser.add_argument("--arch", default=None,
                        help="the architecture in the archive's name "
                             "(default: x86_64, or x64 on Windows)")
    parser.add_argument("--version", default="")
    parser.add_argument("--revision", default="")
    parser.add_argument("--out", default="release", type=Path)
    parser.add_argument("--exe", type=Path, default=None,
                        help="explicit path to the executable (default: found under --build-dir)")
    parser.add_argument("--exe-name", default=None,
                        help="the executable's file name (default: tools/release/exe.name)")
    parser.add_argument("--bundle", action="append", default=[], type=Path,
                        help="extra runtime file to ship beside the executable "
                             "(repeatable; e.g. libSDL2-2.0.so.0)")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    build_dir = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    out_dir = args.out if args.out.is_absolute() else root / args.out

    version = args.version
    if not version:
        version_file = root / "VERSION"
        if version_file.is_file():
            version = version_file.read_text().splitlines()[0].strip()
    version = version or "development"

    # x86_64 everywhere except Windows, which spells it x64 -- and the
    # AppImage beside it is x86_64, so the two must not disagree.
    arch = args.arch or ("x64" if args.platform == "windows" else "x86_64")

    revision = args.revision or git_revision(root)

    exe_name = args.exe_name or (root / "tools/release/exe.name").read_text().strip()
    if not exe_name:
        die("no executable name: tools/release/exe.name is empty")
    exe = args.exe if args.exe else find_executable(build_dir, args.platform, exe_name)
    if not exe.is_file():
        die(f"no executable at {exe}")

    # The engine stages the launcher assets, the mod catalog and the cheat
    # file BESIDE THE EXECUTABLE, and where that is depends on the
    # generator: a single-config build puts them in the build directory's
    # root, a multi-config build puts them in its Release/ subdirectory.
    # Follow the executable rather than assuming a layout.
    exe_dir = exe.parent
    assets = exe_dir / "assets"
    if not assets.is_dir():
        die(f"no launcher assets at {assets}. The build stages them beside the "
            f"executable; a headless build has none and cannot be packaged.")

    name = f"super-dodge-ball-recomp-{version}-{args.platform}-{arch}"
    stage_root = out_dir / "stage"
    if stage_root.exists():
        shutil.rmtree(stage_root)
    stage = stage_root / name
    stage.mkdir(parents=True)

    # 1. the executable
    exe_name = exe.name
    shutil.copy2(exe, stage / exe_name)
    if args.platform != "windows":
        (stage / exe_name).chmod(0o755)

    # 2. runtime libraries the build was told to bundle (SDL2 and friends)
    for bundle in args.bundle:
        bundle = bundle if bundle.is_absolute() else root / bundle
        if not bundle.is_file():
            die(f"bundled runtime file not found: {bundle}")
        shutil.copy2(bundle, stage / bundle.name)

    if args.platform == "windows":
        # The engine's build copies SDL2.dll beside the executable (the
        # vendored SDL2 is a Windows x64 binary with an import library, so
        # the DLL is the runtime). Without it the game does not start, and
        # "it will not start" is the most common report for a Windows
        # release -- so its absence fails the packaging rather than shipping.
        sdl2 = exe_dir / "SDL2.dll"
        if not sdl2.is_file():
            die(f"no SDL2.dll beside the executable ({sdl2}). The Windows "
                f"build stages it there; the published ZIP needs it.")
        shutil.copy2(sdl2, stage / "SDL2.dll")

    # 3. launcher assets
    copy_tree(assets, stage / "assets")

    # 4. mod packages -- the manifests only. state.toml is the player's
    #    selection, not part of the release.
    packages = exe_dir / "mods" / "packages"
    if not packages.is_dir():
        die(f"no mod packages at {packages}")
    copy_tree(packages, stage / "mods" / "packages")

    # 5. cheats
    cheats = exe_dir / "cheats"
    if not cheats.is_dir():
        die(f"no cheats at {cheats}")
    copy_tree(cheats, stage / "cheats")

    # 6. licences
    licences = stage / "licenses"
    licences.mkdir()
    for target_name, source in LICENCE_SOURCES:
        source_path = root / source
        if not source_path.is_file():
            die(f"missing licence: {source}")
        shutil.copy2(source_path, licences / target_name)

    # 7. the readme
    # The platform is spelled for a reader here ("Windows"), while the
    # archive's own name keeps the lowercase form the conventions use.
    platform_label = {"linux": "Linux", "windows": "Windows",
                      "macos": "macOS"}.get(args.platform, args.platform)
    (stage / "README.txt").write_text(
        README_TEMPLATE.format(version=version, platform=platform_label, arch=arch,
                                exe=exe_name,
                                revision=revision or "unknown"),
        encoding="utf-8")

    # 8. the guard
    forbidden = guard(stage)
    if forbidden:
        die("player data or a ROM found in the release stage: " + ", ".join(forbidden))

    # 9. the archive
    out_dir.mkdir(parents=True, exist_ok=True)
    if args.platform == "windows":
        archive = out_dir / f"{name}.zip"
        make_zip(stage, archive)
    else:
        archive = out_dir / f"{name}.tar.gz"
        make_tar(stage, archive)

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    files = sum(1 for path in stage.rglob("*") if path.is_file())
    size = archive.stat().st_size
    print(f"archive  {archive}")
    print(f"         {files} files, {size} bytes")
    print(f"sha256   {digest}")

    shutil.rmtree(stage_root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
