# tools/widescreen — analysis behind research.md §25 and §26

Four scripts, all read a `--frame-log` binary or a trace, and all back a specific
claim in `docs/research.md` §25-26. Nothing here runs the emulator; make the log
first.

## Make the logs

```bash
mkdir -p tmp/ws
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 1100 \
  --input tas/vectors/sdb_throw.input --ram-init zeros \
  --frame-log tmp/ws/throw.bin --frame-log-frames 0:1099
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 1100 \
  --input tas/vectors/sdb_idle.input --ram-init zeros \
  --frame-log tmp/ws/idle.bin --frame-log-frames 0:1099
```

The logs are ~127 MiB each; `tmp/` is scratch, so regenerate rather than commit
them. Use a **copy** of the mods dir if you also pass `--mods-root`, because
`state.toml` is shared mutable state.

## `nametable.py` — why the margins can be read, not remembered

```bash
python3 tools/widescreen/nametable.py tmp/ws/throw.bin --rows 14,16,18,24,28
python3 tools/widescreen/nametable.py tmp/ws/throw.bin --stability 640 760 6,7,8,16,17,27
```

Claims it backs:

- **The court is a 512-pixel-wide looping stadium.** The 64-tile span across both
  nametables reads as `C0 C0 C0 C0 | 4E 4D … | 04 03 … | 2C 2B … | C0 C0 C0 C0` —
  backdrop, left sideline, crowd, right sideline, backdrop. That is why the
  widescreen compositor resolves margin pixels from the nametable instead of
  remembering them the way wideNES does, and why it needs no world cache, no
  isolated guest call and no hook sites.
- **The world is static for the whole match.** `--stability` reports the number
  of differing tile bytes frame to frame; it is 0 over a match, so the nametable
  a margin column needs is already correct at present time.
- **Attributes live at page offset `0x3C0`, not `$2800`.** The last line of every
  dump is the attribute region. This cart is horizontally mirroring, so `$2800`
  aliases `$2000` and the textbook attribute address reads tile bytes.

## `blank_extent.py` — how much overscan may be cropped

```bash
python3 tools/widescreen/blank_extent.py tmp/ws/throw.bin tmp/ws/idle.bin
```

Reports, per row and per column, how many frames carry a non-backdrop pixel. A
band that is 0 in **every** frame can be cropped with certainty; anything else
cannot. Measured result for Super Dodge Ball over 2112 frames with ink:

| band | frames with ink | verdict |
|---|---|---|
| columns 0–7 | **0 / 2112** | removable |
| columns 248–255 | 1372+ | content — keep |
| rows 0–15 | 2 / 2112 | frame 420 only |
| rows 232–239 | 2 / 2112 | frame 420 only |

The two are the **same frame in both vectors**: frame 420, a full-screen wipe
whose top and bottom are deliberately filled. So the vertical bands are not
lossless and the mod does not offer a vertical crop; it *reveals* the 8 clipped
left columns instead, because cutting them would splice the world (see
research.md §25).

## `objects.py` — how many objects the game culls off-screen

```bash
python3 tools/widescreen/objects.py tmp/ws/objs.bin --cull
python3 tools/widescreen/objects.py tmp/ws/objs.bin --records 610
```

Claims it backs (research.md §26):

- **The game places every sprite in a 16-bit screen coordinate and then hides
  anything outside `[4, 255]`.** `$8483` is the last instruction before the cull
  at `$8488`–`$849A`; `$84A1` is where the 16-bit X is truncated to an OAM byte
  (`OAM X = X16 - 4`). The single-sprite emitters reach the identical cull at
  `$8302`/`$8342`. Measured on the running binary: X16 = 208 lands at OAM `$CC`,
  Y16 = 82 at `$99`.
- **This is not an edge case.** Over 61 match frames, **61/61 have at least one
  object off-screen** and **293/732 objects (40.0%) are off-screen at any time**,
  with 12 objects and 6 off-screen in the worst frames. `--records <frame>` prints
  the per-object table with each object's screen column, including negative ones.

The record layout read here is the one the *builder* writes (`$E378`–`$E3A1`):
bytes 0–2 are the state byte and two selectors, bytes 3–4 a 16-bit
camera-relative X, bytes 5–6 a 16-bit absolute value. `--cull` subtracts the
camera from bytes 3–4 to get each object's screen column.

One thing `--cull` cannot show, which cost an afternoon: the record is refreshed
by the game **every frame for every object**, whether or not the object is on the
display list at `$067C` — and most off-screen objects are *not*. Over frames
600–700 of the pan vector `$8231` fires for record 2 four times and record 8 once,
and those are the two objects standing in the margins. So the record, not the
emission, is what the margin sprites are placed from, and any lifetime measured in
emissions blinks exactly those two objects out.

## `margin_track.py` — per-sprite audit of the margin sprites

```bash
mkdir -p tmp/mt
SDB_TRACE_CAPTURES=500-1200 ./build-cycle/nes_game \
  "roms/Super Dodge Ball (USA).nes" --frames 1200 \
  --input tas/vectors/sdb_sweep.input --ram-init zeros --widescreen 16:9 \
  --overscan --present-out tmp/mt/w_ --present-every 1 >/dev/null 2>tmp/mt/trace.log
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 1200 \
  --input tas/vectors/sdb_sweep.input --ram-init zeros \
  --frame-log tmp/mt/sweep.bin --frame-log-frames 0:1199
python3 tools/widescreen/margin_track.py tmp/mt/trace.log --records tmp/mt/sweep.bin
```

`SDB_TRACE_CAPTURES` prints one `sdbw` line per presented frame listing every
margin sprite the compositor painted, with the object index and the screen column
it landed on. That line is the ground truth for "what ended up in the margins",
and this reads it the way an eye cannot: one row per object, one column per frame.

| report | means |
|---|---|
| `gone` | a live record, off-screen, nothing drawn — the blink |
| `ghost` | two parts of one object within a pixel of each other on a row |
| `shift` | the object moved further than its record and the camera did |
| `part` | a wholly off-screen object holding fewer parts than it held |

Current baseline: **701 sweep frames, eleven objects, no anomalies**, at 16:9 and
32:9, with and without `--overscan`, and with the character-flicker feature on.
Objects 2 and 8 — the two the display list almost never contains — read **701/701**.

**That baseline is not the whole story, and the gap has a name.** An object
*straddling* the band is not judged by any of the four checks: `part` only fires
for objects wholly outside the viewport, and an object at screen column 5 holding
2 parts instead of 8 reads clean. That is exactly the shape of the seventh bug in
research §26 — a ghost sprite the game never emitted, drawn in the band and
clipped at column 8. The audit cannot see it; only the trace can, which is what
`oam-band:` on the `sdbw` line is for.

**Use a played session for anything you cannot schedule.** The four vectors here
are hand-written and all four missed it. `tools/play_record.sh` records one:

```bash
tools/play_record.sh bar-artifact 2000
# then, instead of this script's own replay commands:
SDB_TRACE_CAPTURES=1140-1180 nes_game ROM --config tmp/bar-artifact/config.ini \
    --virtual-pad <converted vector> --ram-init zeros --widescreen 16:9 --overscan \
    > tmp/bar-artifact/replay.log 2>&1
python3 tools/widescreen/margin_track.py tmp/bar-artifact/replay.log
```

`Player1Source = 2` is required for a `--virtual-pad` replay — player 1 is the
keyboard and player 2 the controller, and getting it wrong leaves the guest
sitting at the title screen looking like the feature does nothing.

## `band_fault.py` — bar discontinuity across the revealed band

```bash
python3 tools/widescreen/band_fault.py tmp/bf --frames 500:700
```

Needs an `on_*.png` / `off_*.png` pair from the same schedule. Reports, per
frame, any band column whose bar rows disagree with the machine's own columns
immediately right of the band, split into reconstruction (present with
`--margin-sprites 0`) and margin-sprite (only present with them on).

**Its `BAR` colour is a placeholder and its output means nothing until you sample
the real one** — the constant is a guess written before the tool was ever run, and
a wrong colour makes every frame read as "continuous across the band edge",
which is a clean bill of health for a picture you never looked at. Sample the
bar's RGB off a frame first, or use a diff against a `--margin-sprites 0` run
instead, which needs no colour at all and is what actually located the ghost.

**Use `sdb_sweep`, not `sdb_idle`.** A still camera hides every boundary artefact
in this feature; all six of the bugs in research.md §26 needed a character to walk
clean across an edge.

**`ghost` is the one to take seriously.** A character is sixteen columns of two
eight-wide sprites, so two halves placed on different bases come out one pixel
apart and the player is drawn at a sixteenth of his width — which reads as "shifted
a pixel, slightly the wrong size", and comes and goes as the two bases drift in and
out of agreement.

What it deliberately does **not** check is a margin sprite drawn on top of the
machine's own copy. Tile-and-row matching against OAM reports thousands of false
hits (every player shares the same tiles), and matching the OAM *slot* cannot work
at all, because `$0617` only advances for sprites the game actually draws. That
property is proved structurally instead: the blit is clipped to the columns the
machine does not own, so a margin-sprite run and a `--margin-sprites 0` run must
differ by **0 pixels** across the machine's 256 columns, every frame.

## Attribution

Written for this project from the `--frame-log` format documented in
`runner/cyc/cyc_host.c` (`write_frame_log`). No third-party code.