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

- **Recompiler** (`external/nesrecomp/recompiler/`): Scans ROM bytes for instruction patterns, discovers functions, generates `generated/*.c`
- **Runner** (`external/nesrecomp/runner/`): NES hardware emulation (PPU, APU, mapper, input, save states). Linked into the game executable
- **`src/extras.c`**: Game-specific hooks (`game_on_init`, `game_run_nmi`, `game_post_nmi`, `game_on_frame`)
- **`src/game.toml`**: NESRecomp config — MMC1 mapper, `bank_switch = [0xFF08]` (the MMC1 bit-bang routine, never the `$8000` window), `deduplicate_functions = true`, `disable_ptr_scan = true`, `disable_secondary = true`, minimal `fixed` list, dis65-verified `extra_func` entries (banks 0/1/6), two `[[replace_func]]` (`$FCA0` NMI-sync, `$8393_b6` stack-args tail), empty `[force_interp]`
- **`generated/`**: Auto-generated C code (gitignored). `_full.c` includes all bank parts; `_dispatch.c` has the `call_by_address` dispatch table

The recompilation step: `./external/nesrecomp/recompiler/build/NESRecomp "rom.nes" --game src/game.toml --output-prefix "Super_Dodge_Ball_(USA)"` (run from repo root; always confirm `[Eval] Coverage` shows resolved entries)

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
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

```bash
./setup.sh    # Handles venv, submodule update, MesenCE build, runner build, blargg test-ROM checkout
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
- `./setup.sh` — Full setup (venv, submodules, MesenCE, runner)
- `./build_and_run.sh` — Quick build + run (cmake + super_dodgeball)

## Running

```bash
./build/super_dodgeball "roms/Super Dodge Ball (USA).nes" [--smoke N] [--smoke-interval N]
```

- **Smoke test**: `--smoke 30 --smoke-interval 5` — title renders ~99.8% vs Mesen frame 11, exit 0. Use `--smoke 5` minimum for fix verification (must exceed old 3-frame wall).
- **Interactive**: Run without `--smoke` — SDL2 window opens, gamepad/keyboard input works
- **Headless**: `--smoke N` — no display needed
- **ROM** must be at `roms/Super Dodge Ball (USA).nes` (gitignored)
- **Save screenshots**: `--save-screenshot [output_dir]` — saves `frame_XXXX.png` per frame

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
- Build output is in `build/` (gitignored)
- ROM files are gitignored and must be placed in `roms/` manually
- **MesenCE is the oracle** for this project. Frame reference generation uses MesenCE binary directly with `--testRunner` mode and Lua API (`emu.getScreenBuffer()`). Co-simulation compares recompiled game screenshots against MesenCE reference frames.

### Code and build constraints:

- `external/nesrecomp` is a git submodule — changes there need a separate `git commit`
- `external/mesence` is a git submodule — the MesenCE emulator source, requires .NET 10 SDK + SDL2 dev
- Generated C code (`generated/`) is gitignored — do not edit directly
- `make clean && USE_GCC=true make -j$(nproc)` builds the entire MesenCE with UI
- Use venv Python (`venv/bin/python3`) for nesasm work, system `python3` for Pillow/numpy scripts — never mix them up
- `src/` ships code + config + hashes only. Game bytes come from the user's ROM at build/run time via `g_rom_path_for_extras` (set to `argv[1]` before `game_on_init()`): open it, parse the 16-byte iNES header (skip the 512-B trainer iff header[6] bit 2 is set), read at `prg_base + bank*16384 + offset`. Precedents: `tools/rom_to_asm.py`, `tools/frame_gen/oracle_dump.lua`
- Debug byte dumps belong in `tmp/` (gitignored scratch), never in `src/`. Confirm offsets with `tools/dis65.py` plus a python byte-compare against the ROM before coding
- Pre-commit self-check: `git status --short` and `git ls-files | grep -Ei '\.(nes|png|ppm|bin)$'` must show no game-derived blobs; any new `static const uint8_t …[]` in `src/` needs ROM-provenance justification or gets rejected
- Every third-party addition ships with provenance + license + posture, recorded at add time (never as later cleanup): who made it, under what license (or "no license file found"), and how redistribution is avoided — submodule pointer (own license, never vendored), gitignored + user-fetched (test ROMs, oracle binaries), or credited test fixture (vectors keep in-file + README author credit, link pages instead of re-hosting). Code/deps go in `NOTICE.md`; test data goes in the nearest README
- Installed packages (pip/apt) are not distributed — no attribution needed. Code you write from specs (parsers, harnesses) is yours. Data files you didn't create keep their credits intact — never strip author info
- **nesrecomp submodule tracks pristine `origin/master`** (currently `3ef948c` — the pin must stay fetchable upstream, so patch state is never committed in the submodule); the 4 active patches live only as `patches/*.patch`, applied to the submodule working tree by `setup.sh` — runner experiments go in `patches/` with README notes, never as bare edits

## Known Issues (Current State)

### Critical Bugs

- **Never override the MMC1 bit-bang** — `func_FF08` does serial bank switching, not controller polling. Overriding it (or `force_interp`-ing it) breaks all banked execution and garbles graphics.
- **`game_run_nmi()` calls `func_NMI()` gated on `g_ppuctrl & 0x80`** — without the hardware frame the RTS-tail misfires and corrupts the main stack.
- **`func_D98A` stays out of the NMI path** until gameplay boots. `func_F09F` runs on the main context post-tunnel (Start relocates main there via longjmp) — never C-call it from inside NMI.
- **Input works past the title** — Start tunnels to mode/skill/team-select (verified to 400 frames; only benign `$A3C7` interp skips, LOG_RETURN-safe). Current frontier is the court BG draw (sprites/sound/logic run; background stalls as tile-soup, see research §19).

### Known Workarounds (Current)

- **PPUMASK**: The game writes `0x00` to PPUMASK ($2001) every frame at `$F042` and `$FCBB`. When `g_ppumask_translate = 1` in extras.c, `g_ppumask |= 0x1E` is applied in the runner to keep rendering enabled. This is now in upstream nesrecomp origin/master (PR #22).
- **game_post_nmi() not called by runner**: The runner skips `game_post_nmi()` when `runtime_get_vblank_depth() > 1`. Keep per-frame NMI work in `game_run_nmi()`; `game_post_nmi()` holds only OAM restore + `func_DB4E`.
- **Attribute table zeroed each frame**: `func_FF61()` zeroes `g_ppu_nt` every frame. The attribute table (64 bytes) is re-initialized in `game_on_init()` before the first frame.
- **func_FF61**: Starts at `$FF61`, initializes PPU, calls `func_F036()` (game main, never returns).
- **func_FE8A**: PPUCTRL-shadow helper ending in RTS (safe natively; has a narrow dispatch-override arm).

### Current Constraints
- `disable_ptr_scan` and `disable_secondary` are already set in `game.toml` — do NOT remove
- `game.toml` keeps a minimal `fixed` list plus dis65-verified `extra_func` entries (banks 0/1/6, see `src/game.toml`) — never add opcode byte-pairs (`0xA901`, `0x8500`-class) as entries; each bogus `extra_func` risks splitting a real function
- `force_interp` stays empty — `0xFF08` in `force_interp` garbles graphics (native MMC1 bit-bang required)
- `[[merge_range]]` cannot ship while bank codegen splits one bank across part files sharing a TU — merged entries double-emit and break the build (needs a recompiler-side dedup fix; documented in `src/game.toml`)
- No `sram_map` (no-battery MMC1 has no SRAM code)
- Sprite flickering uses **Method 2 Software Sprite Removal** — "Remove Sprite Limit" emulator settings are non-functional
- **nesrecomp submodule tracks pristine `origin/master`** (currently `3ef948c`) plus `patches/002,003,004,005` applied to its working tree by `setup.sh` — never work from a bare checkout without running setup first

### Additional Known Issues

- **Intermittent `$8003` bank-8 CODE miss**: resolved — was downstream of the S-latch (corrupted A at `$FF08`); gone since the interp-guard fix. Current misses are benign interp skips (`$801B`, `$A3C7`, LOG_RETURN-safe, game completes).
- **BRK `$0001`/`$0600` diag-skips**: silent under DIAG policy, non-fatal (one transient `$0001` seen mid-transition, no fallout). Use `NESRECOMP_BRK=fatal` to convert to loud exits with context.
- **Frame comparison**: title frame 1+ matches Mesen 11 at ~99.8% structural similarity. Target stays ≥90% on frame 11 with 15+ frames.
- **`run_roundtrip.py` is trivial**: It only proves `nesasm` preserves bytes through `.db` directives — NOT that recompilation is correct. See `docs/research.md` §12.
- **C→x86→ROM comparison is unsound**: The recompiled C compiles to x86-64, making assembly-level comparison to original 6502 code meaningless. No recompilation project uses this approach. Verification should use co-simulation and dispatch miss monitoring instead.
- **Missing `extra_label`/`inline_dispatch` entries**: Zelda uses ~100+ bank-1 `extra_label` entries and `inline_dispatch` at `0xE5E2`. Super Dodgeball has neither; add only with disassembly proof (`tools/dis65.py`).

### docs/ Directory

- **`docs/research.md`** — ROM specs, NESRecomp framework, NMI dispatcher/`$0100` map (§13), MesenCE oracle (§16), NMI survey (§17)
- **`docs/plan.md`** — Living project plan and todo list

## NESRecomp Submodule Status

**Tracks pristine `origin/master` (currently `3ef948c`); `setup.sh` applies the 4 active `patches/*.patch` to the submodule working tree on fresh checkouts.**

Key upstream commits included:
- `cfc483f` Merge pull request #22 — PPUMASK rendering fix (`g_ppumask |= 0x1E`)
- `c32655e` runner: Force rendering enabled when `g_ppumask_translate` is set
- `fd4b7d5` Integrate PRs 19 and 21 with supported-title regression fixes
- `d2d9744` runner: Add NES-to-runner PPUMASK bit translation layer

## Codegen Notes

- `bank_switch = [0xFF08]` names the game's MMC1 bit-bang routine for bank prophecy (never a window address like `0x8000`)
- `call_by_address()` is used for indirect jumps and dynamic dispatch
- `maybe_trigger_vblank()` is called at every instruction boundary for NMI timing
- Frame boundaries are determined by CPU cycle count (`s_frame_budget`)
- `game_run_nmi()` is called at frame boundaries to execute the NMI handler
- Native RTS is a plain C return (does NOT pop the 6502 stack) — FDS-tail unwinds need runner help, see `patches/`
- The recompiler supports: `[[extra_func]]`, `[[extra_label]]`, `[[data_region]]`, `[[replace_func]]`, `[[sram_map]]`, `[[inline_dispatch]]`, `[[inline_pointer]]`, `[[nop_jsr]]`, `[[push_jsr]]`, `[[push_jmp]]`, `[force_interp]`, `[mod_function_hook]`, `[[ram_read_hook]]`

## Applied Patches (Current State)

The PPUMASK rendering fix is in upstream nesrecomp origin/master (PR #22). Active local patches: `002_interp_ram_dummy` (RAM-trampoline guard), `003_interp_rom_clamp` (ROM-miss pop guard), `004_nmi_tunnel_unwind` (Start-tunnel longjmp), `005_hang_watchdog_default` (90s idle auto-kill) — carried as `patches/*.patch`, applied to the submodule working tree by `setup.sh` (see `patches/active-patches.md`).

- ~~`patches/archived/001_nmi_tail_unwind.patch`~~ (archived 2026-10-01): NMI-tail unwind handling. Written for the `$FCB3` spin hang, bypassed game-side instead (`replace_func $FCA0`), never fired successfully — dormant risk without payoff. Revival conditions documented in `patches/archived/archived-patches.md`.

These game-specific hooks are applied in `src/extras.c`:

1. **`func_FCA0` (`[[replace_func]]`)**: NMI-enable + PPUMASK apply. Instant pre-menu (title stays pixel-perfect); faithful park post-tunnel (`s_menu_phase`) — mirror pushes, NMI-enable, boundary spin till one NMI — pacing the menu loop to hardware rhythm.
2. **`game_run_nmi()`**: `func_NMI()` gated on `g_ppuctrl & 0x80`, with Start-edge inhibit until `$0100=$C0`, nested gate (only `$C0` runs nested — depth 2 is top-level, truly-nested is >2), one-service-per-frame pacing, code-window scoping.
3. **`game_run_main()`**: `setjmp` tunnel landing — `runtime_prepare_guest_resume` + native `func_F09F()` on the main context (a bare vblank reset leaves callback depths stuck and freezes frames).
4. **`game_dispatch_override()`**: only the `0xFE8A` PPUCTRL-shadow arm remains.
5. **`game_on_init()`**: palette, PPUCTRL shadow, MMC1 4KB CHR force, attribute table (ROM bank 0 offset `0x073C` → `$23C0`).
6. **`game_post_nmi()`**: OAM flicker restore + `func_DB4E` scroll/PPUCTRL apply (whether still load-bearing is untested).
7. **`game_on_frame()`**: OAM flicker backup only.
8. **`func_8393_b6` (`[[replace_func]]`, bank 6)**: shared JMP-tail (STX/PLA/PLA/…) consuming 2 ancestor-pushed arg bytes absent under C tail-calls — dummies stand in (values discarded anyway), else the PLAs eat `$0100/$0101` and cascade into bank thrash.

## Verification & Testing

### Iterative Testing Procedure (for compaction recovery)

After any code change to `src/extras.c` or `src/game.toml`:

**CRITICAL: A valid progress test MUST produce JSON output with `frames_run` ≥ requested count. If the process times out or produces no JSON, the change hangs. HEAD (mask toggle) hangs at 3; no-toggle builds must reach 15+.**

1. **Rebuild**: `cmake --build build -j$(nproc)` (after `game.toml` edits, force regen first — the cmake custom command does not reliably retrigger: run `./external/nesrecomp/recompiler/build/NESRecomp "roms/Super Dodge Ball (USA).nes" --game src/game.toml --output-prefix "Super_Dodge_Ball_(USA)"` manually and confirm `[Eval] Coverage` shows resolved entries with no parse error)
2. **Baseline**: back up to `/tmp/` first (never `git checkout`/`restore`/`stash` working files without a backup), rebuild, run `timeout 25 ./build/super_dodgeball "roms/Super Dodge Ball (USA).nes" --smoke 5 --smoke-interval 1`
3. **PASS criteria**: JSON output with `frames_run` ≥ requested count AND `dispatch_miss_count == 0`
4. **FAIL criteria**: Process times out (exit 124/137), no JSON output, or any dispatch miss
5. **Frame 11**: Use `--save-screenshot` + `tools/compare_frames.py` against `nes_reference/`. Target: ≥90% structural match on frame 11 (current: ~99.8%).

**Key commands**:
```bash
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
timeout 25 ./build/super_dodgeball "roms/Super Dodge Ball (USA).nes" --smoke 5 --smoke-interval 1
```

**IMPORTANT**: If the test times out or produces no JSON output, the change hangs. The game MUST complete all requested smoke frames and produce the JSON summary.

**Hang watchdog**: the runner kills itself with a `[HANG]` dump (C stack + dispatch ring) after 90s without a completed frame (default-on; `NESRECOMP_HANG_TIMEOUT_S=N` overrides, `0` disables). A `[HANG]` line means frozen frames, not a slow test — read the dump, don't lengthen the timeout on the same approach.

**Known baseline**: 30 frames, exit 0, static title hash `a84564b5`, 0 dispatch misses. Menus: Start tunnels at frame 10 (`[TUNNEL]` log), mode/skill/team-select verified to 400 frames via `tools/input/title_to_team_select.txt` (only benign `$A3C7` interp skips). TAS probe: `run_tas.py --tas tas/sdb_4976.tas.json --frames 400` + `diff_tas.py` — liveness 400/400, 0 misses; state first-diverges ~TAS frame 44 (mode-menu confirm, research §21).

### Engine-change procedure (submodule bump, `patches/`, recompiler directives)

Game checks can't isolate engine regressions, so engine changes get both gates:

1. **Game gate**: rebuild + `smoke 5` per above (must stay green).
2. **Engine gate**: `python3 tools/blargg/run_blargg.py --tier cpu` (uses `[tool.blargg]` defaults; `NESRECOMP_HANG_TIMEOUT_S=25` shortens HANG triage) — compare `results.json` verdict tuples (`code`/detail/misses/BRKs, HANG stall frames) against the pre-change run. Any PASS→FAIL flip, changed FAIL tuple, or moved stall frame blocks the change. See `tools/blargg/README.md` (pristine A/B via `git stash` of the applied patches; never lose the working-tree patch state without a backup).

### Round-trip decompilation (ROM → Assembly → ROM)

The current `run_roundtrip.py` uses `nesasm` to convert ROM bytes to `.db` directives and reassemble. This is a trivial byte-preservation check — it proves the assembler works, NOT that the recompilation is correct. See `docs/research.md` section 12 for proper verification approaches.

### Verification approach:

The project uses **co-simulation** against MesenCE as the primary verification method:
- Compare recompiled game screenshots against MesenCE reference frames (`compare_frames.py`, tolerance-based: exact RGB differs by palette emulation, structural diff is the metric)
- Monitor dispatch misses (must be zero)
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
# Rebuild game
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run smoke test
./build/super_dodgeball "roms/Super Dodge Ball (USA).nes" --smoke 100 --smoke-interval 10
```

### Frame comparison pipeline:

```bash
# Generate reference frames with MesenCE
./tools/frame_gen/run_frames.sh roms/Super Dodge Ball (USA).nes 100 nes_reference

# Build and run recompiled game with screenshots
./build/super_dodgeball "rom.nes" --smoke 100 --save-screenshot recomp_output

# Compare
python3 tools/compare_frames.py --ref nes_reference --recomp recomp_output --frames 100
```

**Critical: Frame 11 is the title screen.** Frame 0 is blank. This is the frame to verify — it must match ≥90% structural similarity with the reference (tolerance-based diff; exact RGB never matches across renderers). Do NOT cite frame 0 as success.

Current: frame 1+ already matches Mesen 11 at ~99.8% structural similarity (title builds from frame 0 via the real NMI path).

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
- Code window mapping ($8000-$BFFF via `g_code_window_base = 0xE000`) causes address resolution issues for addresses < $C000
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
| `src/extras.c` | Game hooks: `game_on_init`, `game_run_nmi`, `game_post_nmi`, `game_on_frame` |
| `src/game.toml` | NESRecomp recompiler config |
| `src/game.discovery.toml` | **TO CREATE** — pointer scan exclusions, inline dispatch tables |
| `generated/` | Auto-generated C code (gitignored) |
| `external/nesrecomp/` | NESRecomp submodule (recompiler + runner) — **already at origin/master** |
| `external/mesence/` | MesenCE emulator source/binary |
| `patches/` | Active runner patches (`002,003,004,005`, auto-applied by `setup.sh`) + `archived/` dormant (see `patches/active-patches.md`) |
| `tools/dis65.py` | Dependency-free 6502 disassembler for entry verification (ROM + addr + count + file offset) |
| `tools/frame_gen/memwatch.lua` | MesenCE oracle probe: who-writes (with PC) for RAM addrs + optional NMI flag log (`WATCH`, `MAXF`, `START0/1`, `NMI_LOG`) |
| `tools/input/title_to_team_select.txt` | Headless input driver: title → mode → skill → team-select (verified green) |
| `tools/frame_gen/oracle_dump.lua` | MesenCE `--testrunner` oracle: frame-N PPU dump + per-NMI `$0100/$0106` log (see `docs/research.md` §16) |
| `tools/frame_gen/` | Frame export Lua script and wrapper |
| `tools/blargg/` | blargg test-ROM harness (`run_blargg.py` + `manifest.json` + patch-neutral `blargg_extras.c` + `selftest/` canary); per-test builds in `build-blargg/` (gitignored), ROMs via `--fetch` into `external/nes-test-roms/` (gitignored) |
| `tools/tas/` | TAS differential harness (bk2/fm2 converters, `run_tas.py` + `diff_tas.py`, `mesen_tas.lua`); vectors in `tas/vectors/`, runs in `tas/runs/` (gitignored) — see `tools/tas/README.md`, research §21 |
| `build-diag/` | Diagnostic build (rings + stack tracking, gitignored); prod `build/` keeps them off |
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

---

## Frame/Hang Investigation (resolved 2026-10-01)

- The 3-frame hang was the `g_ppumask = 0x00` direct write in `game_post_nmi()` (commit `3194d28`), bypassing the runner's `|= 0x1E` translation and breaking VBlank timing. Rule: never assign `g_ppumask`/`g_ppuctrl` directly; route through `nes_write`.
- The `game.toml` in tree never parsed (one-line `addr = X, bank = Y` is invalid TOML), so every historical regen used default config. Rule: multi-line `addr`/`bank` entries; confirm `[Eval] Coverage` on every regen.
- Manual tile writes cannot survive working bank switches (real code owns the nametable). Title must come from the real NMI path.
- The `$FCB3` NMI-wait spin deadlocks under C returns (tail unwind unrepresentable). Game-side `$FCA0` replace is split: instant pre-menu, faithful park post-tunnel (`s_menu_phase`) — a full park (also at boot) garbles boot strips, and parking with NMI off deadlocks till the cap.
- Never `memcpy` bulk data to `$0100-$01FF` (6502 stack) or `$0200-$02FF` (OAM buffer) — the old 512-byte `$0108` fill smashed return addresses into padding jumps (`BRK $0001`).
- Depth 2 inside `game_run_nmi` IS the top-level NMI (trigger + firing); truly-nested is > 2 — a `> 1` gate silently skips every handler.
- Native JSR sites push no 6502 return address: any interp transfer ending in RTS pops live-caller bytes (+2 S per call). Guarded runner-side (002); never rely on interp exits preserving S.
- C-stack lap growth: cross-function loop laps (`F1AA BNE $F13A` via `call_by_address`) add a C frame per lap. `merge_range` cannot ship while parts share a TU. Independent corroboration: blargg `instr_01` template helpers (`E442`/`E458`/`E8CF`) stack 510 deep the same way (see `tools/blargg/README.md`, research §20).
- Current: 30 frames, exit 0, title ~99.8% vs Mesen frame 11, static hash `a84564b5`; menus verified to team-select at 400 frames (only benign `$A3C7` interp skips).