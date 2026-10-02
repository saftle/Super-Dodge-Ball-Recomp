# Super Dodgeball NES Recompilation — Project Plan

> Cycle backend is the default and only play path. Team-select verified byte-exact vs Mesen; blargg 25/25. Frontier: court verification past team-select, then enhancements (widescreen, flicker fix).

## Where Things Stand

- **Backend**: cycle-accurate (`./build_and_run.sh` → `build-cycle/nes_game`, upstream `runner/cyc`). No game hooks, no patches — stock ROM on accurate timing. The legacy runner is retired (files removed 2026-10-02, backup in `/tmp/legacy_backup/` + git history; game knowledge preserved in research §§18–19).
- **Verification**: TAS 400 frames reach team-select with RAM/OAM/palette/CIRAM byte-exact vs Mesen from boot (`tools/tas/`); blargg cpu tier 25/25 (20 RAM-text PASS + 5 screen-confirmed).
- **Upstream**: maintainer is moving to cycle-default; legacy goes antiquated. Our migration is done early. The one legacy artifact worth saving is patch 002 (S-latch guard) — proposed upstream below, then it lapses with the rest.
- **Frontier**: court BG past team-select is unverified on cycle; after that, enhancements.

## Open Items

1. Court BG past team-select — verify on cycle via full-run TAS vectors (game runs underneath on legacy evidence; confirm on cycle).
2. Propose 002 S-latch guard upstream (draft below), then done with patches forever.
3. Widescreen via `HOST_EXTRAS` custom compositor (SMB pattern: stock center authoritative, host re-draws margins).
4. Flicker fix via `[[mod_function_hook]]`: capture pre-cull OAM at the cull routine, draw host-side, suppress native slots. First step is locating the OAM-cull routine.
5. Resolution honesty: wider canvas only — no upscaling path exists upstream (CHR PNG replacement at most). Don't promise HD.
6. Submodule stays pristine (`git -C external/nesrecomp status` clean; reset 2026-10-02).
7. Docs reconciled to cycle (README, AGENTS.md, plan, research §§21–22, harness READMEs); research §§18–19 stay as game-knowledge history.

## What Other Successful NESRecomp Projects Do Differently

### Zelda 1 (mstan/LegendOfZeldaNESRecomp) — 100% playable
- Uses `bank_switch = [0xFFAC]` (proper MMC1 bit-bang routine)
- Has `sram_map` for bank-1 code mapped to SRAM
- Has `inline_dispatch` at `0xE5E2`
- Has ~100+ `extra_label` entries for bank 1 discovery
- Has `data_region` declarations in `game.discovery.toml`
- `game_dispatch_override()` only handles SRAM remapping (`0x6C90`-$7FFF), NOT controller polling
- Has TCP debug server, verify mode, entity diagnostics
- Keeps `nesrecomp` submodule at `origin/master` (commit `27b281e`)
- Has `game.discovery.toml` with pointer scan exclusions

### Super Mario Bros (mstan/SuperMarioBrosNESRecomp) — Fully playable
- Uses NROM (mapper 0) — no bank switching complexity
- `game_dispatch_override()` returns 0 (not needed)
- Recompiled code handles `ReadJoypads`/`ReadPortBits` natively
- Has extensive `[[mod_function_hook]]` and `[[ram_read_hook]]` sections
- Has TCP debug server on port 4370
- Has verify mode, entity diagnostics
- Has `game.discovery.toml` and `symbols.sym`
- Generated C code is committed to repo (not gitignored)

### Yoshi's Cookie (mstan/nesrecomp reference) — 100% playable, zero oracle divergence
- MMC3 mapper (more complex than MMC1)
- Uses `[[trampoline]]`, `[[inline_dispatch]]`, `[[data_region]]`
- Has proper `game.toml` with all directives

### RetroPortingToolKit — Key Lessons
- **House Invariants** (05_agents/02_house-invariants): "No stubs, fix the tool not generated code, find first divergence"
- **Co-simulation** (02_concepts/05_co-simulation): Run port beside oracle, compare at guest-time checkpoints
- **Verification Rituals** (05_agents/03-verification-rituals): Dispatch misses must be zero, co-sim must match, TCP checks pass
- **Failure Modes** (05_agents/04-failure-modes): 10 common AI failure shapes — stubbing, wrong fixes, ignoring first divergence
- **When You Cannot Run the Game** (05_agents/07-when-you-cannot-run-the-game): Partial checks without game/BIO/oracle

---

## RetroPortingToolKit Key Rules

### House Invariants (from 05_agents/02_house-invariants)
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

### Verification Rituals (from 05_agents/03-verification-rituals)
- Dispatch misses must be zero
- Co-sim must match (RAM state at checkpoints)
- TCP checks pass
- Frame hashes must match
- A green build is weak evidence
- Use hardware events, not frame numbers

### Failure Modes (from 05_agents/04-failure-modes)
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

### When You Cannot Run the Game (from 05_agents/07-when-you-cannot-run-the-game)
1. Check if the ROM loads (CRC verification)
2. Check if the title screen renders (PPU works)
3. Check if dispatch misses are zero (all functions discovered)
4. Check if input is wired (controller buttons change)
5. Check if RAM state progresses (state machine advances)
6. Check if NMI fires (frame counter increments)
7. Check if audio works (APU register writes)

### Port-a-Game 6-Step Workflow
1. Identify the exact game (title, region, revision, hashes)
2. Fetch the framework (pin matters)
3. Teach the recompiler about the game
4. Generate the code (check for dispatch misses)
5. Build and run
6. Fix the right layer

### What Does Done Mean?
Booting is not done. A useful bring-up checklist: file identity works, title screen reaches, input works, gameplay reachable, saves/loads work, audio behaves, menus work, dispatch misses resolved, compared against reference.

---

### Iterative Testing Procedure

After any `src/game.toml` change (display name, discovery, seeds):

1. **Rebuild**: the cyc project reconfigures on ROM/config change — `cmake --build build-cycle -j$(nproc)` (each revision gets its own generated dir; old ones can be discarded with the build dir).
2. **TAS**: `python3 tools/tas/run_tas.py --tas tas/sdb_4976.tas.json --frames 400 --out tas/runs/rXXX` + `diff_tas.py` — liveness 400/400 exit 0, state MATCH (mirrors masked), images informational.
3. **Engine changes** (submodule bumps): add `python3 tools/blargg/run_blargg.py --tier cpu` — 25/25 (20 PASS + 5 screen-confirmed) required; any flip blocks.

**Known baseline**: cyc 400 TAS frames → team-select, state byte-exact vs Mesen (RAM/OAM/palette/CIRAM, mirrors masked); blargg 25/25.

---

## Backend Decision Record (2026-10-02: cycle default, legacy retired)

The cycle backend runs the stock ROM flawlessly (team-select matching Mesen, 25/25 blargg) with zero game-specific work. The legacy function-level runner is retired: `src/extras.c`, `src/CMakeLists.txt`, `patches/`, `tools/input/`, `tools/blargg/{blargg_extras.c,selftest}`, `tools/tas/{emit_script.py,edgewatch.lua}` removed (backup: `/tmp/legacy_backup/` + git history). Game knowledge (NMI dispatcher, tunnel mechanics, input path) lives on in research §§18–19 and transfers to mod work. `setup.sh` no longer applies patches or builds legacy; `./build_and_run.sh` targets cycle.

## Upstream Proposal: 002 S-latch guard (to send, then lapse the rest)

- **Symptom**: interpreter RTS after native JSR lifted S +6..+48/call, clobbered `$0106` to `$90` (polls died), corrupted A into the MMC1 bit-bang (bank-8 `$8003` miss), BRK slides (`$0001`/`$00A2`/`$0600`).
- **Root**: native C calls push no 6502 return address; the `$86C2 JMP $07B4` RAM trampoline runs via interp dispatch whose terminal RTS pops 2 live-caller bytes per trampoline (3/frame).
- **Fix** (`002_interp_ram_dummy`): in `nes_interp_dispatch_bank`'s RAM/SRAM path, pre-push a dummy 6502-stack pair before `interp_run`, strip on exit if untouched — terminal RTS consumes dummies, S restored exactly.
- **Evidence**: post-fix `$0106` clean, balanced S, 0 misses, 0 BRKs (research §19.1). Patches 003 (companion clamp), 004 (game-specific tunnel), 005 (watchdog taste) lapse.
- **Status**: [ ] proposed upstream; on acceptance or legacy-drop, delete local copies (already removed from tree).

## Enhancement Roadmap

Principle from SMB (reference implementation): rendering never changes guest edges — game logic runs stock, the host re-draws. All three items build on `HOST_EXTRAS` + `[[mod_function_hook]]` (content-keyed, no game bytes in repo).

1. **Widescreen** (supported mechanism, real work): custom compositor (256 → 426/560/854, pillarbox fallback for menus), stock center pass authoritative, margins re-drawn from observed world + captured sprites. Port the SMB pattern.
2. **Flicker fix** (no toggle exists anywhere — Method 2 strips sprites in-game): locate the OAM-cull routine → hook it → capture pre-cull OAM → draw host-side → suppress native flicker slots. Same machinery as widescreen; one project, two consumers. Note: the cycle PPU implements real 8-sprite evaluation, so keeping sprites in OAM is not enough — host-side drawing is the robust fix.
3. **Resolution** (honest limit): wider canvas, same chunky pixels. No upscaling path upstream (CHR PNG replacement only). Don't promise HD.

## Removed Legacy Action Items (retired with the backend — kept as a struck record)

- ~~Mode-menu confirm divergence (legacy transition pacing)~~ — moot on cycle.
- ~~`func_DB4E` load-bearing, `func_D98A` NMI path, `func_FE34`/`FE8A` checks, OAM-fix extension, TCP server in `extras.c`~~ — legacy-code questions; extras.c deleted.
- ~~C-stack lap growth / `merge_range` dedup~~ — legacy codegen shape; cycle blocks don't lap.
- ~~`game.discovery.toml`, `known-issues.md` creation~~ — superseded (seeds replace discovery tuning; this plan is the issues list).
- Kept: TAS full-19k storage policy; `fm2` vectors if any appear; `nestest` entry-point work (all backend-agnostic).
...[truncated 1583 chars]