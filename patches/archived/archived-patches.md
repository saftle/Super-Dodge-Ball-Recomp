# Archived patches

`setup.sh` deliberately ignores `patches/archived/` (it only globs `patches/*.patch`).
Dormant patches: written, tested dormant, shelved — with the exact conditions to revive them. To revive, move the file back to `patches/` (the glob picks it up).

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
emulation with a live reproducer. Move the file back to `patches/` (the
`setup.sh` glob picks it up automatically).
