# Super Dodge Ball Flicker Analysis Toolkit

Analysis scripts and reproduction vectors for the two independent sprite-flicker
fixes in `src/sdb_host_extras.c`:

1. **OAM truncation** — the game stops emitting at 64 slots (`$84B6` `BEQ` bail),
   dropping 8-sprite characters. Fixed by capturing all emitted sprites and
   presenting them host-side.

2. **Object culling** — the game builds a display list at `$067C` and deliberately
   leaves one object off each frame (`$E34A` gate + `$828A` gate + `$8200` gate),
   rotating which character. Fixed by overriding all three gates.

All three fixes ship as the opt-in `super-dodge-ball.character-flicker` mod (off by default; pass `--remove-character-flicker` headless).

---

## Scripts

| Script | Purpose |
|--------|---------|
| `records.py` | 13-object display record table (`$0618` + 7×index), per frame |
| `objrec.py` | Per-object state record (`$031F` + 56×index), per frame |
| `slotdiff.py` | OAM dumped slot-by-slot per frame, with change marks |
| `blink.py` | Enemy B's 5-on/4-off blink: OAM presence plus the suspect RAM |
| `bink.py` | Non-background pixel count in a character's on-screen box, from `--present-out` PNGs |
| `memdiff.py` | Diff two `--mem-out` dumps section by section (cycles, RAM, OAM, CIRAM, palette) |
| `refs.py` | Every 6502 instruction in the PRG referencing a given absolute address |
| `emit_sites.py` | OAM cursor-bump sites in the ROM (`LDA $0617…` / `LDX $0617…` shapes, wrap-bail noted) |
| `oam_scan.py` | All loads/stores touching `$0200`–`$02FF` (OAM page) or `$0300`–`$03FF` |
| `oam_analyze.py` | Per-frame OAM census from a CYCFRAME log: active sprites, peak sprites/line |
| `truncation.py` | Which character loses tiles to the 64-slot budget, and whether the cut moves |
| `char_split.py` | Group OAM slots into characters, report completeness |
| `drop_report.py` | Per-frame proof of the hardware 8-sprites-per-line drop |
| `frame_diff.py` | Frame-by-frame pixel diff between two screenshot runs |
| `disbank.py` | Bank-aware disassembly helper |
| `zprefs.py` | Sprite priority (z-order) analysis |
| `spikes.json` | Frame numbers where the runaway capture list spiked (evidence for the trail-bug fix) |

---

## Vectors (`tas/vectors/`)

| Vector | Purpose |
|--------|---------|
| `sdb_idle.input` | Title → team-select → match, then idle. Reproduces both flickers. |
| `sdb_throw.input` | Same, then spam **B** (throw) on a 12-on/12-off cycle from frame 446. Exposes the runaway capture list. |

---

## Quick Reproduction

```bash
# Flicker reproduction (all fixes)
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" \
  --frames 1100 --input tas/vectors/sdb_idle.input --ram-init zeros \
  --mods-root build-cycle/mods --remove-character-flicker \
  --present-out tmp/flicker/on_%05d.png --present-every 1

# Runaway capture list stress (the trail bug)
SDB_TRACE=1 ./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" \
  --frames 8000 --input tas/vectors/sdb_throw.input --ram-init zeros \
  --mods-root build-cycle/mods --remove-character-flicker
```

**Set `SDB_TRACE=1` on every mod run.** It prints one line per frame:
`frame peak-list overflow published-list saw-$04 backstop-trips`

A healthy frame is tens of sprites. Watch the peak-list column, not the pictures.