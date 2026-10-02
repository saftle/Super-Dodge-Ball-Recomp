# blargg harness — test-ROM suite on the cycle backend

Test ROMs by Shay Green (blargg), via the `christopherpow/nes-test-roms`
collection. The ROMs ship no license file and carry no per-ROM terms;
like every emulator project, we use them strictly as local test inputs:
they are never committed (gitignored, fetched per-user by `setup.sh` /
`--fetch`), and neither build trees (`build-cyc-blargg/`) nor run
artifacts leave the machine. This repo's own license (`LICENSE.md`,
PolyForm Noncommercial) covers the harness code.

Each manifest ROM gets its own cycle-backend build (upstream `runner/cyc`
project integration, HEADLESS) and runs headless; the verdict comes from
the shell's on-screen result text in low RAM (`$0201+`, NUL-terminated:
`"Passed..."` / `"Failed..."`), read back via `--mem-frame`/`--mem-out`
(NROM has no `$6000` work RAM on this backend). No extras, no patches.
Re-run after any framework change to diff for regressions.

## Result protocol

The instr-test shell prints its result text near `$0200` in several shapes
(`Passed<suffix>` at `$0200/$0201`, `All N tests passed` elsewhere in the
window, sometimes with binary debris in front). The harness searches
`$0200-$02FF` for a verdict-shaped string instead of assuming an offset:

- `Passed...` (and no `fail`) → PASS (detail = text)
- `...fail...` → FAIL (detail = text)
- nonzero exit, no text → ERROR
- wall-timeout expiry → HANG (stalled)

Some tests (branch timing, `cpu_dummy_reads`, `cpu_timing_test6`) render
their verdict to the screen only — no RAM text exists. Every run also saves
`shot.png`, and text-less runs report UNVERIFIED (screenshot path in
detail, non-blocking like info tier) instead of a wrong TIMEOUT. All five
were confirmed PASSED by reading their screenshots (branch_1/2/3,
dummy_reads; timing_test6 needs ~16 s / 1000+ frames to finish).

Each row also carries `misses` (uncompiled-seed lines from `--miss-log`)
and `native_pct` (share of cycles in recompiled code) for regression
diffing. Seeds are informational here, not failures.

## ROMs

`manifest.json` pins exact paths in `christopherpow/nes-test-roms @ master`
(filenames verified via GitHub API 2026-10-01). Tier `cpu` is the engine
signal. Tier `info` (currently `ppu_vbl_nmi`) is reported but non-blocking.
Only mappers in the cycle set (0, 1, 2, 3, 4, 7, 66) build; anything else is
SKIP with a reason (`--any-mapper` overrides for diagnostics).

```bash
python3 tools/blargg/run_blargg.py --fetch        # clone into external/nes-test-roms
python3 tools/blargg/run_blargg.py --list          # manifest + presence + headers
python3 tools/blargg/run_blargg.py --tier cpu      # the engine signal
python3 tools/blargg/run_blargg.py --only instr_01 # subset
python3 tools/blargg/run_blargg.py --rom path.nes --name label  # ad-hoc ROM
```

Measured 2026-10-02: `instr_01_basics` PASS (100% native). Earlier manual
runs: `01-basics` PASS (100% native), `12-jmp_jsr` PASS (99.4% native) —
the JSR/RTS stack-fidelity case the function-level backend fails.

Full run writes `build-cyc-blargg/results.json` including `patch_state`
(framework HEAD, worktree dirt; patches touch the legacy runner only, so
informational here). Exit 0 iff every `cpu` test that ran reports PASS;
2 if nothing ran.

## Reading results (verdicts are tuples, not just PASS/FAIL)

A FAIL/HANG row still carries regression signal — compare the full tuple
across runs, not just the status word:

- FAIL: `detail` text + `misses` + `native_pct`. A regression inside an
  already-failing test shows up as changed text, new misses, or moved
  native share.
- HANG: wall-timeout expiry with `tail`. Same stall point across runs =
  same execution prefix; a moved stall is a regression flag.
- Sensitivity grows as the backend improves: every FAIL→PASS conversion
  turns a coarse row into a strict PASS detector.

## What this suite covers (and what it doesn't)

A blargg run certifies **whatever is in `external/nesrecomp` right now**
(recompiler + cycle backend sources) against CPU-core behavior. It builds
each ROM with no game config — so it is *blind* to game-specific changes
by design:

| Change | Covered by blargg? | Covered by what instead? |
|---|---|---|
| `external/nesrecomp` sources (recompiler, cycle backend) | Yes — rebuilds + reruns everything | — |
| Submodule bumps (unreviewed upstream code) | Yes — verdict deltas show it | — |
| `src/game.toml` (Dodgeball discovery/display) | No (no game config) | TAS state match + title check |
| Gameplay regressions in Dodgeball | No | TAS runs to team-select |

In short: blargg guards the shared engine; the TAS runs guard the game.
An engine change needs both green.

## Known limitations

- `nestest` excluded: its automation entry is `$C000`, not RESET (needs an
  entry-point override or input driver — future work).
- Verdicts come from screen text, not the `$6000` protocol (no WRAM on NROM
  here); a ROM that never writes text reports TIMEOUT.
- Controller-input tests (`read_joy3`) excluded.
- Per-ROM configure+build takes ~1 min; the full cpu tier parallelizes over
  `--jobs` (default: all cores).
