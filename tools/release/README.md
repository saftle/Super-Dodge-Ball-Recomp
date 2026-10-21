# Release builds

Everything a 1.0 release needs, and the measurements behind it.

```
tools/release/
├── build.sh                     Linux / macOS: configure + build (Release)
├── build.ps1                    Windows: configure + build (Release, MSVC)
├── package.py                   Stage + archive, cross-platform
├── exe.name                     the shipped executable's name, in one file
├── make_boxart.py               draw the launcher's hero art
└── appimage/
    ├── AppRun.in                the AppImage entry point (a template)
    ├── build_appimage.sh        build the AppDir and package the AppImage
    └── test_appimage_layout.sh  the AppDir's layout and run test
```

## The executable's name

`tools/release/exe.name` holds `SuperDodgeBallRecomp`, and everything reads
it: the build (`-DNESRECOMP_EXE_NAME`), the packager, the AppImage's payload
and desktop entry, and the Flatpak manifest. One file, because the first
version had the name in three places and the Flatpak manifest drifted out of
step with the build — it failed loudly, but it should not have been able to.

The CMake **target** stays `nes_game`: that is the engine's convention, every
recomp project has `build-cycle/nes_game`, and this project's own tools and
gates refer to it. `OUTPUT_NAME` renames the *file* without moving the dev
tree's binary, so `build-cycle/nes_game` keeps working for every gate while
the shipped artifact is `SuperDodgeBallRecomp` / `SuperDodgeBallRecomp.exe`.

## What a release contains, and what it does not
The archive carries the recompiled executable, the launcher's assets, the mod
packages (declarations only), the cheat file and the licences.

It carries **no ROM**. The launcher asks the player for their own copy at
first launch, and the engine verifies it against the SHA-256 baked into the
binary. The ROM *is* needed at **build** time, because the build recompiles
the cartridge's code into the executable — so CI reads it from a repository
secret (`SDB_ROM_B64`, base64) and never commits it.

`package.py` refuses to write an archive that contains player data or a ROM:
`.nes`, `.fds`, `.qd`, `.sav`, `.srm`, `.state`, `.cycstate`, `rom.cfg`,
`config.ini`, `keybinds.ini`, `state.toml`, `launcher-window.ini`. The check
is the same list the engine's own Windows packaging recipe uses.

## Building

```bash
# Linux
tools/release/build.sh --build-dir build-release

# Windows (MSVC, Visual Studio 17 2022 x64)
tools\release\build.ps1 -Rom "roms\Super Dodge Ball (USA).nes"

# Package
tools/release/package.py --build-dir build-release --platform linux
tools/release/package.py --build-dir build-release --platform windows
```

`build.sh` finds the platform's SDL2 itself and refuses the engine's vendored
one, which is a **Windows x64 binary** (SDL2.dll and SDL2.lib): the engine
puts its cmake dir on `CMAKE_PREFIX_PATH`, where `find_package(SDL2 CONFIG)`
finds it first, and a Linux link against it fails. Name the platform's own
SDL2 with `-DSDL2_DIR` (the script does) or `SDL2_DIR` in the environment.

The Windows build links the **static** CRT (`CMAKE_MSVC_RUNTIME_LIBRARY=
MultiThreaded`), so the published ZIP needs no Visual C++ redistributable
beside it. That is the single most common "it will not start" report for a
Windows release.

## The AppImage, and why its AppRun is not a one-liner

The AppImage payload is a read-only squashfs, and the engine writes three
things **next to its executable**: `config.ini` (the launcher's settings), the
mod catalog's `state.toml` (which feature the player switched on) and
`rom.cfg` (the ROM the player picked). Those paths come from `/proc/self/exe`.

So a game run from inside the payload **cannot start**:

```
cannot write the settings file .../AppDir/usr/bin/config.ini
mods: cannot write mod state
```

(exit code 2). That is measured, not inferred — `test_appimage_layout.sh`
runs the game with the payload made read-only and asserts both that the run
reports no failed write and that the settings file and the mod state exist
afterwards, in a writable directory.

`AppRun.in` therefore runs the game from a **writable copy** of the payload:
next to the `.AppImage` when that directory is writable, in
`$XDG_DATA_HOME` otherwise. The copy is refreshed when the AppImage's version
changes, and the player's own state (settings, mod state, any mod packages
they installed) is carried across the refresh.

Two details that cost a real bug each:

- `cp -a` copies the payload's read-only modes, so the copy is read-only
  too. A recursive `chmod` does not descend into a directory it cannot
  write, so the directories are opened first (`find -type d -exec chmod
  u+rwx`) and the recursive pass comes second.
- `linuxdeploy` leaves `AppRun` as a **symlink to the executable**. Writing
  it without removing the link replaces the game with the script. The build
  removes the link first, and the layout test asserts that the executable is
  an ELF and not a script.

And one that is not about the modes at all:

- **`$APPIMAGE` is not necessarily ours.** Every AppImage exports it, and
  anything running inside one hands it down to its children — the launcher
  this session runs in does. The first AppRun took it at face value and put
  the game's writable copy in an unrelated directory (a Flatpak run wrote it
  into the player's `~/Downloads`). Believe `$APPIMAGE` only when `$APPDIR`
  is where the script itself lives.

## The Flatpak

`flatpak/org.saftle.SuperDodgeBallRecomp.yml` builds the same program into
`org.saftle.SuperDodgeBallRecomp`:

```bash
flatpak-builder --user --install --force-clean \
    --repo=tmp/flatpak-repo tmp/flatpak-build \
    tools/release/flatpak/org.saftle.SuperDodgeBallRecomp.yml
```

It uses the same `AppRun` script, with the payload at `/app/bin` — which is
read-only, exactly like the AppImage's — so the game runs from a writable
copy in the app's data directory. The ROM is a build-time source (the
manifest points at `roms/`), and the app ships without it.

The manifest's `command:` is a **bare name**: flatpak-builder resolves it
inside `/app/bin`, so `command: AppRun` finds `/app/bin/AppRun` and
`command: bin/AppRun` does not.

Building the Flatpak is also the **strictest compiler gate in the project**:
the SDK's GCC is newer than a typical desktop's, and it rejected a callback
signature that every local compiler had only warned about.

## The release workflow

`.github/workflows/release.yml` runs on a `v*` tag and produces:

| Artifact | Platform | Dependencies |
|---|---|---|
| `super-dodge-ball-recomp-<v>-linux-x86_64.tar.gz` | Linux x86-64 | SDL2 and OpenGL |
| `super-dodge-ball-recomp-<v>-linux-x86_64.AppImage` | Linux x86-64 | none |
| `super-dodge-ball-recomp-<v>-windows-x64.zip` | Windows x64 | none |
| `org.saftle.SuperDodgeBallRecomp` (Flatpak) | Linux x86-64 | none |

The tarball is the plain build; the AppImage bundles SDL2 and the other
libraries, so it runs on a machine that has nothing installed. The Windows ZIP
carries SDL2.dll and is statically linked against the CRT.

## Before you publish

1. `tools/release/package.py` — the archive must contain no ROM and no player
   data. The guard fails the packaging, it does not warn.
2. The AppImage layout test must pass. It is the only gate that proves the
   game *starts* from the packaged artifact.
3. The binary must run from the staged tree. `package.py` writes the archive;
   extract it somewhere else and run it there — the executable finds its
   assets, mods and cheats relative to itself.
4. `VERSION` holds the release version, and it is baked into the binary
   (`NESRECOMP_GAME_VERSION`) and printed in the startup log.

## Provenance

The AppImage tooling is pinned to **versioned** releases with their SHA-256
checksums (`linuxdeploy` 1-alpha-20251107-1, `appimagetool` 1.9.1). The
"continuous" releases are rebuilt upstream and their checksums stop matching,
which breaks the release build for a reason that has nothing to do with the
game.
