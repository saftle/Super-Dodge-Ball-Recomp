# Super Dodgeball NES Recompilation — Project Plan

> Title plays through team-select; the court BG draw is the current frontier.

## Where Things Stand

- **Title**: 99.8% structural match vs Mesen frame 11 from frame 1, `smoke 30` exit 0. NMI-wait spin handled game-side via split `[[replace_func]] $FCA0` (instant pre-menu, faithful park post-tunnel); `$0100` untouched so EFD9/EF9F run every frame.
- **Input**: Start tunnels reliably to team-select (title → city → lineup → mode menu → skill → team-select, renders perfectly, music perfect, all inputs process). Verified to 400 frames with only benign `$A3C7` interp skips (LOG_RETURN-safe).
- **Frontier**: court BG draws as tile-soup past team confirm (sprites/sound/logic run; strips drain top rows only). Mode-menu confirm diverges ~TAS frame 44 of TASVideos #4976 (hardware advances to team-select, recomp stays) — next debug target (`$06B1`/`$F5` edge trace, see research §21).
- **Runner patches** `002/003/004/005` active (`001` archived dormant with revival conditions).

## Open Items

1. Mode-menu confirm divergence ~TAS frame 44 (see research §21).
2. Court BG stalls as tile-soup past team confirm (see research §19).
3. Whether post_nmi `func_DB4E` is still load-bearing for frame advancement (untested).
4. C-stack lap growth (cross-function loop laps, ~5 C-frames/frame) — slow-burn; `merge_range` blocked on recompiler dedup.
5. `docs/known-issues.md` and `src/game.discovery.toml` still to create.

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

After any code change to `src/extras.c` or `src/game.toml`:

1. **Regen (if `game.toml` changed)**: run the recompiler manually (cmake does not reliably retrigger) and confirm `[Eval] Coverage` with no parse error
2. **Rebuild**: `cmake --build build -j$(nproc)`
3. **Smoke**: `timeout 25 ./build/super_dodgeball "roms/Super Dodge Ball (USA).nes" --smoke 5 --smoke-interval 1` — must exit 0 with `frames_run` ≥ requested and 0 dispatch misses
4. **Compare**: `--save-screenshot` + `tools/compare_frames.py` vs `nes_reference/` — frame 11 structural match ≥90% (current ~99.8%)

**Known baseline**: 30 frames, exit 0, static title hash `a84564b5`, 0 misses. Menus to team-select verified via `tools/input/title_to_team_select.txt` (400 frames; only benign `$A3C7` interp skips).

---

## Detailed Action Items

### Completed Foundation (title, NMI, input, config — see research §19 for the how)

- nesrecomp submodule tracks `origin/master` (+ runner patches `002,003,004,005`; `001` archived dormant)
- Title renders via the real NMI path (`func_NMI()` gated on PPUCTRL bit 7, nested `$C0`-only policy, one-service-per-frame pacing); split `[[replace_func]] $FCA0`; Start tunnels past title (`[TUNNEL]` at frame 10) via longjmp unwind, `$F09F` native on the main context
- `game.toml` minimal by design (no `sram_map`, empty `force_interp`, dis65-verified `extra_func`, two `[[replace_func]]`); `func_RESET()` runs natively with no RAM pre-seeding
- Verification infrastructure: MesenCE reference frames + `--save-screenshot` comparison, TAS differential harness (`tools/tas/`), blargg engine suite (`tools/blargg/`), raw `--testRunner` + Lua oracle, `tools/dis65.py` + `memwatch.lua`

### Game Loop & Gameplay (frontier work)

- [ ] Mode-menu confirm divergence ~TAS frame 44 (`$06B1`/`$F5` edge trace; see research §21)
- [ ] Court BG tile-soup past team confirm (main-area rows never staged; see research §19)
- [ ] Re-verify whether post_nmi `func_DB4E` is still load-bearing (untested)
- [ ] `func_D98A` stays out of the NMI path until gameplay boots (do NOT `force_interp` blindly)
- [ ] Verify `func_FE34()` (sound) works from `game_run_nmi()`; verify `func_FE8A` via recompiled code
- [ ] Verify OAM flicker fix extends from title to gameplay
- [ ] TCP debug server in `extras.c` (port 4370): read RAM, breakpoints, registers

### Verification Follow-ups

- [ ] TAS harness: full-19k storage policy; `fm2` vectors if any appear
- [ ] blargg harness: `nestest` (`$C0
...[truncated 1583 chars]