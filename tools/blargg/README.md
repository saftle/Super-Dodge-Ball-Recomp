# blargg harness — test-ROM suite for runner patches

Test ROMs by Shay Green (blargg), via the `christopherpow/nes-test-roms`
collection. The ROMs ship no license file and carry no per-ROM terms;
like every emulator project, we use them strictly as local test inputs:
they are never committed (gitignored, fetched per-user by `setup.sh` /
`--fetch`), and neither recompiled output (`build-blargg/`) nor run
artifacts leave the machine. This repo's own license (`LICENSE.md`,
PolyForm Noncommercial) covers the harness code.

Recompiles blargg CPU test ROMs with NESRecomp, builds them against the
**current runner working tree** (including `patches/*.patch`), runs each
headless, and reports the blargg `$6000` verdict. Re-run after any runner
change to diff for regressions.

## Result protocol

From `instr_test-v5/readme.txt` ("Output at $6000"), confirmed against the
shipped readmes, not from memory:

- `$6001-$6003` = `$DE $B0 $61` — magic: a blargg test owns `$6000+`
- `$6000` = `$80` — running; `$81` — reset requested (v1 notes it, does not
  service it: no runner soft-reset API exists yet)
- `$6000` = `$00-$7F` — completed; `0` = PASS, else FAIL with that code
- `$6004+` — NUL-terminated ASCII detail text (copied into the JSON)

`blargg_extras.c` observes this two ways and prints one `[BLARGG] {...}`
JSON line per verdict (exit 0/1/2 = PASS/FAIL/TIMEOUT). The load-bearing
path is a wall-clock poller thread (50ms): blargg CPU tests run to
completion on the main thread with zero frames and park in the shell's
`forever` loop (`SEI/LDA#0/STA $2000/BEQ-self`, NMI off), so polling only
at frame boundaries in `game_post_nmi` would never observe the verdict —
that exact bug produced a false all-HANG sweep before the thread existed.
No verdict before the frame budget (`BLARGG_MAX_FRAMES`, default 3600)
expires = TIMEOUT. A `[HANG]` watchdog dump = HANG. The JSON also carries
`misses`/`brks` (dispatch-miss and BRK counters) for regression diffing.

## Why a separate extras file

`src/extras.c` is game-specific (NMI tunnel, `$FCA0` replace, mask
translate). This harness is patch-neutral by design:

- tunnel site left at 0 (patch 004 cannot fire — a PASS proves it is inert
  by default),
- no PPUMASK/PPUCTRL forcing, no `g_ppumask_translate`,
- no dispatch overrides, no replaced functions — every target resolves via
  normal discovery + the interp fallback guarded by patches 002/003.

## ROMs

`manifest.json` pins exact paths in `christopherpow/nes-test-roms @ master`
(filenames verified via GitHub API 2026-10-01). Tier `cpu` is the
patch-impact signal — read as verdict *tuples* (see below), not as an
all-PASS gate: several rows fail on documented backend gaps and still
detect regressions through code/detail/miss/BRK changes. Tier `info`
(currently `ppu_vbl_nmi`) is reported but non-blocking. The default gate
builds mapper-0 NROM only; anything else is SKIP with a reason.

```bash
python3 tools/blargg/run_blargg.py --fetch        # clone into external/nes-test-roms
python3 tools/blargg/run_blargg.py --list          # manifest + presence + headers
python3 tools/blargg/run_blargg.py --tier cpu      # the patch-impact signal
python3 tools/blargg/run_blargg.py --only instr_01 # subset
python3 tools/blargg/run_blargg.py --rom path.nes --name label  # ad-hoc ROM
python3 tools/blargg/run_blargg.py --any-mapper    # also build banked mappers
```

Measured 2026-10-01: 20 manifest ROMs are mapper-0 NROM (16
`instr_test-v5` singles, `cpu_timing_test6`, 3 `branch_timing_tests`); 4
are mapper-1 MMC1 (`official_only`, `all_instrs`, `instr_timing`,
`instr_misc`), 1 is mapper-3 CNROM (`cpu_dummy_reads`), and `ppu_vbl_nmi`
(info tier) is mapper-1. The default gate builds NROM only and SKIP-marks
banked mappers; `--any-mapper` builds them with a minimal `game.toml`
(no `bank_switch` prophecy — diagnostics only, expect misses/hangs on
unfamiliar bank-switch sequences).

No downloads needed to prove the harness itself works — the synthetic canary
covers the full pipeline (recompile → build → run → verdict):

```bash
python3 tools/blargg/selftest/make_canary.py
python3 tools/blargg/run_blargg.py --rom tools/blargg/selftest/canary.nes --name canary
# expected: PASS, frames=10, misses=0, brks=0
```

Full run writes `build-blargg/results.json` including `patch_state`
(nesrecomp HEAD, worktree dirt, per-patch applied check). Exit 0 iff every
`cpu` test that ran reports PASS; 2 if nothing ran.

## Reading results (verdicts are tuples, not just PASS/FAIL)

A FAIL/HANG row still carries regression signal — compare the full tuple
across runs, not just the status word:

- FAIL: `code` + `detail` text + `misses` + `brks`. A patch regression inside
  an already-failing test shows up as a changed code, changed detail, or
  new misses/BRKs (all byte-compared in the A/B procedure below).
- HANG: `[HANG]` frame/ops in `tail`. Same stall frame (± wall-clock ops
  noise) = same execution prefix; a moved stall frame is a regression flag.
- Sensitivity grows as the backend improves: every FAIL→PASS conversion
  (e.g. future JSR/RTS-fidelity work) turns a coarse row into a strict
  PASS detector. Until then, tuple comparison is the instrument.

## What this suite covers (and what it doesn't)

A blargg run certifies **whatever is in `external/nesrecomp` right now**
(runner + recompiler sources) **plus `patches/*.patch`**, against CPU-core
behavior. It re-regenerates per-ROM `game.toml`s itself and links its own
`blargg_extras.c` — so it is *blind* to game-specific changes by design:

| Change | Covered by blargg? | Covered by what instead? |
|---|---|---|
| `external/nesrecomp` sources (runner, recompiler) | Yes — rebuilds + reruns everything | — |
| `patches/*.patch` applied/removed | Yes — `patch_state` records it per run | — |
| New recompiler directives for blargg shapes (`merge_range`, `force_interp`) | Yes — verdict deltas show it | — |
| `src/extras.c` (Dodgeball hooks) | No (harness uses its own extras) | `smoke` + frame compare + input scripts |
| `src/game.toml` (Dodgeball discovery) | No (own minimal tomls) | regen coverage + `smoke` + frame 11 |
| Gameplay regressions in Dodgeball | No | title/team-select playthrough checks |

In short: blargg guards the shared engine; the game's own checks guard the
game. A runner patch needs both green (engine suite + Dodgeball smoke).

## Patch A/B procedure (manual — never auto-reverts your tree)

This suite gates *engine change*, not just the current patch stack — a
distinction that matters once patches land upstream and the local stack
empties. The permanent use is regression-gating every engine change:
submodule bumps (`git checkout origin/master` pulls unreviewed upstream
code), recompiler-directive edits, and any future local patch. A run
answers "did the engine move under us?" in minutes; `patch_state` + the
verdict tuples say exactly what moved. Run it before and after every
engine change, forever — the patch list inside is incidental.

1. Run with patches applied: `python3 tools/blargg/run_blargg.py --tier cpu`
   and keep `build-blargg/results.json`.
2. For the pristine baseline: `git -C external/nesrecomp stash` (stashes the
   applied patches; the pin itself is pristine upstream), rebuild, re-run.
   Restore with `git -C external/nesrecomp stash pop` and rebuild again so
   binaries match the tree. (Manual, never auto-reverts your tree.)
3. `setup.sh` produces this same state on a fresh submodule checkout
   (`checkout origin/master` + `git apply` of `patches/*.patch`).
4. Diff the two `results.json` files: any `cpu` PASS→FAIL flip, changed
   FAIL code/detail, new dispatch misses/BRKs, or moved HANG stall frame
   is a patch regression.

## Known limitations (v1)

- `nestest` excluded: its automation entry is `$C000`, not RESET (needs an
  entry-point override or input driver — future work).
- `$81` reset requests are noted (`reset_seen`) but not serviced.
- No screen-text fallback: pre-`$6000`-protocol ROMs report TIMEOUT.
- Banked mappers need `--any-mapper` and a minimal `game.toml` (no
  `bank_switch` prophecy); unfamiliar bank-switch sequences may miss/hang.
- Controller-input tests (`read_joy3`) excluded.
