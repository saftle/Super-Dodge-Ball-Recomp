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
- **Cycle backend** (`external/nesrecomp/runner/cyc/`): Cycle-accurate console model (6502, PPU, APU, DMAs, mappers in `hw_*.c`) + host (`cyc_host.c`: input schedules, screenshots, frame-logs, TCP). Built per game into `build-cycle/nes_game`.
- **`src/game.toml`**: Recompiler config — display `name` (window title), a `cycle_seed_file`, plus the ten content-keyed `[[mod_function_hook]]` sites the character-flicker mod hooks. No discovery tuning; the stock ROM needs none.
- **`src/cycle_seeds.txt`**: Recompiler discovery roots for the sprite emitters. A mod hook site only exists in recompiled code, and `$8040`/`$821E`/`$E32D`/`$966D`/`$A36C` ran on the interpreter — a hook there compiled, bound and never fired. These six lines fix that; total cycle count unchanged.
- **`src/sdb_host_extras.c`**: Our two mods (`HOST_EXTRAS`), the only game-specific code. **(1) character-flicker**: captures every OAM sprite the game emits and presents them host-side, and overrides the game's own object culling (three gates). The truncation half is presentation only (identical cycle count; only `$02FD`/`$02FF` differ); the culling half is **not** — it makes the guest emit sprites it had dropped. **(2) PPU overclocking** (section 4): picks a value for `cyc_set_extra_scanlines` so the guest gets more CPU time per frame and runs faster; re-applies it from `power_on`, because `cyc_power_on()` zeroes the field after mods activate. See research.md §2, §2.1 and §23. (VSync started as a third mod here and moved to the launcher's Graphics section — patch 007, research §24.)
- **`mods/packages/super-dodgeball.character-flicker/`**: The mod *package* — a `manifest.toml` only, copied beside the executable at build time. Data-only by design; `.nesmod` archives may not carry native code, so the package is a declaration and the implementation is `src/sdb_host_extras.c`. Off by default, toggleable in the launcher's Mods screen.
- **`mods/packages/super-dodgeball.ppu-overclock/`**: The PPU-overclocking package (same data-only design). Integer `[[option]]` so the launcher exposes the scanline count. Off by default because it changes console timing. Requires `patches/006_ppu_overclock_mmc1`; without it the backend refuses the setting on our MMC1 cart.
- **`patches/006_ppu_overclock_mmc1.patch`**: The first active submodule patch — widens `cyc_extra_scanlines_supported()` from MMC3-only to MMC1 too. Strictly additive (consulted only when `extra_scanlines != 0`, so stock timing is unchanged for every game) and on its way upstream.
- **`patches/007_window_vsync.patch`**: The second active patch — VSync as a launcher Display setting (`[Display] VSync` in `config.ini`, `has_vsync` lighting up recomp-ui's Graphics row, renderer built with `PRESENTVSYNC` when on). Default off, so stock behavior is unchanged unless the player opts in; presentation only, never guest timing or save states.
- **`CMakeLists.txt`** (repo root): our game project — includes the submodule's `runner/cyc/project.cmake` but adds `CXX`, `MODS`, `GAME_ID` and `HOST_EXTRAS`, which the submodule's own `runner/cyc/project/CMakeLists.txt` cannot express. This is why we wrap the submodule rather than editing it.
- **`build-cycle/`**: Generated per-instruction C (gitignored, one revision dir per ROM/config) + `nes_game` runner, `config.ini`, and the preloaded `mods/` catalog.
- **`external/recomp-ui/`**: Shared recomp-ui launcher (pre-boot settings dashboard) + in-game menu, compiled into the windowed `nes_game`.

Build: `./build_and_run.sh` (windowed) or pass host flags (`--frames`, `--input`, `--shot-every`) for headless scripted runs. Game config changes re-trigger codegen at configure time (each revision gets its own generated dir).

A headless run does **not** read the mod catalog unless you pass `--mods-root build-cycle/mods`; without it mods stay off and the output is byte-identical to stock. That is the intended way to A/B the mod, and it is what the "mod off" baselines below were measured against.

### The launcher (recomp-ui)

nesrecomp already integrates recomp-ui for the cycle host (`runner/cyc/project.cmake` auto-detects a checkout; the glue is `cyc_ui_launcher.c`/`cyc_ui_menu.c`/`cyc_ui_imgui.cpp`). **The work here is pointing it at our checkout, not writing a launcher.** Nothing game-specific: console identity, bindings, and the picture all come from nesrecomp's adapter.

- Both `build_and_run.sh` and `setup.sh` pass `-DNESRECOMP_RECOMP_UI="$PWD/external/recomp-ui"`. The framework's auto-detect only looks in `${CMAKE_SOURCE_DIR}/recomp-ui`, so passing our real path explicitly is load-bearing, not belt-and-braces.
- `-DNESRECOMP_RECOMP_UI=OFF` builds the window without launcher or menu. `NESRECOMP_NO_LAUNCHER=1` skips the launcher at runtime (for debugging).
- Windowed only. A headless run (`--frames` and friends) sets headless before `cyc_sdl_prelaunch()`, so the launcher is never reached — verified: recomp-ui ON and OFF builds emit byte-identical `--frame-log` output over the 400-frame TAS.
- Settings, bindings and the launcher state live in `build-cycle/config.ini` beside the executable. The Menu shortcut (Escape by default) opens the in-game menu.
- **Pin the submodule.** The launcher's C ABI is append-only and heavily churned; headers and sources must come from one revision. Bump deliberately and rebuild whole. See `NOTICE.md` for provenance.

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
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" [--frames N] [--input schedule] [--shot-every N --screenshot shot.png] [--mem-frame N --mem-out mem.txt] [--miss-log seeds.txt] [--align 0-3] [--mods-root DIR] [--present-out FILE --present-every N]
```

- **Interactive**: bare run — window opens (`[game] name` is the title), keyboard/gamepad input works
- **Headless**: any of `--frames`/`--input`/`--screenshot` implies headless — no display needed
- **Deterministic**: always pass `--ram-init zeros` for comparable runs (default pattern RAM otherwise)
- **ROM** must be at `roms/Super Dodge Ball (USA).nes` (gitignored)
- **Snapshots**: `--frame-log-at mesen --frame-log frames.bin` writes per-frame CYCFRAME-v2 snapshots (CPU RAM, CIRAM, palette indices, OAM, picture indices) for the TAS diff
- **Mods**: the window picks them up via the launcher. Headless needs `--mods-root build-cycle/mods` explicitly, or mods stay off and the run is stock.
- **PPU overclocking**: `--ppu-overclock <0..262>` (blank scanlines before NMI). Needs `patches/006`; refuses with the backend's MMC3 message otherwise. Deliberately changes timing; 0 is stock. The mod's manifest feature does the same thing for the window.
- **VSync**: the launcher's Graphics section (needs `patches/007` or the row is absent). Off by default; persisted as `[Display] VSync` in `build-cycle/config.ini`. Presentation only — guest timing untouched.
- **Presentation**: `--present-out` saves what the *window* would show — the only path that exercises the character-flicker mod. `--screenshot`/`--frame-log` capture the machine's native frame and ignore it.

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
- `external/recomp-ui` is a git submodule — never edit it in place, never commit in it. Build it from the pinned revision; do not mix headers across revisions (append-only ABI)
- Adding recomp-ui needs no SDL of its own (nesrecomp provides it) but does pull in OpenGL + Threads at configure time
- Generated C code (`generated/`) is gitignored — do not edit directly
- `make clean && USE_GCC=true make -j$(nproc)` builds the entire MesenCE with UI
- Use venv Python (`venv/bin/python3`) for nesasm work, system `python3` for Pillow/numpy scripts — never mix them up
- `src/` ships **config + our own C only** — `game.toml` and `sdb_host_extras.c`. No game bytes: the cycle project reads the user's ROM at configure time (identity-checked) and the binary takes the ROM path at launch. Hooks identify code by `crc32`, never by `bytes`. Precedents: `tools/rom_to_asm.py`, `tools/frame_gen/oracle_dump.lua`
- Debug byte dumps belong in `tmp/` (gitignored scratch), never in `src/`. Confirm offsets with `tools/dis65.py` plus a python byte-compare against the ROM before coding
- **Every screenshot/frame-dump run gets its own subfolder under `tmp/`** (`tmp/<what-it-is>/`), never the shared `tmp/` top level and never an existing investigation's folder. A per-frame `--present-out`/`--screenshot`/`--frame-log` run writes tens of thousands of files and gigabytes; loose in `tmp/` they are impossible to tell apart and impossible to delete selectively. Delete the folder when the investigation is over, and say what it was before you do
- `tmp/` is scratch, so **anything a reproduction depends on does not live there**: input schedules and vectors go in `tas/vectors/`, analysis scripts that back a research.md claim go in `tools/`
- Pre-commit self-check: `git status --short` and `git ls-files | grep -Ei '\.(nes|png|ppm|bin)$'` must show no game-derived blobs; any new `static const uint8_t …[]` in `src/` needs ROM-provenance justification or gets rejected
- Every third-party addition ships with provenance + license + posture, recorded at add time (never as later cleanup): who made it, under what license (or "no license file found"), and how redistribution is avoided — submodule pointer (own license, never vendored), gitignored + user-fetched (test ROMs, oracle binaries), or credited test fixture (vectors keep in-file + README author credit, link pages instead of re-hosting). Code/deps go in `NOTICE.md`; test data goes in the nearest README
- Installed packages (pip/apt) are not distributed — no attribution needed. Code you write from specs (parsers, harnesses) is yours. Data files you didn't create keep their credits intact — never strip author info
- **nesrecomp submodule tracks pristine `origin/master`** (currently `mstan/nesrecomp` @ `2b4fcdb` — the pin must stay fetchable upstream; never commit in the submodule). The **checked-out tree is expected to be dirty** when a patch is applied: `setup.sh` applies `patches/*.patch` after every checkout, so `git -C external/nesrecomp status` showing a patched file is normal, not a mistake. `git -C external/nesrecomp status` must otherwise be clean. Active patches must be **strictly additive** and are headed for upstream PRs — see `patches/README.md`, `patches/active-patches.md`

## Known Issues (Current State)

### Model Notes (cycle backend behavior, verified)

- Sprite palette entry-0 bytes (`$3F10/$3F14/$3F18/$3F1C`) read stale power-on bytes in snapshots — hardware mirrors them to the backdrop `$3F00`. Masked in the TAS diff; rendering unaffected. Upstream-noted, not ours to fix.
- Uninitialized CIRAM differs at power-on (`--ram-init` covers CPU RAM only); the game fills nametables within frames. Boot-frame diffs are init modeling, not divergence.
- Unseeded runs execute partly on the cycle interpreter (still bit-exact, slower). `--miss-log` seed lines feed future `cycle_seed_file` runs; native share is informational, never a gate.
- Boot staging order differs from Mesen (backdrop-green vs stripped title at file 0); content converges from file 1. Same-index comparison stands — no offset search.
- Sprite flickering is **not** emulator sprite-limit removal — the game throttles its own sprites three ways, and "Remove Sprite Limit" cannot help with any of them. (1) The OAM builder has a hard **64-sprite budget** (8-bit `$0617` cursor, `BEQ` bail at `$84B6`) and simply **stops emitting**, so the missing tiles are never computed; characters are 8 sprites each (16×32), so 79% of match frames truncate. (2) `$E32D` builds a display list at `$067C` and refuses to add an object whose state byte `$43` has bits 4-6 equal to `$20`, rotating which object it refuses, so a whole character blinks out 4 frames in 9 while OAM sits at 46-54 of 64. (3) Two emitter shapes no cursor-bump hook can see: the damage-number pair (bank 3, `ADC #$08` step) and the block copier (bank 4, no bump at all) — hooked per-emitter, with prefix carry-over for sprites that sit in OAM. **All fixed** by the opt-in `super-dodgeball.character-flicker` mod (`src/sdb_host_extras.c` + ten content-keyed `[[mod_function_hook]]` sites in `src/game.toml` + `src/cycle_seeds.txt`). Truncation: body-ink stdev 8.7 → 0.8. Culling: box-ink stdev 55.0 → 16.6, 0 frames still culled. Numbers: 108/108 frames steady. Full write-up in research.md §2 and §2.1.
- Three traps in that work, each of which produces a *different* artefact than the one being fixed: a hook site in interpreted code compiles, binds and never fires (seed it); the `$E35F`-`$E36C` id-high-byte rule is inverted if read at a glance and gives restored objects sort key `$FF`, which shows up as a character drawing *in front* of the one above it; and re-adding a culled object changes the guest's work, so the "presentation only" claim stops being true (OAM differs, sprite slot 0 never does, the picture does not).
- **The mod's own sprite-list boundary is `$0617` going backwards, not `$0617 == $04`.** The game sometimes starts a list at a higher slot when the emitters for slots 1-2 have nothing to draw, and a `$04`-only test misses it: `build` never restarts, runs to `SDB_MAX_SPRITES`, and every frame after paints hundreds of stale sprites — a "copies in a trail" smear that looks like a game bug and is not. Backstop at `SDB_LIST_LIMIT` (256 vs a measured max of 77) publishes and warns. `SDB_TRACE=1` prints `frame peak-list overflow published-list saw-$04 backstop-trips` per frame — reach for it first; `SDB_TRACE_CAPTURES=<from>-<to>` dumps every capture. Never leave that boundary on one condition.
- Two traps when touching that mod, both of which produce a *different* artifact than the one being fixed: emit order **differs between builds**, so never blend a half-built list with the previous one index-wise (draws both, out of order) — present the last complete list only on the *first* capture frame, and only if it was published on the immediately previous frame (after a gap the published list predates OAM; research.md §2.1), or the picture goes a full build stale one frame in six. And the blit must walk pixels itself mirroring `cyc_render_sprites`; calling `cyc_render_tile` once per scanline stacks eight overlapping 8×8 copies that overwrite their own pixels, which reads as doubled, wrongly-coloured sprites.
- A restarted list opens with the OAM slots below its cursor copied verbatim (prefix carry-over): persistent sprites like the damage digits live there and the PPU draws them, so omitting them drops sprites the machine shows. Not blending — those slots are outside the new list by construction.
- `$8200` (bank 5) removes an entry from the object list at `$067C` and the SNES port replaces it with `RTS ; disable sprite flickering`. **That was the second flicker.** It is one of three gates on the same culling decision, all now overridden by the mod; research.md §2.1 has the full chain (`$E34A` builder gate → `$828A` record gate → `$8200` crowded-stack gate) and the measurements. The port's other two "flicker" commits (an inverted OAM priority bit, and capping its own OAM translation at 64 sprites with `CPY #$100`) are not ours.

### Current Constraints
- `src/game.toml` carries no discovery tuning (the stock ROM needs none) and no `[[mod_function_hook]]` beyond the ten the character-flicker mod needs: five OAM-emit sites, three object-culling sites, one damage-number site, one block-copy site. Its `cycle_seed_file` (`src/cycle_seeds.txt`) is **not** optional — the culling/number/block sites sit in code static discovery never reaches, and a hook in interpreted code compiles, binds and never fires. **Never add hook content keys as `bytes` unless you intend to commit ROM-derived bytes** — use `length` + `crc32`, which keeps the bytes out of the repo.
- Hook sites declare **no `id`**: they bind to plugins by address. Sharing one `id` across several sites fails `cyc_hooks_validate`.
- No `sram_map` (no-battery MMC1 has no SRAM code)
- **nesrecomp submodule tracks pristine `origin/master`** — the checked-out tree is dirty exactly when an active patch is applied (see above); `setup.sh` re-checkouts the pin on fresh setups

### Additional Known Issues

- **Frame comparison**: images are informational only (cross-renderer RGB always differs by palette emulation). Same-renderer pairs are strict; cross-renderer pairs are palette-relative. State bytes (`diff_tas.py`) decide.
- **`run_roundtrip.py` is trivial**: It only proves `nesasm` preserves bytes through `.db` directives — NOT that recompilation is correct. See `docs/research.md` §12.

### docs/ Directory

- **`docs/research.md`** — ROM specs, NESRecomp framework, NMI dispatcher/`$0100` map (§13), MesenCE oracle (§16), NMI survey (§17)
- **`docs/plan.md`** — Living project plan and todo list

## NESRecomp Submodule Status

**Tracks pristine `origin/master` (`mstan/nesrecomp`, currently `2b4fcdb`).** The pin stays fetchable upstream; never commit in the submodule. The checked-out tree is dirty exactly when an active patch is applied (see the constraint above) — verify any *other* dirt is yours before blaming the framework.

## Codegen Notes

- One labeled C block per ROM instruction, entered per (PRG bank, CPU address); static jumps/branches compile to `goto`s, `cpu.pc` written only when control leaves compiled code
- Frame boundaries are PPU-driven; `--frame-log-at mesen` aligns snapshot records to Mesen's frame end
- The recompiler also supports `[[mod_function_hook]]` (content-keyed subroutine hooks) — used by the character-flicker mod (ten sites); see `docs/plan.md` roadmap for future ones

## Retired: Legacy Runner (2026-10-02)

The function-level runner (`src/extras.c` hooks, `build/`) is deleted. Its game knowledge (NMI dispatcher, tunnel mechanics, input path) lives on in `docs/research.md` §§18–19 and transfers to mod work. `patches/` is back (reopened 2026-10-12 for patch 006): `setup.sh` applies active patches to the submodule after every checkout — see `patches/active-patches.md` — and archived legacy-runner patches stay unapplied in `patches/archived/`. Never re-add game-specific hooks to work around timing again: time it against Mesen, and if the model is wrong, the fix belongs upstream.

## Verification & Testing

### Iterative Testing Procedure

After any `src/game.toml` change (display name, discovery, seeds, **or a `[[mod_function_hook]]` site**):

1. **Rebuild**: `cmake --build build-cycle -j$(nproc)` (configure re-triggers codegen; each revision gets its own generated dir)
2. **TAS**: `python3 tools/tas/run_tas.py --tas tas/sdb_4976.tas.json --frames 400 --out tas/runs/rXXX` + `diff_tas.py` — liveness 400/400 exit 0, state MATCH (mirrors masked)
3. **FAIL criteria**: nonzero exit, logged frames != requested, any state diff outside the masked mirrors

**Known baseline**: cycle 400 TAS frames → team-select, state byte-exact vs Mesen (RAM/OAM/palette/CIRAM); blargg cpu tier 25/25.

### Verifying the character-flicker mod

`--frame-log`/`--screenshot` capture the machine's native frame and never see the mod's
presentation. Check it with `--present-out`, which writes what the window would show:

```bash
# same input both ways: mod on vs off is the whole test (the feature is off
# by default, so mod-on runs pass --remove-character-flicker explicitly)
SDB_TRACE=1 ./build-cycle/nes_game ROM --frames 1100 \
  --input tas/vectors/sdb_idle.input --ram-init zeros --mods-root build-cycle/mods \
  --remove-character-flicker \
  --present-out tmp/mod/on_%05d.png --present-every 1
./build-cycle/nes_game ROM --frames 1100 --input tas/vectors/sdb_idle.input \
  --ram-init zeros --present-out tmp/mod/off_%05d.png --present-every 1
```

**Set `SDB_TRACE=1` on every mod run.** It prints one line per frame and is what turns "the
screen looked wrong somewhere" into a frame number. A healthy frame is tens of sprites; watch
the peak-list column, not the pictures.

Gates:

1. **No runaway list** — peak list size stays under ~100 and the overflow column is 0. A peak
   that climbs frame after frame is the list-boundary failure (Known Issues); it ends in a
   trail of stale sprites. The backstop column must also be 0.
2. **Flicker gone** — pick a character's on-screen box, count non-background pixels per frame:
   stdev must fall well under stock's. Truncation 8.7 → ≤1; culling (Enemy B, frames 700-909)
   55.0 → 16.6, with **0** frames at stock's culled value.
3. **No new artefacts** — inside the affected character's box the mod matches stock where
   stock drew him (frame 866: 0 differing pixels in B's box). Whole-frame sameness is NOT
   the gate: the culling fix shifts guest timing, so blink-phase elements (menu cursor,
   roster scroll) sit a frame apart between runs with identical game flow. Regional ink
   plus eyeballing decides.
4. **Mod off is stock** — byte-identical output with the feature disabled (1100/1100 frames,
   empty catalog vs no catalog).
5. **Play it** — a scripted idle run never reaches the states that break list bookkeeping.
   Throw (`B`; `A` passes, `A+B` jumps) from frame 445 and watch (`tas/vectors/sdb_throw.input`,
   8000-frame stress: max list 77, 0 overflow, 0 backstop). See research.md §2.1.
6. **Numbers present** — damage digits drawn every frame the guest holds them in OAM
   (108/108 steady, throw vector); menu position digits pixel-identical (menu frames 100-400).

Comparison numbers must come from the *same* schedule and `--ram-init`; a different input changes the match and the figures are meaningless.

**Guest state: not a pass/fail gate, but do record it.** The truncation half is presentation
only. The culling half makes the guest emit sprites it had dropped, so OAM differs (sprite slot
0 never does), per-object timers settle at a constant offset, and the picture (CIRAM) is
identical on 899 of 910 frames. Cycle count moves by 1-2 over 910 frames. "Cannot observe" is
not "byte-identical" — do not write that it is.

**One nes_game per mods dir.** `build-cycle/mods/state.toml` is shared mutable state: two
concurrent runs (e.g. a `--miss-log` run beside a mod run) can leave the feature `enabled =
false`, and the whole run silently goes native. Never overlap runs that share a mods dir.

**The TAS gate is not currently trustworthy.** Super Dodge Ball is a random game, so a fixed input schedule does not reproduce the same run: RNG divergence between the cycle backend and Mesen puts the two sides on different paths, and a red `diff_tas.py` may be that rather than a real defect. Until pre-seeding exists (pin both sides to the same RNG state before frame 0), treat TAS diffs as advisory — investigate the first divergent frame, don't assume a regression. Blargg is unaffected and remains the real engine gate.

### Engine-change procedure (submodule bumps)

Game checks can't isolate engine regressions, so engine changes get both gates:

1. **Game gate**: TAS 400 + diff MATCH per above (must stay green).
2. **Engine gate**: `python3 tools/blargg/run_blargg.py --tier cpu` — 25/25 (20 PASS + 5 screen-confirmed) required; any flip blocks. See `tools/blargg/README.md`.

**Never run blargg without being asked.** It takes many minutes and it is *upstream's* gate, not this project's: blargg tests nesrecomp's CPU/PPU model against blargg's test ROMs and says nothing about Super Dodge Ball. Running it "to be safe", or as an automatic step of a submodule bump, is wasted time here — the owner runs it when they want to know the new engine still passes. Same for TAS: see the advisory note above. Ask before running either.

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

**MesenCE reads `settings.json` from the directory its binary sits in — NOT from `$HOME`.**
Overriding `HOME` does nothing; the real user config is silently used instead. The sandbox must
own the *executable*: copy `Mesen` + `MesenCore.so` + `lib*.so` + `settings.json` into a temp dir
and run that copy. Verified by control: `Nes.Region=Pal` only moved cycles/frame 29781 → 33248
once `settings.json` sat beside a copied binary. `tools/frame_gen/run_frames.sh` does this.

Two more traps in the same script, both of which look like "Mesen produced nothing":
- **`xvfb-run` word-splits the program path.** It execs via `sh -c`, so a path containing spaces
  (this repo's own path has one) breaks *even when quoted*. `cd` into the sandbox and run `./Mesen`.
- **After `cd`, relative ROM paths stop resolving.** Pass an absolute ROM path.

Also: `PpuExtraScanlinesBeforeNmi` exists under **both** `Nes` and `Snes` in the shipped settings.
A naive `grep` matches whichever comes first in the file and reports the wrong console's value —
read it back with a JSON parser scoped to the section. See `docs/research.md` §23.

### MesenCE as Oracle

Frame reference generation uses the **prebuilt MesenCE binary** (`external/mesence/Mesen`) with `--testRunner` mode and a Lua script (`tools/frame_gen/export_frames.lua`). Deeper oracle work (PPU dumps, per-NMI flag logs) uses `tools/frame_gen/oracle_dump.lua` — see `docs/research.md` §16. Headless runs need `xvfb-run` plus a sandbox that owns the Mesen *binary* and its `settings.json` (see the config-location note above; isolating `HOME` alone does not work).

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
| `CMakeLists.txt` | Game project (root). Wraps the submodule's `runner/cyc/project.cmake`, adds `CXX` + `MODS` + `GAME_ID` + `HOST_EXTRAS`, copies `mods/` beside the executable |
| `src/game.toml` | Recompiler config: display `name`, a `cycle_seed_file`, plus ten content-keyed `[[mod_function_hook]]` sites (`length`+`crc32`, never `bytes`) |
| `src/sdb_host_extras.c` | Our mods: the character-flicker mod (OAM sprite capture + host-side presentation + object-culling override) and the PPU-overclocking mod (section 4; sets `cyc_set_extra_scanlines`). Only game-specific C in the tree. Owns the single `cyc_host_extras()` |
| `src/cycle_seeds.txt` | Recompiler discovery roots for the sprite emitters — a mod hook site only exists in recompiled code |
| `tas/vectors/sdb_idle.input` | Reproduction vector for the flicker fixes (idle into a match; B blinks at 865-905) |
| `tas/vectors/sdb_throw.input` | Stress vector for the mod (throw from frame 446; exposes runaway bookkeeping + missing emitters) |
| `tools/flicker/` | Analysis scripts behind the research.md §2/§2.1 claims (OAM/record dumps, ref-finder, memdiff) + toolkit README |
| `tools/overclock/` | PPU-overclocking oracle harness behind research.md §23 (MesenCE sandbox + stock-vs-OC comparison). **Its self-check matters: if cycles/frame stays 29781 for a non-zero `Before NMI`, the setting never applied and the run is worthless** |
| `mods/packages/super-dodgeball.character-flicker/` | Mod package — `manifest.toml` only (data-only by design; implementation is `src/sdb_host_extras.c`). Off by default, toggleable in the launcher's Mods screen; PolyForm NC, same as the project |
| `mods/packages/super-dodgeball.ppu-overclock/` | Mod package for PPU overclocking (research.md §23). Manifest-only like the above; has an integer `[[option]]` so the launcher exposes the scanline count. **Needs `patches/006_ppu_overclock_mmc1`** or the backend refuses on mapper 1 |
| `external/nesrecomp/` | NESRecomp submodule (recompiler + runners) — pinned `origin/master`; **dirty tree is expected** when `patches/*.patch` is applied by `setup.sh` |
| `external/mesence/` | MesenCE emulator source/binary |
| `external/recomp-ui/` | recomp-ui submodule (launcher + in-game menu) — **pinned, never floated**; MIT, own license |
| `patches/` | Active runner patches applied to the submodule by `setup.sh` (`*.patch`, strictly additive, documented in `active-patches.md`); `archived/` holds the retired legacy-runner ones `setup.sh` ignores |
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