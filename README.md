# Super Dodgeball NES — Recompilation *(early WIP)*

Recompile **Super Dodge Ball (USA)** (NES, 1989, Technos Japan) to native C using the [NESRecomp](https://github.com/mstan/nesrecomp) framework and run it on PC.

![Super Dodge Ball title screen (recompiled build)](docs/assets/title.png)

> **Status:** super early WIP — it boots to the title screen and plays through team-select verified byte-exact vs Mesen (state, not just pixels), with music correct, and that's the verified show so far. Past team confirm is unverified territory (court BG next); gameplay feel beyond that is untested. See `docs/plan.md` for the frontier.

> *Built with [NESRecomp](https://github.com/mstan/nesrecomp) by Matthew Stanley — framework questions belong upstream, game-specific issues belong here. This port is in development — expect rough edges.*

---

## Quick Start

```bash
# Full setup (venv, submodules, MesenCE, cycle backend, blargg test ROMs)
./setup.sh

# Or build and run in one go (window opens, play it)
./build_and_run.sh

# Headless scripted run (input schedule + last-frame screenshot)
./build_and_run.sh --frames 400 --input tas/cyc400.input --screenshot out.png

# Per-frame screenshots + memory snapshot at frame N (differential checks)
./build-cycle/nes_game "rom.nes" --frames 400 --input tas/cyc400.input \
  --shot-every 10 --screenshot shots/shot.png --mem-frame 399 --mem-out mem.txt
```

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

- **`src/game.toml`** — Recompiler config: display `name` (window title), plus discovery/seed keys when they exist. Fed to the cycle backend at configure time.
- **`build-cycle/`** — Cycle-backend build (gitignored): per-instruction C (`game_cyc*.c`, one revision dir per ROM/config) + `nes_game` runner. No game-specific hooks — the raw ROM runs on accurate hardware timing.
- **`external/nesrecomp/`** — NESRecomp framework submodule (recompiler + runners)
- **`external/mesence/`** — MesenCE emulator (reference rendering + oracle Lua API)
- **`tools/frame_gen/`** — Mesen frame export Lua script + wrapper
- **`tools/blargg/`** — blargg test-ROM suite on the cycle backend (`run_blargg.py`, `manifest.json`); per-ROM builds in `build-cyc-blargg/`, ROMs in `external/nes-test-roms/` (both gitignored)
- **`tools/tas/`** — TAS-integrated differential harness: bk2/fm2 → canonical `tas.json`, shared-input runners for MesenCE + cycle backend with full state capture, three-layer first-divergence diff (`run_tas.py`, `diff_tas.py`, `mesen_tas.lua`); vectors in `tas/vectors/`, runs in `tas/runs/` (gitignored)

### Project Structure

```
Super Dodgeball Recomp/
├── roms/                          # NES ROM files (gitignored)
├── src/                           # Project source
│   ├── game.toml                  # Recompiler config (display name, discovery, mapper)
│   └── third_party/               # Local gitignored build scratch (not shipped)
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
│   └── frame_gen/                 # Mesen frame export + oracle dump + memwatch scripts
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
- **Past team-select is unverified** — the court BG draw is the next frontier; TAS vectors exist for the full run when needed.
- **Sprite flickering** — Method 2 Software Sprite Removal: the game strips sprites from OAM past 8/scanline, so no emulator toggle can restore them. On PC the OAM-culling routine is the future fix target.
- **Palette mirrors** — sprite entry-0 bytes (`$3F10/$3F14/$3F18/$3F1C`) read stale in cycle snapshots (upstream model keeps power-on bytes; hardware mirrors the backdrop). Masked in the TAS diff; rendering unaffected.

## Developer Docs

**[AGENTS.md](AGENTS.md)** — Developer reference with present-tense constraints, codegen notes, submodule workflow, MesenCE testrunner API, verification procedures, and current-known issues. The canonical document for AI coding agents.

**[docs/research.md](docs/research.md)** — Deep research: ROM specs, NESRecomp framework, NMI dispatcher/`$0100` map, MesenCE oracle, NMI survey, current build state.

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
