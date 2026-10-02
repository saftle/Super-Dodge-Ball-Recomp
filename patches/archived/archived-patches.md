# Archived patches (retired with the cycle switch — see `../README.md`)

All patches below are dormant: they target the retired legacy runner and
`setup.sh` no longer applies anything. Kept as a record with revival
conditions (now historical — revival would mean reverting the cycle
switch). To revive, the old mechanism was moving the file back to
`patches/` for the setup glob; that mechanism no longer exists.

## 001_nmi_tail_unwind.patch (archived 2026-10-01)

**What:** after `game_run_nmi()`, rewind registers/stack only when `S` still
holds the entry value (plain-RTI case). On unwind-detect (`g_rti_target==0`,
`S` moved — RTS-tail/TXS shape), pop the skipped RTS return and resume the
guest there at frame end via `runtime_request_guest_resume`; fall back to
rewind when the popped target is not ROM (`< $8000`).

**Why written:** the `$FCB3: BNE $FCB3` NMI-wait spin exits only via the
`$FF4C` NMI-off tail (`PLA×5, STA $0100, PLA, RTS`), whose stack unwind is
ignored for control flow because native RTS is a C return (see
`docs/research.md` §15).

**Why archived:** never fired successfully end-to-end. The game-side fix
(`[[replace_func]]` for `$FCA0`, spin removed, `$0100` untouched) bypasses
the tail path entirely, so the branch is dormant — zero `[UNWIND]` lines in
any working build, pixel-identical output with/without it. An unverified
patch in the NMI path is liability without payoff.

**Evidence gathered while active:**
- NMI-termination survey (SMB3, SMB1, Zelda, Dr. Mario, Metroid, Faxanadu:
  all RTI-balanced → branch dormant; only the FDS BIOS itself uses an
  RTS tail). Full table: `docs/research.md` §17.
- Live A/B on Super Mario Bros. 3 (USA) (MMC3): 10/10 frames, byte-identical
  JSON (hashes, misses) with and without the patch.
- Our build: pixel-identical title (99.8% vs Mesen 11) with and without it.

**Revive if:** the `$FCA0` replace is ever reverted (tail runs again), a later
game phase hits a tail path (e.g. via the `func_FCA9`/`FCAA` mid-function
spin copies reachable through dispatch), or upstream wants the FDS-tail
emulation with a live reproducer. (Revival mechanism retired with the cycle
switch; kept as design record.)

## 002_interp_ram_dummy.patch (retired 2026-10-02, was active 2026-10-01)

**What:** in `nes_interp_dispatch_bank`'s RAM/SRAM path, pre-push a dummy
6502-stack pair before `interp_run`, and strip it on exit if untouched.
Native JSR/JMP sites are C calls that push no return address, but RAM code
routinely ends in RTS (Super Dodgeball's `$07B4` STA-trampoline:
`JMP $07B4` executes `STA $40xx` + RTS). Without the pair the terminal RTS
pops two bytes of live caller data (+2 S per call).

**Why it mattered:** the sound tick (`$FA00` → bank-0 cluster → `$86C2 JMP
$07B4`, 3 executions/frame) lifted S by +6/NMI. S wrapped into the `$0100`
page within ~18 frames, stack pushes clobbered `$0106` to `$90`, the `$EF9F`
BMI-skip self-sustained, controller polls stopped. Follow-on damage (all
downstream of the S drift): intermittent `$8003` bank-8 dispatch miss
(MMC1 sequence entered with corrupted A), `BRK $0001`/`$00A2`/`$0600`
slides, `$90` in `$0100`, C-stack growth from skipped-RTS control flow.

**Evidence:** `func_FA00` no-op experiment → S perfectly balanced; per-
instruction S-delta trace → +2 single-steps at `$812A` with no intermediate
push/pop. Post-fix: `$0106` clean, 0 dispatch misses, 0 BRKs.

**Blast radius:** RAM/SRAM interp entries only; ROM dispatch untouched.
Other games using RAM trampolines benefit; all others unaffected.

**Afterlife:** the one patch proposed upstream (real legacy bug, not game-
specific) — see `docs/plan.md`. Lapses otherwise.

## 003_interp_rom_clamp.patch (retired 2026-10-02, was active 2026-10-01)

One-sided S-clamp on the ROM-miss fallback path: snapshot S at interp entry;
on a handled exit, erase net pops, never touch net-push runs. Companion to
002 (same phantom-RTS class, ROM targets): the `$8E00` bank-6 state machine
lifted S +2 per call (`$FF`→`$01`), cascading into bank thrash → `$C000`
BRK → hang. Post-fix 240/240 frames clean. Lapses with 002's decision.

## 004_nmi_tunnel_unwind.patch (retired 2026-10-02, was active 2026-10-01)

Opt-in NMI-tunnel unwind (generic mechanism, game-specific arming via
`g_tunnel_site`/`g_tunnel_armed`/`g_tunnel_buf`): a TXS-site marker reached
while armed, at the site, inside NMI resets vblank depth and longjmps to
the main-context landing, which runs `$F09F` natively (C-calling it from NMI
freezes frames). Evidence: `[TUNNEL]` at frame 10, then 30/30+ frames, 0
misses. Game-specific (needs `src/extras.c` companion pieces, deleted);
lapses. Applicable pattern for any NMI-tunnel game, documented in research
§19.2.

## 005_hang_watchdog_default.patch (retired 2026-10-02, was active 2026-10-01)

Hang watchdog on by default (90 s without a completed frame; override/
disable via `NESRECOMP_HANG_TIMEOUT_S`). The `[HANG]` dump (C stack +
dispatch ring) was the primary hard-hang diagnostic. Taste call; lapses.
