# Super Dodgeball NES Recompilation

## Maintaining This File

- Everything here is a present-tense directive, constraint, or fact an agent needs *today*.
- Do NOT add changelog entries, "previously X, now Y" framing, or bug postmortems.
- When a bug is fixed, encode the lesson as a rule below, then delete the narrative.
- Keep entries concise. Prefer "Do X" / "Never do Y" over paragraphs.
- If a pitfall was a one-time infrastructure fix with no lasting constraint, remove it entirely.
- **Ask before installing packages, deleting venv, or committing.** Never assume.
- Use `todowrite` at the start of every session to track what's done and what's pending. Mark items complete as you verify them. Commit when a todo item is verified complete, not when everything feels done.
- Don't re-run tests that already passed — same binary + same inputs = same results.
- Check `git status` once at session start, then work from the known state.
- If a test times out, try a smaller version or accept the partial result. Don't retry with a longer timeout on the same approach.
- Don't re-read files you already read this session — you know the content, move on.

## Overview

NESRecomp recompiles NES 6502 → native C via a two-part system:

- **Recompiler** (`external/nesrecomp/recompiler/`): Per-instruction codegen (`cyc_codegen.c`) — one labeled C block per ROM instruction with every CPU cycle's bus activity spelled out, entered per (PRG bank, CPU address).
- **Cycle backend** (`external/nesrecomp/runner/cyc/`): Cycle-accurate console model (6502, PPU, APU, DMAs, mappers in `hw_*.c`) + host (`cyc_host.c`: input schedules, screenshots, frame-logs, TCP). Built per game via the project integration into `build-cycle/nes_game`.
- **`src/game.toml`**: Recompiler config — display `name` (window title) plus discovery/seed keys when they exist. The stock ROM needs no function entries.
- **`build-cycle/`**: Generated per-instruction C (gitignored, one revision dir per ROM/config) + `nes_game` runner. No game-specific hooks — raw ROM on accurate timing.

Build: `./build_and_run.sh` (windowed) or pass host flags (`--frames`, `--input`, `--shot-every`) for headless scripted runs. Game config changes re-trigger codegen at configure time (each revision gets its own generated dir).

### ROM Specifications
- **ROM**: `roms/Super Dodge Ball (USA).nes` — iNES 2.0 format
- **Mapper**: 1 (MMC1 / NES-SLROM PCB, MMC1B2 variant)
- **PRG ROM**: 128 KB (8 × 16KB banks) · **CHR ROM**: 128 KB (16 × 8KB banks)
- **Mirroring**: Horizontal · **TV System**: NTSC · **Battery**: None
- **PRG CRC32**: `689971F9` · **SHA-1**: `42f954e9bd3256c011aba14c7e5b400abe35fde3`
- **File size**: 262,160 bytes (16-byte header + 262,144 data)

### Key References
- **`docs/research.md`** — ROM specs, NESRecomp framework details, sprite flickering analysis, Zelda/SMB/MesenCE research, input-frontier dossier (§18 pre-fix, §19 resolution)
- **`docs/plan.md`** — Consolidated project plan and todo list (merged from plan.md + todo.md)
- **`README.md`** — Project overview, MesenCE installation options, frame comparison pipeline, TAS differential harness
- **RetroPortingToolKit Docs**: https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs

## Setup & Build

```bash
./build_and_run.sh                                    # windowed, play it
./build_and_run.sh --frames 400 --input tas/cyc400.input --screenshot out.png
```

```bash
./setup.sh    # Handles venv, submodule update, MesenCE, blargg test-ROM checkout, cycle backend build
```

- **Venv**: `venv/` with `nesasm` 0.0.8 — use `venv/bin/python3` for nesasm work only. Venv pip is broken (no installs possible).
- **System `python3`** has Pillow + numpy — use it for `compare_frames.py` and frame analysis.
- **Never recreate `venv/`** — it is pre-configured
- **Never `pip install`, `apt install`, or run any package installer without asking first**
- **New packages go in `pyproject.toml` first** — but venv pip is currently broken, so verify installs work before depending on them
- **Never commit without asking** — always present what changed and ask

### MesenCE installation
Three options (setup.sh uses Option C by default):
- **Download pre-built** binary from MesenCE releases → extract to `external/mesence/Mesen`
- **Build from source**: `cd external/mesence && USE_GCC=true make -j$(nproc)`
- **`./setup.sh`** handles everything (clones submodule, builds MesenCE, builds runner)

### Build scripts
- `./setup.sh` — Full setup (venv, submodules, MesenCE, runner, cycle backend)
- `./build_and_run.sh` — Quick build + run on the cycle backend (windowed; pass `--frames`/`--input`/`--screenshot` for headless scripted runs)

## Running

```bash
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" [--frames N] [--input schedule] [--shot-every N --screenshot shot.png] [--mem-frame N --mem-out mem.txt] [--miss-log seeds.txt] [--align 0-3]
```

- **Interactive**: bare run — window opens (`[game] name` is the title), keyboard/gamepad input works
- **Headless**: any of `--frames`/`--input`/`--screenshot` implies headless — no display needed
- **Deterministic**: always pass `--ram-init zeros` for comparable runs (default pattern RAM otherwise)
- **ROM** must be at `roms/Super Dodge Ball (USA).nes` (gitignored)
- **Snapshots**: `--frame-log-at mesen --frame-log frames.bin` writes per-frame CYCFRAME-v2 snapshots (CPU RAM, CIRAM, palette indices, OAM, picture indices) for the TAS diff

## Critical Rules

### Never do these without asking the user first:

1. Never install packages (apt, pip, snap, flatpak) without explicit approval
2. Never recreate `venv/`
3. Never run `sudo apt-get install` without asking
4. Never commit without asking — always present what changed first
5. Never remove pip packages without asking
6. Never download .deb files or extract packages without asking
7. Never modify generated C code in `generated/` directly — regenerate via NESRecomp
8. **Never run `git checkout HEAD -- <file>` or `git restore <file>` on modified files** — always back up to `/tmp/` first (e.g., `cp <file> /tmp/<file>.bak`)
9. **Never revert working changes without asking** — `git checkout`, `git restore`, and `git stash` all destroy uncommitted work
10. Never embed ROM-derived bytes in tracked source — no tile/CHR/nametable/attribute/palette arrays, no `.db` dumps, no multi-line disassembly excerpts. Addresses, sizes, and hashes are facts and fine; bulk bytes are not.

### System state:

- **MesenCE binary is prebuilt** at `external/mesence/Mesen` — no build needed. xvfb-run + isolated HOME with `settings.json` for headless `--testRunner`.
- **Available Python packages**: venv holds `nesasm` 0.0.8 only (pip broken — `pyproject.toml` entries like capstone/py65 never installed). System `python3` has Pillow + numpy.
- Build output is in `build-cycle/` (gitignored; windowed SDL build — the vendored SDL2 ships Windows libs only, so configure passes `-DSDL2_DIR=/usr/lib/x86_64-linux-gnu/cmake/SDL2`; see `build_and_run.sh`)
- ROM files are gitignored and must be placed in `roms/` manually
- **MesenCE is the oracle** for this project. Frame reference generation uses MesenCE binary directly with `--testRunner` mode and Lua API (`emu.getScreenBuffer()`). Co-simulation compares cycle-backend state (`--frame-log`) and screenshots against MesenCE references.

### Code and build constraints:

- `external/nesrecomp` is a git submodule — changes there need a separate `git commit`
- `external/mesence` is a git submodule — the MesenCE emulator source, requires .NET 10 SDK + SDL2 dev
- Generated C code (`generated/`) is gitignored — do not edit directly
- `make clean && USE_GCC=true make -j$(nproc)` builds the entire MesenCE with UI
- Use venv Python (`venv/bin/python3`) for nesasm work, system `python3` for Pillow/numpy scripts — never mix them up
- `src/` ships config + hashes only (currently just `game.toml`). Game bytes come from the user's ROM at build/run time: the cycle project reads the ROM at configure time (identity-checked) and the binary takes the ROM path at launch. Precedents: `tools/rom_to_asm.py`, `tools/frame_gen/oracle_dump.lua`
- Debug byte dumps belong in `tmp/` (gitignored scratch), never in `src/`. Confirm offsets with `tools/dis65.py` plus a python byte-compare against the ROM before coding
- Pre-commit self-check: `git status --short` and `git ls-files | grep -Ei '\.(nes|png|ppm|bin)$'` must show no game-derived blobs; any new `static const uint8_t …[]` in `src/` needs ROM-provenance justification or gets rejected
- Every third-party addition ships with provenance + license + posture, recorded at add time (never as later cleanup): who made it, under what license (or "no license file found"), and how redistribution is avoided — submodule pointer (own license, never vendored), gitignored + user-fetched (test ROMs, oracle binaries), or credited test fixture (vectors keep in-file + README author credit, link pages instead of re-hosting). Code/deps go in `NOTICE.md`; test data goes in the nearest README
- Installed packages (pip/apt) are not distributed — no attribution needed. Code you write from specs (parsers, harnesses) is yours. Data files you didn't create keep their credits intact — never strip author info
- **nesrecomp submodule tracks pristine `origin/master`** (currently `3ef948c` — the pin must stay fetchable upstream; never commit in the submodule, never work with a dirty tree: `git -C external/nesrecomp status` must be clean or the dirt is yours to explain); local runner experiments go upstream or they don't happen — no `patches/` anymore (retired, see `patches/README.md`)

## Known Issues (Current State)

### Model Notes (cycle backend behavior, verified)

- Sprite palette entry-0 bytes (`$3F10/$3F14/$3F18/$3F1C`) read stale power-on bytes in snapshots — hardware mirrors them to the backdrop `$3F00`. Masked in the TAS diff; rendering unaffected. Upstream-noted, not ours to fix.
- Uninitialized CIRAM differs at power-on (`--ram-init` covers CPU RAM only); the game fills nametables within frames. Boot-frame diffs are init modeling, not divergence.
- Unseeded runs execute partly on the cycle interpreter (still bit-exact, slower). `--miss-log` seed lines feed future `cycle_seed_file` runs; native share is informational, never a gate.
- Boot staging order differs from Mesen (backdrop-green vs stripped title at file 0); content converges from file 1. Same-index comparison stands — no offset search.
- Sprite flickering uses **Method 2 Software Sprite Removal** (game strips OAM past 8/scanline) — "Remove Sprite Limit" emulator settings are non-functional. Fix requires a game mod (hook the cull routine), not a renderer flag.

### Current Constraints
- `src/game.toml` stays minimal — the stock ROM needs no function entries; future seed files go in `[game] cycle_seed_file` / `cycle_capture_file`
- No `sram_map` (no-battery MMC1 has no SRAM code)
- **nesrecomp submodule tracks pristine `origin/master`** — never work with a dirty tree; `setup.sh` re-checkouts the pin on fresh setups

### Additional Known Issues

- **Frame comparison**: images are informational only (cross-renderer RGB always differs by palette emulation). Same-renderer pairs are strict; cross-renderer pairs are palette-relative. State bytes (`diff_tas.py`) decide.
- **`run_roundtrip.py` is trivial**: It only proves `nesasm` preserves bytes through `.db` directives — NOT that recompilation is correct. See `docs/research.md` §12.

### docs/ Directory

- **`docs/research.md`** — ROM specs, NESRecomp framework, NMI dispatcher/`$0100` map (§13), MesenCE oracle (§16), NMI survey (§17)
- **`docs/plan.md`** — Living project plan and todo list

## NESRecomp Submodule Status

**Tracks pristine `origin/master` (currently `3ef948c`).** The tree must stay clean — verify with `git -C external/nesrecomp status` before blaming the framework.

## Codegen Notes

- One labeled C block per ROM instruction, entered per (PRG bank, CPU address); static jumps/branches compile to `goto`s, `cpu.pc` written only when control leaves compiled code
- Frame boundaries are PPU-driven; `--frame-log-at mesen` aligns snapshot records to Mesen's frame end
- The recompiler also supports `[[mod_function_hook]]` (content-keyed subroutine hooks for future mods) — see `docs/plan.md` roadmap, not current use

## Retired: Legacy Runner (2026-10-02)

The function-level runner (`src/extras.c` hooks, `patches/`, `build/`) is deleted. Its game knowledge (NMI dispatcher, tunnel mechanics, input path) lives on in `docs/research.md` §§18–19 and transfers to mod work. The one artifact proposed upstream is patch 002 (S-latch guard) — see `docs/plan.md`; the rest lapsed. Never re-add game-specific hooks to work around timing again: time it against Mesen, and if the model is wrong, the fix belongs upstream.

## Verification & Testing

### Iterative Testing Procedure

After any `src/game.toml` change (display name, discovery, seeds):

1. **Rebuild**: `cmake --build build-cycle -j$(nproc)` (configure re-triggers codegen; each revision gets its own generated dir)
2. **TAS**: `python3 tools/tas/run_tas.py --tas tas/sdb_4976.tas.json --frames 400 --out tas/runs/rXXX` + `diff_tas.py` — liveness 400/400 exit 0, state MATCH (mirrors masked)
3. **FAIL criteria**: nonzero exit, logged frames != requested, any state diff outside the masked mirrors

**Known baseline**: cycle 400 TAS frames → team-select, state byte-exact vs Mesen (RAM/OAM/palette/CIRAM); blargg cpu tier 25/25.

### Engine-change procedure (submodule bumps)

Game checks can't isolate engine regressions, so engine changes get both gates:

1. **Game gate**: TAS 400 + diff MATCH per above (must stay green).
2. **Engine gate**: `python3 tools/blargg/run_blargg.py --tier cpu` — 25/25 (20 PASS + 5 screen-confirmed) required; any flip blocks. See `tools/blargg/README.md`.

### Round-trip decompilation (ROM → Assembly → ROM)

The current `run_roundtrip.py` uses `nesasm` to convert ROM bytes to `.db` directives and reassemble. This is a trivial byte-preservation check — it proves the assembler works, NOT that the recompilation is correct. See `docs/research.md` section 12 for proper verification approaches.

### Verification approach:

The project uses **co-simulation** against MesenCE as the primary verification method:
- Compare cycle state snapshots (`--frame-log`: RAM, CIRAM, palette indices, OAM) byte-exact against MesenCE bins per video-frame (`diff_tas.py`; mirrors masked)
- Compare screenshots tolerance-based (`tools/compare_frames.py`): exact RGB differs by palette emulation, structural diff is the metric, state bytes decide
- Disassemble with `tools/dis65.py` (venv pip is broken — no capstone/py65)

**C→x86→ROM comparison is unsound** — the recompiled C compiles to x86-64, making assembly-level comparison to original 6502 code meaningless. No recompilation project uses this approach.

### 6502 disassembly (for analysis, not verification):

```bash
python3 tools/dis65.py "roms/Super Dodge Ball (USA).nes" 831C 4 $((16 + 0*16384 + 0x831C - 0x8000))
# args: ROM file, hex CPU addr, instruction count, file offset of addr
```

Official opcodes only, linear sweep (no resync after data). System `python3` (venv lacks PIL needed for frame work).

### Build & run verification:

```bash
# Rebuild cycle backend
cmake --build build-cycle -j$(nproc)

# Headless scripted run with screenshots + memory snapshot
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 100 \
  --shot-every 1 --screenshot cyc_shots/shot.png --mem-frame 99 --mem-out mem.txt
```

### Frame comparison pipeline:

```bash
# Generate reference frames with MesenCE
./tools/frame_gen/run_frames.sh "roms/Super Dodge Ball (USA).nes" 100 nes_reference

# Generate cycle-backend frames (shot_00000.png scheme; rename to frame_XXXX.png first)
./build_and_run.sh --frames 100 --shot-every 1 --screenshot cyc_shots/shot.png

# Compare
python3 tools/compare_frames.py --ref nes_reference --recomp cyc_shots --frames 100
```

**Critical: Frame 11 is the title screen.** Frame 0 is blank. Pixels never match exactly across renderers (palette emulation differs) — compare structure tolerance-based, and let state bytes (`diff_tas.py`: RAM/OAM/palette/CIRAM byte-exact) decide. Do NOT cite frame 0 as success.

Current: cycle state matches Mesen byte-exact from boot through team-select (400 TAS frames); title pixels are structurally the same screen in both renderers.

### MesenCE testrunner (Lua API — MesenCE 2.2.1):

The MesenCE 2.2.1 Lua API is completely different from older versions. **Never use `emu.frame()`, `video.exportFrame()`, or `input.set()`** — these do not exist. (`os.getenv` works and is used for script parametrization.)

Correct API:
- `emu.addEventCallback(func, emu.eventType.endFrame)` — register callback
- `emu.getScreenBuffer()` — returns table of RGB integers
- `emu.getScreenSize()` — returns `{width, height}`
- `emu.setInput(subport, port, table)` — set controller input
- `emu.stop()` — stops emulation
- **`emu.resume()` and `emu.reset()` crash with exit code 255** in --testrunner mode

When launched with `--testRunner rom.lua`, MesenCE runs frames automatically. The Lua script only registers callbacks — do NOT call `emu.resume()` or `emu.step()`.

Required `settings.json` for headless operation:
- `EnableTestMode: true`
- `Debug.ScriptWindow.AllowIoOsAccess: true`
- `Debug.ScriptWindow.ScriptTimeout: 60`
- `Debugger.BreakOnCpuCrash: false`
- **Remove `mesen.lock`** from `external/mesence/` before running

### MesenCE as Oracle

Frame reference generation uses the **prebuilt MesenCE binary** (`external/mesence/Mesen`) with `--testRunner` mode and a Lua script (`tools/frame_gen/export_frames.lua`). Deeper oracle work (PPU dumps, per-NMI flag logs) uses `tools/frame_gen/oracle_dump.lua` — see `docs/research.md` §16. Headless runs need `xvfb-run` plus an isolated HOME with `settings.json`.

### Known limitations:
- The NESRecomp toolchain is one-way (ROM → C). No built-in way to compile C back to ROM.
- `nesasm` works via Python API only; CLI `nesasm asm` has a `.db` directive bug
- MesenCE testrunner exits with code 255 when `emu.resume()` or `emu.reset()` is called

## RetroPortingToolKit Rules

Reference: https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs

### House Invariants (from 05_agents/02_house-invariants):
1. **No stubs** — Never create stub functions that return default values
2. **Fix the tool, not generated code** — If generated code is wrong, fix the recompiler or game.toml config
3. **Find first divergence** — When a port breaks, find the FIRST frame where it differs from the oracle
4. **Classify the layer** — Is the divergence in codegen, runner, config, or game logic?
5. **Fix the source** — Fix the root cause, not the symptom
6. **Dispatch misses are blocking** — Every `call_by_address` target must be a recompiled function
7. **Never edit generated/ directly** — Always regenerate via NESRecomp
8. **Align on hardware events** — Prefer VBlank, DMA completion, timer overflow over frame numbers
9. **Use always-on traces** — Ring buffers recording before the bug happened
10. **Unknown is allowed** — Guessing is worse than saying "unknown"

### Verification Rituals (from 05_agents/03-verification-rituals):
- Dispatch misses must be zero
- Co-sim must match (RAM state at checkpoints)
- TCP checks must pass
- Frame hashes must match
- A green build is weak evidence
- Use hardware events, not frame numbers

### Failure Modes (from 05_agents/04-failure-modes):
1. **Stubbing** — Creating fake functions instead of fixing the real issue
2. **Wrong fix** — Fixing the symptom, not the cause
3. **Ignoring first divergence** — Not finding the first frame where things break
4. **Overfitting** — Making changes that work for one frame but break others
5. **Config blindness** — Not checking game.toml for missing directives
6. **Generated code editing** — Editing generated/ files directly (they get overwritten)
7. **Submodule staleness** — Not keeping the nesrecomp submodule up to date
8. **Bank switching bugs** — Overriding MMC1 bit-bang with wrong dispatch override
9. **NMI handler issues** — Calling RTI-based functions as C functions
10. **Input not wired** — Not connecting runner input to game controller reads

## Configuration Files & Locations

| Path | Purpose |
|------|---------|
| `src/game.toml` | Recompiler config: display `name`, discovery/seed keys (stock ROM needs no function entries) |
| `external/nesrecomp/` | NESRecomp submodule (recompiler + runners) — **pristine origin/master, tree must stay clean** |
| `external/mesence/` | MesenCE emulator source/binary |
| `patches/` | RETIRED legacy patches, all in `archived/` (see `patches/README.md`) — no new patches, ever |
| `tools/dis65.py` | Dependency-free 6502 disassembler for entry verification (ROM + addr + count + file offset) |
| `tools/frame_gen/memwatch.lua` | MesenCE oracle probe: who-writes (with PC) for RAM addrs + optional NMI flag log (`WATCH`, `MAXF`, `START0/1`, `NMI_LOG`) |
| `tools/frame_gen/oracle_dump.lua` | MesenCE `--testrunner` oracle: frame-N PPU dump + per-NMI `$0100/$0106` log (see `docs/research.md` §16) |
| `tools/frame_gen/` | Frame export Lua script and wrapper |
| `tools/blargg/` | blargg test-ROM harness on the cycle backend (`run_blargg.py` + `manifest.json`); per-ROM builds in `build-cyc-blargg/` (gitignored), ROMs via `--fetch` into `external/nes-test-roms/` (gitignored) |
| `tools/tas/` | TAS differential harness on the cycle backend (bk2/fm2 converters, `run_tas.py` + `diff_tas.py`, `mesen_tas.lua`); vectors in `tas/vectors/`, runs in `tas/runs/` (gitignored) — see `tools/tas/README.md`, research §21 |
| `build-cycle/` | Cycle backend build (gitignored; `./setup.sh` builds it, `./build_and_run.sh` runs it) |
| `config/settings.json` | MesenCE runtime configuration (gitignored) |
| `pyproject.toml` | Python project configuration |
| `roms/` | ROM files (gitignored) |
| `venv/` | Python virtual environment |
| `build/` | Build output (gitignored) |
| `config/` | MesenCE settings (gitignored) |

## Keybinds

- Player 1: A=Z, B=X, Select=\, Start=Return, Up=Up, Down=Down, Left=Left, Right=Right
- Gamepad 1: A=a,b; B=x,y; Select=back; Start=start; Up/Down/Left/Right=d-pad

## MesenCE Lua API Quick Reference

### Memory
- `emu.read(addr, memType, debug)` — read bytes
- `emu.write(addr, value, memType)` — write bytes
- `emu.read16(addr, memType, debug)` — read 16-bit
- `emu.write16(addr, value, memType)` — write 16-bit
- `emu.read32(addr, memType, debug)` — read 32-bit
- `emu.write32(addr, value, memType)` — write 32-bit
- `emu.convertAddress(addr, memType)` — convert address
- `emu.getLabelAddress(label)` — get label address
- `emu.getMemorySize(memType)` — get memory size

### CPU
- `emu.getCpuState(cpuType)` — get registers
- `emu.setCpuState(state, cpuType)` — set registers
- `emu.getCpuCycleCount()` — get cycle count
- `emu.getMasterClock()` — get master clock

### Events/Callbacks
- `emu.addEventCallback(func, eventType)` — register event callback
- `emu.removeEventCallback(ref, eventType)` — remove callback
- `emu.addMemoryCallback(func, callbackType, addr1, addr2, cpuType, memType)` — memory watch/breakpoint
- `emu.removeMemoryCallback(ref, callbackType, addr1, addr2, cpuType, memType)` — remove watch

### Video
- `emu.getScreenSize()` — returns `{width, height}`
- `emu.getScreenBuffer()` — returns table of RGB integers
- `emu.setScreenBuffer(buf)` — set screen buffer
- `emu.getPixel(x, y)` — get pixel color
- `emu.takeScreenshot(path)` — save screenshot

### Input
- `emu.setInput(buttons, port, subport)` — set controller input
- `emu.getInput(port, subport)` — get current input
- `emu.isKeyPressed(key)` — check key
- `emu.getPressedKeys()` — get all pressed keys

### State
- `emu.getState()` — full serialized console state
- `emu.setState(state)` — restore state
- `emu.createSavestate(path)` — create save state
- `emu.loadSavestate(path)` — load save state
- `emu.reset()` — **crashes in --testrunner mode (exit 255)**
- `emu.stop(reason)` — stop emulation
- `emu.breakExecution()` — pause execution
- `emu.resume()` — **crashes in --testrunner mode (exit 255)**
- `emu.step(stepType)` — step one instruction

### Debug
- `emu.getCdlData(memType)` — Code/Data Logger coverage
- `emu.getAccessCounters()` — get access counters
- `emu.resetAccessCounters()` — reset counters
- `emu.getRomInfo()` — ROM metadata
- `emu.getScriptDataFolder()` — script folder path

### Event Types
- `emu.eventType.inputPolled` — input poll time (safe for `emu.setInput`)
- `emu.eventType.startFrame` / `emu.eventType.endFrame`
- `emu.eventType.Nmi` / `emu.eventType.Irq` / `emu.eventType.Reset`
- `emu.eventType.StateLoaded` / `emu.eventType.StateSaved`
- `emu.eventType.CodeBreak` / `emu.eventType.HaltStarted` / `emu.eventType.HaltEnded`

### Memory Types (NES)
- `emu.memType.nesInternalRam` — $0000-$07FF
- `emu.memType.nesPrgRom` — PRG ROM
- `emu.memType.nesChrRom` — CHR ROM
- `emu.memType.nesNametableRam` — $2000-$2FFF
- `emu.memType.nesPaletteRam` — $3F00-$3F1F
- `emu.memType.nesSpriteRam` — $0000-$00FF (OAM)
- `emu.memType.nesSecondarySpriteRam` — secondary sprite RAM

### CPU Types
- `emu.cpuType.nes` — NES 6502

### Settings for Headless
- `EnableTestMode: true`
- `Debug.ScriptWindow.AllowIoOsAccess: true`
- `Debug.ScriptWindow.AllowNetworkAccess: true`
- `Debug.ScriptWindow.ScriptTimeout: 60`
- `Nes.RamPowerOnState: 1` (AllZeros)
- `Nes.RandomizeMapperPowerOnState: false`
- `Nes.RandomizeCpuPpuAlignment: false`
- Remove `mesen.lock` from `external/mesence/` before running