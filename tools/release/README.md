# Release builds

Everything a 1.0 release needs, and the measurements behind it.

```
tools/release/
├── build.sh                     Linux / macOS: configure + build (Release)
├── build.ps1                    Windows: configure + build (Release, MSVC)
├── build-mingw.sh               Windows: the same, cross-compiled from Linux
├── mingw-toolchain.cmake        the mingw-w64 toolchain file
├── fetch_mingw_sdl2.sh          fetch and checksum the mingw SDL2
├── gate-mingw.sh                the Windows build's four gates
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

# Windows (MSVC, Visual Studio 17 2022 x64) -- on a Windows machine
tools\release\build.ps1 -Rom "roms\Super Dodge Ball (USA).nes"

# Windows (mingw-w64), cross-compiled from Linux -- see below
tools/release/build-mingw.sh

# Package
tools/release/package.py --build-dir build-release --platform linux
tools/release/package.py --build-dir build-release/mingw --platform windows
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

## The Windows build from Linux

There are two Windows builds, and they are not the same artifact.

| | MSVC (`build.ps1`) | mingw (`build-mingw.sh`) |
|---|---|---|
| where it runs | a Windows machine, or a CI runner | this machine |
| needs the ROM in a secret store | **yes** | **no** |
| can be *run* before it ships | no | **yes**, under Wine |
| the C++ runtime | static (MSVC) | static (mingw) |
| the compiler | MSVC 17 x64 | mingw-w64 GCC 13.2.0, win32 threads |
| the SDL2.dll | the engine's vendored MSVC one | the official mingw package, pinned |

**The reason the second one exists is the ROM.** The build recompiles the
cartridge's code, so it needs the ROM; the ROM is not in GitHub and one
GitHub Actions secret cannot hold it (349,548 bytes of base64 against a
48 KB cap — see `docs/plan.md` item 21). Cross-compiling keeps the ROM on
the machine that owns it and needs no secret at all.

**The cost is honest:** it is a mingw binary, not an MSVC one. A different
compiler, a different CRT and a different `SDL2.dll`. Both routes should
exist — the cross build for the artifact that can be produced today, MSVC on
CI when an MSVC binary is wanted.

```bash
sudo apt install g++-mingw-w64-x86-64 wine wine64

tools/release/build-mingw.sh
tools/release/gate-mingw.sh
tools/release/package.py --build-dir build-release/mingw --platform windows
```

`wine64` is not optional. `wine` alone installs only the **32-bit** loader,
and it answers a 64-bit executable with *"Bad EXE format"* — which reads
like a broken binary and is not.

### The four gates

`gate-mingw.sh` is the Windows build's gate, and the fourth one is the one
that matters:

1. **PE32+.** `PE32+ executable (console) x86-64, for MS Windows`. The
   subsystem is `Windows CUI`: the engine's entry point is `main()`, not a
   `WinMain` nobody wrote, so the binary is a console application — as the
   MSVC build is.
2. **The imports.** `SDL2.dll`, `OPENGL32.dll`, `WS2_32.dll`, `d2d1.dll`,
   `DWrite.dll` and the system DLLs — and **no** `libgcc_s_seh-1.dll`,
   `libgcc_s_dw2-1.dll`, `libstdc++-6.dll` or `libwinpthread-1.dll`. The
   published ZIP needs `SDL2.dll` beside the exe and nothing else.
3. **It runs.** Under Wine: exit 0, and the **same cycle count** as the
   Linux build over the same vector.
4. **The guest state matches.** `--frame-log` over 3000 frames of
   `sdb_throw`: **381,900,012 bytes on each side, byte-identical**. The
   cross build executes the same machine.

### Two traps, each of which reads like a broken build

- **The engine's vendored MSVC SDL2 wins the `find_package(SDL2 CONFIG)`
  search on every platform**, because the engine puts its cmake dir on
  `CMAKE_PREFIX_PATH`. `-DSDL2_DIR` at the mingw package is therefore not
  optional, and the config it finds otherwise names an MSVC import library.
  `build-mingw.sh` checks that the CMake cache took the mingw path and
  fails the build if it did not — the alternative is an executable that
  links and cannot run.
- **Wine's `wine` wrapper prefers the 32-bit loader** and answers a 64-bit
  PE with *"Bad EXE format"*. The 64-bit loader is `/usr/lib/wine/wine64`,
  and a prefix created by the 32-bit loader is refused by it (*"is a
  32-bit installation"*). The gate names the loader and rebuilds the prefix
  rather than reporting either as a failure of the binary.

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

**The workflow cannot build yet, and says so in its own header.** Every job
needs the ROM at build time and reads it from the secret store, and the fetch
step is not written: one GitHub Actions secret cannot hold this ROM (349,548
bytes of base64 against a 48 KB cap), so it has to be reassembled from parts
and checked against its SHA-1 first — `docs/plan.md` item 21 has the spec.

**So there are two ways to publish, and they are not the same thing.**

- **By hand, from this machine.** Build the artifacts here, tag, and upload
  them yourself (`gh release create v1.0.0 release/*`). The cross build makes
  the *Windows* artifact possible on this route, because it needs no secret.
  This is the SMB/Zelda model: the release is built where the ROM lives.
- **By the workflow, from a tag.** Once the split-secrets fetch exists, the
  workflow builds all four artifacts itself, and nothing has to be built by
  hand at all.

Either way the artifacts are the same files, and `tools/release/gate-mingw.sh`
is the gate for the Windows one whichever route built it.

## Before you publish

1. `tools/release/package.py` — the archive must contain no ROM and no player
   data. The guard fails the packaging, it does not warn.
2. The AppImage layout test must pass. It is the only gate that proves the
   game *starts* from the packaged artifact.
3. The binary must run from the staged tree. `package.py` writes the archive;
   extract it somewhere else and run it there — the executable finds its
   assets, mods and cheats relative to itself.
4. The Windows build must pass `tools/release/gate-mingw.sh`. The fourth gate
   — the guest state byte-identical to the Linux build — is what says the
   cross build is the same machine, and it is the one a picture comparison
   would have stood in for.
5. `VERSION` holds the release version, and it is baked into the binary
   (`NESRECOMP_GAME_VERSION`) and printed in the startup log.

## Provenance

The AppImage tooling is pinned to **versioned** releases with their SHA-256
checksums (`linuxdeploy` 1-alpha-20251107-1, `appimagetool` 1.9.1). The
"continuous" releases are rebuilt upstream and their checksums stop matching,
which breaks the release build for a reason that has nothing to do with the
game.

The mingw SDL2 is pinned the same way: `SDL2-devel-2.30.0-mingw.tar.gz` from
the `release-2.30.0` tag, SHA-256
`d44bd8cd869219f43634a898d7735d0c92d4b2cca129f709dc8b98402b36d5f3`.
`fetch_mingw_sdl2.sh` refuses a tarball whose checksum does not match, and
checks that the unpacked package still has the three files the build needs
(the cmake package, the import library and the runtime DLL).
