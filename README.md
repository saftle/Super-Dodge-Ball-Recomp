# Super Dodgeball NES — Recompilation

Recompile **Super Dodge Ball (USA)** (NES, 1989, Technos Japan) to native C using the [NESRecomp](https://github.com/mstan/nesrecomp) framework and run it on PC.

![Super Dodge Ball title screen (recompiled build)](docs/assets/title.png)

> **Status:** playable — it boots to the title screen, plays through team-select verified byte-exact vs Mesen (state, not just pixels), and plays full matches end-to-end (throws land, damage digits show, HUD/menus render). Not yet tested on a full playthrough to completion, and there is one known mod bug below. See `docs/plan.md` for the frontier.

> *Built with [NESRecomp](https://github.com/mstan/nesrecomp) by Matthew Stanley — framework questions belong upstream, game-specific issues belong here. This port is in development — expect rough edges.*

---

## Quick Start

```bash
# Full setup (venv, submodules, MesenCE, cycle backend, blargg test ROMs)
./setup.sh

# Or build and run in one go (window opens on the launcher — press Play)
./build_and_run.sh

# Headless scripted run (input schedule + last-frame screenshot)
./build_and_run.sh --frames 400 --input tas/vectors/sdb_idle.input --screenshot out.png

# Per-frame screenshots + memory snapshot at frame N (differential checks)
./build-cycle/nes_game "rom.nes" --frames 400 --input tas/vectors/sdb_idle.input \
  --shot-every 10 --screenshot shots/shot.png --mem-frame 399 --mem-out mem.txt
```

## The launcher

The window opens on **[recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui)'s** pre-boot
launcher: a settings dashboard where you set display, audio and per-player bindings before the
game starts. Press **Play** to boot the ROM, or **Escape** to skip straight in.

Once the game is running, **Escape** opens the in-game menu — display and audio, save states,
and quit. (The same key does both, because the launcher only exists before boot.)

Settings and bindings persist to `build-cycle/config.ini` beside the executable. Keybinds are
SDL key names; defaults are P1 = `Z`/`X`/`\`/`Return` + arrows, gamepad = `a`/`x`/`back`/`start`
+ d-pad. Delete `config.ini` to restore defaults.

The launcher is built from the pinned `external/recomp-ui/` submodule — nothing game-specific lives in
this repo. To build the window without it (launcher and in-game menu both off):

```bash
NESRECOMP_RECOMP_UI=OFF ./build_and_run.sh
```

See `NOTICE.md` for attribution.

## Mods

The launcher has a **Mods** screen. Two ship with the build, both **off by default**:

- **Remove Character Flicker** — the original release flickered badly, three ways, none of them
  the NES 8-sprites-per-line limit that emulator "remove sprite limit" options target; this game
  throttles its own sprites, so no emulator setting can help:

  1. **Truncation** — OAM is built in place behind a cursor and the character emitter *stops*
     once OAM's 64 slots are full, so the rest of a character is never computed. This mod keeps
     emission going and draws every sprite the game produces, which also lifts the NES
     8-per-scanline limit.
  2. **Culling** — the game builds a display list each frame and deliberately leaves an object off
     it, rotating which object, so a whole character blinks out for 4 frames in 9. Three gates on
     that path are overridden; see `docs/research.md` §2.1.
  3. **Orphan emitters** — the floating damage numbers and the team-select position numbers are
     drawn by emitters with no cursor bump for a hook to sit on, so they were never captured.
     Both are hooked per-emitter now (`docs/research.md` §2.1).

Game logic runs stock either way — nothing reads sprite OAM back, and sprite slot 0 never differs.
The truncation half is presentation only (identical cycle count, 2 bytes of the OAM shadow page
differ); the culling half is not, because making the game emit an object it had dropped *is* extra
guest work — the picture itself is byte-identical on 899 of 910 frames. The feature is off
unless you turn it on in the Mods screen (or pass `--remove-character-flicker` headless);
with it off the output is byte-identical to the unmodded game.

> **Known issue:** with the mod on, the level-switch screen (after winning a match) briefly
> shows white ghost sprites. Under investigation — this is why the mod stays off by default.

- **PPU Overclocking** — the game feels slower than it should, because its main loop is
  an NMI-synced spin: it parks waiting for the next NMI, so the guest only gets 262
  scanlines of CPU per frame. This adds **blank scanlines before the NMI**, so the guest
  completes more of its own work each frame and the game runs faster. Nothing new is
  drawn — the added lines render nothing and audio pauses across them, so the picture
  stays 256×240; the effect is purely extra CPU time. Same mechanism as MesenCE's
  *Additional scanlines before NMI* and FCEUX's post-render scanlines. The Mods screen
  exposes the count (0–262; 262 roughly doubles the per-frame CPU budget).

  **This deliberately changes console timing, so it is off by default and is not
  hardware-accurate** — leave it off if you want original behaviour.

  Headless:

  ```bash
  ./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 800 \
    --input tas/vectors/sdb_throw.input --ram-init zeros --ppu-overclock 262
  ```

  It needs `patches/006_ppu_overclock_mmc1` (applied by `setup.sh`); without it the
  backend refuses the setting on an MMC1 cartridge. Measured numbers and the
  three-emulator comparison are in `docs/research.md` §23.

For headless runs the catalog is only read when you point at it, and the feature is
off unless you ask for it:

```bash
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 470 \
  --input <schedule> --ram-init zeros --mods-root build-cycle/mods \
  --remove-character-flicker \
  --present-out out.png --present-every 1
```

`--present-out` writes what the *window* would show, which is the only path that exercises the mod —
`--screenshot` and `--frame-log` capture the machine's native frame and ignore it.

## Display

The launcher's **Graphics** section has a **VSync** row. With it on, each present
syncs to the display's refresh, so motion lands evenly at 60 Hz, 120 Hz or
anything else. **Presentation only**: the guest keeps its stock clock either way.
(Adaptive in that row behaves the same as On.) Needs `patches/007_window_vsync`
(applied by `setup.sh`). See `docs/research.md` §24.

## Getting MesenCE (for frame comparison)

The recompiled game's frame output can be compared against an accurate NES emulator reference. **MesenCE** (`nesdev-org/MesenCE`) is used as the reference renderer. Three ways to get it:

### Option A: Download pre-built binary (easiest)

```bash
wget -O /tmp/mesen.zip https://github.com/nesdev-org/MesenCE/releases/download/2.2.1/Mesen_2.2.1_Linux_x64.zip
unzip /tmp/mesen.zip -d /tmp/mesen_extract
cp /tmp/mesen_extract/Mesen external/mesence/Mesen
```

Fastest way to get a working Mesen binary. No build tools or .NET SDK required.

### Option B: Build from source

```bash
# Prerequisites: g++/clang++, SDL2 dev, make, .NET SDK
cd external/mesence
USE_GCC=true make -j$(nproc)
```

`setup.sh` does this automatically if the binary doesn't already exist.

**Dependencies:** C++17 compiler, SDL2 development headers, make, and a .NET SDK (8.0 available from apt; 10.0 requires `ppa:dotnet/backports`).

### Option C: Let setup.sh handle it

```bash
./setup.sh
```

Clones `external/mesence` from GitHub, builds with `USE_GCC=true make -j$(nproc)`, and copies the binary to `external/mesence/Mesen`. If the binary already exists, it skips the build.

## Controls

| NES Button | Keyboard | Gamepad 1 |
|---|---|---|
| A | Z | a |
| B | X | y |
| Select | \ | back |
| Start | Return | start |
| Up | Up | d-pad Up |
| Down | Down | d-pad Down |
| Left | Left | d-pad Left |
| Right | Right | d-pad Right |

## Architecture

Static recompilation, not an emulator. The original 6502 machine code is translated to C at build time — one block per ROM instruction with every CPU cycle's bus activity spelled out — then compiled to native code and run on a cycle-accurate console model (CPU, PPU, APU, DMAs, mappers).

- **`CMakeLists.txt`** — The game project (root). Wraps the submodule's `runner/cyc/project.cmake` and adds `CXX`, `MODS`, `GAME_ID` and `HOST_EXTRAS`, which the submodule's own project file can't express — so the submodule stays pristine.
- **`src/game.toml`** — Recompiler config: display `name` (window title), a `cycle_seed_file`, and ten content-keyed `[[mod_function_hook]]` sites. Hooks are keyed by `crc32`, never by ROM bytes. Fed to the cycle backend at configure time.
- **`src/cycle_seeds.txt`** — Recompiler discovery roots. A mod hook site only exists in *recompiled* code, and the sprite emitters the mod hooks ran on the interpreter; these six lines make them native. Total cycle count unchanged.
- **`src/sdb_host_extras.c`** — Our two mods: the **character-flicker mod** (captures every OAM sprite the game emits and presents them host-side, overriding the game's own object culling — research.md §2.1) and the **PPU-overclocking mod** (picks a value for the backend's extra-scanlines setting — research.md §23).
- **`mods/`** — The mod packages (`manifest.toml` only; `.nesmod` archives can't carry native code). Off by default, toggleable in the launcher's Mods screen.
- **`build-cycle/`** — Cycle-backend build (gitignored): per-instruction C (`game_cyc*.c`, one revision dir per ROM/config) + `nes_game` runner + the preloaded `mods/` catalog.
- **`external/nesrecomp/`** — NESRecomp framework submodule (recompiler + runners)
- **`external/mesence/`** — MesenCE emulator (reference rendering + oracle Lua API)
- **`tools/frame_gen/`** — Mesen frame export Lua script + wrapper
- **`tools/overclock/`** — PPU-overclocking oracle harness (research.md §23)
- **`tools/blargg/`** — blargg test-ROM suite on the cycle backend (`run_blargg.py`, `manifest.json`); per-ROM builds in `build-cyc-blargg/`, ROMs in `external/nes-test-roms/` (both gitignored)
- **`tools/tas/`** — TAS-integrated differential harness: bk2/fm2 → canonical `tas.json`, shared-input runners for MesenCE + cycle backend with full state capture, three-layer first-divergence diff (`run_tas.py`, `diff_tas.py`, `mesen_tas.lua`); vectors in `tas/vectors/`, runs in `tas/runs/` (gitignored)

### Project Structure

```
Super Dodgeball Recomp/
├── roms/                          # NES ROM files (gitignored)
├── src/                           # Project source
│   ├── game.toml                  # Recompiler config (display name + mod hook sites)
│   ├── sdb_host_extras.c          # Character-flicker mod (capture + host-side presentation)
│   └── third_party/               # Local gitignored build scratch (not shipped)
├── mods/                          # Mod package(s), copied beside the executable at build time
│   └── packages/
│       └── super-dodgeball.character-flicker/   # manifest.toml only (data-only by design)
│       └── super-dodgeball.ppu-overclock/       # manifest.toml only (data-only by design)
├── external/nesrecomp/            # NESRecomp submodule
├── external/mesence/              # MesenCE emulator (reference rendering)
├── external/nes-test-roms/        # blargg suite checkout (gitignored, via setup.sh --fetch path)
├── build-cyc-blargg/              # blargg per-ROM cycle builds (gitignored)
├── build-cycle/                   # Cycle-backend build (gitignored)
├── tools/
│   ├── compare_frames.py          # Mesen reference vs cycle screenshot comparison
│   ├── rom_to_asm.py              # ROM → assembly listing helper
│   ├── run_roundtrip.py           # ROM → asm → ROM byte-preservation check
│   ├── dis65.py                   # Dependency-free 6502 disassembler
│   ├── blargg/                    # blargg suite on the cycle backend (run_blargg.py, manifest)
│   ├── tas/                       # TAS differential harness (converters, runners, diff)
│   ├── frame_gen/                 # Mesen frame export + oracle dump + memwatch scripts
│   └── overclock/                 # PPU-overclocking oracle harness (research.md §23)
├── tas/
│   ├── vectors/                   # TAS movies (.bk2 + format samples, committed)
│   └── runs/                      # Per-TAS run artifacts (gitignored)
├── venv/                          # Python virtual environment (gitignored)
├── docs/
│   ├── research.md                # Deep research notes
│   ├── plan.md                    # Project plan
│   └── assets/                    # README images (title screen)
├── LICENSE.md                   # PolyForm Noncommercial 1.0.0
├── NOTICE.md                      # Attribution + no-ROM disclaimer
├── .github/
│   ├── workflows/ci.yml           # ROM-less CI (engine + scripts + config)
│   └── raid-discord.png           # R.A.I.D. invite badge (footer)
├── setup.sh                       # Environment setup (venv + submodules + build)
├── build_and_run.sh               # Quick build + run (cycle backend, windowed)
├── CMakeLists.txt                 # Game project: wraps the submodule's project.cmake + MODS/HOST_EXTRAS
├── README.md
└── AGENTS.md                      # Developer reference for AI coding agents
```

## Frame Comparison Pipeline

Compare cycle-backend output against an accurate NES reference using MesenCE.

```bash
# Step 1: Generate reference frames with MesenCE
./tools/frame_gen/run_frames.sh roms/Super Dodge Ball (USA).nes 100 nes_reference --start

# Step 2: Generate cycle-backend frames with screenshots
./build_and_run.sh --frames 100 --shot-every 1 --screenshot cyc_shots/shot.png

# Step 3: Compare pixel-by-pixel (tolerance-based: exact RGB never matches across renderers)
# (cyc names shots shot_00000.png; rename to the frame_XXXX.png scheme first)
python3 -c "import glob,os; [os.rename(p,'cyc_shots/frame_%04d.png'%int(p.split('_')[-1].split('.')[0])) for p in glob.glob('cyc_shots/shot_*.png')]"
python3 tools/compare_frames.py --ref nes_reference --recomp cyc_shots --frames 100
```

### TAS-integrated differential testing (`tools/tas/`)

Same TAS input drives both the MesenCE oracle and the cycle backend; diffs CPU RAM, CIRAM, palette indices, and OAM byte-exact per video-frame to the first divergence (liveness → state → informational images). Vectors: TASVideos #4976 `.bk2` in `tas/vectors/` (headerless MD5 matches our ROM).

```bash
python3 tools/tas/bk2_to_tas.py tas/vectors/sdb_4976.bk2 tas/sdb_4976.tas.json
python3 tools/tas/run_tas.py --tas tas/sdb_4976.tas.json --frames 400 --out tas/runs/r400
python3 tools/tas/diff_tas.py tas/runs/r400
```

The cycle side runs with an absolute-state input schedule plus `--frame-log-at mesen` binary snapshots (RAM/CIRAM/palette/OAM/picture per frame) and `--shot-every` screenshots. Honesty is sealed: cycle snapshots are byte-identical across repeat runs, and real differences get reported (sprite entry-0 mirror bytes masked — hardware mirrors them to the backdrop, see `diff_tas.py`). Current state: RAM/OAM/palette/CIRAM match from boot; 400 TAS frames reach team-select. See `tools/tas/README.md`.

### Cycle-accurate backend (`build-cycle/`, gitignored)

This is the backend everything runs on: per-cycle 6502/PPU/APU/DMA with own
mappers, via the upstream project integration. `./build_and_run.sh` and
`./setup.sh` target it; `./build-cycle/nes_game` takes the host flags
(`--frames`, `--input` schedule `<frame> NAMES`, `--ram-init zeros`,
`--shot-every`, `--mem-frame`/`--mem-out`, `--miss-log`, `--align 0-3`).

Findings: 400 TAS frames reach team-select, matching Mesen; blargg cpu tier
25/25 green (20 RAM-text PASS incl. `12-jmp_jsr` at 99.4% native, 5
screen-confirmed PASS: branch_1/2/3, dummy_reads, timing_test6).

### Deeper oracle work (PPU dumps, per-NMI flag logs)

```bash
OUT_DIR=oracle_out DUMP_FRAME=11 MAX_FRAMES=13 HOME="$HOME/mesen-home" \
  xvfb-run -a external/mesence/Mesen \
  --testrunner "roms/Super Dodge Ball (USA).nes" \
  tools/frame_gen/oracle_dump.lua --timeout=60
```

Isolated HOME needs a `settings.json` (`AllowIoOsAccess`, `ScriptTimeout`, `RamPowerOnState: 1`); see `docs/research.md` §16. Raw `--testRunner` + Lua only — no MCP/bridge layer.

## ROM Specifications

| Property | Value |
|---|---|
| **ROM** | `roms/Super Dodge Ball (USA).nes` |
| **Format** | iNES 2.0 |
| **Mapper** | 1 (MMC1 / NES-SLROM PCB, MMC1B2) |
| **PRG ROM** | 128 KB (8 × 16KB banks) |
| **CHR ROM** | 128 KB (16 × 8KB banks) |
| **Mirroring** | Horizontal |
| **Battery** | None |
| **TV System** | NTSC |
| **PRG CRC32** | `689971F9` |
| **SHA-1** | `42f954e9bd3256c011aba14c7e5b400abe35fde3` |
| **File size** | 262,160 bytes (16-byte header + 262,144 data) |

## Known Limitations (current state)

- **Menus work to team-select** — 400 TAS frames (TASVideos #4976) reach `CHANGE POSITION?` with rosters and courts rendering; state matches Mesen byte-exact (RAM/OAM/palette/CIRAM).
- **Matches play end-to-end** — scripted throw runs (`tas/vectors/sdb_throw.input`, 8000 frames) play full matches: throws land, damage digits show, HUD and menus render, no hangs. Full playthrough to completion (winning rounds, level-switch screens) is untested.
- **Mod off by default** — the flicker mod needs a full-playthrough test first. Known issue with it on: white ghost sprites on the level-switch screen (under investigation).
- ~~**Sprite flickering**~~ — **fixed, all causes.** Not emulator sprite-limit removal: the game (1) stops emitting once OAM's 64 slots fill, (2) deliberately culls one object off its own display list each frame, rotating which, so a character blinks out 4 frames in 9, and (3) draws its damage numbers and menu digits through emitters no bump hook could see. Characters now stay whole with the opt-in `super-dodgeball.character-flicker` mod — toggleable in the launcher's **Mods** screen. See `docs/research.md` §2 and §2.1.
- **Palette mirrors** — sprite entry-0 bytes (`$3F10/$3F14/$3F18/$3F1C`) read stale in cycle snapshots (upstream model keeps power-on bytes; hardware mirrors the backdrop). Masked in the TAS diff; rendering unaffected.

## Developer Docs

**[AGENTS.md](AGENTS.md)** — Developer reference with present-tense constraints, codegen notes, submodule workflow, MesenCE testrunner API, verification procedures, and current-known issues. The canonical document for AI coding agents.

**[docs/research.md](docs/research.md)** — Deep research: ROM specs, NESRecomp framework, NMI dispatcher/`$0100` map, MesenCE oracle, NMI survey, current build state, PPU overclocking (§23).

## References

- NESRecomp: https://github.com/mstan/nesrecomp
- NESRecomp Discord (R.A.I.D.): `discord.gg/Ad9BwSzctP`
- MesenCE: https://github.com/nesdev-org/MesenCE
- SNES Port (Rumbleminze): https://github.com/rumbleminze/super-super-dodgeball
- Romhacking.net: https://www.romhacking.net/games/814
- StrategyWiki: https://strategywiki.org/wiki/Super_Dodge_Ball_(NES)

## License

PolyForm Noncommercial 1.0.0 — see [LICENSE.md](LICENSE.md), with attribution in [NOTICE.md](NOTICE.md). The pinned `external/nesrecomp` submodule carries its own license. You must supply your own legally obtained ROM; no game content is included.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp & recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
