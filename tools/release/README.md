# Release builds

Everything a 1.0 release needs, and the measurements behind it.

**The whole release is one command:**

```bash
tools/release/release.sh              # build, gate, package, verify
tools/release/release.sh --publish    # ... and publish the release
```

It builds all four artifacts (the Linux tarball, the AppImage, the Windows
ZIP and the Flatpak), runs the Windows build's five gates, packages them,
verifies that none of them carries a ROM and that the commit the binary
names is the commit being released, writes the release notes, and — with
`--publish` — creates the tag and the release.

**There is no release workflow.** The release is built and published by hand,
on the machine that owns the ROM, and the ROM never leaves it. See "The ROM
never leaves this machine" below for why that was a decision and not a
shortcut.

```
tools/release/
├── release.sh                    build, verify and publish a release
├── build.sh                      Linux / macOS: configure + build (Release)
├── build-mingw.sh                Windows: the same, cross-compiled from Linux
├── mingw-toolchain.cmake         the mingw-w64 toolchain file
├── fetch_mingw_sdl2.sh           fetch and checksum the mingw SDL2
├── gate-mingw.sh                 the Windows build's five gates
├── package.py                    Stage + archive, cross-platform
├── exe.name                      the shipped executable's name, in one file
├── make_boxart.py                draw the launcher's hero art
└── appimage/
    ├── AppRun.in                 the AppImage entry point (a template)
    ├── build_appimage.sh         build the AppDir and package the AppImage
    └── test_appimage_layout.sh   the AppDir's layout and run test
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
the cartridge's code into the executable — so the build runs on the machine
that owns it, and the ROM never leaves that machine. See "The ROM never
leaves this machine" below.

`package.py` refuses to write an archive that contains player data or a ROM:
`.nes`, `.fds`, `.qd`, `.sav`, `.srm`, `.state`, `.cycstate`, `rom.cfg`,
`config.ini`, `keybinds.ini`, `state.toml`, `launcher-window.ini`. The check
is the same list the engine's own Windows packaging recipe uses.

## Building one artifact

`release.sh` is the whole release; these are its parts, for when you want
one of them — a quicker iteration on the Windows build, say, or a rebuild
after touching a mod.

```bash
# Linux
tools/release/build.sh --build-dir build-release

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

The Windows build links the **static** mingw runtime
(`-static-libgcc -static-libstdc++`), so the published ZIP needs no C++
runtime beside it. That is the single most common "it will not start"
report for a Windows release.

## The Windows build

It is **cross-compiled from Linux** with mingw-w64, and that is the only
Windows route in this project.

| | |
|---|---|
| where it runs | this machine |
| the ROM | stays on this machine, beside the build |
| can be *run* before it ships | **yes**, under Wine |
| the C++ runtime | static (mingw) |
| the compiler | mingw-w64 GCC 13.2.0, win32 threads |
| the SDL2.dll | the official mingw package, pinned by SHA-256 |

**The reason it is cross-compiled is that there is no Windows machine.**
The engine supports cross builds explicitly (`runner/cyc/project.cmake`:
*"Cross builds require RECOMPILER pointing to a runnable host NESRecomp"*), and
a cross-compiled binary can be **run here under Wine** before it ships —
which the CI-with-MSVC route could never do, because nothing on a CI runner
can execute a Windows binary.

**What it costs, stated plainly:** it is a **mingw** binary, not an MSVC
one. A different compiler, a different CRT and a different `SDL2.dll` from
the artifact an MSVC build would produce, and the MSVC build is the one
Super Mario Bros. and Zelda ship. There was an MSVC route here once
(`build.ps1`) and it is gone: it needed a Windows machine, nothing on this
one could run it, and it could not be verified before it shipped — which is
exactly what the cross build can do. If an MSVC binary is ever wanted, the
engine's own `external/nesrecomp/tools/build_cycle_windows.ps1` is the
starting point.

```bash
sudo apt install g++-mingw-w64-x86-64 wine wine64

tools/release/build-mingw.sh
tools/release/gate-mingw.sh
tools/release/package.py --build-dir build-release/mingw --platform windows
```

`wine64` is not optional. `wine` alone installs only the **32-bit** loader,
and it answers a 64-bit executable with *"Bad EXE format"* — which reads
like a broken binary and is not.

### The five gates

`gate-mingw.sh` is the Windows build's gate, and the fourth one is the one
that matters:

1. **PE32+.** `PE32+ executable (console) x86-64, for MS Windows`. The
   subsystem is `Windows CUI`: the engine's entry point is `main()`, not a
   `WinMain` nobody wrote, so the binary is a console application.
2. **The imports.** `SDL2.dll`, `OPENGL32.dll`, `WS2_32.dll`, `d2d1.dll`,
   `DWrite.dll` and the system DLLs — and **no** `libgcc_s_seh-1.dll`,
   `libgcc_s_dw2-1.dll`, `libstdc++-6.dll` or `libwinpthread-1.dll`. The
   published ZIP needs `SDL2.dll` beside the exe and nothing else.
3. **It runs.** Under Wine: exit 0, and the **same cycle count** as the
   Linux build over the same vector.
4. **The guest state matches.** `--frame-log` over 3000 frames of
   `sdb_throw`: **381,900,012 bytes on each side, byte-identical**. The
   cross build executes the same machine.
5. **The launcher opens.** The launcher is the *first* thing a player sees,
   and it is a different code path from the headless one — Direct2D,
   DirectWrite, OpenGL and SDL's window. A headless run touches none of
   it, so a binary can pass the first four gates and still show a player a
   black rectangle. The gate runs it on an Xvfb display and looks for the
   window. It runs from a **copy** of the build tree: the engine writes
   `config.ini`, `rom.cfg` and the mod catalog's `state.toml` next to its
   executable, and that state is the *player's*.

### Three traps, each of which reads like a broken build

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
- **SDL's video driver is named for the target, not the display.** A Windows
  SDL2 has no `x11` driver to name — its default is `windows`, which Wine
  draws on X11 underneath. So the launcher gate *unsets*
  `SDL_VIDEODRIVER` rather than setting it: the headless gates set it to
  `dummy` (a windowless run needs no display), and a launcher told to use
  `dummy` says *"SDL_CreateWindow failed"* and stops, and a launcher told
  to use `x11` says *"x11 not available"*. Both read like a broken build.

## The ROM never leaves this machine

A release build needs the ROM, because it recompiles the cartridge's code.
The ROM is not in the repository — it is copyrighted, and a player supplies
their own copy. So the build happens **on the machine that owns the ROM**,
and the ROM never leaves it: not into the repository, not into a secret
store, not onto a runner, not into an artifact.

**This is a decision, and the alternative was designed and measured.** One
GitHub Actions secret cannot hold this ROM — it is 262,160 bytes, its
base64 is 349,548 bytes, and a secret is capped at 48 KB (seven times over;
even xz-compressed and base64'd it is 118,060 bytes, still 2.4 times over).
So it could have gone into the secret store as **eight parts**, reassembled
by the CI job and checked against the cartridge's SHA-1 before the build
read it. That route was built and tested, and it was **not taken**, for
reasons that are about the copyright rather than the engineering:

- A secret store puts a copy of a copyrighted cartridge on a third party's
  servers, in the repository of a **public** project, and a workflow's log
  is readable by any logged-in GitHub user. Secret redaction is by exact
  string match, so a *partial* print of a part would not be masked. A
  rights holder's real objection is to a copy of the game travelling, and
  this is the one place in the project where one would.
- The private-copying exception that makes a backup of a cartridge you own
  lawful is narrower in the EU and the UK, and the "copy made on the
  internet" question is live for cloud backups. A build that never uploads
  the ROM does not have to rely on it.
- The cross build removed the reason to want CI at all: the Windows
  artifact now builds here too, so the release is built where the ROM
  lives either way.

`release.sh` therefore takes the ROM from `roms/` and checks it is the
right cartridge (**SHA-1 `75dad8b4…`**, the whole file) before it builds
anything. A wrong ROM would produce an artifact that is not this project.

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

**`flatpak-builder` may be a program or the `org.flatpak.Builder` flatpak**,
and `release.sh` finds either:

```bash
flatpak install --user flathub org.flatpak.Builder
flatpak run org.flatpak.Builder --user --install --force-clean ...
```

The Builder flatpak's command *is* `flatpak-builder`, so the arguments pass
straight through — do not put another `flatpak-builder` in front of them. It
becomes a stray positional argument, and flatpak-builder then reads the
*build directory* as the manifest and fails with *"Is a directory"*.

It uses the same `AppRun` script, with the payload at `/app/bin` — which is
read-only, exactly like the AppImage's — so the game runs from a writable
copy in the app's data directory. The ROM is a build-time source (the
manifest points at `roms/`), and the app ships without it: the installed
payload is the binary, the assets, the mod manifests, the cheat file and the
licences, and **no `.nes`** (measured — `release.sh` looks).

The manifest's `command:` is a **bare name**: flatpak-builder resolves it
inside `/app/bin`, so `command: AppRun` finds `/app/bin/AppRun` and
`command: bin/AppRun` does not.

Building the Flatpak is also the **strictest compiler gate in the project**:
the SDK's GCC is newer than a typical desktop's, and it rejected a callback
signature that every local compiler had only warned about.

## The release

It is built and published from here — the SMB and Zelda model. The reasons
are in "The ROM never leaves this machine" above: a CI build would need the
ROM in GitHub's secret store, and the cross build means the Windows artifact
builds here anyway.

The four artifacts:

| Artifact | Platform | Dependencies |
|---|---|---|
| `super-dodge-ball-recomp-<v>-linux-x86_64.tar.gz` | Linux x86-64 | SDL2 and OpenGL |
| `super-dodge-ball-recomp-<v>-linux-x86_64.AppImage` | Linux x86-64 | none |
| `super-dodge-ball-recomp-<v>-windows-x64.zip` | Windows x64 | none |
| `super-dodge-ball-recomp-<v>.flatpak` | Linux x86-64 | none |

The tarball is the plain build; the AppImage bundles SDL2 and the other
libraries, so it runs on a machine that has nothing installed. The Windows
ZIP carries SDL2.dll and is statically linked against the mingw runtime.

### What `release.sh` does, and what each step is for

| step | what it is for |
|---|---|
| **preflight** | the tree is **clean**, the tag agrees with `VERSION`, the ROM is the right cartridge, the cross compiler is installed, `gh` is authenticated |
| **build** | Linux, then the Windows cross build, then the AppImage and the Flatpak |
| **gate** | the Windows build's **five gates**, before the ZIP is made |
| **package** | the tarball and the ZIP, each through the guard that refuses a ROM |
| **verify** | HEAD has not moved and the tree is still clean, so the artifacts are this commit; and **no artifact carries a ROM** |
| **notes** | `release/RELEASE-NOTES.md`, with the artifact table and the checksums |
| **publish** | the tag, the release and the assets — and then a check that the release really has the four assets |

**The ROM check looks inside all four artifacts**, not just the two
archives: the tarball and the ZIP are listed, the AppImage is
**extracted** (`--appimage-extract` reads the payload without FUSE, so it
works on a machine with no FUSE), and the Flatpak's payload directory is
listed. **A check that read nothing is not a check that passed** — the
script dies if an extraction fails or a listing comes back empty, rather
than reporting a clean pass it did not measure. (That is the same rule the
project's other gates follow, and it is there because a gate that measures
nothing is the failure mode that keeps coming back.)

**The clean-tree rule is the one that matters.** The build bakes the commit
it came from into the binary (`NESRECOMP_BUILD_REVISION`) and prints it in
the startup log, so an artifact built from a dirty tree names a commit that
is not what you shipped. `release.sh` refuses to run on a dirty tree, and
refuses to publish if HEAD moved while it was building.

**The Flatpak needs `flatpak-builder`**, which may be a program or the
`org.flatpak.Builder` flatpak — the script finds either, and tells you how
to get one if there is neither. `--skip-flatpak` leaves it out, and says
so in the release notes.

**Publishing is idempotent.** If the release already exists, its assets
are replaced (`--clobber`) and its notes rewritten, rather than the
release being deleted — which would take its URL and its discussion with
it.

**Without `--publish` nothing leaves the machine.** The script builds,
verifies, writes `release/RELEASE-NOTES.md` and prints the command that
would publish it, so the notes can be read before they are used. The
release notes name every artifact and carry its SHA-256, and they say so
when the Windows build was **not** gated (`--skip-gate`).

## Before you publish

`release.sh` does all of these, and stops if any of them fails. They are
listed here because they are the things that have to be true of a release,
not because they are steps you have to remember.

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
   (`NESRECOMP_GAME_VERSION`) and printed in the startup log. The tag is
   derived from it, so the two cannot disagree.
6. The tree is clean and HEAD has not moved, so the commit the binary
   names is the commit the release is tagged with.

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
