# Super Dodgeball NES Recompilation — Project Plan

> Cycle backend is the default and only play path. Team-select verified byte-exact vs Mesen; blargg 25/25. Flicker fix **shipped** for all causes (opt-in mod until a full playthrough is tested). PPU overclocking **shipped** (opt-in mod; deliberately changes timing). Widescreen **shipped** (opt-in mod, 16:9 / 21:9 / 32:9). Frontier: full-movie TAS vectors (level-switch screens), then sprites in the widescreen margins.

## Where Things Stand

- **Backend**: cycle-accurate (`./build_and_run.sh` → `build-cycle/nes_game`, upstream `runner/cyc`). Stock ROM on accurate timing. Three game-specific mods live in `src/sdb_host_extras.c`: the character-flicker mod (truncation half presentation-only; the culling half makes the guest emit sprites it had dropped, so its timing shifts slightly), the PPU-overclocking mod (deliberately changes console timing so the guest runs faster) and the widescreen compositor (presentation only; the guest is never touched). VSync is a launcher Graphics setting (patch 007), not a mod. The legacy runner is retired (files removed 2026-10-02, backup in `/tmp/legacy_backup/` + git history; game knowledge preserved in research §§18–19).
- **Verification**: TAS 400 frames reach team-select with RAM/OAM/palette/CIRAM byte-exact vs Mesen from boot (`tools/tas/`); blargg cpu tier 25/25 (20 RAM-text PASS + 5 screen-confirmed).
- **Upstream**: maintainer is moving to cycle-default; legacy goes antiquated. Our migration is done early. Patch 006 (PPU overclocking for MMC1) is the live upstream proposal — see `patches/active-patches.md`. The legacy 002 S-latch guard is drafted below.
- **Frontier**: court BG past team-select is unverified on cycle.

## Open Items
1. Court BG past team-select — substantially verified on cycle: `tas/vectors/sdb_throw.input` plays matches end-to-end (throws land, digits show, menus render correctly, 8000-frame stress clean). What remains is the full-movie TAS path (TASVideos #4976 actually *wins* rounds — needed for level-switch screens), still open.

2. Propose 002 S-latch guard upstream (draft below), and 006 PPU-overclock MMC1 gate (see `patches/active-patches.md`).

3. ~~Widescreen~~ — **DONE.** `super-dodgeball.widescreen`, opt-in, off by default. No engine patch: the cycle backend already ships the widths (`common/nes_video_geometry.h` — 426/560/854) and the compositor API. One compositor in `src/sdb_host_extras.c` fills the margins from the nametable, CHR, palette and the frame's own per-line scroll; the stock 256 columns are copied back over the centre so they stay pixel-identical to the unmodded game, and a per-frame seam check against `cyc_render_background()` pillarboxes anything that fails (so title screens and menus fall back for free). Reconstruction agrees with the machine to 1-2 px of 61440. Second feature `overscan` reveals the 8 blank left-hand columns the game hides with PPUMASK (blank in 2112/2112 measured frames); vertical is not offered because one wipe-transition frame uses those bands. Margins past the court's own 512-px world are backdrop. The four bugs the seam check caught, the wideNES comparison and the overscan measurement are in research.md §25.
4. **Sprites in the margins** (the one part of widescreen not done). OAM X is a byte and camera-relative, so the sprite list alone cannot place anything outside the viewport. The display records at `$0618` are camera-invariant across a pan (measured: unchanged while OAM shifts 7 px with the court), so world-space positions are in there — the byte is not identified yet. `cyc_render_sprites()` with a placement callback is the mechanism; wideNES cannot do this at all, which is why it is worth doing here.

5. ~~Flicker fix~~ — **DONE, all causes.** `super-dodgeball.character-flicker`, opt-in from the launcher's Mods screen (off by default: the culling half shifts guest timing and the level-switch screen shows ghosts with it on — needs a full playthrough test). (a) Hard 64-sprite OAM budget with a `BEQ` bail on `$84B6` — stops emitting. (b) Display-list culling at `$067C` (`$43` bits 4-6 `$20`), three gates overridden. (c) Two sprite classes no bump hook could see: floating damage numbers (bank-3 pair emitter, `ADC #$08` step) and team-select position numbers (bank-4 block copier, no bump at all) — hooked per-emitter plus prefix carry-over and done-freshness for sitters. Ten content-keyed `[[mod_function_hook]]` sites + host-side redraw + six `cycle_seed_file` roots (hook sites need recompiled code; all three emitters were on the interpreter). Truncation: body-ink stdev 8.7 → 0.8 (truncation-only build). Culling: B's box-ink stdev 55.0 → 16.6 with 0 frames still culled (re-verified current binary). Numbers: 108/108 frames steady. Traps and the guest-cost caveat in research.md §2 and §2.1.

6. **Run the gates on patch 006 before it lands.** A submodule change is exactly when both are wanted: TAS 400 + `diff_tas.py` (must stay MATCH) and blargg cpu 25/25. Not yet run — AGENTS.md says ask first. The patch is strictly additive and the 600-frame stock run is byte-identical with and without it, but that is not the same as the full gates.

7. ~~PPU overclocking~~ — **DONE.** `super-dodgeball.ppu-overclock`, opt-in, off by default (it changes console timing, so hardware parity must stay the default). Integer `[[option]]` so the launcher exposes the count. Needs patch 006; without it the backend refuses on mapper 1. Measured: 2.0x cycles/frame, and the guest populates its display list on 565 of 800 frames vs 316 stock. Research section 23.

8. ~~VSync~~ — **DONE as a launcher setting** (it started as a `super-dodgeball.vsync` mod and moved: vsync is host-global, and recomp-ui already had the Graphics row waiting behind `has_vsync`). Pre-boot Graphics → VSync toggle, persisted per-player in `config.ini` `[Display]`; needs patch 007. Presentation only — guest clock untouched. Windowed `--exit-after 60` exit 0 both ways via the config key, and the launcher path is confirmed (Graphics → VSync → Play prints the vsync line). Research section 24.

9. Resolution honesty: wider canvas only — no upscaling path exists upstream (CHR PNG replacement at most). Don't promise HD.

10. Docs reconciled to cycle (README, AGENTS.md, plan, research §§21–25, harness READMEs); research §§18–19 stay as game-knowledge history.

11. **Flicker follow-ups** (not blocking): the mod draws sprites with no scanline evaluation at all, so it also drops the NES 8-per-scanline rule — real hardware behaviour some may want kept, which would be an `[[option]]` on the same feature rather than a second package. The blit also ignores OAM attribute bit 5 (behind-background priority); this ROM never sets it, but a future change would need the coverage buffer.

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

Principle from SMB (reference implementation): rendering never changes guest edges — game logic runs stock, the host re-draws. All items build on `HOST_EXTRAS`; only the flicker fix needed `[[mod_function_hook]]` sites (content-keyed, no game bytes in repo).

1. ~~**Widescreen**~~ — **SHIPPED** (`super-dodgeball.widescreen`, off by default, opt-in). Custom compositor, 256 → 426/560/854, stock centre copied back so it stays byte-identical, per-frame seam check pillarboxes anything that fails. **The SMB pattern was not what shipped**: SMB re-runs the game's own area parser in isolation and caches the world, because SMB's world does not fit in its nametables. This game's world *does* — a 512-pixel looping stadium that is byte-identical across a whole match — so the margins are read straight out of the nametable, with no world cache, no isolated guest call and no new hook sites. wideNES is the same story from the other side: it warps remembered pixels, so it can only show what the guest already drew and cannot do sprites at all. Research.md §25.
2. **Flicker fix — SHIPPED** (`super-dodgeball.character-flicker`, off by default, opt-in). Three measured mechanisms, none of them the emulator sprite limit: (a) hard 64-sprite OAM budget with a `BEQ` bail on `$84B6` (79% of match frames truncate); (b) display-list culling, three gates; (c) two emitter shapes no bump hook sees (damage-number pair step, block copier with no bump). Ten content-keyed `[[mod_function_hook]]` sites + host-side redraw + six seed roots. Truncation: body-ink stdev 8.7 → 0.8. Culling: 55.0 → 16.6, 0 frames culled. Numbers: 108/108 steady. Guest machine: truncation half untouched (same cycle count; `$02FD`/`$02FF` only); culling half shifts NMI phasing slightly by construction. Traps in research.md §2/§2.1 and AGENTS.md.
3. **Resolution** (honest limit): wider canvas, same chunky pixels. No upscaling path upstream (CHR PNG replacement only). Don't promise HD.
4. **Sprites in the widescreen margins** — the open end of item 1, tracked as open item 4 above.

## Removed Legacy Action Items (retired with the backend — kept as a struck record)

- ~~Mode-menu confirm divergence (legacy transition pacing)~~ — moot on cycle.
- ~~`func_DB4E` load-bearing, `func_D98A` NMI path, `func_FE34`/`FE8A` checks, OAM-fix extension, TCP server in `extras.c`~~ — legacy-code questions; extras.c deleted.
- ~~C-stack lap growth / `merge_range` dedup~~ — legacy codegen shape; cycle blocks don't lap.
- ~~`game.discovery.toml`, `known-issues.md` creation~~ — superseded (seeds replace discovery tuning; this plan is the issues list).
- Kept: TAS full-19k storage policy; `fm2` vectors if any appear; `nestest` entry-point work (all backend-agnostic).
...[truncated 1583 chars]