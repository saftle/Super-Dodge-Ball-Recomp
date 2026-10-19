# tools/widescreen — analysis behind research.md §25 and §26

Eight scripts, and one of them is shared rather than run. Five read a
`--frame-log` binary or a trace and back a specific claim in
`docs/research.md` §25-26; `json_trace.py` reads the `SDB_TRACE_JSON` stream
instead, and `bounds_gate.py` reads the `--present-out` PNGs rather than a log, and
`zp_writers.py` is a ROM byte scan rather than either.
`cycframe.py` is the `--frame-log` reader the other four sit on.
Nothing here runs the emulator; make the log first.

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

## `cycframe.py` — the `--frame-log` reader (a library, not a tool)

Four of the scripts here each re-implemented the same twenty lines of CYCFRAME
header parsing. That is not a style complaint. A change to the format breaks
three of them **silently**, and a tool that reads nothing reports nothing wrong
— the same failure the project has already paid for twice with its own trace
(research.md §26). One reader, one place to be wrong.

Format, from `runner/cyc/cyc_host.c` `write_frame_log()`:

```
"CYCFRAME"  u32 version(2)
per frame:  u32 frame, u32 cycles lo, u32 cycles hi, u32 lengths[6]
            u8 palette[32], u8 oam[256], then ram, ciram, cart, chr, picture, audio
```

`lengths` is CPU RAM, CIRAM, cartridge RAM, CHR RAM, picture bytes, FDS sound
state. The picture is 256x240 little-endian u16 colour indices (`index |
emphasis << 6`). Version 1 had neither the FDS length nor its blob.

```python
import sys; sys.path.insert(0, "tools/widescreen")
from cycframe import records, picture_indices, backdrop
for r in records(path, want=("frame", "cycles", "ram")):
    ...
```

- **`want`** names the fields to materialise; everything else is `seek`-ed past.
  That is free, and it is **not a speedup for this game**: its cart has no
  cartridge RAM and no CHR RAM blobs, so `blank_extent.py` skips 4 KiB of a
  127 KiB record and the timings are unchanged. The saving is real on a cartridge
  that has them. `want` is worth having for the honesty, not the microseconds.
- **`r.cycles`** is the per-frame cycle count, which every one of the four
  parsers was discarding as an unnamed unpack field. Measured on a 400-frame
  `sdb_sweep` log: **29780.5 cycles/frame**, against the documented NTSC 29781.
  That is a free cross-check on any `--frame-log`.
- **Truncation is bounds-checked.** Seeking past EOF succeeds in Python and the
  next read just returns `b''`, so a log cut mid-record would otherwise still
  yield that record — silently, on exactly the runs where the data is short. The
  original four all had that hole. Verified: cutting 1 B … 400 KB off a 400-frame
  log yields exactly `400 - floor(cut / 127300)` records for every `want`-set.
- A bad magic or an unknown version is a `SystemExit` naming the path, not a
  traceback.

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

## `json_trace.py` — the structured trace (`SDB_TRACE_JSON`)

The other four read prose. This reads the machine-readable stream, which exists
because two of the seven §26 bugs were the prose lying rather than the feature
misbehaving: a missing newline made `margin_track.py`'s regex match nothing on
exactly the runs where you enabled the trace to run it, and a `part` check that
only fired for wholly off-screen objects let the seventh bug through. Both were
parsing and ordering faults. `json.loads` per line cannot care where a closer
was printed.

```bash
mkdir -p tmp/jt
SDB_TRACE_JSON=600-660 ./build-cycle/nes_game \
  "roms/Super Dodge Ball (USA).nes" --frames 661 \
  --input tas/vectors/sdb_sweep.input --ram-init zeros --widescreen 16:9 \
  --overscan --present-out tmp/jt/p_ --present-every 1 >/dev/null 2>tmp/jt/trace.log
python3 tools/widescreen/json_trace.py summary tmp/jt/trace.log
python3 tools/widescreen/json_trace.py flicker tmp/jt/trace.log
python3 tools/widescreen/json_trace.py margin  tmp/jt/trace.log --objects 2,8
```

Three record kinds, discriminated by `"k"`, on one line each:

| kind | when | carries |
|---|---|---|
| `f` | frame begin | flicker counters, `scr`/`cam`, and **every live OAM entry** |
| `m` | composite | what this presented frame painted, per object, per part |
| `r` | exit | run totals |

`f` and `m` describe different instants and do not share a frame — the
compositor runs after the frame it is presenting. Join them on `"f"` knowing
that.

Each margin part carries the branch the draw site took: **`drawn`**, `machine`
(the machine owns those columns), `ghost` (in the revealed band with no OAM
entry behind it — §26's seventh bug), `clip` (straddles into columns we do not
own), `oob`. A recycled record is emitted as `"stale": n` with an **empty**
`parts`, because the draw site skips that object before any placement decision
exists for it.

**`flicker` is the instrument plan item 14 is waiting on.** It prints live OAM
against the mod's published list in the same frame, which is the question that
bug turns on — does the game emit fewer sprites in Bean Ball, or does the mod
drop them? `pub` is the presented list, `oamn` the live OAM count. It only
prints the interpretation on a ≥30% collapse, because `pub` jitters by a few
sprites per frame and a hint that fires on every run trains you to ignore it.

**What it does not replace.** `margin_track.py`'s *temporal* checks — `gone`,
`shift`, a `ghost` that persists across frames — are still its job. This gives
you the per-frame truth without a regex; that script gives you the trend.

Verified inert: with `SDB_TRACE_JSON` unset, 1937 human trace lines and 661
presented PNGs are byte-identical to a run with it set, and the JSON stream is
simply absent (0 records vs 123).

## `bounds_gate.py` — nothing is drawn past the court's own bounds

The bounds are **`[40,480)`**, and they come out of the game rather than off the
art: `$C3/$C4` is the camera, clamped at `$E6AB` to `[$28,$E0]` = `[40,224]`, so
the machine can only ever display `[40, 224+256)`. There is no per-level table.
See research.md §25 for the disassembly and for why an earlier nametable-based
version was wrong.

This asserts the property on the **presented frames**, not on a counter:

```bash
mkdir -p tmp/bg
SDB_TRACE=1 ./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 1200 \
  --input tas/vectors/sdb_sweep.input --ram-init zeros \
  --mods-root build-cycle/mods --widescreen 16:9 \
  --present-out tmp/bg/s_%05d.png --present-every 1 2>tmp/bg/trace.log
python3 tools/widescreen/bounds_gate.py tmp/bg 16:9 s 40 480 tmp/bg/trace.log 430
```

Every margin column whose world column falls outside the bounds must be the
frame's own backdrop. Results:

| capture | frames | judged at | result |
|---|---|---|---|
| stadium sweep | 770 court | `[40,480)` | **0 fail** |
| level 2, second attempt | 2617 | `[40,480)` | **0 fail** |
| level 2, first attempt (old build) | 681 | `[0,512)` | 0 fail |
| level 2, first attempt (old build) | 681 | `[40,480)` | **275 fail** |

The last row is the control: same capture, same gate, new bounds — and the
failures bracket the reported frame 4172.

**It was wrong five times, and every error was a false POSITIVE**, which is the
safer direction to be wrong in and still not a reason to believe it. In order:
the backdrop is not always black (it is palette entry 0, a per-screen value —
hardcoding it reported 635 of 4355 frames as overrun when the margin was entirely
backdrop); the scroll alignment was assumed rather than measured (substituting a
camera of 40 for one of 216 reported 20550 px on a frame that is exactly clean);
`native_x0` is in image pixels while 256 is in canvas columns, so the right margin
started 128 columns early at scale 2 and swept the machine's own picture into the
check; the output prefix has three observed forms so it globs rather than formats;
and it exits **2** rather than 0 when it examined nothing.

`from_frame` skips boot/title/menu, where there is no court to bound.

## `zp_writers.py` — every instruction that writes a zero-page byte

A raw byte scan for `STA`/`STX`/`STY`/`INC`/`DEC`/`ASL`… against a zero-page
address. It is how `$C3/$C4` was found to be written by seven separate
routines, one of which (`$E6AB`) turned out to be the camera clamp:

```bash
python3 tools/widescreen/zp_writers.py C3 C4
python3 tools/dis65.py "roms/Super Dodge Ball (USA).nes" E5D8 20 $((16 + 7*16384 + 0x25D8))
```

It reports **data that looks like an opcode as well as code** — a linear sweep
cannot tell them apart, and the output is a list of candidates to disassemble,
not a list of answers. Confirm every hit with `dis65.py`.

## `anchor_gate.py` — did the camera lock hold still, and change nothing?

For `widescreen`'s `camera = locked` option (research §28). Three claims:

1. **The view does not move.** `anchor` is the world column at canvas column 0;
   locked it is one value for the whole run, derived from the width alone.
2. **The anchor changes nothing else.** Each run's own machine block is compared
   against the *same* stock frame using *each run's own* recorded anchor, and the
   per-frame differences must be identical. The machine's own 256 columns are the
   strongest thing to check, and this form of the check has no frame-alignment
   assumption in it.
3. **The court is on its own picture.** Canvas column 0 holds world `anchor`, so
   the court's edges are at `40 - anchor` and `480 - anchor`; both must be inside
   the canvas on every frame.

`--stock` is not optional. `SDB_TRACE_JSON` must be on for the anchor records —
they are emitted as `{"k":"anchor",...}`, one per composed frame.

```bash
mkdir -p tmp/anchor
for m in locked follow; do
  d=tmp/anchor/$m; mkdir -p $d; extra=""; [ $m = locked ] && extra="--camera locked"
  SDB_TRACE=1 SDB_TRACE_JSON=400-760 ./build-cycle/nes_game ROM --frames 760 \
    --input tas/vectors/sdb_sweep.input --ram-init zeros --widescreen 32:9 $extra \
    --present-out $d/f --present-every 1 >/dev/null 2>$d/trace.log
done
mkdir -p tmp/anchor/stock
SDB_TRACE=1 ./build-cycle/nes_game ROM --frames 760 --input tas/vectors/sdb_sweep.input \
  --ram-init zeros --present-out tmp/anchor/stock/f --present-every 1 >/dev/null 2>&1
python3 tools/widescreen/anchor_gate.py --frames tmp/anchor/locked \
  --trace tmp/anchor/locked/trace.log --follow tmp/anchor/follow \
  --follow-trace tmp/anchor/follow/trace.log --stock tmp/anchor/stock \
  --frame-from 421 --expect locked --reveal
```

**Run it against the follow capture as well. It must FAIL**, on claim 1, with a
list of anchor values. A gate that only ever passes is not a gate, and claim 1
is one integer that a broken run would report as a constant.

Baseline: **339/339 frames, one anchor, 0 off-canvas, machine block identical on
every frame** at both 32:9 and 21:9; 1800 frames of `sdb_throw` give one anchor
over 1379 anchored frames, the other 373 being menus and pre-court frames left
centred on purpose.

The gate treats the two regimes separately, using the per-frame `here` flag:
frames the anchor engaged on must share one anchor, and frames it declined must
have `x0 == xh`. That second half is the menu check — a title screen or a menu
has a scroll of 0, which is outside the game's camera window `[40, 224]`, so the
court bound does not describe it and the anchor stands down. Without the split, a
run that examines menus would report their centred positions as "the view moved".

### Two traps, both of which produce a failure that is not there

- **The anchor record with `f = N` was written by the compose that produced
  presented file `N-1`.** Pair it with file `N` and the gate reports 26
  failures, every one a camera-stepping frame, each looking exactly like a
  7-pixel placement bug. It is only visible when the scroll *changes*, because
  otherwise a one-frame offset compares two identical pictures. To settle it,
  scan the canvas for where the block actually is: file 596's block is where
  anchor **597** says.
- **Never compare one run's pixels to another's.** Claim 2 reads as "locked
  canvas == follow canvas translated", which is true and was measured directly,
  but it needs both runs' presented indices to agree and they do not reliably.
  Compare each run to the same stock frame instead.
- **The flicker mod has to be ON for a compositor gate.** Every scripted widescreen
  check runs with it off, and with it off the wide path in `sdb_present` copies
  the compositor's canvas straight through — `redraw` is false and no copy is
  made. That is how the camera anchor shipped a compositor bug (the HUD sliding,
  because `sdb_present` stamped all 240 rows at the court's one offset while the
  compositor places the court and held bands at two) with 701/701 frames
  byte-identical and two clean aspect gates. Gate the pair.

`--reveal` / `--no-reveal` says whether the run revealed the 8 clipped columns,
because that changes what the machine's block contains: widescreen leaves them
reconstructed rather than stamping the machine's picture over them. The reveal is
**on by default on every path** now (`--widescreen`, the launcher, `--mods-root`),
so `--reveal` is right for the command above and `--no-reveal` is right for a
`SDB_OVERSCAN=0` run.

## Attribution

Written for this project, with no third-party code. `cycframe.py` reads the
`--frame-log` format documented in `runner/cyc/cyc_host.c` (`write_frame_log`);
`anchor_gate.py` reads the presented PNGs the host writes and the compositor's own
JSON trace lines, both of which it documents the shape of inline. Pillow is the
system package the rest of `tools/` already uses.