# PPU overclocking oracle (`tools/overclock/`)

Measures whether **PPU overclocking** makes Super Dodge Ball run faster, against
MesenCE as the oracle. Backs `docs/research.md` §23.

## What this measures

`Nes.PpuExtraScanlinesBeforeNmi` grants the 6502 extra cycles per frame *before* its
NMI. It renders no extra picture (always 256×240) — it is a speed feature. Super
Dodge Ball's main loop is a NMI-synced spin (`$FCA0` parks waiting for the next NMI,
research.md §19.3/§19.6), so extra cycles per frame means more work done per frame.

Result at `Before NMI = 300`: **+12.0 extra `$069E` work-ticks per frame**, and the
lead grows over time (+7.3 → +17.5 ticks/frame across the run), which distinguishes a
real speedup from a phase shift.

## Usage

```bash
# Two runs: stock and overclocked, same length.
./tools/overclock/run_overclock.sh 0   1200 stock
./tools/overclock/run_overclock.sh 300 1200 oc300

# Compare them.
python3 tools/overclock/compare_runs.py \
    tmp/overclock_run_stock/m.csv tmp/overclock_run_oc300/m.csv
```

Each run writes `tmp/overclock_run_<tag>/m.csv` and prints a self-check.

## ALWAYS check the self-check

```
cycles/frame: 29781 (expected ~29781 for before=0)
cycles/frame: 63881 (expected ~63881 for before=300)
```

If `cycles/frame` is still ~29781 for a non-zero `before`, **the setting did not apply
and the run is worthless.** The harness warns, but do not ignore it — a run where
nothing changed looks identical to a real "no effect" result.

## Three traps, all of which silently produce a false "no difference"

1. **MesenCE reads `settings.json` from the directory its binary sits in, NOT from
   `$HOME`.** Isolating `HOME` does nothing; you silently get the real user config.
   The harness copies `Mesen` + `MesenCore.so` + `lib*.so` + `settings.json` into a
   temp dir and runs that copy. Proof: `Nes.Region=Pal` moves cycles/frame 29781 →
   33248 only when `settings.json` sits beside a copied binary. (This was a live bug in
   `tools/frame_gen/run_frames.sh`; fixed.)
2. **`xvfb-run` word-splits the program path.** It execs via `sh -c`, so a path with a
   space — this repo's own path has one — fails *even when quoted*. `cd` into the
   sandbox and run `./Mesen`.
3. **After that `cd`, relative ROM paths stop resolving.** Pass an absolute path.

`PpuExtraScanlinesBeforeNmi` exists under both `Nes` and `Snes` in the shipped settings,
and the `Snes` copy comes first in the file — a naive `grep` reports the wrong console.
The harness reads it back scoped to the section.

## Validating a harness before trusting a negative

The first version of this harness set the value, got identical output, and looked like
proof that overclocking does nothing. It had changed nothing because the setting never
reached the emulator. The control that caught it: **wall-clock time**. A change that
quadruples PPU work must change how long a run takes. If a harness cannot detect a
known-positive effect, its "no effect" result means nothing.

## Files

- `run_overclock.sh` — sandbox, config, run, self-check.
- `overclock_probe.lua` — drives the menus into a real match and logs per-frame state
  (`$0100/$0106/$0107/$06B1/$06EA/$069E/$067A/$067B` plus object-block and OAM
  fingerprints). Start at 11–12, WORLD CUP at 60, SKILL at 140, CHANGE POSITION at 900.
- `compare_runs.py` — stock vs overclocked; reports the `$069E` lead by window and
  whether it grows.