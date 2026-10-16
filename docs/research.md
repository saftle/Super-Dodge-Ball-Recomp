# Research Notes — Super Dodgeball NES Recompilation

> Living notes. RetroPortingToolKit docs reference: https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs

## Summary of Findings

Cycle backend is the only play path; legacy runner retired (§§18–19 keep its game knowledge).
Team-select verified byte-exact vs Mesen from boot (RAM/OAM/palette/CIRAM, 400-frame TAS); blargg
cpu tier 25/25.

**Character flicker: SOLVED, all three causes** (§2, §2.1). None of them is emulator
sprite-limit removal; the game throttles its own sprites three ways, and two more sprite
classes turned up afterwards that no cursor-bump hook could see.

1. OAM is built in place at `$0200` behind a cursor `$0617`, characters are 8 sprites each, and
   the `BEQ` bail at `$84B6` stops emission at 64 slots, so the missing tiles are never
   computed. Truncated on 79% of match frames. Fixed by host-side redraw over a captured
   sprite list.
2. A whole character vanished 4 frames in 9 while OAM sat at 46-54 of 64. The cause is the
   game's own **object culling**: `$E32D` refuses to put an object on the display list at
   `$067C` when its state byte `$43` has bits 4-6 equal to `$20`, and rotates which object it
   refuses. Three gates on that path (`$E34A`, `$828A`, `$8200`), all overridden. Box-ink
   stdev 55.0 → 16.6 with zero frames still culled.

Shipped as the opt-in `super-dodgeball.character-flicker` mod (off by default pending a full-playthrough test; 10 hook sites,
`src/cycle_seeds.txt` so the hooked code is recompiled — hook sites do not exist on the
interpreter). Fix 1 is presentation only; **fix 2 is not** — it makes the guest emit more
sprites, so its cycle count and NMI phasing shift slightly (§2.1). Two more sprite classes
turned up after that and are hooked the same way: the floating damage numbers and the
team-select position numbers (§2.1).

Frame comparison (MesenCE reference) and the blargg suite are the standing gates.

**Presentation mods: three, all opt-in** — `super-dodgeball.character-flicker` (§2, §2.1),
`super-dodgeball.ppu-overclock` (§23) and `super-dodgeball.widescreen` (§25, §26). The
widescreen package is one feature: it presents the wider canvas, reveals the clipped
left-hand columns and draws the players the game culled off-screen, because the same
compositor does all three. Only the flicker fix changes guest behaviour, and only in
its culling half; overclocking changes console *timing*; **widescreen changes nothing at
all** — it reads the nametable the frame already wrote and re-presents the picture.

**Widescreen: the margins are already in the nametable, and so are the sprites** (§25, §26).
Presenting 16:9 / 21:9 / 32:9 (426 / 560 / 854 columns) needed **no engine patch** — the widths
and the compositor API are already upstream. The court's world is a 512-pixel looping stadium
that is byte-identical across a whole match, so margin pixels are resolved the way the PPU
resolves them rather than remembered the way wideNES does. Reconstruction agrees with the
machine's own background capture to 1-2 px in 61440, and the stock 256-column centre is copied
back so it stays pixel-identical. Overscan is measured, not assumed. And the margins hold
sprites too: the game places every sprite in a **16-bit** screen coordinate and then hides
anything outside `[4, 255]`, so the placement is recovered from the accumulator one instruction
before the cull — **40% of objects are off-screen at any time** (§26).

---

## 2.1. Second Flicker: the game's own object culling (SOLVED)

A second, independent flicker: one character vanished entirely for **4 frames out of every 9**
and came back for 5, forever. Unlike the truncation above, OAM sat at **46-54 of its 64 slots**
the whole time, so this was never a budget. It looked deliberate, and the first attempt --
holding a sighting and redrawing it -- produced *doubling*, which suggested game logic.

It was not game logic. It is the game **culling its own object list**, in three places, and all
three are now overridden.

### What the list is

Objects do not go straight to OAM. `$E32D` (fixed bank) builds a display list each frame:

- the object's **state byte `$43`** decides whether it is added at all,
- its 7-byte **display record** is `$0618 + 7*index` (pointer table at `$E3A2`, mirrored at
  `$80B8`), record byte 0 is a copy of `$43`,
- the list itself is 16-bit ids at **`$067C`**, count **`$067A`**, byte cursor **`$067B`**,
- **`$8040`** (switchable bank) bubble-sorts that list by the id's high byte (`$067D` vs
  `$067F`, swapping four bytes at a time) and then calls **`$821E`** per entry, which reads the
  record, sets up the emitter state and jumps into the sprite-list walk at `$8348`.

So `$4E` (record byte 0) is the same `$43` the builder gated on, and the whole path is keyed
off that one byte.

### The three gates

**1. `$E34A` -- the builder refuses to add the object.**

```
$E34A: LDA $43 / AND #$70 / CMP #$20 / BEQ $E378
$E352..$E377   append: id into $067C/$067D, INC $067A, $067B += 2
$E378:         either way, write the record from $43/$41/$42
```

Bits 4-6 of `$43` equal to `$20` means the object never enters the list, so `$821E` never sees
it. **No hook at the emitter can reach it** -- that is what made the first attempt look like a
dead end, and why the continuation note's "hooks at emission sites are too late" was right for
the wrong reason.

Measured over a 905-frame match: the flag is set on **229 frames, never on two objects at
once**, and the object it names rotates (object 4 early, object 3 from frame ~748). Each
object's own counter decrements one step every 9 frames and bit 5 is simply its low bit. A
cull that always sacrifices someone, never the same someone twice running, is not design.

**2. `$828A` -- the emitter skips a record whose flag bit 5 is set.**

```
$827A: BIT $4E   bit 6 -> extra path      $8281: AND #$10  bit 4 -> $82DD
$828A: AND #$20 / BNE $8293  (RTS: emit nothing)     $8290: JMP $8348  (emit normally)
```

Same byte, so an object put back on the list is still filtered here. This gate alone is
unreachable: with gate 1 closed the object never arrives.

**3. `$8200` -- the crowded-stack cull.** `$8156-$8172` runs once per frame; when `$81A2`
counts four or more list entries within `$20` pixels it calls `$8200`, which shifts the list
down over one entry (the rotating table at `$8173` picks which) and decrements `$067A`. Much
rarer -- 42 calls in 910 frames -- and it was the unconfirmed lead section 2 left behind.

**The SNES port disabled gate 3 outright**: `RTS ; disable sprite flickering`. That comment was
the whole answer, sitting in the one routine nobody had looked at.

### Why nothing about the object changes while it is hidden

Same record, same position, same tiles as the frames either side. The game is hiding something
it drew a frame earlier, not skipping a transitional state -- which is also why temporal
holdover doubled instead of filling in.

### The fix

Five more content-keyed `[[mod_function_hook]]` sites for §2.1's emitters (`src/game.toml`) plus six
`cycle_seed_file` roots (`src/cycle_seeds.txt`):

| site | bank | what the callback does |
|---|---|---|
| `$E378` | 7 | replicate the append `$E352-$E377` skipped: id, high byte per `$E35F-$E36C`, `INC $067A`, `$067B += 2` |
| `$8231` | 5 | clear bit 5 of `$4E`, the zero-page copy the emitter is about to branch on |
| `$8200` | 5 | return 1, which emulates that routine's RTS and leaves the list alone |
| `$96B9` | 3 | capture the floating damage-number pair (below) |
| `$A394` | 4 | capture one block-copied sprite per iteration, slot from X (below) |

All three culling gates are needed. Re-adding the entry without `$8231` still filters at emit time; `$8231`
alone never sees the object.

**The seeds are the whole reason this was possible.** A hook site only exists in recompiled
code (`hook_site_here()`: `if (!s_hook_cfg || INTERP(e)) return false`). `$8040`, `$821E` and
`$E32D` are reached only through a dispatch path static discovery does not walk, so they ran on
the cycle interpreter -- 37% of all cycles in a match. A hook declared at `$8231` compiled,
bound, and never fired: the instruction was emitted as an orphan entry with nothing reaching
it. `4k:14:8040`, `4k:14:821E` and `4k:1E:E32D` (measured with `--miss-log`) fix that;
native share went 62.4% -> 65.1% and the total cycle count did not move.

**Gotcha, cost one build:** `$E35F-$E36C` is `LDA $49 / BPL +5 / LDA #$00 / STA $067D,X` then
`BEQ +5 / LDA #$FF / STA $067D,X`. So `$49` zero leaves `$48` alone and any other positive
value forces `$FF`. Read it the other way round and the restored entry gets sort key `$FF`,
lands behind everything, and the character above it draws in front -- a layering artefact that
looks like a z-order bug and is really a transcription error in the sort key.

### A third, separate artefact: the runaway capture list (SOLVED)

Long after the two flickers were fixed, playing on produced a much uglier symptom: **hit
anyone with the ball and the screen filled with copies of the court in a trail**, like an old
mouse cursor. It is not the game's, and not the culling fix's doing directly — it is the mod's
own list bookkeeping failing in a state the earlier tests never reached.

The mod assembles each frame's sprites into `build` and detects a new list by "the cursor went
back to slot 1" (`$0617 == $04`). That is the common case, but it is **not the only one**. In some
game states a list begins at a higher slot, because the emitters that would fill slots 1-2 have
nothing to draw. Measured, after throwing all game:

```
frame 940  last capture cur=$F4   list length 61
frame 941  first capture cur=$0C  ... $F0        <-- $04 never appeared
frame 942  ... $F4 $F8 $FC $FC   <-- the cursor pin holds it there
```

With no boundary, `build` never restarted: it grew by ~57 sprites every five frames, hit
`SDB_MAX_SPRITES` (512) and overflowed ~57 more **per frame, forever**, while
`sdb_present()` happily painted every one of them. That is the smear.

**Two fixes, both needed.**

1. **Treat a cursor that has gone backwards as a new list**, alongside `$04`. Nothing inside one
   list moves it backwards: it only steps +4, and once the pin engages the tail reads `$FC,
   $FC, $FC...`. The game's save/restore around `$DBDB` re-reads the same value, which is not a
   decrease. So the test cannot fire mid-list.
2. **A backstop.** Past `SDB_LIST_LIMIT` (256, against a measured maximum of 77) the mod
   publishes what it has and starts again, and says so on stderr. This exists so a future
   detection miss degrades into one odd frame instead of a permanent smear — and it is never
   silent, because a silently wrong picture is the failure mode this mod exists to remove.

`SDB_TRACE=1` prints one line per frame (`frame peak-list overflow published-list saw-$04
backstop-trips`). That is what localised this in a headless run without watching 1600 frames,
and it is the thing to reach for first next time. `SDB_TRACE_CAPTURES=<from>-<to>` dumps every
capture in a range, which is what showed the `$F4 -> $0C` jump.

Reproduction: `tas/vectors/sdb_idle.input` up to frame 445, then hold **B** (throw — A passes,
A+B jumps) on a 12-on/12-off cycle, into the match. The runaway begins around frame 941.
That schedule is now the tracked vector `tas/vectors/sdb_throw.input`.

### Two more sprite classes the capture never saw (SOLVED)

**Damage numbers.** When a player is hit, a digit pair floats above him showing the lost
energy (tiles `$F0`-`$F9`, OAM slots 1-2). Their emitter (`$966D`, bank 3) writes both
sprites in one go, so its cursor bump steps by 8, not 4 (`LDA $0617 / ADC #$08 /
STA $0617` -- the only emitter with that shape, and it has no wrap bail). It was never
hooked, so the numbers were never captured and the mod never drew them: the one thing on
screen that says a hit landed was missing. Hooked at `$96B9` with a two-sprite capture;
a cursor that would wrap is pulled back to `$F4`, whose `ADC #$08` lands on `$FC`, so
only slot 63 is ever overwritten -- the same deal as the single-sprite pin.

Measured (`tas/vectors/sdb_throw.input`, frames 895-1400): guest OAM holds the digits
108 frames on both sides (stock 922-1029, mod 939-1046 -- same length, shifted by ordinary
guest divergence), and the mod presents them all 108 with zero gaps. Duration is
untouchable by construction: the mod copies OAM bytes, and emission and clearing stay
100% guest-side.

**Menu position numbers.** The team-select court diagrams' position digits (tiles
`$05`-`$08`, `$22`) come from a third emitter shape: the block copier at `$A36C`
(bank 4), which streams up to 8 sprites from a static table (`$A3DC`) stepping an X
register loaded from the cursor -- no per-sprite bump at all, just one `STX $0617`
when the loop is done. No bump hook can ever see them. Hooked per copied sprite at
`$A394` (the first instruction after all 4 bytes are written, and only on the copy
path -- iterations the `$A3A1` gate skips jump over it), reading the slot from the CPU
registers and sharing the boundary/prefix/backstop handling. Nothing is pinned: the
loop's own final store commits the cursor, so wrapping behaves exactly as stock.

### Prefix carry-over and done-freshness (the rest of the missing-sprite story)

Hooking the emitters was not enough for sprites that *sit* in OAM. The damage digits
persist in slots 1-2 long after their emitter last ran, and a list that restarts at
`$0C` never re-reads them -- so after the one emission frame, later lists silently
dropped them again. Hence two follow-ups, both in the shared restart path:

1. **Prefix carry-over.** On a restart at cursor `$0C` or above, the slots below it keep
   whatever OAM holds, and the native picture draws them -- so the restarted list opens
   with those slots copied verbatim, in slot order. That is not blending two builds: it
   is reading the machine's own OAM page for slots this list does not cover, which is
   exactly what the PPU draws there.
2. **Done-freshness.** The first-capture frame used to present the published list
   unconditionally -- but after an emission gap that list predates what sits in OAM now
   (measured: digits shown 1 frame, then gone while OAM still held them). Now it falls
   back only to a list published on the immediately previous frame; anything older loses
   to the new build, partial or not, because the new build with its prefix is what the
   machine's OAM page actually holds. Normal two-frame builds behave exactly as before.

### Verified (cycle backend, `--present-out`, same schedule both ways)

Reproduction vector `tas/vectors/sdb_idle.input`, `--ram-init zeros`, frames 700-909
(re-verified on the current binary: prefix, freshness, numbers and block-copy hooks all
landed after the first measurement, and the numbers are unchanged):

| | B's box ink, frames 700-909 |
|---|---|
| stock | 543-741, **stdev 55.0** (B culled on 106 of 210 frames) |
| mod | 677-741, **stdev 16.6**, **0 frames** at stock's culled value |

- **0 frames** where the culled character is still missing; **0 frames** below the ink of any
  frame stock drew him on.
- Inside B's box the mod matches stock exactly where stock drew him (frame 866: 0 differing
  pixels in the box). Whole-frame sameness is NOT claimed: the culling fix shifts guest
  timing by a couple of cycles, so blink-phase elements (menu cursor, roster scroll) sit a
  frame apart between runs -- same game flow, different phase. Regional ink plus eyeballing
  is the gate, not whole-frame equality.
- Damage numbers (`tas/vectors/sdb_throw.input`, frames 895-1400): guest OAM holds them 108
  frames both sides, mod presents all 108 steadily, zero gaps. Duration matches by
  construction (the mod copies OAM bytes; emission and clearing are guest-side).
- Menu position numbers: bottom halves of menu frames 100-400 pixel-identical except
  blink-phase cursor frames; the court-diagram region at frame 150 is 0 differing pixels.
- Cycle count unchanged: 27,097,877 stock vs 27,097,876 mod over 910 frames; mod-off
  (empty catalog) output byte-identical to stock on all 1100 frames.

### What it costs the guest (not nothing, and not "presentation only")

The first fix was presentation only: same cycle count, and only `$02FD`/`$02FF` differ. **This
one is not**, and the claim had to change. Measured mod-on vs mod-off over 910 frames:

- **OAM** differs on 484 frames -- that is the point, and it is the only intended change.
- **Sprite slot 0 never differs on any of 910 frames**, so sprite-0 hit is unaffected.
- **CPU RAM**: the display-list machinery (`$067A`, `$067C`-`$0693`), the OAM page, transient
  zero-page/stack scratch, and a set of per-object timers that settle at a constant offset
  (usually +5 ticks). Object positions and the ball do not differ.
- **CIRAM -- the picture -- is identical on 899 of 910 frames**; the other 11 differ by 1-2
  bytes at `$2070`, a frame-boundary phase effect.

The cause is unavoidable: making the game emit an object it had culled *is* extra guest work,
so the guest runs a couple of cycles ahead and NMI phasing moves. Nothing in gameplay reads
`$2002` (all ten reads are in the fixed bank's vblank code) and nothing reads `$067A`/`$067C`
outside the sprite machinery, so the game cannot observe the difference -- but "cannot observe"
is not "byte-identical", and the docs should say so.

---

## 1. ROM Technical Specifications

- **Format**: iNES 2.0 header (`4E 45 53 1A 08 10 10 08 00 00 00 00 00 00 00 01`)
- **Mapper**: 1 (MMC1 / NES-SLROM PCB, MMC1B2 variant)
- **PRG ROM**: 128 KB (8 × 16KB banks) · **CHR ROM**: 128 KB (16 × 8KB banks)
- **Mirroring**: Horizontal · **Battery**: None · **TV System**: NTSC
- **CRC32**: `689971F9` · **SHA-1**: `42f954e9bd3256c011aba14c7e5b400abe35fde3`
- **CIC chip**: 6113B1 · **File size**: 262,160 bytes

---

## 2. Sprite Flickering Analysis

> **Measured, not inferred.** Everything below is from `--frame-log` OAM/CIRAM dumps of the
> cycle backend plus 6502 disassembly (`tools/dis65.py`). Analysis scripts lived in `tmp/scan/`
> (scratch, gitignored). The earlier "Method 2 software sprite removal" hypothesis is **wrong**.

### The Symptom

A character renders as head-and-arms only, lower body missing, and **flickers frame to frame**:
the same on-screen character is whole on frames 444/446/448 and truncated to 3 tiles on
450/452/454. Reproduce: drive a TAS past team-select into a match, then `--shot-every 1`.

### Corrected mechanism

The game builds OAM **in place** in the DMA source page and walks an **8-bit byte cursor**:

- OAM DMA source is page `$02` — `$FE34`: `LDA #$02 / STA $4014` → OAM = `$0200-$02FF`.
- The byte cursor is `$0617`, reset to `#$04` (sprite slot 1) each frame. Every emitter writes a
  complete 4-byte sprite at `$0200+cursor`, then does `LDA $0617 / CLC / ADC #$04 / STA $0617`.
- Five cursor-bump sites: **`$A2F7`, `$A330`** (bank 4) and **`$8116`, `$8338`, `$84B6`** (bank 5).
  Each fires once per emitted sprite.
- **Characters are 8 sprites each** (16×32 px = four 2-tile rows). A full match needs ~9
  characters ≈ 72 tiles, more than OAM's 64 slots.

**The truncation:** only **`$84B6`** has a bail — `BEQ $84E5` right after the cursor store — so when
the cursor wraps `$FC → $00` the character emitter **stops emitting**. The other four sites simply
wrap and overwrite slot 0, so they never truncate.

**The game never computes the dropped tiles.** `$0300+` (just past the DMA window) holds no sprite
data at the cut, so the missing tiles do not exist anywhere. That is why emulator "Remove Sprite
Limit" does nothing: the sprites were never emitted, not merely hidden.

**Measured frequency:** over a 466-frame match, **370 frames (79%)** have a truncated character and
**the cut position moves** (X=46, 63, 70, 207, 37, …) as sprites come and go — that movement *is*
the flicker. OAM saturates at 64 on 102 frames.

**Secondary, genuine hardware cause:** the cycle PPU implements the real 8-sprites-per-line limit
(`hw_ppu.c`: `oam2` full + `overflow`), and 8-tile characters push several scanlines over 8, so the
hardware independently drops ~8 sprites/frame. Real, but far less frequent than truncation.

### Fix (two layers)

1. **Remove the budget truncation.** `[[mod_function_hook]]` at the `$84B6` bail, content-keyed on
   its bytes so no game bytes live in the repo. When the cursor is about to wrap, capture the sprite
   just written at `$0200+$FC` into a host-side list and pin `$0617` so the `BEQ` never fires; the
   game then emits every sprite it intends. Capture the other four bump sites too, so the host list
   holds *all* emitted sprites in OAM-slot order (slot 0 = frontmost).
2. **Host-side redraw.** `cyc_render_capture_background(true)` + `cyc_render_background()` for a
   sprite-free background, then draw the captured list (CHR via `cyc_render_chr`, palette via
   `cyc_render_palette`, `cyc_render_tile` for 8×8/8×16, H/V flip, palette select). Presented through
   the `present` callback in `cyc_host_extras.h`, which works at native width 256 — unlike
   `cyc_render_set_compositor`, which is dormant at width ≤ 256.

Native PPU evaluation, sprite-0 hit and IRQ are untouched; only presentation changes. Ship as a
`.nesmod` package + trusted plugin, `default_enabled = false` for now (level-switch ghosts open), surfaced in recomp-ui's Mods screen.
Requires our own `CMakeLists.txt` (declaring `CXX`) calling `nesrecomp_add_cycle_game(... MODS
GAME_ID ... HOST_EXTRAS ...)`; the shared `project/CMakeLists.txt` is inside the submodule.

**The build spans two frames.** The game resets `$0617` to slot 1, then emits a slice per frame
until the whole list is in `$0200`. The host list must therefore span a whole build, and emit
order *differs between builds* - a half-assembled list must never be blended index-wise with the
previous one (that draws both, out of order, which is a different, worse artefact).

Present the last COMPLETE list only while the build is genuinely still in progress, i.e. on the
**first** capture frame. From the second capture frame on the build is whole. Falling back on
*both* capture frames leaves the picture one full build stale on one frame in six - a 6-frame
pose shimmer, since characters animate on that same 6-frame period.

**Blit.** Walk the pixels yourself, mirroring `cyc_render_sprites` (runner/cyc/cyc_render.c) row
for row. Do NOT call `cyc_render_tile` once per scanline: it paints a whole 8x8 tile per call, so
that stacks eight overlapping copies that overwrite each other's pixels - which shows up as
doubled, wrongly-coloured sprites.

**Verified result** (cycle backend, `--present-out`; "ink" = non-background pixels in the
recovered character's box, frames 440-469):

| | body ink | stdev |
|---|---|---|
| stock | 651-670 | **8.7** (flickers) |
| mod on | 651 | **0.0** (steady) |
| mod off | 651-670 | 8.7, byte-identical to stock |

On the frames stock already drew correctly the mod is pixel-identical (3/3). The overrun tail is a
coherent mirrored character (same Y rows, same tiles, X swapped left/right, `attr=$41` = HFLIP +
palette 1) - genuine game output, not overrun garbage. Guest machine unchanged: identical cycle
count and only `$02FD`/`$02FF` differ (sprite slot 63, which the cursor pin overwrites).

**Second artefact (also fixed).** Beyond truncation, on one frame in six the game emits so little
that a whole character is absent from OAM entirely - at f437 stock renders *nothing* where a
character stands. It is present in the other five frames, so it is a build-slicing artefact, not
game logic. Presenting the last complete list on the first capture frame only keeps it drawn. That
region's ink stdev went 8.7 -> 0.8, and the originally-truncated character's 8.7 -> 0.0.

**Related, now resolved in §2.1:** `$8200` (bank 5) removes an entry from the object list at
`$067C` (count `$067A`), and the SNES port replaces it with `RTS ; disable sprite flickering`.
That comment turned out to be the second flicker's whole mechanism - the game culling its own
object list, three gates of it, all now overridden by the mod. The port still leaves `$84B6`
byte-identical and caps its own OAM translation at 64 sprites (`CPY #$100`), i.e. it keeps the
truncation fix; its "flicker" commits were the list-compaction routine and an inverted OAM
priority bit.

### SNES Port

Rumbleminze's SNES port (`rumbleminze/super-super-dodgeball`) needs no software fix — the SNES has
128 OAM slots and 32 sprites/scanline, so the same game logic runs with far more headroom.

---

## 3. NESRecomp Framework

### Architecture

NESRecomp is a **static 6502 recompiler** — not an emulator. It translates NES ROM machine code to C at build time, compiled to native x64.

**Three main components:**
- **Recompiler** (`recompiler/src/`): Function discovery via BFS from RESET/NMI/IRQ vectors, C code generation. Key files: `code_generator.c`, `function_finder.c`, `game_config.c`, `cpu6502_decoder.c`
- **Runner** (`runner/src/`): NES hardware emulation — PPU, APU, mapper, input, save states. Key files: `runtime.c`, `main_runner.c`, `ppu_renderer.c`, `mapper.c`, `interp.c`, `apu.c`, `controller.c`, `debug_server.c`
- **Game project** (`src/`): `extras.c` (hooks), `game.toml` (config), `generated/` (auto-generated C)

**How function discovery works:**
1. Start from vectors (RESET, NMI, IRQ) in the fixed bank (bank 7, `$C000-$FFFF`)
2. BFS: follow JSR targets, branch targets, JMP targets
3. On JSR: add target as new function
4. On RTS/RTI/BRK/JMP: stop current function
5. Register state propagation tracks A/X/Y constants to detect bank switches
6. Pointer table scanner finds dynamically-dispatched functions (4+ consecutive 16-bit LE values where each target decodes as valid code for 7+ instructions)
7. RTI hijack detection: scans for `STA $01NN,X` writes followed by `LDA #imm` pushes

**How the dispatch table works:**
- `call_by_address(uint16_t addr)` — basic dispatch, returns 1 if recompiled function exists
- `call_by_address_cb(uint16_t addr, int caller_bank)` — with bank fallback
- `nes_dispatch_call(addr, caller_bank)` — depth-counted, for JSR sites
- `call_by_address_tail(addr, caller_bank)` — deferred JMP tails
- On miss: falls through to interpreter (if enabled) or dispatch-miss policy

**Supported game.toml directives (full list):**
- `[game]`: output_prefix, push_all_jsr, disable_ptr_scan, disable_secondary, deduplicate_functions, symbol_file
- `[mapper] bank_switch`: addresses of bank switch routines
- `[[trampoline]]`: bank-switch trampoline config
- `[[inline_dispatch]]`: indexed dispatch via inline address table
- `[[inline_pointer]]`: JSR reads inline bytes into zero page
- `[[nop_jsr]]`: skip JSR entirely
- `[[extra_func]] bank, addr`: force-create a function entry
- `[[extra_label]] bank, addr`: secondary entry point within existing function
- `[[data_region]] bank, start, end`: exclude from pointer scanning
- `[[sram_map]] sram_start, rom_start, bank, size`: SRAM-to-ROM mapping
- `[[replace_func]] addr, bank, scope`: replace function with extras.c
- `[[mod_function_hook]] addr`: mod callback hook
- `[[ram_read_hook]] addr, indexed`: hook for RAM reads
- `[functions] fixed = [...], bankN = [...]`: fixed native functions
- `[force_interp] fixed = [...], bankN = [...]`: force interpreter mode
- `[[stack_bail_func]]`: stack-bail function addresses
- `[[cond_bail_func]]`: conditional bail functions
- `[[merge_func]]`: merge two function bodies
- `[[merge_range]]`: merge all functions in a range
- `[[indirect_continuation]]`: indirect continuation targets
- `[[push_jsr]]`, `[[push_jmp]]`: JSR/JMP needing dummy push

**Runtime frame flow:**
1. `game_run_main()` is called (never returns)
2. CPU executes native code, calling `nes_instruction_boundary()` per instruction
3. `maybe_trigger_vblank()` checks cycle budget and fires NMI
4. `nes_vblank_callback()` runs `game_run_nmi()` then `game_post_nmi()`
5. `ppu_render_frame()` renders PPU to framebuffer
6. `game_post_render()` does post-render compositing
7. SDL presents frame; `g_frame_count++`

**Nested NMI policy:**
- `NESTED_NMI_POKE_SPIN_FLAGS` (default): skips handler, sets $1A/$20 spin-wait bytes
- `NESTED_NMI_RUN_HANDLER`: runs handler nested — used by SMB3

**Controller input:**
- `g_controller1_buttons` / `g_controller2_buttons` hold 8-bit button state
- Button bitmask: bit7=A, bit6=B, bit5=Select, bit4=Start, bit3=Up, bit2=Down, bit1=Left, bit0=Right
- NES controller uses serial shift register: strobe pin starts shift, each clock pulse shifts out one bit
- `$4016` returns `0x40 | (g_controller1_buttons >> 7)` when strobe is set; otherwise shifts out bit 7 first

**Key runtime globals:**
- `g_ram[0x0800]`: 2KB work RAM
- `g_sram[0x2000]`: 8KB battery SRAM
- `g_ppu_oam[0x100]`: 64 sprites × 4 bytes
- `g_ppu_pal[0x20]`: Palette
- `g_ppu_nt[0x1000]`: Nametable RAM
- `g_controller1_buttons` / `g_controller2_buttons`: Controller state
- `g_ppuctrl` / `g_ppumask` / `g_ppustatus`: PPU registers
- `g_code_window_base`: Code window base for bank switching
- `s_vblank_depth`: Current NMI nesting depth
- `s_ops_count`: Per-frame CPU cycle accumulator
- `s_frame_budget`: ~29781 cycles (NTSC dot-accurate)

### Proven MMC1 Support

- **Yoshi's Cookie** (MMC3): 100% playable, zero oracle divergence
- **Zelda 1** (MMC1): Fully playable
- **Metroid** (MMC1): Fully playable
- **Dr. Mario** (MMC1): Fully playable
- **Faxanadu** (MMC1): Fully playable
- **Super Mario Bros** (NROM/0): Fully playable

### Pipeline

```
NES ROM (.nes) + game.toml → recompiler binary → generated/<game>_full*.c + _dispatch.c
→ CMake + SDL2 → native executable
```
Regen command (repo root): `./external/nesrecomp/recompiler/build/NESRecomp "roms/Super Dodge Ball (USA).nes" --game src/game.toml --output-prefix "Super_Dodge_Ball_(USA)"`

### Limitations

- Generated C code is **not human-editable** (auto-generated, overwritten on recompile)
- Audio is basic (APU register writes captured, full mixing is WIP)
- Undocumented 6502 opcodes (DCP, ISC, SLO, RLA, SRE, RRA, SAX, +90 more) emitted as sized NOPs
- `BRK` treated as comment/stop, not real software interrupt
- Cycle accuracy: base cycle counts only; page-cross and branch penalties mostly not modeled
- Mappers 2, 3, 7, 9 not yet supported

### game_dispatch_override Deep Analysis

**Critical finding**: `game_dispatch_override` is ONLY checked in the **interpreter fallback path**, NOT in the native C dispatch chain.

- Native JSR/JMP to `func_FF08`: calls `func_FF08()` directly via dispatch table (`case 0xFF08: func_FF08(); break;`) — `game_dispatch_override` is NOT consulted
- Interpreter fallback for `$FF08`: `nes_interp_dispatch_bank()` calls `game_dispatch_override(addr)` — if returns 1, the real function never runs
- `nes_log_dispatch_miss()` also calls `game_dispatch_override(addr)` — if returns 1, no dispatch miss is recorded

This means `game_dispatch_override(0xFF08)` creates a **split-brain**:
- **Native code sees**: `func_FF08()` runs the MMC1 bit-bang (writes to `$8000-$FFFF`)
- **Interpreter sees**: Controller read sequence (strobes `$4016`, reads 8 bits)
- **Dispatch miss logging**: `$FF08` never appears as a miss (override swallows it)

**Runner's `$4016` handling is correct**: The runner (`runtime.c`) properly implements the NES controller strobe/shift register:
- `$4016` WRITE 0x01: sets `s_ctrl1_strobe = true`
- `$4016` WRITE 0x00 (after strobe): latches `g_controller1_buttons` into `s_ctrl1_shift`
- `$4016` READ: shifts out bits from `s_ctrl1_shift`, returns `0x40 | bit`
- `g_controller1_buttons` defaults to 0 in headless/smoke mode

**Working `game.toml` state**: `bank_switch=[0xFF08]`, minimal `fixed`, dis65-verified `extra_func` (banks 0/1/6), two `[[replace_func]]` (`$FCA0`, `$8393` bank 6), empty `[force_interp]`. Historical note: the override was removed from `extras.c`; the old `force_interp` entries never loaded (TOML didn't parse, §14).

**Historical hang (resolved, see §15)**: with the ppumask toggle, 3 frames then hang. Without it, frames complete.

### NMI Handler and func_NMI() Pattern

All successful NESRecomp games follow a standard pattern for NMI handling:

1. **The code generator auto-generates `func_NMI()`** (`code_generator.c` lines 4920-4978). It:
   - Resets `g_rti_target = 0`
   - Sets `g_code_window_base = 0xE000` (bank 7 for NES)
   - Calls the NMI vector handler (e.g., `func_FF3A`)
   - If `g_rti_target != 0` (RTI hijack), dispatches to the hijacked target via `call_by_address`

2. **The runner sets up the interrupt stack frame** before calling `game_run_nmi()`:
   - Pushes PCH placeholder, PCL placeholder, P (status flags) onto 6502 stack
   - Sets `g_cpu.I = 1` (interrupt disable)

3. **All games call `func_NMI()` from `game_run_nmi()`**:
   ```c
   // Dr. Mario: simplest case
   void game_run_nmi(void) { func_NMI(); }
   // Zelda, Yoshi, etc: wraps with verify/oracle sync
   void game_run_nmi(void) { verify_mode_run_nmi(); }
   ```

4. **RTI is now handled correctly** by both the code generator and interpreter. The codegen emits RTI as:
   ```
   g_rti_target = (PCH << 8) | PCL;  // stored from stack
   ```
   The runner checks `g_rti_target` after `game_run_nmi()` returns:
   - If `0`: normal RTI - restore registers from pre-interrupt snapshot
   - If non-zero: RTI hijack - post-NMI code runs via `call_by_address(g_rti_target)`

5. **No `force_interp` needed**: `force_interp` stays empty — `0xFF08` in it garbles graphics. All NMI paths run natively via `func_NMI()` gated on PPUCTRL bit 7 in `game_run_nmi()`.

**Debunked**: earlier notes claimed coroutine-scheduler code (`coroutine_scheduler_setjmp`, fibers) on the NMI path. No such symbol exists in the tree; `func_NMI()` runs cleanly. Do not re-add workarounds for it.

## 4. Zelda NES Recomp — Detailed Comparison

**Repository**: https://github.com/mstan/LegendOfZeldaNESRecomp
**Status**: Believed 100% playable — overworld, dungeons, bosses, items, caves, inventory all working
**NESRecomp submodule**: commit `27b281e` (pinned)
**Two builds**: stock (CRC `0x3FE272FB`) + HD Remastered (CRC `0xFD9C577F`)

### game.toml Configuration

```toml
[game]
output_prefix = "zelda"
symbol_file = "symbols.sym"

[mapper]
bank_switch = [0xFFAC]    # Proper MMC1 bit-bang routine at $FFAC

[[inline_dispatch]]
addr = 0xE5E2             # ASL A; TAY; PLA×2; LDA ($00),Y ×2; JMP ($0002)

[[sram_map]]
sram_start = 0x6C90
rom_start = 0xA500
bank = 1
size = 0x1370             # Zelda copies bank 1 ROM $A500 -> SRAM $6C90 during init

[functions]
fixed = [...]             # ~80+ entries
bank0 = [0x8300, 0xBFAC, 0xBF98]
bank1 = [0xA69D, 0xB3F9, 0xBFAC, 0xBF98, ...]   # ~100+ extra labels
bank4 = [0xBFAC, 0xBF98, ...]                    # Enemy AI dispatch targets

[[extra_label]]           # ~100+ entries for bank 1
addr = 0xA500, bank = 1
addr = 0xA570, bank = 1
...
```

### extras.c Architecture

- **SRAM persistence**: Uses shared `save_ram` backend
- **TCP debug server** on port 4370 (gated by `debug.ini`)
- **`game_on_init()`**: Debug server init, voxel init, SRAM legacy path migration
- **`game_on_frame()`**: Voxel hotkeys, debug server polling, input override from debug server
- **`game_post_nmi()`**: Debug frame recording
- **`game_run_nmi()`**: Delegates to `verify_mode_run_nmi()`
- **`game_run_main()`**: Supports `RUN_MODE_EMULATED` and `RUN_MODE_VERIFY`
- **`game_dispatch_override()`**: **Only handles SRAM remapping** (`0x6C90`-$7FFF → bank 1 `$A500`+). **Does NOT handle controller polling.**
- **Entity diagnostics**: `game_fill_frame_record()`, `game_handle_debug_cmd()` with `entity_snapshot`, `entity_slot` commands

### Key Differences from Super Dodgeball

| Aspect | Zelda | Super Dodgeball |
|--------|-------|-----------------|
| `bank_switch` | `[0xFFAC]` (proper MMC1 routine) | `[0xFF08]` (proper MMC1 routine) |
| `game_dispatch_override` | SRAM remapping only | Narrow `0xFE8A` PPUCTRL-shadow arm only |
| `sram_map` | `0x6C90`-`0x7FFF` from ROM `0xA500` | None (no-battery MMC1) |
| `extra_label` | ~100+ entries for bank 1 | None (add only with proof) |
| `data_region` | In `game.discovery.toml` | None |
| `inline_dispatch` | `0xE5E2` | None (add only with proof) |
| `force_interp` | Not present | Empty (stays empty) |
| Debug server | TCP on port 4370 | None |
| `game_run_nmi()` | Delegates to `verify_mode_run_nmi()` | `func_NMI()` gated on PPUCTRL bit 7 |
| Input | Recompiled code handles natively | Recompiled code handles natively (`$4016` served by runner; title waits for Start) |

### Zelda Issues (from ISSUES.md)

- **Black overworld background** — SRAM functions bank-switched but code generator hardcoded JSR/JMP to bank 1. Fixed by modifying `code_generator.c` to emit `call_by_address()` for SRAM-sourced functions.
- **Cave soft lock** — Entity spawning broken
- **Enemies don't spawn** — Smoke clouds play but enemies never fully spawn
- **Link only has upper body** — 8x16 sprite missing lower body tiles
- **New area renders as all-zeros briefly** — PPU nametable update timing
- **HUD "DCE" garbled** — Tile mapping for counter digits

---

## 5. Super Mario Bros NES Recomp — Detailed Comparison

**Repository**: https://github.com/mstan/SuperMarioBrosNESRecomp
**Status**: Fully playable
**249 commits**, mapper 0 (NROM) — no bank switching

### Key Differences from Super Dodgeball

| Aspect | SMB | Super Dodgeball |
|--------|-----|-----------------|
| **Mapper** | NROM (0) — no bank switching | MMC1 (1) — working via `bank_switch=[0xFF08]` |
| **game.toml** | Has `[[mod_function_hook]]`, `[[ram_read_hook]]`, `[[inline_dispatch]]` | Minimal: `fixed` + bank-0 `extra_func` + `replace_func $FCA0` |
| **extras.c complexity** | Moderate (delegates to mods) | Low (family-shape hooks + one replacement) |
| **NMI handling** | Native, no override | Native `func_NMI()` gated on PPUCTRL bit 7 |
| **Input** | Recompiled `ReadJoypads`/`ReadPortBits` handles natively | Recompiled code + runner `$4016` emulation |
| **Bank switching** | None needed | Working (`$FF08` routine, `$A220`-table entries verified) |
| **Debug server** | TCP on port 4370 | None |
| **game_dispatch_override** | Returns 0 (not needed) | Narrow `0xFE8A` arm only |
| **`game_run_nmi()`** | `verify_mode_run_nmi()` | `func_NMI()` gated |

### SMB extras.c Key Features

- **No game-specific initialization** in `game_on_init()` — just watchdog, widescreen, voxel, mod init, debug server
- **`game_run_nmi()`** simply calls `verify_mode_run_nmi()` — the recompiled code handles NMI natively
- **`game_run_main()`** supports `RUN_MODE_EMULATED` and `RUN_MODE_VERIFY`
- **Input handling**: Via `game_smash64_update_input()`, `game_sonic_update_input()`, etc.
- **Audio**: Mod-specific (Sonic uses YM2612 via ymfm)
- **TCP debug server** on port 4370 with full RAM inspection commands
- `game_dispatch_override()` returns 0 (not needed for NROM)
- `game_ram_read_hook()` delegates to `game_smash64_ram_read_hook()`

### SMB Commit History Insights

Key commits showing what made it work:
1. Started with working NESRecomp base, then layered on widescreen, character replacements, voxel, debug server
2. `Merge pull request #6 from vibecodekun/feat/sonic-s3k-accuracy` — Latest merge
3. `Vendor ymfm for YM2612 emulation` — Audio dependency
4. `Pin merged widescreen engine master` — "Build passed; 120/120 stock smoke hashes match"

---

## 6. RetroPortingToolKit Documentation

### Source

Fetched from https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs — 54+ markdown files across 7 sections:
- **01_start**: What is static recompilation, how a port is made, is this emulation, quickstart
- **02_concepts**: Recompiler and runtime, code discovery, HLE vs LLE, code you can't see ahead of time, co-simulation, accuracy and burndowns, timing models, determinism, the game file you supply, glossary, start with original game
- **03_platforms**: NES, SNES, PlayStation, GBA, Genesis, Master System, DS, Virtual Boy, CD-i
- **04_guides**: Build a toolchain, port a game, write a mod, translate a game, add widescreen, debug a divergence, set up co-simulation, release a port
- **05_agents**: Start here, house invariants, verification rituals, failure modes, machine surfaces, contributing as an agent, when you cannot run the game
- **06_reference**: CLI, TCP protocol, TCP port registry, mod manifest, configuration, catalog schema, status vocabulary, errors and exit codes, site tools
- **07_fleet**: Repositories, lineage and credit, licenses, provenance

### House Invariants (from 05_agents/02_house-invariants)

1. **No stubs** — Never create stub functions that return default values
2. **Fix the tool, not generated code** — If generated code is wrong, fix the recompiler or game.toml config
3. **Find first divergence** — When a port breaks, find the FIRST frame where it differs from the oracle
4. **Classify the layer** — Is the divergence in codegen, runner, config, or game logic?
5. **Fix the source** — Fix the root cause, not the symptom
6. **Dispatch misses are blocking** — Every `call_by_address` target must be a recompiled function
7. **Never edit generated/ directly** — Always regenerate via NESRecomp
8. **Align on hardware events** — Prefer VBlank, DMA completion, timer overflow over frame numbers
9. **Use always-on traces** — Ring buffers and history queries recording before the bug happened
10. **Treat dispatch misses as blocking** — Can skip a whole subroutine without crashing
11. **Unknown is allowed** — Guessing is worse than saying "unknown"
12. **Prove the change** — Build output, test output, dispatch-miss status, coverage, oracle comparison

### Failure Modes (from 05_agents/04-failure-modes)

| What you see | What it may mean |
|---|---|
| One behavior never happens | A dispatch miss skipped a subroutine |
| A fix disappears later | The agent edited generated output |
| The fix only works for one game | A framework bug was hidden in game config |
| Tests pass but the game is wrong | The test did not cover the route |
| Native and oracle diverge at different places each run | Route not deterministic, or input timing changed |
| Two implementations agree on a wrong value | They share the same bug |
| A visual bug appears after a timing tweak | Timing tweak changed real game behavior |
| A release archive is too large or suspicious | May include generated junk or forbidden inputs |

### Verification Rituals (from 05_agents/03-verification-rituals)

- Build proves the compiler accepted code, not that the game is correct
- Dispatch misses must be empty or unchanged for a known reason
- Coverage must not regress
- Co-simulation: both sides must stop at same guest-time checkpoints
- TCP input: good for menus and simple button presses, not replacement for skilled gameplay
- Use hardware events (VBlank, DMA, timer overflow) not frame numbers
- A green build is weak evidence — a green test suite is stronger

### Port-a-Game 6-Step Workflow (from 04_guides/02_port-a-game)

1. Identify the exact game (title, region, revision, hashes)
2. Fetch the framework (pin matters — newer framework can change everything)
3. Teach the recompiler about the game (functions, data, banks, indirect calls)
4. Generate the code (check for dispatch misses)
5. Build and run (expect game to ask for game file)
6. Fix the right layer (problem → belongs in: recompiler, runtime, config, discovery, mod)

### What Does Done Mean?

Booting is not done. A useful bring-up checklist:
- Does the file identity check work?
- Does the game reach title screen?
- Does input work?
- Can you reach gameplay?
- Do saving and loading work?
- Does audio behave?
- Do menus and transitions work?
- Are dispatch misses resolved?
- Has the port been compared against a reference?

---

## 7. MesenCE API Research

### MesenCE as Oracle

Frame reference generation uses the **MesenCE binary directly** (`external/mesence/Mesen`) with `--testRunner` mode and a Lua script (`tools/frame_gen/export_frames.lua`). The Lua script registers an `endFrame` callback using `emu.addEventCallback()` and captures frames via `emu.getScreenBuffer()` and `emu.getScreenSize()`.

The co-simulation compares recompiled game screenshots (via `--save-screenshot`) against MesenCE reference frames. See `tools/frame_gen/run_frames.sh` and `tools/frame_gen/export_frames.lua` for the implementation.

### MesenCE Lua API (Full Reference)

**Memory:** `emu.read()`, `emu.write()`, `emu.read16()`, `emu.write16()`, `emu.read32()`, `emu.write32()`, `emu.convertAddress()`, `emu.getLabelAddress()`, `emu.getMemorySize()`

**CPU:** `emu.getCpuState()`, `emu.setCpuState()`, `emu.getCpuCycleCount()`, `emu.getMasterClock()`

**Events/Callbacks:** `emu.addEventCallback()`, `emu.removeEventCallback()`, `emu.addMemoryCallback()`, `emu.removeMemoryCallback()`

**Video:** `emu.getScreenSize()`, `emu.getScreenBuffer()`, `emu.setScreenBuffer()`, `emu.getPixel()`, `emu.takeScreenshot()`

**Input:** `emu.setInput()`, `emu.getInput()`, `emu.isKeyPressed()`, `emu.getPressedKeys()`

**State:** `emu.getState()`, `emu.setState()`, `emu.createSavestate()`, `emu.loadSavestate()`

**Control:** `emu.stop()`, `emu.breakExecution()`, `emu.step()`, `emu.rewind()`
- **`emu.reset()` and `emu.resume()` crash with exit code 255 in --testrunner mode**

**Debug:** `emu.getCdlData()`, `emu.getAccessCounters()`, `emu.resetAccessCounters()`, `emu.getRomInfo()`

**Info:** `emu.getScriptDataFolder()`, `emu.getLogWindowLog()`

### Event Types
- `emu.eventType.inputPolled` — safe point for `emu.setInput()`
- `emu.eventType.startFrame` / `emu.eventType.endFrame`
- `emu.eventType.Nmi` / `emu.eventType.Irq` / `emu.eventType.Reset`
- `emu.eventType.StateLoaded` / `emu.eventType.StateSaved`
- `emu.eventType.CodeBreak` / `emu.eventType.HaltStarted` / `emu.eventType.HaltEnded`

### Memory Types (NES)
- `emu.memType.nesInternalRam`, `nesPrgRom`, `nesChrRom`, `nesNametableRam`, `nesPaletteRam`, `nesSpriteRam`, `nesSecondarySpriteRam`

### CPU Types
- `emu.cpuType.nes` — NES 6502

### Headless Settings
- `EnableTestMode: true`, `Debug.ScriptWindow.AllowIoOsAccess: true`, `Debug.ScriptWindow.AllowNetworkAccess: true`, `Debug.ScriptWindow.ScriptTimeout: 60`
- `Nes.RamPowerOnState: 1` (AllZeros), `Nes.RandomizeMapperPowerOnState: false`, `Nes.RandomizeCpuPpuAlignment: false`
- Remove `mesen.lock` from `external/mesence/` before running

### mesen-for-ai Project (Reference — evaluated, rejected)

**URL**: https://github.com/paulomanrique/mesen-for-ai

MCP server wrapping the Lua API (stdio JSON-RPC + TCP bridge, xvfb, per-session Mesen). Rejected: every read it does is one `emu.read()` line in plain Lua; the raw `--testRunner` + file-writing script (§16, `tools/frame_gen/oracle_dump.lua`) covers all oracle needs with no extra processes.

---

## 8. Existing NES Decompilation Projects (Reference)

| Game | Stars | Approach | Status |
|------|-------|----------|--------|
| SMB (nukep) | — | C/C++ translation | Complete |
| Zelda 1 (aldonunez) | 262 | Full disassembly | Complete |
| Contra (vermiceli) | 226 | Annotated disassembly | Complete |
| Tetris (CelestialAmber) | 106 | Byte-for-byte rebuild | Complete |
| Final Fantasy (Entroper) | 99 | Full disassembly | Complete |
| SMB (mstan/nesrecomp) | 18 | Static recompile | Playable |
| Zelda (mstan/nesrecomp) | 11 | Static recompile | 100% playable |
| Bomberman (emu-russia) | 30 | Decompiled source | Complete |
| Tecmo Super Bowl | — | Exhaustive disassembly | 2024 |

**No *finished* public Super Dodge Ball disassembly exists** — corrected 2026-10-14,
the previous version of this line said no disassembly exists at all and was wrong.
There is a partial one: aj_187's [temporary disassembly
page](https://ajworld.net/sdb/), last updated 2012/12/11 (Release 02), cc65-based,
credited on that page to Rahga and Hyde. It is **not usable as-is**: the author
describes it as needing re-structuring before others could work with it, the CHR
build was unfinished, and **the ROM it reconstructs differs from stock by several
bytes** — all of them re-enabling 3/4-player Bean Ball, plus nametable text. So it
is a lead, not a reference, and nothing here was derived from it. The SNES port by
Rumbleminze remains the closest usable reference (§13 takes its fixed-bank
annotations from there).

---

## 9. Related Resources

- NESdev Wiki: https://www.nesdev.org/wiki/Tools
- RetroReversing (NES): https://www.retroreversing.com/nes
- NESdev Forum: https://forums.nesdev.org/
- Romhacking.net game page: https://www.romhacking.net/games/814
- StrategyWiki: https://strategywiki.org/wiki/Super_Dodge_Ball_(NES)
- SNES Port repo: https://github.com/rumbleminze/super-super-dodgeball
- Partial NES disassembly (WIP, byte-modified ROM): https://ajworld.net/sdb/
- wideNES (the approach §25 deliberately is not): https://github.com/daniel5151/ANESE, https://prilik.com/blog/post/widenes/
- TASVideos submission #4976 (the co-simulation input, §21): https://tasvideos.org/4976
- blargg test ROMs (the engine gate, §20): https://github.com/christopherpow/nes-test-roms
- NESRecomp docs: `/docs/` directory in repo, Discord: `discord.gg/Ad9BwSzctP`
- RetroPortingToolKit Docs: https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs
- mesen-for-ai: https://github.com/paulomanrique/mesen-for-ai
- Zelda NES Recomp: https://github.com/mstan/LegendOfZeldaNESRecomp
- SMB NES Recomp: https://github.com/mstan/SuperMarioBrosNESRecomp
- NESRecomp: https://github.com/mstan/nesrecomp

---

## 10. NESRecomp Submodule Status

**Tracks `mstan/nesrecomp` `origin/master` (currently `2b4fcdb`), pinned.** The
checked-out tree is dirty exactly when an active patch is applied: `setup.sh`
applies `patches/*.patch` after every checkout (006 PPU-overclock MMC1,
007 window vsync — see `patches/active-patches.md`); archived legacy-runner
patches stay unapplied. The retried-legacy 002 S-latch guard is drafted in
plan.md. Our game project is the repo-root `CMakeLists.txt`, which wraps the submodule's
`runner/cyc/project.cmake` and adds `CXX` + `MODS` + `GAME_ID` + `HOST_EXTRAS` — the submodule's own
`runner/cyc/project/CMakeLists.txt` cannot express those, which is why it stays untouched.

Upstream history includes the PPUMASK rendering fix (PR #22) and register-propagation/8x16-sprite
fixes. The cycle backend is upstream's primary path; the legacy function-level runner is "retained",
not primary.

---

## 11. Current Project Build State

- **nesrecomp submodule**: `mstan/nesrecomp` `2b4fcdb` (origin/master), pinned; tree dirty only with active patches 006/007 applied — see §10
- **Game project**: repo-root `CMakeLists.txt` (adds `CXX`/`MODS`/`GAME_ID`/`HOST_EXTRAS`); builds `build-cycle/nes_game`
- **Generated C code**: `build-cycle/cycle-nes_game/<rev>/generated/game_cyc*.c`, gitignored, one revision dir per ROM/config revision
- **Builds**: `build-cycle/` only (the legacy `build/` and `build-diag/` are gone with the legacy runner)
- **Config**: `src/game.toml` carries the display name plus twelve `[[mod_function_hook]]` sites (ten for the character-flicker mod, two for the widescreen margins) and a `cycle_seed_file`; no `extra_func`, no `force_interp`, no discovery tuning needed for the stock ROM
- **Mods**: `super-dodgeball.character-flicker`, `super-dodgeball.ppu-overclock` and `super-dodgeball.widescreen`, all off by default; the widescreen package is **one feature** (`widescreen`, with an `aspect` choice) that also reveals the clipped left-hand columns and draws the off-screen players. Packages preloaded into `build-cycle/mods/` at build time; window picks them up via the launcher, headless needs `--mods-root build-cycle/mods`. A `state.toml` naming a feature this build does not have (e.g. one written before the overscan fold) **hard-fails the whole catalog** rather than degrading — delete it and let the launcher rewrite it. VSync is a launcher Graphics setting (patch 007), not a mod. Reproduction vector for both flicker fixes: `tas/vectors/sdb_idle.input`
- **Native share**: ~60–77% of cycles run recompiled code, the rest via the interpreter — bit-exact either way, informational only, never a gate. `src/cycle_seeds.txt` roots the sprite emitter natively so the mod's hooks can exist there at all; total cycle count unchanged by it
- **Verification**: 400-frame TAS byte-exact vs Mesen from boot (RAM/OAM/palette/CIRAM); blargg cpu tier 25/25
- **Venv**: `nesasm` 0.0.8 only (pip broken); system `python3` has Pillow + numpy
- **MesenCE binary**: `external/mesence/Mesen` (pre-built, fetched by `setup.sh`)
- **Keybinds**: configured in the launcher, persisted to `build-cycle/config.ini` (no `keybinds.ini`)

---

## 12. Verification Approaches & Disassembly Tooling

### Why C→x86→ROM Comparison Is Unsounded

The NESRecomp toolchain produces C source code compiled to native x86-64. Comparing generated x86 assembly against original 6502 machine code is a category error — they are different ISAs with no meaningful mapping. No recompilation project uses this approach. The verification target is C source behavior, not its compiled form.

### The Gold Standard: Co-Simulation

The entire RetroPortingToolkit ecosystem uses **co-simulation** for verification:
- Run the recompiled binary alongside a trusted emulator (MesenCE) as oracle
- Feed both the same game and same input
- Stop both at the same guest-time checkpoint (VBlank, DMA, etc.)
- Compare CPU registers, RAM, VRAM, cycle counts
- "Self-agreement is NOT accuracy" — both sides can be identically wrong
- See `COSIM.md` and `NES_ACCURACY_BURNDOWN.md` in the upstream nesrecomp repo

### Available Disassembly Tools

| Tool | Location | Purpose |
|------|----------|---------|
| **`tools/dis65.py`** | repo | Dependency-free 6502 disassembler (official opcodes, linear sweep). Verified `$82BD`-family, `$A220`, `$FF3A`/`$FF4C`/`$FCA0`. Use this. |
| **nesasm** | venv (`nesasm` 0.0.8) | Assembler only, **no disassembly mode** |
| **capstone/py65** | Listed in `pyproject.toml` but **never installed** (venv pip broken) | Unavailable — do not reference as usable |

**py65** would need an MPU device instance anyway; capstone has no NES memory-map awareness. Neither is worth fixing while `dis65.py` covers entry verification.

### Practical Verification Pipeline

What actually works for this project:
1. **Co-simulation** against MesenCE (primary method)
2. **Dispatch miss monitoring** — 0 misses means all `call_by_address` targets are recompiled
3. **Symbol files** — Use ca65 disassembly or capstone to generate `.sym` files for `game.toml` to verify function boundaries
4. **Frame comparison** — MesenCE reference vs recompiled screenshots (frame 1+ ~99.8% structural match vs Mesen 11; metric is tolerance-based, exact RGB never matches across renderers)
5. **`run_roundtrip.py`** — Trivial byte-preservation check via `.db` directives (not meaningful for correctness)

### The "True" Round-Trip Problem

A true C→6502→ROM round-trip would require decompiling the generated C back to 6502 assembly, then assembling it back to ROM. This is the inverse of the recompiler — a hard research problem nobody solves automatically. Not feasible as a verification tool today.

---

## 13. NMI Dispatcher, RESET Sequence, Title writer (from SNES Port Fixed-Bank Bytes)

Source: `rumbleminze/super-super-dodgeball` `src/bank7.asm` (verbatim NES fixed-bank bytes with annotations). No public full disassembly or RAM map exists elsewhere.

### NMI dispatcher (`$FF3A`) — 4-way decode of `$0100` (N=bit7, V=bit6)

```
BIT $0100
BPL → NMI-off tail     ($0100 = $00)
BVC → JMP $EF9F        ($0100 = $80, gameplay/menu NMI)
JMP $EFD9              ($0100 = $C0, title NMI)
; at $FF47: BVC $FF4C else JMP $EF80  ($0100 = $40, third path)
; $FF4C tail: PPUCTRL NMI-off, 5×PLA, STA $0100, PLA, RTS
```

- `$EFD9` (title): `JSR $FE34` (OAM DMA) → `JSR $F700` (title staging) → if `$0106` bit0: `JSR $E49C` + `JSR $DB7A/$DF58/$DB4E` → clear `$0106` → RTI
- `$EF9F` (gameplay): set `$0106` bit7 → `JSR $FD5D` → Start check (`$F7 & $F5 & #$10`) → on Start: `JSR $FE8A` + stack reset (`LDX #$01FF; TXS`) + `JMP $F09F`, else sound/scroll work → clear `$0106` → RTI. **Never C-call `$F09F`** — it must be entered via the NMI stack-reset path.
- `$0100=$80` at NMI time runs `$EF9F`, which never calls `$E49C`. Note: the working build shows the title with `$0100=$80/$91` persistent — the strips arrive via main-thread setup (`$F378` chain), not the `$E49C` gate, so `$C0`-or-bust is not the whole story; `$0106` stays `$00` throughout.

### Writers of `$0100`

- RESET: `LDA #$C0 STA $0100`, `LDA #$50 STA $0101`
- `$F378` (title setup): `LDA #$80 STA $0100`, `LDA #$00 STA $0106`
- `$FCA0`: saves `$0100`, writes `$0100=$00` (NMI-off critical section), re-enables NMI
- NMI exit tail: `STA $0100` (lock handoff from stack)

### RESET (`$FF61`, via `$FFE8` stub: `INC $FFF1; NOP×3; JMP $FF61`)

`SEI; LDA #$10 STA $2000/$FF; CLD`; 2× vblank wait; `STX $FD/$FC/$FB`; `STX $4016`; `$F9=$FF`; `$4017=$40`; `$4015=$0F`; `LDA #$1E JSR $FE96` (MMC1 init = PRG mode 3, 4KB CHR); `$0100=$C0, $0101=$50`; soft-reset magic (`$0102==$35 && ($0103==$53||$AC)` → warm via `JSR $FCDF`, else cold clears `$0001–$00F8`); `CLI; JMP $F036`. FDS-BIOS `$0100/$0101/$0102` convention — see https://www.nesdev.org/wiki/FDS_BIOS.

### Title writer (`$E49C`)

PPU addr `$20:($40+X)` with `$5F`-clamp, `X=$0107`, up to `$20` bytes/frame from `$0108,X → $2007`, `STX $0107`; if `$70` V-flag: `$0168→$20A2`, `$0169→$20BD`. Staged by `$F700`, gated by `$0106` bit0. Menu indices `$06B1` (mode) / `$06EA` (difficulty).

## 14. game.toml Never Parsed — All Prior Runs Used Default Config

`addr = 0x82BD, bank = 0` on one line is invalid TOML ("extra chars after value"). The committed `game.toml` had this shape, so every regen fell back to default config: no `bank_switch` modeling, no `fixed`/`bankN` lists, no `force_interp`, no `extra_func`. Correct shape:

```toml
[[extra_func]]
addr = 0x82BD
bank = 0
```

Current working config: `bank_switch = [0xFF08]`, minimal `fixed` (verified entries only), dis65-verified bank-0/1/6 `extra_func` (30/30 resolved), empty `[force_interp]`. Validate every regen with the `[Eval] Coverage` line (must show resolved entries, no "could not load game config").

### Verified bank-0 function entries (hand-disassembled with `tools/dis65.py`)

`$82BD, $82D4, $82DB (tail JMP $81B5), $82E6, $82ED, $831C, $8323, $81B5` + `$B0D1` — small `LDA ($61),Y → STA $07xx,X` writers dispatched from `$A220` (bank 6) with X-index. `$AC89` (bank 6) is zero padding — never add `extra_func` for `ZERO_FILLED` targets. `$31D1`-style sub-`$8000` addresses are PPU mirrors, never codegen targets.

## 15. Hang Cause Verified; Cheat Obsolete With Working Bank Switch

- The `g_ppumask = 0x00` direct write in `game_post_nmi()` (commit `3194d28`) is the 3-frame-hang cause: it bypasses the runner's `|= 0x1E` translation and breaks VBlank timing. Removal → `smoke 15` finishes, exit 0, 0 dispatch misses.
- `force_interp` on `0xFF08` (MMC1 bit-bang) garbles graphics — the bit-bang must run natively. Bisected: empty `force_interp` restores baseline pixels exactly.
- With `game.toml` loading, `bank_switch=[0xFF08]` works, so real game code runs and owns the nametable (`func_FF61` zeroes it per frame). Manual tile writes cannot survive: orange-background frame (21k orange px vs 49k black) proves real code overwrites cheat tiles. Title must come from the real NMI path, not `game_post_nmi`.
- Stripped build (family-shape hooks) hangs in frame 0 at `$FCB3: BNE $FCB3` (fixed bank, after NMI-enable). The only exit is the NMI-off tail (`$FF4C`: NMI-off, `PLA×5`, `STA $0100`, `PLA`, `RTS`), which ends in `RTS` so `g_rti_target==0` and the runner restores registers+stack, rewinding the unwind. Dispatch trace goes silent after 5 events (last: NMI dispatch from `$FCB3`). Candidate submodule-level fix: NMI-tail unwind semantics for RTS-terminated handlers. Debug with `NESRECOMP_DISPATCH_TRACE=8000:FFFF` + `NESRECOMP_DISPATCH_TRACE_FILE` (needs a diagnostic build — prod compiles tracing out).
- `tools/dis65.py`: minimal dependency-free 6502 disassembler (ROM file + hex addr + count + file offset). Use for entry verification; venv has no capstone/pip.
- An NMI-off spin freezes smoke: 40M+ boundary ticks at `$FCB3` with ops sawtoothing (budget consumed, no completions; temporary tick-rate sampler). Separately, entering `func_NMI` with NMI disabled corrupts the main stack (no hardware frame was pushed for the tail to pop) — `game_run_nmi` must gate on PPUCTRL bit 7. NMI-off branch behavior (`nes_cosim_emit_boundary` vs callback) is as-read in `runtime.c`; the exact counting interplay when PPUCTRL flickers mid-frame is unverified.
- Native RTS is a C return: the `$FF4C` tail's g_ram unwind is ignored for control flow, so execution resumes inside the spin. `game_run_nmi` must gate on `g_ppuctrl & 0x80` (ungated NMI-off entry corrupts the main stack — no hardware frame was pushed). Forcing `$0100=$C0` keeps NMI enabled and frames advancing (15, static wrong screen) but strands main in the spin.
- Runner unwind idea archived, not shipped (`patches/archived/001_nmi_tail_unwind.patch` + `archived-patches.md`): pop the skipped RTS return and resume the guest there at frame end. Never fired successfully — the game-side `replace_func` bypassed the tail path first. Evidence kept: NMI survey (§17), SMB3 A/B (byte-identical), pixel-identical title with/without. Revival conditions in the archive README.
- RESOLVED via `[[replace_func]]` for `$FCA0` instead: the `$FCB3` spin is removed game-side (NMI-enable + PPUMASK apply retained). Current form is split — instant pre-menu, faithful NMI-waiting park post-tunnel (`s_menu_phase`) pacing the menu loop (see §19.6). Result: title 99.8% structural match vs Mesen frame 11 from frame 1, `smoke 30` exit 0, 0 dispatch misses.
- Open (updated 2026-10-01): `$8003` bank-8 miss resolved (S-latch fallout); BRK skips benign/transient; `func_DB4E` load-bearing untested; input tunnels past title (see §19).

## 16. MesenCE Oracle: Raw --testrunner + Lua, Not mesen-for-ai

mesen-for-ai (MCP server) adds TCP/xvfb/session machinery for zero extra PPU visibility — every read it does is one `emu.read()` line in plain Lua. Use raw `--testrunner` + file-writing Lua. (Re-evaluated in full for §23: it cannot set emulator config at all, so it is still rejected — but three things in it are worth stealing, and the MesenCE config-location bug it works around is recorded there.)

- Mapped reads (what the game sees, MMC1 applied): `emu.read(addr, emu.memType.nesPpuMemory)` for `$0000–$1FFF` (live CHR), `$2000–$2FFF` (nametables, mirroring applied). Raw ROM: `nesChrRom` (131072 bytes), `nesPrgRom`. RAM: `nesInternalRam`; PPU regs: `nesPaletteRam` (32B), `nesSpriteRam` (256B), `nesNametableRam` (CIRAM 2K pre-mirroring). Prefer `*Debug` memTypes (`nesDebug`, `nesPpuDebug`) — plain reads have side effects (`$2002` clear, `$4016` advance).
- No mapper-state API. Live CHR banks via `emu.convertAddress(ppuAddr, emu.memType.nesChrRom, emu.cpuType.nes)` per frame, or fingerprint the 4K mapped window against CHR-ROM slices.
- Per-NMI flags: `emu.eventType.nmi` callback + `emu.getCpuState(emu.cpuType.nes)` + RAM reads (`$0100/$0106/$0107/$0108/$0168`). Scroll/video-latch keys come from `emu.getState()` (key names vary by build — enumerate, never hardcode).
- Frame counting in `endFrame`; exit with `emu.stop(0)`; output via files (`io.open`) or `emu.takeScreenshot` — testrunner stdout does not carry logs. Never call `emu.reset()`/`emu.resume()` in testrunner (exit 255). `createSavestate` only works inside `exec` callbacks.
- Headless needs `xvfb-run` + isolated HOME with `settings.json` (`AllowIoOsAccess`, `ScriptTimeout: 60`, `RamPowerOnState: 1`, no mapper/CPU-PPU randomization); delete `mesen.lock`.
- Reference oracle script: `tools/frame_gen/oracle_dump.lua` (frame-N PPU dump + per-NMI flag log + Start injection + mapper-key enumeration).
- Who-wrote-it queries: `emu.addMemoryCallback(fn, emu.callbackType.write, start, end, emu.cpuType.nes, memType)` + `pc` from `getCpuState` inside; CDL tables are 0-indexed in a 1-indexed language.

---

## 17. NMI Termination Survey (Patch Blast-Radius Evidence)

Question: which games would behave differently if post-NMI rewind is skipped whenever `S != entry S`? Surveyed via disassembly repos + nesdev wiki.

| Game | Verdict | Detail |
|---|---|---|
| SMB3 | RTI | `IntNMI` pushes PHP/PHA/TXA/PHA/TYA/PHA + temps; all paths converge to PLA/PLP/RTI epilogue. Nested re-entry only DECs `VBlank_Tick`, same RTI. Only TXS is reset. |
| SMB1 | RTI | `NonMaskableInterrupt` ends `rti`. RTS routines are NMI-called subroutines, not top level. |
| Zelda 1 | RTI | `IsrNmi` ends PPUCTRL-restore + `RTI`. |
| Dr. Mario | RTI | NMI falls through into single-`rti` IRQ stub. |
| Metroid | RTI | Ends `LC113: RTI`, `NMIStatus` + `WaitNMIEnd` protocol. |
| Faxanadu | RTI | Pops into `OnHardwareInterrupt: RTI`. RTS-dispatch (`JSR $F859`) exists but only in main code. |
| Yoshi's Cookie | Likely RTI | No public disassembly; zero-divergence report under rewind runner implies balanced. |
| FDS BIOS | RTS (tail only) | `BIT $0100` dispatch; NMI-off path: 3×PLA discard + `PLA/STA $0100`, `PLA, RTS`. Only retail-FDS shape; cart ports (Metroid/Zelda) were rewritten to RTI. |

No surveyed cart leaves the NMI stack unbalanced; every TXS found is in reset/init. No nesrecomp issue covers NMI RTS-vs-RTI. Conclusion: the unwind branch is dormant for all surveyed titles (zero expected blast radius); it activates only for FDS-tail shapes. Caveats: Faxanadu's in-NMI RTS thunks could unbalance S via recompiler artifacts; SMB3 nested re-entry is safe by construction (each level snapshots its own entry S). Gate narrowly + log S-delta histograms before enabling globally.

---

## 18. Input Frontier (Title → Menu Transition)

Status: input flows but the game never reacts; Start press derails. All verified by execution (`--script` HOLD/RELEASE/WAIT_RAM8/DUMP_RAM + `NESRECOMP_TRACE_CTRL4016` + MesenCE oracle + diag-build chains).

> Superseded by §19 (all items below root-caused and fixed); kept as the pre-fix evidence trail.

- **Runner input path is correct**: scripted `HOLD START` shows `btn=10` on `$4016` reads frame 1. `$FDC2` is a textbook strobe/shift reader; runner latch/shift verified bit-for-bit. `g_controller1_buttons` fills from script/TCP/keyboard as designed.
- **Polls stop ~frame 18, deterministically**: `$4016` traffic (strobe writes + reads) ends; `$0100=$90` stable, `$0106` latches `$90` from ~20. EF9F's `BMI` skip then self-sustains (skip path never clears bit7; nothing in menu clears it). Clearing bit7 pre-NMI does not restore polls; clearing all of `$0106` hangs boot (staging bits load-bearing) — both reverted.
- **Hardware idles the same way**: oracle `nmi_log` shows every NMI from frame 14 taking the tail path (`$0100=$00`, `$0106=$80` stuck). Title/menu idle with silent NMI is authentic. Hardware Start must therefore arrive via **main-thread polls**: `JSR $FD5D` sites exist in bank1 `$BA6A` and bank4 menu code (`$8EE9/$9732/$999D/$99BA/$99E8/$9A04/$9ECE/$9F1E`). Our main evidently never reaches them (menu state frozen).
- **Start-during-poll derails**: EF9F Start branch (`JSR $FE8A`, `LDX #$FF; TXS`, `JMP $F09F`) → BRKs at `$0001`/`$0091`/`$0092` (RAM zeros), `$092D`/`$093D` (bank-3 data), `$0600`, `$00A2` (BRK-slide through zero page from ~frame 18) → hang. BRKCTX chain: `FCBF→FA15→8000(b0)→FA00→8003(b0)→80EF→8466/84A8/86A8` sound-cluster recursion, nested `FF3A` frames.
- **MMC1 bank-8 corruption at frames 14/16** (mapper trace): clean `6↔0` toggling until `$FF08` runs with entry A=`$90` — first write hits the bit7 RESET, the 5-bit sequence loses alignment, later commits land on invalid bank 8 (`$8003` miss, sound tick skipped → frozen tone = user's repeating-tone report). No static `LDA #$08`/`LDA $FF` call site exists; 12 PLA-fed `JSR $FF08` sites exist only in bank-7 file bytes (mapping unclear). Corruption precedes the frame-18 derail.
- **C-stack growth ~1 `func_FF3A`/frame** (diag HANG dump: 511 deep at frame 558, interleaved `EF9F/FA00/sound-cluster`): NMI C-frames accumulate without returning. Ticking bomb for long sessions; likely the same control-flow wrongness as the `$F09F` hang.
- **Frozen tone explained**: `$FA00` tick starved (`$8003` skipped on bank-8 miss) + polls stopped. Same root causes as input, not a separate audio bug.
- Interactive run (347 frames, clean window close): same BRKs plus `$00A2`@18. SDL_QUIT handling works; the unkillable case is headless-only deep hangs.

---

## 19. Input Frontier Resolution (Start Works; Menu Boots; 2026-10-01)

> **Historical.** This section (and §18) documents the *legacy function-level runner*, retired
> 2026-10-02. Every fix described below belongs to that runner and is **not** in the cycle build.
> Kept as game-knowledge history — the NMI/tunnel/input findings transfer to mod work, and the
> `patches/` files they mention no longer exist.

Status: Start tunnels to menus reliably (30/30 through 330/330
frames, 0 misses, 0 BRKs): title → city → lineup → MODE MENU → skill
(animated sprite) → team-select (`CHANGE POSITION?` + rosters + courts,
renders perfectly). Menu navigation verified (`$06B1` moves on dpad,
Start/A confirms advance screens). Fixed along the way: the frame-~182
derail (empty-stack `PLA PLA` in bank-6 `$8393` shared tail — see below).
Remaining: court BG stalls as tile-soup past team confirm (sprites/sound/
logic run; strips drain top rows only), and unbounded lap growth
(~5 C-frames/frame, slow-burn).

### 19.1 S-latch root cause: interpreter RTS after native JSR (RUNNER BUG, FIXED)

Polls stopped ~frame 18 with `$0106=$90` and no `nes_write` to `$0106`
after frame 18. Per-instruction S trace (`nes_cpu_instruction_boundary`
TEMP log of every S delta + PC) showed `func_FA00` exiting with S lifted
+6..+48 per call (entry F9 → exit FF/01/29). Narrowed to single +2 steps at
`$812A` (post-`JSR $84A8`) with no intermediate push/pop: the `$86C2
JMP $07B4` RAM trampoline (`STA $40xx; RTS` written by `$8035`) runs via
`call_by_address_tail` → `nes_interp_dispatch` → interpreter, whose RTS
pops 2 bytes as a return address. But the outer `JSR $84A8` was a NATIVE C
call that pushed nothing — so each trampoline RTS pops 2 bytes of live
caller data. 3 trampolines/frame → +6/frame → S wraps into `$0100`,
clobbers `$0106` to `$90`, `EF9F` BMI-skip self-sustains, polls die. All
follow-on symptoms (bank-8 `$8003` miss from corrupted A at `$FF08`,
`BRK $0001`/`$00A2`/`$0600` slides from interp garbage-wandering,
`$90` in `$0100`, C-stack growth) were downstream of the S drift.

Fix (`patches/002_interp_ram_dummy.patch`, active): in
`nes_interp_dispatch_bank`'s RAM/SRAM path, pre-push a dummy 6502-stack
pair before `interp_run` (standing in for the missing hardware JSR push),
strip it on exit if untouched. Terminal RTS then consumes the dummies and
the stack-lift rule exits with S exactly restored. Post-fix: `$0106`
clean, 0 misses, 0 BRKs. Also verified: `func_FA00` no-op experiment gave
perfectly balanced S (EFAE/EFD0 = F9 every frame), proving the sound
engine was the sole imbalance source.

### 19.2 NMI-tunnel unwind (RUNNER FEATURE + GAME WIRING, ACTIVE)

`EF9F` Start branch (`JSR $FE8A`, `LDX #$FF; TXS`, `JMP $F09F`) relocates
main into `$F09F` from inside NMI. C-calling `$F09F` never returns: the
outer frame callback never completes, frames freeze, no render/count.
Fix (`patches/004_nmi_tunnel_unwind.patch`, active): opt-in
longjmp unwind — game arms `g_tunnel_site = $EFC1`; a TXS-site marker
(`coroutine_scheduler_setjmp`, which already exists at every
`LDX #$FF; TXS`) reached while armed, at the site, inside NMI
(`depth > 0`) resets vblank depth and longjmps to `game_run_main`'s
landing, which runs `func_F09F()` natively on the main context. NMIs then
interrupt normally (hardware behavior). Generic mechanism (site 0 =
disabled, zero effect otherwise), game-specific arming. Companion
game-side pieces in `src/extras.c`: `NESTED_NMI_RUN_HANDLER` (nested
`$C0`/`$EFD9` must run — the transition lap's `$F1A2` wait is released
only by a real `$EFD9`, runnable solely from nested callbacks while the
tunnel is in flight), nested gate (only `$C0` runs nested — blocks
re-tunnel; `$80`/`$40`/`$00` skip S-safely via runner restore), Start-edge
inhibit until `$F0C3` (`$0100=$C0`, held Start can't re-tunnel mid-init),
one-service-per-frame pacing gate (nested `$C0` skips when the outer
already ran — without it the deferred budget-cap NMI double-clears
`$0106` and menu timers run ~2× ahead of hardware), code-window scoping.

Depth subtlety: inside `game_run_nmi`, depth 2 IS the top-level NMI
(trigger 1 + firing 1); truly-nested is > 2. A `> 1` gate silently skips
every handler (title still renders main-thread-only, polls never run).

### 19.3 Post-tunnel hardware behavior (ORACLE)

With Start@11-12, hardware shows: `$0100` oscillates `$80`/`$00` (never
`$C0` past frame 13!), `$0106=$80` stuck, ~1 NMI/frame (tails), `$F5/$F7`
decay to `$00`, `$06B0=$00` steady, `$78=$00` steady, `$69E` counting
~+1/frame, sp steady `$F6`. I.e. main runs everything, NMI is tails, and
the lineup idles indefinitely awaiting input. Our `$06B0` cycling (0-3)
vs oracle steady, and our `$78=$41` (authentic `F5E8`-init code reached
early via instant-`$FCA0` timing) vs oracle `$00`, are pacing divergences:
instant-`$FCA0` (spin removed) blows through NMI-syncs in one frame where
hardware parks ≥1 frame each. A faithful park version (pushes + boundary
spin till next NMI) was tried and REVERTED (title boot garbles to 23% —
park timing vs boot strip staging; cause unisolated). Spin-less `$FCA0`
stands (title 98.9%).

### 19.4 Discovery loop (all entries dis65-verified before adding)

Menu/init writer family (bank 0, `$A220`-table `LDA ($61),Y` writers +
APU-silence + setters): `$8308, $8312, $8381, $8388, $83AD, $83D3, $83E1,
$83FE, $82C1, $83C0, $8295, $82F6, $801B` (sound-engine alternate entry);
menu input checker `$897D` + sibling `$89A9` bank 1
(`BIT/BVS/BMI` + `LDA $27/CMP #$10` Start test, called from `$8809` at the
lineup→match transition). Excluded with proof: `$B183` bank 0 (data soup),
`$31D1` (ZERO_FILLED PPU mirror), `$8024` bank 0 (mid-entry `$8027`
trampoline setup — symptom of bad-vector dispatch, not an entry).
30/30 resolved, 0 suspicious. `$8388` silences the APU at Start
(`STA $4015`); title music verified perfect interactively. State machine
`$8E00` bank 6 (called from fixed `$C0F3`) added with proof. Benign skips
needing no entries: `$801B` via interp path and `$A3C7`
return-continuation (LOG_RETURN-safe, game completes).

### 19.5 Open: frame-~182 derail, lap growth, menu navigation

- Idle path derailed ~frame 182 (FIXED via `[[replace_func]] $8393` bank 6): single +2 S-lift, then `A=$10` stack garbage → `FF08($10)` → bank thrash → `BRK $C000` + `$8024`/`$8003` misses → hang. Root cause: bank-6 shared tail `$8393` reached with an empty stack ate `$0100/$0101`. Post-fix 240/240 frames clean.
- C-stack lap growth: `F1AA BNE $F13A` compiles to `call_by_address`
  (+1 C-frame/lap, depth 72→84 in ring, 509 at hang). `[[merge_range]]`
  over `F09F`/`F13A` ranges converts laps to gotos (verified in codegen)
  but CANNOT ship: merged entries double-emit across bank-7 codegen parts
  sharing one TU (`_full.c`) and break the build (recompiler limitation,
  documented in `game.toml`). Needs a recompiler-side dedup fix.
- Menu navigation: VERIFIED working — dpad moves `$06B1`, Start/A confirms advance mode → skill → team-select (`CHANGE POSITION?` + rosters + courts, renders perfectly). Confirm is Start-edge driven (`$F5&$10` at `$F445`/`$F63A`); A navigates nothing in `$F49A` (`AND #$2F` excludes it). Cursor/progress tracked via `$06B1` DUMP_RAM, not just pixels.
- User-reported (interactive): sprites glitchy in the two post-title
  menus (since improved — menus render cleanly paced), music perfect
  throughout; gameplay-time freeze reports predate the `$8393`/tunnel fixes
  and need re-testing against current build.

### 19.6 Menu loop + pacing (FCA0 park split, 2026-10-01)

Oracle who-writes trace (Start@10-12, frames 12-60) shows the hardware menu
loop is exactly 4 writes/frame: tail `STA $0100=$80` (interrupted PC low
byte `$80`), `INC $069E` @ `$F424`, `FCA0 STA $0100=$00` + park. Main runs
`$F403`-loop (FCA0-sync, FD5D polls, Start-edge check at `$F445`,
mode tables at `$F494/$F497`); `$06B0`/`$0106`/`$78` untouched, `$69E`
+1/frame, `$70` evolves. The game NEVER runs F09F-dispatch post-tunnel.

Recomp consequences: (a) instant-`$FCA0` lets the menu loop lap ~30×/frame
(`W69E` trace: 29 INCs in frame 10) — timers race, input uncontrollable.
(b) A full faithful park (also at boot) garbles boot strips (23%).
Fix: SPLIT `$FCA0` — instant pre-menu (title stays 98.9%), faithful park
(pushes + NMI-enable + boundary spin till one NMI) post-tunnel
(`s_menu_phase`, set at the tunnel landing). Park order matters: park must
run NMI-enabled (hardware enables before spinning); parking with NMI off
deadlocks till the iteration cap. With parks: title → city → lineup →
MODE MENU (`GAME MODE ... SKILL LEVEL ...`) → skill confirm animates.
Start/A/confirm inputs all process (hashes advance, 0 misses).

Caveat: the tunnel longjmp skips frame/vblank callback-depth decrements
(and tail/dispatch/stack-tracking cleanup). The landing must call
`runtime_prepare_guest_resume` (not just vblank reset) or frames freeze
(post_nmi/render/count skipped as "nested") and NMI never fires in C
spins. Symptom of the stuck depth: `[PARK] depth=0` yet no NMI for 100M
iterations (NMI-off path also implicated: park entry with ppuctrl=$00).

Transition tears: skill-confirm leads through 1-2 garbled frames (partial
redraw) that resolve into the next clean screen; not a stuck state.
`BRK $0001` seen once mid-transition (frame 135, skipped, no fallout).

### 19.7 Court BG stall (current frontier, 2026-10-01)

Past team confirm the BG stalls as tile-soup (uniform cleared nametable +
fragments) while sprites animate, sound ticks, input processes, and frames
advance with 0 misses to 330+ frames. Evidence: PPU-write trace during the
stall shows ONLY scroll (`$2005` via `DB4E`) — zero `$2006`/`$2007`; strips
drain top rows only (`$2060/$2080/$2040` via `E49C`, repeating); uploaders
(`$BA80` reader, `$BACC`, `$BB56`) never fire. Prep (`$B922-$B993`,
`$B9D9` 6-round `$90` cycle) and `$BD6A` waits (incl. 128-frame `$80`) run
repeatedly; `$017A` stream pointer never reaches its post-wait value, so the
long waits never complete (suspect: per-lap re-entry resets them, or the
draw phase is gated behind unmet state). `$0100=$00` (tails, no NMI
services), `$0106=$80` stuck, `$FF=$80` (BG pattern `$0000`).
`$B8C8`-region draw entries have no static callers (fallthrough/computed
only). Next: find the draw sequence's per-lap caller, or the gate that
withholds the content phase.

---

## 20. Engine validation via blargg harness (2026-10-01, `tools/blargg/`)

Recompiling blargg CPU singles against the current tree (patch-neutral
extras, wall-clock `$6000` poller — tests complete with 0 frames and park
in the shell's `forever` loop, so frame-boundary polling alone false-HANGs
everything). Baseline (runner = pristine `origin/master` + `patches/*.patch`): PASS 2 (`01-basics`, `16-special`),
FAIL 14, HANG 4 (`cpu_timing_test6`, `branch_1/2/3`), SKIP 5 (banked).

Durable backend-gap evidence (all reproduced on pristine `origin/master`,
so upstream's, not ours):
- JSR/RTS-template checksums over S and pushed return addresses mismatch:
  native JSR/RTS are C calls pushing nothing (`12-jmp_jsr`, `13-rts`;
  cf. upstream `NES_ACCURACY_BURNDOWN.md` Axis 4/6).
- `11-stack` → `internal_error` code 255: PLA/PLA stack-introspection pops
  live data (same class as the `$8393` tail, §19.5).
- `15-brk` → BRK skip-stub (BRK vectoring unimplemented, upstream-known).
- HANGs: APU-length-counter `$4015` spins (branch family) and periodic-NMI
  waits (`cpu_timing_test6`).
- Lap-growth corroboration: `instr_01` template helpers (`E442`/`E458`/
  `E8CF`) stack 510 deep like Dodgeball's `F1AA`/`F13A` (§19.5) — same
  `call_by_address` lap class, independent ROM.
Patches exonerated twice over (construction + pristine A/B with identical
verdicts and HANG stall frames); details in `tools/blargg/README.md`.

---

## 21. TAS-integrated differential harness (`tools/tas/`)

Shared-input co-sim: one TAS drives both the MesenCE oracle and the cycle
backend; diffs land on the first divergence. (Early design ran the legacy
runner via `--script` + smoke JSON + WRAM/PPUMEM deltas with stack masking —
retired with the cycle switch; the join-validation and metric lessons below
survived it.)

- **Vectors**: TASVideos #4976 (ShesChardcore & Dasrik 2022, BizHawk 2.8,
  19692 frames, hardest difficulty) in `tas/vectors/sdb_4976.bk2` —
  headerless MD5 matches our ROM (`6e44871b…`), first Start at frame 11
  (matches `oracle_dump.lua` Start@11-12). No USA Dodgeball `.fm2` exists
  anywhere (only `.fcm` #742 + `.bk2` #4976); `fm2_to_tas.py` covers text +
  binary logs and is validated on an SMB2 `.fm2` sample (1016 frames as
  listed; `RLDUTSBA→0xFF` unit-checked). Manufacturing a USA `.fm2` =
  FCEUX `Tools → Convert FCM` on the #742 `.fcm` (sync not guaranteed).
- **Design (cycle)**: bk2/fm2 → canonical `tas.json` (button mask bits) →
  MesenCE `mesen_tas.lua` (per-frame state.jsonl + NMI log; RAM/CHR/NT/
  pal/OAM/ciram bins + PPM at stride; input in `inputPolled` only) and
  cycle absolute-state schedule (`<frame> NAMES`, `-` = release) +
  `--frame-log-at mesen` binary snapshots (RAM/CIRAM/palette/OAM/picture)
  + `--shot-every` PNGs + `--miss-log` → `diff_tas.py` gates liveness
  (logged frames == N, exit 0) → renderer-independent state (snapshots
  byte-compared, stack included — the cycle CPU is real; sprite entry-0
  mirror bytes masked, §22) → images informational only. Frame 0 is black
  warmup (title from frame 1); no offset search — same-index flags real
  drift (neighbor-validated: same-index 9 diffs vs 20–110 off-by-one).
- **Metric lesson**: `compare_frames.py`'s palette was mostly `[0,0,0]`
  rows, collapsing all dark colors to one index (matched solid-green vs
  solid-black at "100%"). Table now equals the cycle renderer's palette
  (exact for cycle shots, nearest for Mesen). Cross-renderer RGB is still
  palette-relative by construction — state bytes are the cross-side
  signal, pixels are secondary.
- **Boot datum**: oracle shows backdrop-green at file 0, cycle stages
  strips over ~10 more files — systematic staging lag, filed as timing
  gap, not failure.
- **Honesty**: cycle snapshots byte-identical across repeat runs; Mesen
  A/A identical under pinned settings; real modeling gaps (power-on
  palette, uninitialized CIRAM, palette mirrors) get named byte-exact.
- **First finding**: 400-frame TAS run green on liveness but mode-menu
  confirm diverged ~TAS frame 44 on the legacy runner — hardware advances
  to team-select by f≈50–60, legacy idled on the mode/skill menu.
  RESOLVED as legacy timing (transition pacing ~2×), see §22.

---

## 22. Cycle backend (default play path, `build-cycle/`)

Upstream `runner/cyc`, built by our repo-root `CMakeLists.txt` (see §10): `build-cycle/nes_game`
from our ROM + `src/game.toml`, windowed (`./build_and_run.sh`; headless flags imply headless).
The guest machine runs **stock** — patches 006/007 are inert at stock settings,
and of the three mods only the flicker mod's truncation half is presentation-only
(§2); its culling half is not, and the overclocking mod changes console timing by
design (§23), while widescreen changes nothing at all (§25, §26).
Schedulable
input (`<frame> NAMES` absolute state, same bit map).

- **Mods**: all three are opt-in (off by default). The widescreen package is now a
  single `widescreen` feature — enabling it also reveals the clipped left columns and
  draws the off-screen players, because one compositor does all three (§25). They never appear in
  `--frame-log`/`--screenshot` output (those capture the machine's native frame); only
  `--present-out` and the window show them. The truncation fix (§2) is presentation only — same
  cycle count, `$02FD`/`$02FF` the only differing bytes. The culling fix (§2.1) is **not**: it
  makes the guest emit sprites it had dropped, so OAM differs (sprite slot 0 never does) and a
  few per-object timers settle at a constant offset, while the picture (CIRAM) is identical on
  899 of 910 frames.

- Game, 400 TAS frames: reaches team-select, matching Mesen byte-exact
  (RAM/OAM/palette/CIRAM from boot, mirrors masked) — the §21 f≈44
  divergence was legacy timing (transition pacing ~2×), not game logic.
  (~80% native unseeded; behavior hardware-true regardless.)
- blargg cpu tier 25/25: 20 RAM-text PASS (incl. `12-jmp_jsr` at 99.4%
  native — the JSR/RTS case the legacy runner fails) + 5 screen-confirmed
  PASS (branch_1/2/3, dummy_reads, timing_test6 at ~16 s). Verdicts read
  from shell text near `$0200` (`--mem-frame`/`--mem-out`; NROM has no
  `$6000` WRAM here). Screen-only tests save `shot.png` and report
  UNVERIFIED (non-blocking) instead of a wrong TIMEOUT.
- Palette-mirror note: sprite entry-0 bytes (`$3F10/$3F14/$3F18/$3F1C`)
  read stale in cycle snapshots (upstream model keeps power-on bytes;
  hardware mirrors the backdrop). Masked in the TAS diff; rendering
  unaffected.
- Full-movie finding (2026-10-02, #4976 19692f): byte-exact gate extends
  to frame 450 (RAM/CIRAM/palette/OAM, `$0106/$0107` ignored as volatile
  NMI/main handshake). First non-handshake split during frame 474
  (126 zero-page bytes; PPU still identical): entity-walk `$ACF6`
  (bank 4, `$8000` slot, `prgReg=4`) + shift/multiply `$D9xx` (bank 7)
  run the same code on both sides with different DATA — downstream, not
  causal. Cumulative cycle counts agree within +1..+8 over ~586M, so no
  timing-model drift; root is sub-frame NMI/main phasing or power-on
  hardware state (APU-phase candidate, untested). `$0100` agrees all
  19692 frames on both; tipoff visuals match Mesen/cycle/published video.
  The movie is AI-manipulation-dense (author notes), so it likely needs
  BizHawk-2.8-exact timing past tipoff — full-movie sync is out of scope
  for the gate; the gate stays 400 (450 observed).
- Input-phase ruled out (2026-10-02): ±1-frame shifted schedules run
  headless on cycle vs Mesen show identical `$0100` mode flow for 600
  frames under the same shift (both take the same fast/slow menu paths),
  while either shift breaks vs unshifted at frame 6 (stack byte `$01F6`).
  Both players share input semantics; the 474 split is execution phasing,
  not input handling.
- Windowed == headless — **SUPERSEDED 2026-10-13, do not rely on this.** The original
  2026-10-02 measurement (`--virtual-pad` vs `--input`, pixel-identical at
  frames 1/101/…/501) was five sampled frames at 256×240 with **no mods**, and it does
  not hold further in. A windowed recording replayed headless diverges at **frame 4**
  (scroll 32 vs 256, 496 frames matching on 0-3) and ends on a different game state —
  2666 vs 2121 off-edge captures, 32200 vs 15504 margin sprites drawn, 769 vs 752
  frames composed. Two windowed runs of one schedule *are* byte-identical to each other.
  Cause is **unattributed**; it is not input (the recorded frames are exactly the frames
  the schedule fired on, and the divergence starts four frames before the first press) and
  it is not the pacing/slowness noted under `--present-out`. Consequence for tooling:
  **analyse a windowed capture where it was made** — `trace.log` is the ground truth,
  `margin_track.py` reads it directly, and a headless replay describes a different game.
  The earlier "the window adds no divergence of its own" conclusion does not survive.
- Power-on state feeds the game (2026-10-03): Mesen `RamPowerOnState`
  `Random` vs `AllZeros` splits from cycle-zeros at frame 25 (same 16
  title/menu bytes, deterministic across boots) while frame-0
  RAM/OAM/palette/CIRAM dumps are identical — the game clears RAM on
  boot, so the seed enters via non-cleared state (APU/PPU-latch/mapper
  candidate, unprobed). Seed sensitivity is real but does not explain
  the 474 split (identical seeds there) and cannot recover BizHawk's
  stream.

---

## 23. PPU Overclocking: what the three emulators do, and that it works here
Goal: make the game feel less sluggish. Researched MesenCE, Nestopia UE and FCEUX,
then measured against the oracle. **It works**: `Nes.PpuExtraScanlinesBeforeNmi = 300`
gives the guest **+12.0 extra work-ticks per frame on average**, and the lead *grows*.

### The three implementations are not the same feature

| | mechanism | renders more lines? | NMI scanline moves? |
|---|---|---|---|
| **MesenCE** | `_nmiScanline += Before; _vblankEnd += Before+After` (`Core/NES/NesPpu.cpp:154` `UpdateTimings`) | **no** — always 256×240, `_scanline < 240` gate in `ProcessScanline` | **yes** |
| **FCEUX** | `X6502_Run(postrenderscanlines * 341)` before vblank; `X6502_Run(vblankscanlines * 341)` after NMI (`src/ppu.cpp:1862`, `:1784`). Keys `postrenderscanlines` / `vblankscanlines`, 0–999 | **no** — `normalscanlines` unchanged | **no** |
| **Nestopia UE** | `Cpu::EndFrame()` runs ~327,360 extra `ExecuteOp()` cycles post-blit, then `cycles.count = startCycle` **rewinds** (`NstCpu.cpp:1887`) | no | no |

All three grant the 6502 more cycles per frame. **None shows more picture** — the
picture is always 256×240, so this is a *speed* feature, not a resolution one. The
Nestopia entry is the odd one out: it is a CPU-starvation fix whose cycle counter is
rewound, so it does not change frame timing at all. Its public API comment
(`NstApiVideo.hpp`: *"Adds extra scanlines to fix lag"*) is simply wrong — the libretro
label `"CPU Speed (Overclock)"` is the honest one.

FCEUX ships the only concrete presets (fceumm `libretro_core_options.h:938`):
`2x-Postrender` = 266 post-render lines, `2x-VBlank` = 266 vblank lines. Both ≈ 2×.
Documented use case is **removing game slowdown**, naming Contra Force — not 240p.

### Why this game benefits (the part I initially got wrong)

The premise I started from was that extra scanlines only add *drawing* room and so
could not affect a game paced once per NMI. **That is wrong for this game, and measurably
so.** The main loop is a *NMI-synced spin*: `$FCA0` parks waiting for the next NMI
(§19.3, §19.6). Each NMI the guest gets only 262 scanlines of CPU before it must park
again. Hand it 562 and it completes more of its own work before parking — so it runs
faster. That is exactly FCEUX's documented mechanism, and exactly the game class the
feature was written for.

### Measured (MesenCE 2.2.1, `tools/overclock/`)

Start at 11–12, confirm WORLD CUP at 60, SKILL at 140, CHANGE POSITION at 900 — the
input pattern from `tools/frame_gen/oracle_dump.lua` plus menu walks, which reaches a
real match (verified by screenshot: full court, HUD, players, energy bars).

| | stock | `Before NMI = 300` |
|---|---|---|
| CPU cycles/frame | 29,781 | 63,881 (+34,100) |
| mean extra `$069E` laps/frame | — | **+12.0** |
| cumulative extra ticks over 1200 frames | — | 14,424 |

The lead **grows monotonically by window** — +7.3 (frames 1–200) → +17.5 (1001–1200) —
which is the signature of the guest doing *more work per frame* rather than merely
running out of phase. Reproduce:

```bash
./tools/overclock/run_overclock.sh 0   1200 stock
./tools/overclock/run_overclock.sh 300 1200 oc300
python3 tools/overclock/compare_runs.py tmp/overclock_run_stock/m.csv \
                                     tmp/overclock_run_oc300/m.csv
```

### Our backend already has this mechanism, gated to the wrong mapper

`cyc_set_extra_scanlines()` (`hw_machine.c:580`) does exactly Mesen's thing, and
`PROJECTS.md` describes it as *"blank scanlines before NMI, APU paused"* — blank lines
are never rendered, so it is a pure CPU-time grant. But:

```c
bool cyc_extra_scanlines_supported(void) { return hw_cart.mapper == 4; }
```

Super Dodge Ball is **mapper 1**, so it is rejected. Lifting that gate (via
`HOST_EXTRAS`, not by editing the pristine submodule) is the whole implementation.

### Harness bugs found and fixed — all three produced "no difference" when wrong

These are recorded because each one yields a *plausible false negative*, which is worse
than an error:

1. **MesenCE reads `settings.json` from its own binary directory, not `$HOME`.**
   Overriding `HOME` silently uses the real user config. Proof: `Nes.Region=Pal` moved
   cycles/frame 29781 → 33248 (exactly PAL) *only* once `settings.json` sat beside a
   copied binary. **This was a live bug in `tools/frame_gen/run_frames.sh`**, which had
   copied config to `$CONFIG_DIR` and exported `HOME` — i.e. it never isolated anything.
   Fixed; it now owns the executable and fails loudly if no frames appear.
2. **`xvfb-run` word-splits the program path.** It execs via `sh -c`, so a path
   containing a space (this repo's own path has one) breaks *even when quoted*. Symptom:
   `xvfb-run: .../Mesen: not found` while the file is present and executable. Invoke
   `./Mesen` from inside the sandbox directory.
3. **After that `cd`, relative ROM paths stop resolving.** Needs an absolute path.

Also: `PpuExtraScanlinesBeforeNmi` exists under **both** `Nes` and `Snes` in the shipped
`settings.json`, and the `Snes` copy appears *earlier in the file* — a naive `grep`
reports the SNES value and makes the edit look like it failed. Read it back scoped to
the section with a JSON parser.

**The meta-lesson, and the mistake I actually made:** the first version of this harness
set the value, saw identical output, and I reported "overclocking changes nothing".
It had changed nothing because the setting had never reached the emulator. The control
that caught it was wall-clock time: a setting that quadruples PPU work *must* change how
long a run takes, and 0.606 s vs 0.612 s says it didn't. **Validate that a harness can
detect the effect it is looking for before concluding the effect is absent.** A negative
result from a tool never tested against a known positive is not a negative result.

### Shipping it: patch 006 + the `ppu-overclock` mod

Our backend already implemented Mesen's mechanism, so the work was to make it
reachable. `cyc_extra_scanlines_supported()` returned `mapper == 4` (MMC3 only)
and `--extra-scanlines` refused with *"requires an MMC3 cartridge"* on our
mapper-1 cart.

**`patches/006_ppu_overclock_mmc1.patch`** widens that to `mapper == 1 || 4` --
one line. The criterion is principled: the added lines are blank (rendering is
off above scanline 239) and the APU is paused, so they are inert for any mapper
whose IRQ is not PPU-clocked; in this engine only MMC3 counts PPU A12 rises,
while MMC1 triggers its IRQ from writes to `$8000`/`$8001` bit 7. The predicate
is consulted only when `extra_scanlines != 0`, so **at stock timing nothing
changes for any game** -- verified: the 600-frame `sdb_throw` run is byte-identical
(`cycles=17865922 native_cycles=13225160 (74.0%)`) with and without the patch.

A second effect matters: `cyc_state.c` validates a loaded state's
`extra_scanlines` against the same predicate, so **before** the patch a save
written while overclocking would have been *rejected on load* for MMC1. The
patch is what makes the feature save-state-safe.

**The mod** is `super-dodgeball.ppu-overclock` (manifest-only, like the flicker
one), with an integer `[[option]]` so the launcher exposes the scanline count.
Implementation is section 4 of `src/sdb_host_extras.c`: it only picks a value
for the backend setting. Off by default -- it deliberately changes timing.

One trap worth recording: **`cyc_power_on()` does
`memset(&hw_extra_timing, 0, ...)` and runs AFTER mods activate**, so a value
applied in the activation plugin is wiped before the first frame. The mod keeps
the requested value and re-applies it from `power_on`. Without that the feature
reported itself active while running stock timing -- the same
"looks enabled, changes nothing" failure as the broken oracle harness above.

Measured on our own backend (600-frame / 800-frame `sdb_throw`):

| | stock | +262 scanlines |
|---|---|---|
| cycles/frame | 29,740 | 59,521 (2.0x) |
| frames with a populated display list (`$067A`>0), of 800 | 316 | **565** |
| cycle count, 600 frames | 17,865,922 | unchanged by the patch at stock |

The guest spends far more of its time actually drawing players, on the same
absolute input schedule -- i.e. it progresses further. Note `$069E` advances at
the *same* rate in both (202 vs 200 per 800 frames): in a match it is not a work
counter, so it must not be read as a speed measurement. The clean per-frame work
comparison is the MesenCE one above.

### `mesen-for-ai` (paulomanrique) — evaluated, still rejected

MCP server + Lua bridge for driving Mesen from an agent. Read in full. It is well made
but **cannot set the setting**: the only config in the repo is a hardcoded
`settings.json` heredoc in `scripts/run_headless.sh`, with no CLI flag, env var or Lua
hook to add a key. Its data path is also worse for batch work — `cpu.read_memory`
returns a JSON integer array per read, against our `--frame-log` CYCFRAME-v2 snapshots.
Three things worth stealing: `export DOTNET_ROLL_FORWARD="${DOTNET_ROLL_FORWARD:-Major}"`
(added to `run_frames.sh`), the isolated-`HOME` rationale, and `scripts/check_socket.lua`
as a LuaSocket probe. Zero stars, no CI, last commit 2026-09-02, GPL-3.0.

---

## 24. VSync presentation: pacing fixes the choppiness overclocking cannot

The overclock mod (§23) makes the *guest* run faster, but panning still looked
choppy — steppy in a way that felt like 30 fps. Two sub-agent research tracks
agreed on why, and on what does and does not exist to fix it:

- **Super Dodge Ball is already 60 Hz** (NTSC 60.0988, logic per NMI; TASVideos
  lists it at 60). There is no 30→60 cap to lift. The "low-fps feel" is lag
  frames under load plus the game's own flicker — both already addressed.
- **No NES project ships higher FPS.** The recomp efforts that do 120 Hz+
  (Zelda64Recomp via RT64, Ship of Harkinian's matrix interpolation,
  psxrecomp's frame blending) all interpolate **3D transforms** or crossfade
  whole frames ("blending never invents motion: moving objects ghost").
  Nobody interpolates 2D sprite content: integer-pixel positions, stepped
  animation cels, no single camera under raster splits.
- **The choppiness is presentation-side.** Stock `cyc_sdl.c` presents on an
  `SDL_Delay` + busy-wait timer with no `SDL_RENDERER_PRESENTVSYNC`
  (renderer created `SDL_RENDERER_ACCELERATED` only, `cyc_sdl.c:1240`), so
  every present jitters by milliseconds — and the 60.0988 Hz guest drifts
  against a fixed-60 Hz panel (a repeated frame roughly every ten seconds).
  Mesen ships Integer FPS mode for exactly this drift; RetroArch's BFI help
  documents the 60-content-on-144-panel judder.

**Shipped:** patch 007 makes VSync a launcher Display setting:
`CycSettings` gains `[Display] VSync` (default off, persisted in
`config.ini`), `cyc_ui_launcher.c` sets `gi.has_vsync = 1` — lighting up
recomp-ui's existing Graphics row, the same one PSX uses — and `cyc_sdl.c`
builds its renderer with `PRESENTVSYNC` when set. Guest clock untouched at
any refresh rate — 60/120/144 Hz all just repeat finished frames as needed.
No save-state footprint. Windowed `--exit-after 60` exit 0 both ways;
`VSync = 1` logs the renderer line under xvfb. (This started as a
`super-dodgeball.vsync` mod with live runtime toggles; it worked, but the
in-game menu has no vsync row and cannot gain one without editing the pinned
recomp-ui — and a `state.toml` entry for a deleted mod hard-blocks boot
("does not provide this trusted plugin"), so the mod was deleted outright
rather than left dormant.)

Measured scroll granularity (1200-frame `sdb_throw` screenshots, phase
correlation on the playfield band): every background move is **6-7 px in one
frame, purely horizontal** (44 of 52 high-motion pairs; the rest are scene
cuts). No 1-px smooth scrolling exists in this game. A 7-px step at 60 Hz is
~420 px/s during pans — that is the "individual pixels very fast" feel, and
no present timing can subdivide a step the guest never drew.

---

## 25. Widescreen: the margins are already in the nametable

Goal: present 16:9 / 21:9 / 32:9, and reveal the "remove overscan" columns
emulators ship. The reveal was originally a second feature of this package and
was folded into `widescreen` — see "One more trap, in the mod state file" below
for why the separation was not just redundant. **No engine patch is needed** — unlike §23, where the feature
existed but was refused on MMC1. The whole mechanism is already upstream at our
pin (PR #58, `3ef948cd`, an ancestor of `2b4fcdb`):
`common/nes_video_geometry.h` (the widths), `runner/cyc/cyc_video.*` (the
presented-width state machine), `runner/cyc/cyc_render.*` (the compositor and
its helpers). The work is one compositor in `src/sdb_host_extras.c` plus a
manifest.

### What the framework gives, and the one thing that had to change

`CycCompositorFn` is handed a `width x 240` opaque-black canvas, the machine's
own 256x240 picture, and `native_x0`; return nonzero if you painted it, **0** to
have the engine pillarbox the native picture as it does today. Margins are drawn
from `cyc_render_line_scroll_x/y(y)`, `cyc_render_nametable/chr/palette`,
`cyc_render_tile()` and `cyc_render_sprites()`. At width 256 the compositor is
not called at all.

Widths for a 240-row picture, all from the shared geometry rather than numbers
invented here: **426 / 560 / 854** for 16:9 / 21:9 / 32:9, plus `fit`. Note
`--widescreen` also exists on the *function-level* runner and yields 428 there;
on the cycle path it is 426 and the two must not be treated as interchangeable
(`docs/CUSTOM_RENDERER.md:349`).

**The first of two collisions with the compositor.** This one: the character-flicker mod's
`present()` returned a non-NULL 256x240 buffer, and both the window
(`cyc_sdl.c:306`) and `--present-out` (`cyc_host.c:886`) take `present()` over
`cyc_render_present()`. A compositor would have been silently bypassed whenever
flicker was on. `present()` now always asks `cyc_render_present()` first — it is
the only place the compositor runs and the only place the width is decided — and
returns the engine's buffer untouched when there is nothing else to do.

**The second collision was found much later, and only by playing** (§26, "Two mods
on at once"): once both features *were* on, the compositor's native region already
carried the machine's sprites and the mod's captured list was blitted on top of
them, so every sprite was drawn twice, 1-2 px apart because the captured list is a
build old. Fixing it established the invariant that keeps the two apart —
**wide native 256 == flicker-only native 256, byte for byte** — measured over 199
frames of the menu walk with 0 differing frames. Neither collision was reachable
from a single-feature gate; both needed both features on, and the second needed a
human.

### Not wideNES

Reproduce every measurement in this section with `tools/widescreen/` — `nametable.py`
prints the court's nametable and reports whether it changes frame to frame, and
`blank_extent.py` produces the overscan table below from `--frame-log` captures.
See `tools/widescreen/README.md`.

wideNES (prilik, ANESE `wideNES` branch, `src/ui/SDL2/gui_modules/widenes.cc`)
is the standard answer to "NES widescreen" and it is the **opposite** design. It
does not reconstruct anything: it snoops `$2005`/`$2006`, warps each frame's
**background-only** framebuffer by the scroll delta, and accumulates the result
into an unbounded scene-keyed pixel lattice. Consequences, all confirmed in its
source and documented by its own maintainer:

- it can only ever show pixels the guest has **already drawn**;
- it knows nothing about nametables or mirroring (`grep -c nametable` = 0), so a
  looping world unfolds into an infinite straight tube instead of wrapping;
- **it ignores sprites entirely** — the maintainer's 2024 retrospective names
  "cannot handle off-screen sprites" as the reason it never shipped;
- it has **no warm-up and no consistency check** — unknown territory renders
  black and is shown immediately.

None of that machinery is needed here, because **this game's world is entirely
resident in CIRAM before the frame starts**. Measured over a match, the court's
nametable is byte-identical from frame 450 to frame 900 and spans the full
64-tile / 512-pixel space of both nametables:

```
C0 C0 C0 C0 | 4E 4D 4E 4D ... | 04 03 04 03 ... | 2C 2B 2C 2B ... | C0 C0 C0 C0
backdrop     left sideline     crowd             right sideline     backdrop
```

So a margin pixel is *read*, not remembered: scroll → world column → tile →
attribute → pattern byte → palette, exactly as the PPU would. No isolated
guest calls and no world cache; the background margin needs no
`[[mod_function_hook]]` sites and no new seeds. (§26 later adds two sites, but
for the *sprites* in the margin rather than the court under them.)

**The four bugs the seam check caught** (each produced a plausible picture, none
produced an error; found by comparing our reconstruction against the machine's
own background capture):

1. **Attribute table address.** This cart is horizontally mirroring, so `$2800`
   aliases `$2000`: the textbook attribute address reads *tile bytes*. The
   attributes are at page offset `0x3C0` (`$23C0`/`$27C0`). Proof from the dump:
   the byte at `0x3C2` is `0x55`, and `0x55` is palette 1 in all four quadrants
   — exactly the palette the machine painted and we did not.
2. **Attribute granularity.** Not the standard 16x16 metatile: the index that
   matches is `(tile >> 2)`, one byte per **4x4 tiles**, quadrant
   `(tile >> 1) & 1`. Measured against the capture: `(tile>>2)` leaves 2613
   pixels disagreeing of 61440; textbook `(tile>>1)&7` leaves 18420.
3. **PPUMASK bit 1 polarity.** Bit 1 *set* means "show background in the
   leftmost 8 columns", so the clip exists when the bit is **clear**. Backwards
   here painted real court in the eight columns the PPU painted with backdrop.
   wideNES's blog prose states the opposite of wideNES's own code here; the code
   is right.
4. **Compare against the background, not the picture.** A first pass compared
   against `native`, which has sprites composited on top — every player standing
   in the court counted as a reconstruction failure (~9000/frame). The engine
   already exposes the right thing: `cyc_render_background()`, the capture
   taken *before* the sprite priority mux.

After all four: **1–2 differing pixels per frame out of 61440.** The seam check
compares reconstruction against capture and returns 0 (engine pillarboxes) when
more than 2% disagree, so a title screen, a menu or a transition falls back for
free instead of growing garbage margins.

### Verification

- **Mod off = stock**: with the catalog present and every feature off, the
  `--present-out` frame is byte-identical to a run with no `--mods-root` at all.
  **One caveat since the `overscan` fold**: a `state.toml` written *before* it
  still names the removed feature, and the catalog then refuses to run at all —
  "does not provide this trusted plugin", nothing starts. That is a harder
  failure than a soft one, and it is the one way this gate can go from green to
  "nothing runs". Delete `build-cycle/mods/state.toml` and let the launcher
  rewrite it.
- **Stock centre**: the presented native 256 columns are pixel-for-pixel the
  unmodded picture at every aspect ratio (verified by array compare against a
  stock run), because the compositor copies the machine's picture over its own
  reconstruction before returning.
- **Widths**: 426/240 = 1.775, 560/240 = 2.333, 854/240 = 3.558. The width for
  a ratio must use the engine's own rounding (`2*floor(rows*aspect/2 + 0.5)`);
  truncating turns 32:9's 853.3 into 852, i.e. 3.55:1 rather than 3.556:1.
- **Arms from the manifest**: `state.toml` with the `widescreen` feature enabled
  and `aspect = "32:9"` produces 854 columns, seam diff 1 px. Default-off: the
  runtime writes `enabled = false` for both features when nothing is selected.
- Frame 0 pillarboxes on every mode — the background capture is not available
  until after the first frame, so there is nothing to verify against and the
  compositor declines rather than claiming a seam it did not check.

### Overscan: measured per game, and left side only

Per-game is not a preference here. Over **2112 frames with ink** (both vectors,
1100 frames each; 88 fully blank frames skipped):

| band | frames with any ink | crop |
|---|---|---|
| columns 0–7 | **0 / 2112** | 8 |
| columns 248–255 | 1372+ | 0 (real content) |
| rows 0–15 | 2 / 2112 | — |
| rows 232–239 | 2 / 2112 | — |

The two frames with ink in the top/bottom bands are the **same frame in both
vectors**: frame 420, a full-screen wipe whose top and bottom are deliberately
filled. So the vertical crop is not lossless and is not offered. The left 8
columns are blank in *every* frame, and they are blank because the game clears
PPUMASK bit 1 — the NES's left overscan, hidden by the developer. Every emulator
treats that bit as "the game is hiding artifacts here": wideNES crops 8 when it
reads clear, and the 240p Test Suite's NES safe-area test calls rows 0–7 and
232–239 the "danger zone".

**Reveal, not crop.** Every emulator's overscan subtracts the border. Here that
would splice the world: the left margin ends at world column `sx-1` and the
picture would resume at `sx+8`, an 8-column skip through the court. So the
widescreen compositor instead draws the real background in those columns and
leaves them alone when copying the machine's picture over the centre. The whole canvas
is then one continuous run of world columns with no black gutter. Measured on the
output: the fully-black 8-column strip at the margin boundary (columns 85–92 at
16:9) is present without the feature and **absent** with it.

This costs the "centre is byte-identical to stock" claim for exactly those 8
columns, which the machine renders as backdrop anyway. Everything else is
unchanged. The feature only has an effect together with widescreen, because at
width 256 the engine never calls the compositor.

### Past the edge of the world: black

The world is 512 pixels wide and the canvas at 32:9 is 854, so the margin runs
off the end. Mirroring and wrapping were both implemented and both rejected on
the output: the world's outer band holds the scoreboard and the Statue of Liberty
skyline, so **mirroring renders the text backwards** and **wrapping shows the
same skyline two or three times**. Both invent picture the game never had, and at
32:9 the margin is 299 columns — most of the screen. The margin is filled with
the backdrop, which for this game is black. At 21:9 and 32:9, on camera positions
that put the world's edge inside the view, this leaves a black band at that end.
That is the honest limit: the stadium is 512 pixels wide and a 854-column frame
does not fit inside it.

### The world's own bounds are the game's camera window (`$E6AB`)

This section used to say the margin past the world is filled with backdrop
"which is already the colour of the world's own outermost columns (`C0`)".
**That was wrong, and it is the cause of a visible artefact.**

Tile `$C0` is not a blank tile. Read out of this ROM's CHR it is 70 set bits
across its two planes and renders an arch:

```
$C00  plane0 fa 7d fa ff ff ff ff ff   plane1 87 87 87 00 00 00 00 00
      row 0: 31111232   row 1: 21111323   row 2: 31111232   rows 3-7: 11111111
```

The nametable's outer four tiles at **each** end hold `$C0` for rows 6–28 — the
stadium's end wall — and `$00` above and below it, and `$00` really is blank (0
set bits), which is why only the `C0` band showed. The attribute byte over
those columns is `$C0`, which selects palette 0, whose entries 1–3 are `$31` /
`$12` / `$06` — not the backdrop. So reconstructing the world's outer columns
paints **a row of blue arches on black, at both ends, cut off from the court and
in a different palette from it**. Measured on `sdb_throw` at 32:9, frame 430
(`scr=128`, `x0=299`): left margin arches at canvas 174–201 = world 3–29, right
margin at 654–680 = world 483–509, exactly four tiles each side. Reproduced
headlessly from a played report; it is present with margin sprites on **and**
off, and with the flicker mod on and off — it is the reconstruction, nothing
else.

#### The bound, and where it comes from

**It is the game's camera window, and the game states it in code.** `$C3/$C4`
is the camera — `$823E/$8246: SEC / SBC $C3 / SBC $C4` is how OAM X is made, and
the SDB trace's `cam=` column reads exactly those two bytes. It is clamped at
`$E6AB`, reached from both directions of `$E5D8`:

```
$E5D8: LDA $61 / CMP #$28 / BCC $E5E8        <- moving left
$E5E8: LDA #$28 / STA $61 / LDA #$00 / STA $62       40  = $28
$E5FF: JMP $E6AB
$E644: LDA $61 / CMP #$E4 / BCC $E652        <- moving right
$E64A: LDA #$D8 / STA $61 / ...                       216 = $D8
$E661: JMP $E6AB
$E6AB: LDA #$00 / STA $C2
$E6AF: LDA #$28 / CMP $61 / BCC $E6B9 / STA $61      lower clamp, 40
$E6B9: LDA #$E0 / CMP $61 / BCS $E6C1 / STA $61      upper clamp, 224
$E6C1: LDA $61 / STA $C3 / LDA $62 / STA $C4          -> the camera
```

So the window is `[$28, $E0]` = `[40, 224]`, and the world the machine can
display is the camera plus the viewport:

```
[ 40, 224 + 256 ) = [40, 480)
```

**There is no per-level table.** The bounds are two immediate operands, and both
ends of the evidence agree with the code rather than being fitted to it: on two
completely different courses — the stadium and the London one — the camera stays
inside `[40, 224]`, resting at 40 with an observed maximum of 216 (the target
path caps at `$D8` before `$E6AB`'s 224 can bite). On the stadium the court's
own drawn extent is `[32, 480)`, so the game's window sits 8 columns inside its
own art at one end and flush at the other — which is what a hand-tuned clamp
looks like, and is the cross-check that the disassembly has been read correctly.

#### What this replaced, and why the replacement was necessary

The first version of this fix did **not** read the camera. It measured the
nametable — "how many tiles in from the world's edge repeat that edge column",
capped at 32 px — because the band is level data and a hardcoded 32 would crop
real court on a stage without end walls. That reasoning was sound and the
mechanism was wrong. It gave the right answer on the stadium *by luck*, because
the end wall happens to be a uniform band, and it failed everywhere else: a
level whose edge column varied immediately fell to a floor of one tile and
received **no bound at all**. The London course did exactly that, measured
`0/0`, and kept drawing past its own edge for a whole 4355-frame played
session. The art of an end wall is not a fact about the game's limits; the
camera is.

Two further measurements, both from that played session, are what settled it:

- The London course's left margin at `scr=70` shows world 0–69, and at `scr=0`
  the machine's own native columns 0–7 (world 0–7) are inked **480/480 rows**. It
  looked like the margin was inventing that; it was not — for 204 frames from
  level start the machine displays it too, because `$DBBB` (called once, from
  `$F3A0`) initialises the camera to **0** and the clamp only governs movement
  after that. Those 204 frames are one contiguous run, frames 3675–3878.
- Nothing is drawn past world `[0,512)` anywhere in that session. An earlier
  claim that it was, was an analysis error: the capture is 2× scaled with one
  extra column that carries a constant artefact, and the backdrop is **not
  always black** — the London course's is `(0,62,122)`, so a scan for
  "not black" reports a full backdrop as content on 635 frames.

#### Where it is applied, and what it costs

Applied to **margin columns and margin sprites only**. Inside the native 256 the
machine's own picture is what is shown, and the seam check compares the
reconstruction against the machine's background over exactly those columns;
bounding them would report a reconstruction failure for a camera position that
legitimately shows the end wall and pillarbox a correct frame. Margin sprites get
the same bound, clipped rather than dropped so a character straddling it is
still whole up to it.

The cost is the 204-frame London intro, where the margin is bounded 40 columns
tighter than the native picture beside it. That is the right way round: the
alternative is inventing columns the game chose not to show.

#### Measured

Stadium `sdb_sweep`, 1200 frames, **every counter identical to the pre-fix
baseline**: `1169 composed, 1 pillarboxed, seam diff 0 px`, `1200 captured
(3353 off-edge), 48359 drawn, peak 67, backstop 8`. `margin_track.py`: 701
frames, eleven objects, `no anomalies`. The frame-log A/B is byte-identical with
the capture on and off at an unchanged cycle count.

The gate is `tools/widescreen/bounds_gate.py`, which asserts the property on the
presented frames rather than a counter. Each capture judged at the bounds that
were actually in force for it, and at the new ones:

| capture | build | frames | judged at | result |
|---|---|---|---|---|
| stadium `sdb_sweep` | new | 770 court | `[40,480)` | **0 fail** |
| `tmp/level-2-boundary-try2` | new | 2617 | `[40,480)` | **0 fail** |
| `tmp/level-2-boundary` (try1) | old | 681 | `[0,512)` | 0 fail |
| `tmp/level-2-boundary` (try1) | old | 681 | `[40,480)` | **275 fail**, top hits 4176–4183 |

The last row is the control: the same capture and the same gate, judged at the
new bounds, and the failures bracket the reported frame 4172. The try2 session is
also the first to reach camera **224** — the clamp's upper bound, which try1 never
hit (max 216) — so the right-hand end of the window is exercised too.

Three instrument bugs had to be fixed for those numbers to mean anything, all in
`bounds_gate.py`, and all found by believing its output:

- **the backdrop is not always black.** `cyc_render_color(0)` is palette entry 0
  and therefore a per-screen value; the London course's is `(0,62,122)`. The gate
  measures it per frame from the margin it judges. Hardcoding black reported 635
  of 4355 frames as overrun when the margin was entirely backdrop.
- **the scroll alignment was assumed, not measured.** The gate took frame n+1's
  scroll for capture n. On frame 5080 that substituted a camera of 40 for one of
  216, moved the margin 176 columns, and reported 20550 px of overrun on a frame
  that is **exactly clean** — 0 px at the right alignment. Both alignments are now
  scanned and the one that fits the capture as a whole wins; one decision per
  capture, printed with the result, not a per-frame guess.
- **`native_x0` is in image pixels and 256 is in canvas columns.** The right margin
  started 128 columns early at scale 2 and swept the machine's own picture into
  the check, reporting 61440 px — exactly `256 × 240`, the viewport's size — on a
  frame whose margins are black on both sides.

Two more, from writing it: the output prefix has three observed forms depending on
how the capture was invoked, so the gate **globs** rather than formats (it had
twice reported a clean pass on a capture it never opened), and `--present-out`
captures the **window**, so a `play_record.sh` session at `WindowScale 2` writes
853×480 PNGs for a 426×240 canvas. It exits **2** on "no frames examined" for the
same reason.

### One more trap, in the mod state file

Hand-writing `state.toml` to test the package: **`[feature.values]` binds to the
`[[feature]]` block declared immediately above it.** When the package carried two
features, a values table written after `overscan` but meant for `widescreen` was
silently dropped — `overscan` had no `aspect` option, so nothing was written back
and `widescreen` fell back to its manifest default (`fit`). Headless, `fit`
resolves to the stock aspect, the width stays 256, and the engine does not call
the compositor at 256 — so the feature appears to do nothing at all, with no
error anywhere. The launcher never hits this because it writes the file; only a
hand-built test file does.

That trap is now **unreachable by construction**: the package has one feature, so
there is nothing for a values table to bind to the wrong block. It is kept
because the rule is about the file format, not about this package.

The fold was decided on the same grounds as the margin sprites (§26), and the
decisive argument is the artefact rather than the tidiness: with the two features
separately switchable, "widescreen on, reveal off" is a legal state, and it is a
wide frame with the game's own black clip back at the margin boundary — the
exact thing widescreen exists to stop. Two toggles for one compositor's output is
one too many. `SDB_OVERSCAN=0` in the environment still forces the clip back,
because the gate 5 measurement needs the band to be measurable in the first place.

A consequence worth recording, because it is a hard failure and not a soft one:
**a `state.toml` written before the fold keeps referencing the removed feature,
and the whole catalog then refuses to run** —

```
[Mods] super-dodgeball.widescreen/overscan: The executable does not provide this trusted plugin.
mods: Resolve the highlighted mod conflicts before Play.
```

It does not degrade to stock; nothing starts. Delete `build-cycle/mods/state.toml`
and let the launcher rewrite it.

### Not sprites in the margins

Resolved in §26, and **not as a separate feature**: widescreen draws them,
because a margin with no players in it is not a wider game. See §26.

---

## 26. Sprites in the margins: the game's own 16-bit position, culled one instruction before it is used

§25 ended with margins that were background only, and named the reason:
*"OAM X is a byte and camera-relative, so the sprite list alone cannot place
anything outside the viewport; it needs the display records' world-space X,
which is not yet identified."*

The records were the wrong place to look. The answer is that the game **does
have** the placement, computes it in full, and throws it away itself.

### The cull, and why it is reversible

The per-object emitter at `$8348` folds the object's camera-relative X
(`$0A/$0B`) and Y (`$0C/$0D`) into the per-part position (`$04/$05`, `$06/$07`),
and *then* decides whether to draw it:

```
$8483: INC $4D                              <- the 16-bit position is final here
$8488: LDA $04 / CMP #$04 / BCC $849A       X < 4          -> hide
$848E: LDA $06 / CMP #$04 / BCC $849A       Y < 4          -> hide
$8494: LDA $05 / ORA $07 / BEQ $84A1        a high byte set -> hide
$849A: LDA #$F8 / STA $0200,X               hide (Y = $F8)
$849F: BNE $84C1                             ... and do not bump the cursor
$84A1: LDA $04 / SEC / SBC #$04 / STA $0203,X      OAM X = X16 - 4
$84A9: LDA $06 / CLC / ADC #$04 / EOR #$FF / SEC / SBC #$10
      STA $0200,X                                  OAM Y = $EB - Y16
```

Measured on the current binary, both halves: a sprite the game placed at
X16 = 208 lands in OAM at `$CC` (208 − 4) and one at Y16 = 82 at `$99`
(`$EB` − 82). So the placement is a genuine 16-bit screen coordinate and the
**byte truncation at `$84A1` is the only thing that loses the sprite** — after
the tile, the flips, the palette and both halves of the position are all
computed.

That is the whole difference between this and wideNES. wideNES warps a
framebuffer it has already been given, so a sprite that was never drawn is
unreachable in principle (its maintainer names this as the reason it never
shipped). A compositor resolves any canvas column from guest state, so the
only question is whether the guest still knows where the object is. Here it
does, because the cull happens *after* the arithmetic. This is the same
arrangement as `mstan/SuperMarioBrosNESRecomp`'s `docs/WIDESCREEN.md`, which
reads enemy records in world coordinates out of game RAM — the only NES
widescreen that renders margin sprites.

**Two sites, not one.** The single-sprite emitters have the same cull on the
same accumulators and are reached by three jumps:

```
$82B9, $82DD, $8294  ->  $8302:
$8307: LDA $04 / CMP #$04 / BCC $8342       X < 4          -> hide
$830D: LDA $06 / CMP #$04 / BCC $8342       Y < 4          -> hide
$8313: LDA $05 / ORA $07 / D0 29 BNE $8342  a high byte set -> hide
$8342: LDA #$F8 / STA $0200,X / RTS        hide (Y = $F8)
```

One thing differs and the capture has to branch on it: the strip walk stores
its tile and attribute into the OAM slot at `$8419-$8427`, *before* the cull,
while `$8302`'s paths still hold them in `$02/$03` and only store them after it
(`$8326`/`$832B`). Reading `$0200+cursor` unconditionally gets the right bytes
for one emitter and the wrong bytes for the other three.

`$8483` is in the same routine as the existing `$84B6` site, so the `$821E`
seed already compiles it natively and **no new seed was needed** — which is
the first time that file has not grown a line for a new hook.

### How much is off-screen

This is not a rare edge case. `tools/widescreen/objects.py --cull` over 61
match frames:

| | |
|---|---|
| frames with at least one object off-screen | **61 / 61** |
| objects off-screen | **293 / 732 (40.0%)** |
| worst frames | 12 objects, 6 of them off-screen |

Frame 610 for detail — camera 147, six of twelve objects in view, with two at
*negative* screen columns:

```
 idx  state sel   rel16  abs16   screenX  off-screen
   0  $02  18    161    106        14
   2  $02  30     67     64       -80   YES
   8  $02  28    316     48       169
   8  $01  28    445     64       298   YES
   9  $01  18    112     64       -35   YES
```

At 16:9 (426 columns, 85 either side) both negative-column objects are inside
the left margin; at 32:9 (854 columns, 299 either side) the right-hand one is
289 columns past the viewport edge.

### Two bugs the first version had, both from ignoring the cadence

The first working version put sprites at the right *columns* but the wrong
*rows*, and blinked. Both causes are the same thing: this game does not rebuild
its sprite list every frame, and a per-frame assumption was wrong twice.

**1. The Y accumulator is not a screen row.** `$06/$07` is inverted by the game
before it becomes an OAM Y:

```
$84A9: LDA $06 / CLC / ADC #$04 / EOR #$FF / SEC / SBC #$10 / STA $0200,X
```

which is `$EB - Y16`, and the PPU draws from one line below that, so a sprite's
top line is `$EC - Y16`. Passing the raw accumulator through as the row drew
every margin sprite about 36 lines too high — players in the upper corners,
standing on the skyline instead of on the court. Fixed by doing the conversion
at capture time with the game's own arithmetic (`SDB_MARGIN_Y0`), which is why
it cannot drift from the picture: X16 = 208 → OAM `$CC`, Y16 = 82 → OAM `$99`,
first line `$9A` = 154 = 236 − 82.

**2. The emitter runs on about one frame in three, and a build spans two.**
Measured over 200 match frames the cursor bump hooks fire on **66** of them: the
game emits a list over two frames, then leaves OAM alone for three to six more.
Two failures came from ignoring that:

- *Clearing the margin list per frame* emptied it on every quiet frame, so the
  players blinked out once per cycle. `drawn` went from 1300 to 21068 over 700
  frames once the list was held, and the margin's frame-to-frame churn came out
  the same shape as the machine's own — zeros on exactly the frames the native
  centre has zeros.
- *The held list was the wrong list.* The flicker mod publishes a build when the
  **next** one starts, so its published list is one build behind by
  construction. Presenting that on a quiet frame showed 16 sprites where the
  build in progress had 40, blinking the other 24 in and out. The rule is the
  flicker mod's own, which the first version had mis-copied by adding
  `run == 0` to the hold condition: on a **quiet** frame the build in progress
  *is* the last complete one and must be presented; only the **first capture
  frame** of a new build is half-assembled and needs the previous list.

**3. `cur == SDB_FIRST_SLOT` is not a boundary, it is a *transition*.** `$0617`
only advances for sprites the game actually draws, so a run of culled sprites
sits at one cursor value — and the flicker mod's `cur == SDB_FIRST_SLOT` test
fires once per sprite in that run. Measured: one frame published 40 sprites and
then six one-sprite fragments over the top of it, collapsing the held list from
40 to 1. The boundary is the *transition* into slot 1, plus a first capture
after a quiet frame as a backstop. That backstop must be
`s_margin_run == 0 && !s_margin_captured`, not `!s_margin_captured` alone: a
build spans two frames, so the second frame's first capture otherwise looks like
a fresh run and splits the build — which quietly cost 30 of 40 sprites a frame
until it was measured.

Net effect of all three: `drawn` 1300 → 21068 per 700 frames, and a character's
column now marches outward in even steps across builds (−6, −18, −30, −41 …),
which is the continuity property a margin sprite has to have. Watch it with
`SDB_TRACE_CAPTURES=<from>-<to>`, which dumps the list the compositor drew.

### Six more, all found by driving the camera across the world

The three above were found with a still camera. `tas/vectors/sdb_pan.input`
(24-frame LEFT/RIGHT alternation) and then `tas/vectors/sdb_sweep.input` (72-frame
holds, the width of the whole 512-pixel world, alternating) exist because a still
camera hides the entire class below: a boundary artifact only shows once a
character has walked clean across an edge rather than stepped over it once.
`tools/widescreen/margin_track.py` is the instrument — it reads the compositor's
own `sdbw` trace and audits every object on every frame.

**1. The margin must not expire, because most off-screen objects are not emitted.**
`$067C` does not contain them. Over frames 600-700 of the pan vector `$8231`
fires for records 0,1,3,5,6,7,9,10,11 on 14-16 of every 100 frames, for record 2
**four** times and record 8 **once** — and 2 and 8 are precisely the two objects
standing in the margins. Any lifetime measured in emissions blinks those two out
and only those two; a TTL of 48 frames hid it for a while and then the player
vanished at frame 682 and never came back. The record at `$0618` is refreshed by
the game **every frame for every object**, emitted or not (verified: records
0,1,4,10 walk across a run while 2, 8 and 9 stand still, and all are being
written), so liveness is the record, and there is no TTL to tune.

**2. The part's column is an OFFSET, not a column.** `$821E` computes `$0A/$0B`
as (record X − camera) and `$8470` adds it in, so the camera cancels and a part's
world column is `record_X + part_offset`. Storing the resolved screen column meant
it went stale the instant the camera moved.

**3. Four columns, twice.** `$04/$05` is the game's *accumulator*; the screen
column is that minus 4, which `$84A1` subtracts for the guest. Reading the
accumulator as a column pushed every part 4 px right of where the machine puts
it, which is not cosmetic: it moved parts across the boundary test, so a part the
margin owns got dropped as if the machine had drawn it — a **stripe of missing
pixels** through a character — and a part the machine owns got drawn a second
time over the machine's own picture.

**4. The wrong camera, by one step — and the right answer was only *mostly*
right.** There are two candidates for "where is this frame's column zero" and
they are not the same quantity. `cyc_render_line_scroll_x` is what the PPU
latched; `$C3/$C4` is the game's camera variable. On the sweep they agree except
on the frames where the camera is stepping, where `$C3/$C4` differs by one 6-7 px
step — because at composite time it already holds the camera for the frame
*after* the one being presented.

The answer is the scroll, and it is the game's own behaviour rather than a
preference: **the machine glues its sprites to its background.** Measured by
world-aligning the stock picture frame by frame through the fastest pan in the
sweep — consecutive frames match to a mean absolute difference of **0.00**, and a
tracked sprite's `OAM X + scroll` holds constant across a 72-pixel pan. Swapping
the basis wholesale is measurably worse, not a wash: over 1581 whole-object
comparisons on the pan vector, placing with the scroll gets ~75% of the
camera-stepping frames right and placing with `$C3/$C4` gets ~33%.

**What this did not settle, and cost a later bug to find:** "the scroll" is right
as a *default*, and it was written here as though it were the whole answer. It is
not. On roughly a quarter of the stepping frames a whole object sits **exactly one
camera step** from the machine's copy of it — see "The margin and the machine are
on different cameras, and only sometimes" below, which is the seam doubling and
the sliver beside it. The two measurements are not in conflict; the second is the
exception the first generalised over, and at the time nobody tabulated the
exception because the only detector being used (`margin_track.py`) audits the
margin's *internal* consistency and cannot see a margin that agrees with itself
and disagrees with the machine.

**5. The 8 revealed columns lose sprites too, and the filter was inverted.**
PPUMASK bit 1 is clear for this game, so screen columns 0-7 are never painted —
and the game does not cull those sprites, it writes them to OAM and the hardware
declines. With the reveal drawing those columns from the nametable, a sprite
standing in them is lost exactly as surely as one the game culled, and nothing
drew it. The first version of the filter had the sense backwards, so the band
dropped its sprites instead of keeping them: a **head with no body** at the
screen edge, which is what the overscan band looked like.

**6. A visit is not a list, and a part is identified by (offset, row).** `$8231`
fires once per object per *visit*, but one visit does not emit the whole object:
object 9 is visited at frame 624 and emits four parts, at 626 and emits seven, at
632 and emits two, and the character is eight. Resetting the slot per visit
published a **fragment** — frames 631-632 carried the head and no body. The
accumulate-across-visits boundary is `$0617` going backwards, which is the only
boundary this feature can see (it walks 04, 24, 40, 60, 80, 84, 94, C8, E8, 00
and restarts, never backwards in between). And a part must then be keyed on
(offset, row) rather than appended blindly, or the previous emission's tiles sit
in the slot until the next list — up to six frames of an older animation frame
pinned in the band while the machine draws the new one beside it.

**The one that is not in this list, because it was wrong and is now removed:** the
attempt to use `$C3/$C4` for parts completing a clipped sprite and the scroll for
the rest. It reads like a refinement and it is a disaster — a character is sixteen
columns of two eight-wide sprites, so splitting its halves across two bases
collapses the eight pixels between them to one and draws it at a sixteenth of its
width. Measured at frame 1060 of the sweep: object 10's rows 184, 168 and 176 came
out one pixel apart instead of eight, then were correct again at 1063 when the two
bases happened to agree. That was the "shifted a pixel, slightly the wrong size"
report. `margin_track.py`'s `ghost` check exists to catch exactly it.

### Verification

- **Presentation only, and proven rather than asserted.** A `--frame-log` with the
  feature on is **byte-identical** to the same run with it off — CPU RAM, CIRAM,
  palette, OAM and the whole 256×240 picture — on all four vectors, with and
  without the reveal: `sdb_sweep` 1200, `sdb_pan` 1200, `sdb_idle` 1200,
  `sdb_throw` 1500. Both capture callbacks return 0, the site's own instruction
  and the game's own cull still run, and `$0617` is untouched because the culled
  path branches past the cursor bump at `$84B6`.
- **The machine's own columns are untouched, and that is also the no-doubling
  proof.** Over all 1200 sweep frames the presented native columns differ from a
  `--margin-sprites 0` run by **0 pixels on every frame**, at 16:9, 21:9 and 32:9,
  while the canvas as a whole differs by 1.5-1.6 million pixels. The blit is
  clipped to the columns the machine does not own, so a margin sprite cannot land
  on the machine's picture — which is the same statement as "no player is drawn
  twice". This is where the no-doubling claim is proved; the per-sprite audit
  cannot make it (see below).
- **No blink, no ghost, no jump — audited per sprite, per frame.**
  `tools/widescreen/margin_track.py` over frames 500-1200 of the sweep, **eleven
  objects, 701 frames, one `cshift` and nothing else**, at 16:9 and 32:9, with and
  without the reveal, and with the character-flicker feature on. A `cshift` is a
  shift the compositor's own printed correction explains (frame 760, corr+3,
  visually verified whole); it exits 0 but each one wants eyes once, because a
  wrong correction lands there too. Objects 2 and 8 — the two
  the game's display list almost never contains — are drawn on **701/701** frames.
- **Glued to the court.** World-aligning the presented frame by
  `cyc_render_line_scroll_x` and stepping through the fastest pan in the sweep
  (frames 1189-1198, a 7 px scroll step), margin sprites hold their position in
  the world-aligned crop exactly as the machine's own sprites do in the native
  one. The margin background and the margin sprites are on the same basis, and so
  is the machine's.
- **The revealed columns carry sprites.** With the reveal on, the 8 columns the
  PPU hides differ from a `--margin-sprites 0` run on 344 of 1200 frames, mean 74
  pixels, max 627. Before this work the band could only ever show the
  reconstructed background.
- **Continuity.** A player's column marches outward in even steps across builds
  (−6, −18, −30, −41), so where a character appears in the margin is where it
  walked off the edge, not a fresh location. `SDB_TRACE_CAPTURES` prints the list
  the compositor drew, per frame, to check this by eye.
- **Declaring the sites costs nothing.** With the feature off the run is
  byte-identical to the committed baseline — same 18312628 cycles and same
  13466382 native cycles at 615 frames, and 0 callbacks.
- **Mod off = stock**: the catalog present with every feature disabled is
  byte-identical to no `--mods-root` at all, **1200/1200 frames** on both the
  sweep and the pan vector.
- **Stress vector** (`sdb_throw`, 3000 frames, 16:9 with the reveal, flicker on):
  **104769 margin sprites drawn, peak 64, 0 backstop trips**, 0 part-limit
  warnings (re-baselined 2026-10-14; the chain of figures is in "The `drawn`
  re-baseline" at the end of this section). Re-baselined 2026-10-13: the previously recorded 104240 does not
  reproduce on this binary and is **not explained** — see below. `SDB_MARGIN_PARTS`
  is 24, not 8: objects 9 and 11 legitimately emit more than a character's eight,
  and at 8 they were losing sprites on 15 frames per 3000.
- Arms from the manifest (`state.toml`, `widescreen` ticked; margin sprites are
  part of it).

**What the per-sprite audit deliberately does not check.** A margin sprite drawn
on top of the machine's own copy of the same player. Matching tile and row against
OAM reports thousands of false hits — Super Dodge Ball gives every player the same
tiles — and the apparently exact version, matching the OAM *slot* the emitter was
about to use, cannot work at all: `$0617` only advances for sprites the game
actually draws, so a run of culled sprites shares one cursor value and the slot
identifies nothing. The property is proved by the 0-pixel gate above instead, and
`margin_track.py` says so in its own docstring rather than shipping a check that
looks rigorous and cannot fire.

### The content-mismatch count is the content key working

Enabling these sites reports **386 content mismatches** where the existing ten
report 0. That is the mechanism doing its job, not a defect:

`cyc_hooks_due()` (`cyc_hooks.c:89`) tests the *address bit alone*, so a site at
`$8483` is entered whenever the PC is `$8483` in **any** bank — and MMC1 maps a
different 16 KiB bank there at different times. `cyc_hooks_fire()` then calls
`holds()`, which reads the bytes **in the currently mapped bank** and rejects
on a mismatch (`cyc_hooks.c:119-123`). Verified rather than inferred:

- with the hooks **disabled**: 0 mismatches, 0 callbacks, identical cycles;
- with them enabled: 4150 callbacks fire, so the declared CRC *is* bank 5's —
  a wrong key could never fire — and the 386 rejections are other banks;
- and the guest is byte-identical throughout, so nothing was dropped that
  mattered.

`$8302` is the same story (`$8302` in bank 3 is `A9 00 85 D1 …`, in bank 7
`69 02 C6 00 …`). The existing ten sites report 0 only because their addresses
happen not to be reached with a foreign bank mapped.

### The seventh: a ghost in the band (found by playing it, not by the sweep)

The audit above is clean over 701 sweep frames and this bug was still there.
It was found by *playing* the game and recording the run, which is why
`--record-input` exists (see below) — no hand-written vector had ever put a
sprite inside the PPU's own hidden band while the camera was still.

**The symptom.** A two-column stripe of black and grey stands in the revealed
band, a few pixels left of the machine's own picture, with nothing on the other
side of the seam. It shrinks, flickers off for a few frames, and comes back.

**Reproduced from a recorded playthrough.** `tools/play_record.sh bar-artifact`
→ 16:9 with the reveal + the flicker mod, played by hand. Frames 1148/1152/1164 have
it, 1157/1161 do not. Two more instrumented replays of the same recorded vector
(±1 frame and ±2 frames) put it at 1142–1150 and 1148–1154.

The capture itself is scratch (`tmp/`, gitignored) and is not committed — but the
**vector is not needed** to reproduce the bug, only to find it: the identification
below rests on the `sdbw` trace and the `oam-band:` line, which any run that puts
a sprite in the band will print. What the played session bought was the *route*
into the state.

**Attributed by A/B, not by eye.** The same run with `--margin-sprites 0` has a
band that is *completely clean* at every one of those frames. So it is a margin
sprite, not a reconstruction fault.

**Identified from the per-sprite trace.** `SDB_TRACE_CAPTURES` prints one
`sdbw` line per presented frame listing what the compositor painted:

```
f1148: … [o10:6,161,tB0] … [o11:5,137,tC4] [o11:5,145,tC6]
f1156: … [o11 is gone] …
f1164: … [o11:5,137,tC4] [o11:5,145,tC6] …
```

Object 11 contributes **two** parts, both at screen column 5. Every other object
in that frame is a full 8-part character. `margin_track.py` says
`obj 11: column 5..5, parts min 2 max 2` — and, importantly, reports
**`no anomalies`**, because its `part` check only fires for objects *wholly*
outside the viewport and object 11 is on-screen at column 5. That is a real gap
in the gate, not in the feature.

**The cause.** A new trace line answers it directly:

```
oam-band: [17:y160,$B0,$40,x6] [18:y168,$10,$40,x4] [19:y176,$20,$40,x3] [20:y183,$30,$40,x3]
```

The band holds exactly four OAM sprites — slots 17–20, tiles `$B0/$10/$20/$30`,
the same rows and columns as **object 10**. Object 11's tiles `$C4/$C6` are
**nowhere in OAM at any column**. The game never emitted it.

So the overscan capture rule — "take anything the PPU's left clip would hide" —
was admitting a sprite the game had thrown away. It then clipped the blit at
column 8 like every other band sprite, on the assumption that the machine owns
everything past it. Nothing did. Columns 5–7 survived; columns 8–12 were thrown
away; what was left was the sliver. And because object 11's emission comes and
goes, the ghost blinks with it rather than with the camera — `scr == cam == 184`
throughout the window, so this is **not** the camera-step basis problem of bug 4.

**The fix.** `sdb_oam_has()` — a part in the band is drawn only when OAM really
holds a sprite at that column and row, matched with a sprite's width of slack.
Measured, same vector:

```
11  [o10:3,184,t30]        still drawn   (OAM slot 20)
11  [o10:4,169,t10]        still drawn   (slot 19)
11  [o10:6,161,tB0]        still drawn   (slot 18)
 8  [o11:5,137,tC4 GHOST]   rejected     (no OAM entry)
 8  [o11:5,145,tC6 GHOST]   rejected     (no OAM entry)
```

`drawn` 26795 → 26555 over 1180 frames. Object 10 is unaffected, so a sprite the
game *did* emit and the hardware hid is still completed, which is the whole
point of the feature.

### Two mods on at once: every sprite drawn twice

With the flicker mod **and** widescreen both on, sprites visibly double — a
character's two 8-wide halves drawn 2 px apart. Stock, widescreen alone and flicker
alone are all clean, so no single-feature gate could ever have found it; the owner
found it by playing on the game-mode menu.

Which side is wrong, measured against live OAM on the menu walk:

| | offset vs stock | frames off |
|---|---|---|
| widescreen alone | **0 px**, 14/14 | 0/14 |
| flicker alone | +1/+2 px, 12/14 (+11, −7 on build boundaries) | 12/14 |

Widescreen is exact; the mod's captured list is a build old, so its x is stale. Both
were being drawn.

**Cause.** `sdb_present()` blits the whole captured list whenever flicker has
something to say. At `cw == 256` that is right — `s_picture` there is a sprite-free
background and the list *is* the sprite layer. At `cw == 426` it is not: the
compositor's own contract is that the native 256 columns win, so its output is the
machine's finished picture, sprites included.

**Fix, and the invariant.** Present what the flicker-only path presents: replace the
native columns with that same sprite-free redraw, draw the whole list over it, leave
the margins alone.

    wide native 256 == flicker-only native 256, byte for byte

199 frames of the menu walk, **0 differing frames**, build boundaries included. That
is the durable part — the two paths cannot drift apart again, and any future change
to either is caught by diffing them.

Copying all 256 columns instead of working around `[0, SDB_WS_CLIP)` puts the
backdrop back over the reveal and returns the black band; the compositor's own seam
check already has the right expression (`int first = s_ws_reveal ? SDB_WS_CLIP : 0`)
and reusing it is the fix. Band measured on 0/900 frames with the reveal implied,
against 495/900 with it off.

Two dead ends, both of which look obviously right: content-matching the list against
live OAM (`tile+y+x` misses on build boundaries; `tile+y` misses on animation
changes, painting two whole characters), and drawing only entries past the live OAM
count (live OAM is *not* a prefix of the list — prefix carry-over — and it suppressed
the truncation, reported as bodies missing parts). The list must be compared against
OAM as of the *same* build, which is the open item in **plan.md 15**.

At the time of that fix, sweep 1200 / throw 3000 both mods on: `drawn` 33726 /
104519, `off-edge` 3951 / 3237, peak 64, backstop 0, with 3952 sprites still
restored. Both figures have since moved (current: **33885 / 104769**); the
one-build staleness was the last open half of it and is fixed by `sdb_reconcile`
above — plan.md 15.

### The menu ghost: a stale sprite list outliving the screen it belongs to — OPEN

Found by playing, twice, and **not fixed**. From two captures in `tmp/`: the
GAME MODE menu ends with an 8-sprite character in OAM slots 1-8; the next screen
retires it by writing `y=$F0` over the y byte and leaves tile/attr/x intact; the
game then emits nothing for 14 frames. `s_build_count` is deliberately never reset
per frame — the build spans two frames — so the mod repaints those 8 sprites for
14 frames onto a screen with no character on it. It is a jumble, because it is a
character's parts drawn where nothing is.

The retirement value is `y=$F0` and the cursor pin writes `$F0` to the same byte
for the same reason, so `$F0` cannot distinguish "stopped here" from "hid this
one". Three rules were built and measured; **all three trade this one artifact
for a worse one**, which is why none shipped:

| rule | what it fixed | what it broke |
|---|---|---|
| reject the list when any entry disagrees with OAM | the ghost | **87 of 128 disagreements are the attribute byte alone** — the game flipping a sprite's palette mid-build. Suppressed the mod through a whole super-throw capture: a character lost body parts on 8 frames |
| reject when OAM is entirely empty | the ghost | correct, but only catches the case where the machine has drawn *nothing*; it misses the frame the machine has already started the new screen (1 live sprite, 8 stale) |
| reject when the presented list exceeds the machine's live sprite count | the ghost | **structurally impossible**: the mod presents up to 77 sprites and OAM holds 64, so exceeding it is the truncation fix working. Fired on 34 sweep frames |
| `sdb_publish` stamps the frame the list was CAPTURED, not handed over | the ghost | correct in isolation, and it fixes the blob — but it also refuses the stale fallback on ordinary build transitions, where the published list is the right thing to show |

Measured footprint of that last one, against the pre-change build: **132 of 1200
sweep frames (11%) differ, 128 of them drawing LESS sprite ink**, on a regular
every-6-frames cadence. Frame 430 is the clearest single picture: 8 players
before, 4 after. The fallback it suppresses is not a nicety — it is what stops a
half-built list from blanking the pitch, and 64 sprites is simply more correct
than the 32 half of it that remains.

So the two cases are **not separable by any signal tried**: the ghost's published
list is 14 frames old and the healthy fallback's is 5, and a sweep also publishes
at ages 4-8, so no age threshold separates them either. What the ghost needs is to
know *the screen changed*, and the mod has no notion of a screen. Until something
does, the ghost stays: one frame on a menu transition, and no worse than stock.

What did ship is the `build-list` column on the `sdb` trace line, because
`peak-list == 0 && build-list > 0` is the fingerprint of every frame in this
family — the mod is about to paint a list it captured on an earlier frame. It is
read-only and changed no presentation decision (verified: 0 of 1200 sweep frames
differ from the pre-change build).

Also measured here, and worth keeping: **`--present-out` indexes one frame ahead
of the trace's frame counter.** Replay frame *f* is capture frame *f+1*. Both of
the captures above were misread for several passes because of it.

### The seam doubling, measured and fixed by MOVING a stale entry, not dropping it

The band holes above used to sit on top of this. A character straddling the edge
of the 4:3 viewport is drawn by **two** sources at once: the machine paints the
half inside its own picture from current OAM, the margin paints the band half from
the captured list. Those are a build apart, so the halves registered 1-2 px out of
line and the character read as doubled with a jog down it — reported as "the
partial person from stock isn't lined up with the widescreen drawing of the rest of
the body". Every camera step re-registered the halves, which is why it read as
worse the more the view panned.

**Measured for the first time**, which is what made it fixable. `SDB_TRACE_JSON`
now emits the published list (`pl`) alongside live OAM, and `json_trace.py flicker`
matches them on (y, tile, attr) and reports the histogram of `oam.x - published.x`.
Only **unique** matches count, on both sides: every player in a match shares tiles,
so a content match with two candidates says nothing, and this file already records
that tile-and-row matching reports thousands of false hits.

    over the 1200-frame sweep, 54479 unambiguous matches:
      captured (what the list says):  42514 agree, 11965 off-position  (22.0%)
      presented (what gets drawn):     54479 agree,     0 off-position  ( 0.0%)

and the location of the disagreement is the whole diagnosis: **every one of the
11965 sits on a frame that saw no build reset**. Zero mismatches on reset frames,
29.8% on the rest. So it is not "the list is old", it is "the list is old *while*
OAM is mid-next-build" — which is the ordinary state of the machine, not an
exceptional one.

**The fix is to move the entry, not to drop it.** If a list entry is the unique
(y, tile, attr) match of a live OAM sprite, draw it at that sprite's x. The
sprite is still drawn; it is drawn where the machine has that exact sprite, which
is the only position that is not a guess. Ambiguous entries keep their captured x,
so the change can only move a sprite toward the machine, never invent one.

This is the fifth attempt at this artifact and the first one that did not trade.
The four that were rejected all **rejected** the stale entry, and that is the
distinction: a stale entry and an absent one look the same to a player, and
dropping it removes a body. Measured effect on a played capture: **33 of 830
frames change, every one inside canvas columns 93-150** — the seam and nothing
else.

What remains is not this. A straddling character is still drawn by two sources,
now agreeing on position; and the list is still a build old for every entry that
cannot be matched unambiguously, which is most of them once several players share
a tile.

### The revealed band had holes in it: `sdb_oam_has` was the bug it claimed to fix

Reported as *not* truncation — light pixels blinking out of a sprite, head to
toe, as the edge of the 4:3 viewport swept across it, sometimes in the middle of
the wide frame. That is the **revealed band**: screen columns 0-7, which PPUMASK
clips, so the PPU never paints them. The compositor refills them from the
nametable, and a sprite standing in them is drawn only by the margin path.

The margin path carried this guard:

    if (completes && !sdb_oam_has(sx, m->y)) continue;   /* "GHOST" */

— skip a band part when OAM does not hold it. **That is the wrong question.** OAM
not holding the sprite means the machine did not emit it *this frame*, which for
this mod is the truncation it exists to undo; and the PPU could not have painted
those columns anyway, so a skipped part is a hole nothing else fills. What shows
through is the reconstructed band background: at sweep frame 618 a light-blue
block appears *inside the player's head*, and at 611 a dark vertical chunk is
missing from his body.

It flickered because `sdb_oam_has` is a live scan of a list the game rebuilds over
four frames and truncates at 64 sprites, so its answer changes frame to frame.

Removing it costs nothing at the seam: the blit already clips a band part to
`px0 + 8 - left_limit` columns, and `left_limit` is `native_x0 + SDB_WS_CLIP`, so a
part that reaches the draw paints ONLY inside the clipped band and cannot touch a
column the machine owns. The doubling the guard was written for was impossible.

    GHOST skips on a played capture:  14 -> 0
    changed pixels:  34 of 1100 sweep frames, ALL inside canvas columns 85-92

**The 129 "ghost" parts were misclassified by the very predicate that proved
unreliable.** "no OAM entry behind it" was read as "nothing is there"; the correct
reading is "the machine did not emit it this frame". Re-baselined: sweep 1200
`drawn` 33726 -> **33855** (off-edge 3951, peak 64, backstop 0 unchanged); throw
3000 104519 -> **104612** (off-edge 3237, peak 64, backstop 0 unchanged). Two
further steps followed — the band anchor and then the basis correction, current
**33885 / 104769** — and both are set out in "The `drawn` re-baseline" at the end
of this section. Read this pair as what the filter removal did, not as current.

Same lesson as the menu ghost one section down, approached from the opposite
direction: the recurring mistake is **asking live OAM whether the machine holds a
sprite**, because OAM is unreliable in exactly the way this mod exists to fix.

**What this did NOT fix, and it is the next one.** Removing the filter leaves the
seam *doubling*: a character straddling the edge of the 4:3 viewport is drawn by
two sources at once — the machine paints the half inside its own picture from
current OAM, the margin paints the band half from the captured list — and the two
are a build apart, so the halves register 1-2 px out of line and the character
reads as doubled with a jog down it. Reported as "the partial person from stock
isn't lined up with the widescreen drawing of the rest of the body". That is the
one-build staleness (plan.md 15), not a new defect, and the band holes used to sit
on top of it and hide it. Every camera step re-registers the halves, which is why
it reads as worse the more the view pans. Fixing it needs each entry compared
against the OAM snapshot from its **own build**, not live OAM — the same wrong
question as above, answered from a snapshot instead of a scan.

### The margin and the machine are on different cameras, and only sometimes

Reported from a played session as a **ball duplicated at the left seam** — half a
ball to the left of the real one on frames 885, 889, 893, 897, 900, 901 and the
other half missing on 904, all of them frames where the character holding it was
running into the edge and the camera was stepping — and, on the way back, as a
**sliver left behind at the seam** that flickered on and off at 934-935, 946-947
and 950-951 and appeared to travel with the player.

**It reproduces headlessly, and the vector that has it was already committed.**
Nothing about this needed a human to reproduce: `tas/vectors/sdb_pan.input` carries
it at **frames 624 and 625**, where the character straddling the seam is visibly
drawn twice, 7 px apart. It was missed for a long time because the audit in use
checks the margin against *itself* — `margin_track.py` reports `no anomalies` on
both those frames, correctly, because the margin's parts are evenly spaced and
consistently placed. The duplicate is between the margin and the **machine**, and
nothing in that file compares the two.

**The signature, measured.** For every object and frame, ask whether a whole
object sits a constant distance from the machine's copy of itself. Require two
parts to agree on the distance, so a coincidental match with a neighbouring
player — every player shares tiles — cannot produce one. Then tabulate that
distance (`delta = oam_x - margin_x`) against `d = $C3/$C4 - scroll`, counting
only parts the compositor actually paints (a `machine`-skipped part is never
drawn, so its disagreement is invisible by construction):

| | `d = −7` | `d = −6` | `d = 0` | `d = +6` | `d = +7` |
|---|---|---|---|---|---|
| **before** | 0 (143), **+7 (31)** | 0 (140), **+6 (25)** | **0 (790)** | 0 (136), **−6 (56)** | 0 (150), **−7 (57)** |
| **after** | **0 (69)** | **0 (58/60)** | **0 (341/342)** | **0 (50/53)** | **0 (48/51)** |

575 painted whole-object pairs on the pan vector (566 at exactly 0; the 9
stragglers sit 1-5 px out, inside a step). Before the fix the disagreement is 0
or exactly one camera step, and never anything between — which is what identifies
it as the same sprite placed on two cameras rather than as staleness, and it is
also why this was findable at all once the detector was built to ask the right
question. After it, every painted part agrees with the machine's copy.

**The fix**, which is the whole of it:

    sdb_object_delta()  ->  move this object onto the machine's columns

Before drawing an object, if two or more of its parts agree the machine holds
the same sprite a constant distance away, move the whole object there. Three
conditions, and each is load-bearing:

1. **Whole object, never per part.** A character is sixteen columns of two
   eight-wide sprites; moving one half onto the machine's column and leaving the
   other on the record collapses the eight pixels between them to one.
   `margin_track.py` reads that as a `ghost`, and it measured **22** of them on the
   sweep when this was first done per part.
2. **Only objects the machine cannot be painting.** Every part must sit outside
   the machine's columns (screen column < 8 or > 251) *before* the move. This is
   what makes it safe, and it replaced an earlier gate that required the
   disagreement to be exactly the camera step: that gate admitted a crowd
   mismatch at sweep 618 (an 8-part character straddling band and viewport,
   moved -7 onto a neighbour, jogging head against body) and refused the real
   d=0 case below. A part outside the machine's columns can never overlap
   anything the machine paints, so moving it can only fix a join or defer to
   the machine -- never double. It is the opposite polarity to `sdb_oam_has` --
   this *acts* on OAM, that one acted on its absence -- but where that one asked
   tile-and-row and matched strangers, this one matches world columns.
3. **Two parts must agree on it**, which (1) already needs. A coincidental match
   with a neighbouring player cannot hold one offset across an object's parts;
   live OAM being stale for 3-6 frames is exactly what the vote is for -- the
   sweep's 759-761 reads 3 columns out, which is no multiple of anything, and a
   lone part agreeing on it is ignored.

Matched on **world column**, never on tile: `oam_x + scroll + 4` against
`record_x + dx`, both carrying the guest's own `$84A1` OAM bias of 4. Two objects
are never at the same world column, so this names one sprite and cannot name the
wrong one -- which is the failure that killed `sdb_oam_has` and then the
`sdb_band_anchor` that briefly replaced it. The window is a camera step plus one
(`SDB_MATCH_SLOP` 12): the step itself, plus the frame of ball travel separating
a fresh margin capture from a 3-frame-stale OAM. An exact agreement (delta 0) is
a match and votes for 0 rather than abstaining, so a split object resolves
toward leaving it alone.

**What is still not established, and the fix does not depend on it:** *why* the two
sides disagree. The margin's `record_x` is read live and the record is rewritten
every frame, so the margin's world column is current; OAM answers for a build up
to six frames old. The gate does not need the answer, because it only acts where
the machine holds the sprite and the margin sits outside the machine's columns,
and every frame that is not that shape is left exactly as it was.

**Reading it off the trace.** `SDB_TRACE_CAPTURES=<from>-<to>` prints `[corr+N]`
next to an object on the `sdbw` line whenever the correction fires (and
`[corry+N]` for the row correction below), and nothing when it does not. Over the
701-frame sweep audit window that is **56 column corrections** (mostly ±6/±7, tail
+3/+4/+5) plus **3 row corrections**; over the pan window **52**, all columns.
Printing the zeroes as well fills the line with one `[corr+0]` per object per
frame, which reads as a column of data and is none: the same "a green line that
measured nothing" shape this file keeps having to undo.

**Re-baselined:** sweep 1200 `drawn` 33874 -> **33885**, throw 3000 104615 ->
**104769** (`off-edge` 3951 / 3237, peak 64, backstop 0 unchanged in both). The
increase is parts the cull-bit mask below puts back (objects the game flagged
but kept emitting) net of parts the correction moves into the machine's own
columns, where the draw site correctly skips them because the machine paints
them. Per-part outcomes over the sweep are now
**drawn 32341 / machine 6985 / oob 1405**, with no `ghost` outcome -- the
`ghost` classification was removed with the filter that invented it.

Gates, all re-run on this build: margin off vs on byte-identical over 900 frames;
mod off vs stock 0/400; the machine's own 256 columns untouched 0/600;
`margin_track.py` reports **one `cshift` on the sweep** (frame 760, corr+3,
visually verified whole) **and none on the pan vector**; the
doubling invariant unchanged at **479/900** (below). Against `HEAD` over 800 sweep
frames the picture moves on **17 frames, 3932 pixels, none of them confined to the
machine's columns** -- all of them at the band or margin boundary, which is where
the doubling was.

### The identity check knew the object by its culling bits

A whole character blinking out at the margin edge for exactly as long as the
game holds a flag: object 3 draws 8 parts, then nothing for three frames, then 8
again at identical columns -- no placement problem, the slot is simply empty.
The margin keeps `id0/id1` (record bytes 0-1 at capture) and refuses to draw a
slot that no longer matches. Byte 0 is the object's state byte `$43`, and
section 2.1's three culling gates read exactly `($43 & $70) == $20` and `& $20`
-- bits the flicker mod overrides on purpose, so the object keeps being emitted
while the game insists it is hidden. Byte 0 goes `$02` -> `$22`, the test reads
that as "a different object took the slot", and the whole character drops. That
is also why it happens with the flicker mod on and never without it: with the
mod off the object genuinely does vanish.

Measured on a played capture at frames 910-912 (and 925-926), reproduced
headlessly at 919-921 with the identical signature (56 -> 48 -> 56 parts, a drop
of exactly 8). The fix masks `$70` out of the byte-0 comparison and nothing
else: byte 1 (`$41`, which does not move with the culling state) and the
remaining bits still have to match, so a genuinely recycled slot is still
caught -- which is the reason the check exists at all. Purely additive on the
same vector: 89 more parts painted over 1000 frames, **zero** painting less, and
`off-edge` unchanged, so the capture side is untouched.

### The join the basis gate refused

A character straddling the seam with its halves 4 px apart and the camera
**still** (`scr == cam`, so no basis is involved at all): the margin draws the
band half at columns 1-3 while the machine draws the rest at 5-13. The old
basis-shape gate refused it -- 4 is not a camera step -- and the picture showed
a jog down the body. The mechanism is intra-frame, not inter-frame: the game
advances its camera partway through the frame, so sprites emitted before the
advance and captures taken after it disagree by less than a step with neither
side "stale". `corr+4` moves the band half onto the machine's columns and the
character reads whole; verified frame by frame, 716-720.

This is the case that replaced the gate rather than widening it. Widening the
shape to admit a 4 px disagreement also admits the sweep-618 crowd mismatch (a
full character moved -7 onto a neighbour, jogging head against body -- measured,
with pictures, before the widening was reverted). The reach check admits the
real one and refuses that one for a structural reason instead of a numerical
one: f718's object sits wholly outside the machine's columns, 618's straddles
them. What the audit sees of this: nothing -- `margin_track.py` compares the
margin against itself and both frames are internally consistent, which is the
standing blind spot below, now with a `cshift` outcome for the cases the
compositor acted on (exit 0, each one still wanting eyes once).

### Rows leapfrog the same way columns do

A split ball with the columns in full agreement: margin and machine both put the
ball's left half at column 4, but the margin's rows read 138/146 against the
machine's 137/145. One pixel of vertical disagreement between two copies of an
8 px sprite, blinking as the two sides update on different frames -- the row
axis of the same staleness the column correction handles, and nothing corrected
it. The conversion itself is exact (`$EC - Y16`, verified: the frames either
side agree to the pixel), so a 1 px difference is motion between the capture
and the emission, not arithmetic.

The fix votes rows the same way it votes columns -- two parts must agree the
machine's copy sits N rows away, with the vote window itself bounding N to ±2,
so a genuinely different animation cel is never touched -- and paints the
corrected row (`[corry+N]` on the trace, mirrored in the JSON twin).
`corr+9 corry-1` on the reported frame; one ball on every frame either side,
verified 783-787.

The flicker side needed the same widening for the same reason: `sdb_reconcile`
matched list entries to OAM on exact `(y, tile, attr)`, so an entry whose sprite
both moved and was re-emitted -- new row as well as new column -- never matched
and kept its stale x while the margin completed the same sprite at the live
column. Rows now match within ±2 there too: over 23820 presented-list entries,
**771 move from "no candidate at all" to unique matches with zero new
ambiguity**; at ±3 ambiguity starts (9 entries), so ±2 is the last free width.
The list-side ambiguity guard moved with it -- uniqueness is now tested against
the matched OAM slot itself rather than pairwise between entries, because with
a row window two entries a row apart can chase the same slot.

### The doubling invariant does not hold on a match

Measured while verifying the above; it qualifies a claim in this file. "Wide native
256 == flicker-only native 256, byte for byte" was established on a **menu walk**
(199 frames, 0 differing). On a full match it does not hold: **479 of 900 sweep
frames differ, and 289 of 900 on a played vector** — at HEAD, before any of this
session's work, and still **479 of 900** with the basis correction above, which is
neutral on it for the same structural reason: a corrected part moves into columns
the machine owns, and is then skipped rather than drawn.

So it is a property of quiet screens, not a general one, and the §26 measurement
should not be read as "the two paths cannot drift". What the band fix does
establish is narrower and checked: it changes pixels in canvas columns 85-92 and
nowhere else.

### The `drawn` re-baseline: one figure explained, one not

`SDB_TRACE_JSON` makes this decidable rather than a matter of opinion, because
it reports the *outcome of every part* rather than only a total. Over the
1200-frame `sdb_sweep` vector at 16:9 with the reveal on:

| outcome | count |
|---|---|
| drawn | 32341 |
| machine (the machine owns those columns) | 6985 |
| ghost | **gone — the outcome was removed with the filter that invented it** |
| clip | 0 |
| oob | 1405 |

**The sweep figure is fully explained — and the explanation was itself wrong.**
The previously documented `33855` is exactly what `drawn` was *before* the
`sdb_oam_has()` ghost filter existed: the run's own counter reported 33726, and
33726 + 129 = **33855** with no residue. That was read as a stale measurement and
the filter as a fix.

It is the other way round. **33855 was the correct figure and 33726 was the
broken one**, because the filter asks live OAM whether the machine holds a sprite
and acts on the answer. See "The revealed band had holes in it" immediately
below. With the filter removed this run's counter reported 33855 again; the band
anchor moved it to 33816, the basis correction to 33874, and the cull-bit mask
plus the row/reach corrections to the current **33885**. Every
step is in this file, and the two that went the wrong way are the ones worth
keeping.

**The throw figure is still not explained.** The previously documented `104240`
is *below* what this binary produces (104519 at the time, **104769** now) while
that vector's ghost count was only 93, so the ghost filter cannot account for it
in that direction. Every configuration axis available was ruled out:

| axis | result |
|---|---|
| aspect 16:9 / 21:9 / 32:9 | `drawn` identical (33726 at all three, at the time) |
| reveal | 32108 without — and `off-edge` 3665 ≠ the documented 3951 |
| character-flicker mod off | 33538, `off-edge` 3791 ≠ 3951 |
| vector length, `--present-every` | ruled out; `off-edge` pins both vectors to flicker-on + reveal |

`off-edge` reproducing **exactly** on both vectors (3951 and 3237, both still
current) is what makes this tractable at all: the capture side is bit-identical,
so the discrepancy is entirely in the presentation count and is not RNG
divergence. The throw figure is recorded as stale; `AGENTS.md` gate 5 says not to
expect it back.

Every `drawn` figure here is a `--present-every 1` figure. That flag sets the
*present* interval, so a stride above 1 skips the compositor and the counts drop
meaningfully rather than slightly (at stride 200, 167 instead of 33726). AGENTS.md
has the full note.

**Current, for the next person to re-baseline from:** sweep 1200 `drawn` **33885**,
throw 3000 **104769**, `off-edge` 3951 / 3237, peak 64, backstop 0, seam diff 2 /
0 px, `composed` 1152 / 2952. The step from 33874 / 104615 is the cull-bit mask
putting culled objects' parts back, net of the row/reach corrections moving
parts into the machine's columns (correct skipping, as before). Every
`drawn` figure here is a `--present-every 1` figure.

### Recording a playthrough: `--record-input`, and why it is not optional

`CycHostExtras` has an `input` callback — "final NES controller bytes, after host
mapping and before latching". Both host loops call it immediately before
`cyc_session_frame_begin()` (`cyc_sdl.c:1470`, `cyc_host.c:1602`), so `s_frame`
there *is* the index of the frame about to run, which is exactly what
`input_tick()` matches steps against. `--record-input FILE` therefore writes the
plain `--input` schedule format, not a new one, and `tools/play_record.sh` wraps
it with the trace and the screenshots.

This is not a convenience. Two hand-written vectors had already failed here for
reasons nobody could have guessed by reading the code — the court input window
opens at frame 594 and a margin boundary artifact needs the camera to pan the
whole width before it shows anything — and **neither is expressible as a
schedule someone typed**. The seventh bug above was found by a played session and
had survived the sweep vector, the pan vector, the idle vector and the throw
stress vector.

Two windowed runs of one schedule are byte-identical to each other (same recorded
vector, same trace), which is what makes a played session reproducible.

### Three bugs in the instrumentation, all found by using it

1. **The `sdbw` trace line had no newline before `sdbm`**, so the per-sprite
   entries were glued onto the summary line and `margin_track.py`'s regex matched
   nothing — it reported `no sdbw lines in that log` on exactly the runs where
   you turn the trace on to run it. Fixed by printing the line closer first.
2. **…which then hit the mirror image of the same fault.** The commit that fixed
   (1) also added the `oam-band:` line, and that block ends with its own `\n` — so
   the closer, moved above the entries, ended up *below* that block and printed
   on a line of its own. The regex wants `… n=NN` at the end of the `sdbw` line,
   so **0 of 701** lines matched and the audit was inoperable: a healthy run
   reported "no sdbw lines in that log". Measured 2026-10-13 while running the
   §26 gates, which is the third time this class has cost an audit. The closer
   now prints *before* the `oam-band:` block; the order of the two blocks is the
   entire fix.
3. **`margin_track.py`'s `part` check only fires for wholly off-screen objects.**
   An object straddling the band at column 5, holding 2 parts instead of 8,
   passes clean. That is how the seventh bug survived an audit that was otherwise
   green.

Two of the three produced a **clean-looking pass over zero examined frames**,
which is the failure mode worth naming: an instrument that cannot fire is
indistinguishable from an instrument reporting nothing wrong. Check that a gate
*ran* — `margin_track.py` should print a per-object table — before believing it.

**The structural answer to that class, added afterwards: `SDB_TRACE_JSON`.**
Both of the above are parsing and ordering faults in a hand-formatted trace, not
places where a reader looked in the wrong place — and a missing newline took out
the whole audit on exactly the runs you enabled it for. So the mod now emits one
JSON object per line, two kinds: `f` at frame begin (the flicker counters plus
**every live OAM entry for that frame**) and `m` at composite (each margin part
with the branch the draw site took for it — `drawn` / `machine` / `ghost` /
`clip` / `oob`; a recycled record as `"stale": n` with no parts, because the
draw site skips that object before any placement decision exists). Read with
`tools/widescreen/json_trace.py`. `json.loads` per line cannot care where a
closer was printed.

Its `flicker` command is the instrument **plan item 14** has been waiting on:
live OAM against the mod's published list in the same frame, which is the whole
question that bug turns on. Verified inert — with the variable unset, the human
trace and every presented PNG are byte-identical to a run with it set
(1937 trace lines, 661 frames, 0 differing).

### Open, shelved: Bean Ball loses sprites while running across the full map

Found by the owner playing, not by a vector. Game mode **"bean ball"** lets you
run across the entire map; widescreen's margins and the flicker mod both work in
that mode in general, but **running from one end of the screen to the other**
makes sprites disappear one at a time until a single player sprite is left, cut
in half. `overflow 0`, `backstop 0` — not §2.1's runaway list — and the
background is perfect (`1183 composed, 1 pillarboxed, seam diff 2 px`), so the
compositor is not involved. The mod replaces the machine's sprites with its own
captured list, so a short list is missing sprites.

**Not attributed.** Whether the game emits fewer sprites or the mod drops them is
unknown, and the two need opposite fixes. See **plan.md item 14**, which carries
the owner's frame-by-frame description, the measured per-frame list decay, the
next measurement, and how to get a fresh capture.

### What this does not fix

- **Vertical culling is untouched, deliberately.** A sprite culled for being
  above or below the viewport is not captured: the camera scrolls horizontally
  only, so an off-screen row is off-screen for good and there is no vertical
  margin to put it in.
- **Only the two culled emitters.** The damage-number pair (`$966D`) and the
  block copier (`$A36C`) have no position accumulator and no such cull; they
  are HUD-ish and never strayed off-screen in any measured frame.
- **Past the stadium's end there is nothing.** §25's limit stands, and it is now
  enforced rather than accepted: the world's own bounds are **`[40,480)`**, which is
  the game's camera window (`$C3/$C4` clamped at `$E6AB` to `[40,224]`) plus the
  256-column viewport — not the 512-pixel address space, and not the court's drawn
  extent of `[32, 480)`, which is a different quantity that happens to be 8 columns
  wider at one end. A margin sprite wholly outside the bound is not drawn while one
  straddling it is clipped at it. The earlier note here that "the sprite is drawn
  where the game put it, which may be over black" described the same bound applied to
  nothing; see §25 for the measurement, the two captures that decided it, and the
  gate.
- **The 8-per-scanline limit is still not modelled** in the margin (nor in the
  flicker mod — research §2 open item 11).
- **A margin sprite's part set is the last list's, not this frame's.** The game
  rebuilds a list over two frames and leaves OAM alone for three to six more, and
  the margin follows that cadence by construction — it re-places from the record
  every frame but re-reads tiles only when the object is emitted. That is the same
  staleness the machine has, and it is why the audit's `part` check tolerates a
  partial set while an object is on-screen.

---

## 27. Sprite judder: the game's own cadence (RESOLVED 2026-10-13)

**The judder is the game updating its camera and its sprite OAM two frames
apart. MesenCE reproduces it exactly. Nothing to fix, and specifically not in
this engine's PPU model.** Reproduce in one command with `tools/tas/judder.py`;
the measurement is below and the script is kept so nobody re-chases it.

### The metric, and why it needs no picture correlation

While the camera pans, the player walks left one 6–7 px step and the camera
walks right one 6–7 px step. If the game does both on the *same* frame they
cancel and the player is glued to the court. If they are `N` frames apart the
player detaches from the floor for `N` frames and snaps back — which is exactly
what is seen.

So for the player sprite (OAM y=143, tile `$20`):

```
world_x(frame) = OAM X + $C3/$C4
```

`world_x` sits on a base value and blips by one step for `N` frames. Both
quantities are in artefacts each side *already* writes — the cycle log carries
RAM and OAM, MesenCE dumps `.ram.bin` and `.oam.bin` per frame at
`SHOT_STRIDE` 1 — so this needs no picture analysis and no new emulator probe.

An earlier attempt compared consecutive frames by column-SAD and locked onto
the flat regions of the court, reporting shifts that contradicted the `scr`
trace. **Do not measure this by correlating pictures.**

### Measured

| | camera steps at | sprite steps at | N | world_x base | blips |
|---|---|---|---|---|---|
| cycle backend | 601, 608, 615, 622 | 603, 610, 617, 624 | **2, 2, 2, 2** | 207 | 15 / 34 |
| MesenCE 2.2.1 | 603, 610, 617 | 605, 612 | **2, 2** | 207 | 6 / 20 |

Same N, same base, same blip shape. A second, independent emulator does the
same thing, so the two-frame offset is the game's cadence.

The two sides log on different events (cycle via `--frame-log-at mesen`, MesenCE
via `endFrame`), so absolute frame numbers are offset by a constant — **N is a
difference of frames and compares directly; the frame numbers do not.**

### What it means for this project

Nothing to fix upstream, and nothing to fix in a mod. A presentation mod
cannot reach it: the judder is in the machine's own 256-column picture, which
the compositor's contract is to reproduce faithfully, and smoothing it would
break the 0-pixel gate from §25/§26.

### Three things that had to be untangled first

**The harness was not logging scroll at all.** This section previously claimed
*"`mesen_tas.lua` already logs scroll and OAM per frame in
`mesen_state.jsonl`"*. It does not: five of its state columns have been `null`
since it was written, because the state keys are namespaced (`ppu.xScroll`,
not `ScrollX`) and the script hardcoded the bare names — the exact thing
research §16 warns about. Fixed in `7cfa35a`.

**`ppu.xScroll` is not the court's scroll.** With the key fixed it populates,
and it reads 0–7 through the whole pan while the game's `$C3/$C4` goes
128→134→147→154. So it is some PPU-internal value and is **not** comparable to
`cyc_render_line_scroll_x`. Reading it as the scroll would have produced a
confidently wrong answer. The metric above sidesteps the question by using the
game's own camera variable, which both sides dump in RAM.

**The two sides are not in the same game for the whole run.** Cameras agree at
frame 600 (134 on both) and have diverged by 650 (cycle 75, MesenCE 115). That
is the known RNG divergence (AGENTS.md), and it is why the window above is
597–618 rather than the whole pan. A comparison that ran to the end of the pan
would have been comparing two different games.

### What it looks like

While the camera is moving, a player does not slide smoothly across the court.
He holds still for several frames, then jumps several pixels — and the jump is
not the same size as the court's, so he visibly detaches from the floor he is
standing on and snaps back. It is most obvious with a **still** camera
elsewhere in the frame to compare against, and on a **flat** stretch of court
where the background has little horizontal detail to hide it.

### Reproducing it

`tas/vectors/sdb_bar.input`. Measured timings on this ROM, all of which had to
be found rather than guessed, and each of which silently swallows input aimed
before it:

| frame | what |
|---|---|
| **412** | the court appears — camera 128 shows up, the display records start populating at 421 |
| **594** | the input window opens. Direction presses before this are **dropped**: pulsing LEFT from frame 380 leaves the camera at 128 until 594, when it starts stepping 128, 121, 115, 108, 102, 95, 89 … |
| **617** | a single left tap starts a run |

```bash
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 700 \
  --input tas/vectors/sdb_bar.input --ram-init zeros \
  --shot-every 1 --screenshot tmp/jud/s_
# frames 614-619, native 256 columns, side by side
```

It needs **no mods at all** — this is the stock picture. That is the first thing
to establish, because the obvious suspects (the flicker mod restoring sprites,
the widescreen compositor re-placing margin sprites) are all absent here.

The original `X + scroll` table that first recorded the symptom is kept in git
history; it used `cyc_render_line_scroll_x(160)` for the scroll, and the
attribution above supersedes it only in its *conclusion*, not its observation —
the 200↔207 alternation is exactly what `world_x` shows.

### What is ruled out

- **A mid-frame scroll split.** `cyc_render_line_scroll_x` at rows 60, 120, 160,
  200 and 239 is **identical on every frame checked** (597-625). So this is not
  the game writing `$2005` part-way down the screen and the screen tearing at
  the write point.
- **A deliberate OAM snapshot in the renderer.** `cyc_render_sprites`
  (`cyc_render.c:126`) reads `ppu.oam` live, so there is no frame-boundary buffer
  that could be a frame stale.
- **Fine scroll.** The scroll never takes a value between its 6-7 px steps at all
  (128 → 134 at frame 598 → 141 at 605 → 147 at 612 → 154 at 619), so the game is
  scrolling coarsely — there is no sub-pixel motion being dropped.

### Related, and *not* the same thing

§2's character flicker is sprites being **dropped**; this is sprites being drawn
at a position two frames behind the floor. Both come from the same underlying
cadence — the game rebuilds a list over two frames and then leaves OAM alone —
but they are different symptoms, different measurements, and a fix for one says
nothing about the other. §2's fix is opt-in because it changes the guest; this
one cannot be changed by a mod at all.
---

## 28. The camera anchor: the court is 440 columns and the panning buys nothing

*2026-10-15. `widescreen`'s `camera = locked` option, `tools/widescreen/anchor_gate.py`.*

### The claim that started it

The margin bound from §25 (`60b94e7`) is not only a stopping rule for the
reconstruction. It is also a statement about the *game*: the camera is clamped to
`[$28, $E0]` = `[40, 224]` and the viewport is 256 wide, so the world the machine
can ever display is `[40, 480)` — **440 columns** — and the game's own viewport
slides 184 columns of it.

That makes "show the whole court and stop panning" answerable from two immediate
operands, with no per-level detection and no measurement of the art. Which is
what §25's first attempt got wrong: the end wall's appearance is not a fact
about the game's limits, but the clamp is.

### Why it is worth doing, stated correctly

The first framing — "show more of the court" — is **false at both wide
aspects**, and it is worth recording because it was the framing that produced the
overscan and margin-sprite work.

The canvas shows world `[camera - pad, camera + 256 + pad)` with
`pad = (width - 256) / 2`:

| canvas | pad | court (440) fits? | with the measured camera range |
|---|---|---|---|
| 426 (16:9) | 85 | no, 14 short | which end is missing depends on where the ball is |
| 560 (21:9) | 152 | yes, for camera ≥ 72 | measured range **102..180** — always |
| 854 (32:9) | 299 | yes, for every camera the clamp allows | always |

Measured on the sweep vector's match (frames 422-760), the camera spans
**102..180**. So at 21:9 and 32:9 the entire court is **already on screen at
every camera position play reaches**, today, with no mod at all.

The panning therefore adds motion and no information, and it competes with the
one thing being tracked. The feature is a *camera policy*, not a wider view: the
canvas becomes a fixed window on the court and the game pans behind it.

### The rule

```
s = (SDB_WS_LO + SDB_WS_HI - width) / 2      /* centre the court's own window */
if (s > court) s = court;                    /* native column 0 on the canvas */
if (s < court - (width - 256)) s = court - (width - 256);
x0_court = court - s;                        /* x0_held = native_x0           */
```

`s` is a function of the **width alone**, recomputed per presented frame, so
`fit` re-evaluates it on a window resize. The two clamps exist only to keep the
machine's own 256 columns on the canvas; the first bites during the level intro,
where `$DBBB` initialises the camera to 0.

Measured anchors, and they are the formula evaluated:

| canvas | `anchor` (world column at canvas column 0) | court at canvas columns |
|---|---|---|
| 560 | **-20** | 60..500 — centred, 60 columns of backdrop each side |
| 854 | **-167** | 207..647 |

### The held band, and why the offset is per row

This game splits its scroll mid-frame. Rows 0-47 hold scroll 0 and carry the
skyline and the scoreboard; rows 48-239 scroll with the camera:

```
sdbc 601 scr=134 cam=134 sxs=0,0,0 band=48-239
```

(`sxs` samples rows 0, 46 and 47; measured on the sweep vector mid-pan.)

So anchoring the whole canvas on the court **slides the HUD 6-7 px every time
the camera steps** — a new artefact, introduced by the thing meant to remove
one. The held rows are not world content: the game pins them in screen space,
which is why `native_x0` has always been the right place for them. Hence two
offsets, and the band is decided per row by measurement
(`cyc_render_line_scroll_x(y) == s_ws_court`), never by hardcoding 48.

No margin sprite has been observed in the held band — 3333 parts over 81
captured frames of a pan, rows 102..217 — so this only decides background rows
and the native block's placement. It is per row anyway so that a row's scroll
and its offset are always read together and cannot disagree, which is the
"basis split" §26 records as a character drawn at a sixteenth of his width.

### `cyc_render_set_native_origin` is what this was for

`cyc_render.h`: *"Called by a compositor that placed the native picture somewhere
other than the native_x0 it was given (a camera anchored at an area's edge)."*
Nothing in the tree used it. The flicker mod's wide path reads it
(`cyc_render_native_origin`) to place its own sprite list, so declaring it is
what keeps the two sprite sources together when the block is not centred.

### What is measured

All on the sweep vector, `--ram-init zeros`, `--present-out --present-every 1`
(the compositor only runs on a presented frame; a stride above 1 skips it and
every count below is meaningless).

| claim | 32:9 | 21:9 |
|---|---|---|
| distinct anchor values over 339 frames | **1** (`-167`) | **1** (`-20`) |
| frames with the court off its own picture | 0 | 0 |
| machine's own 256 columns vs the same stock frame, at each run's own anchor | **339/339 identical** | **339/339 identical** |

And over 1800 frames of `sdb_throw` at 21:9 — team-select, roster, innings,
level transitions: **1752 anchored frames, one anchor value, 0 frames with the
court off canvas, seam diff 0 px, margin backstop 0, peak 63.**

**Follow mode changed nothing of its own**: 701 of 701 presented frames
byte-identical to the pre-change build, and every counter (`composed 652,
pillarboxed 1, diff 1, off-edge 1458, drawn 9372, peak 48`). That is why `locked`
defaults off and why the option exists rather than the behaviour replacing it.
(Follow mode *does* differ from the pre-change build now — see the reveal fix
below, which is a deliberate behaviour change and not the anchor's.)

**The `drawn` and `off-edge` baselines do NOT move with the camera.** Locked and
following read **12507** and **1795** — to the byte, at both aspects, with
`peak 59, backstop 0` — which is the right answer and was not the prediction:
both counters count sprites outside the *machine's* run, and the run is defined
relative to the block, which the anchor moves but does not resize. Every earlier
statement that the anchor would re-baseline these was wrong.

(The 12507/1795 figures include the reveal, which is now on by default — see
below. With `SDB_OVERSCAN=0` they read 9372 and 1458 at 700 frames.)

### Four things this cost, recorded because they are reusable

1. **The anchor record belongs to presented file `f-1`, not `f`.** The presented
   index runs one ahead of the trace's frame counter (AGENTS.md). Pairing them
   the wrong way round reports **26 failures, every one of them a
   camera-stepping frame**, and each looks exactly like a 7-pixel placement bug.
   It is only visible when the scroll *changes*, because on every other frame a
   one-frame offset compares two identical pictures. Scanning the canvas for the
   block's real position settles it: file 596's block is where anchor **597**
   says it is. This one nearly became a "the camera is misregistered by a camera
   step" conclusion.
2. **Do not compare one run's pixels to another's.** The obvious form of claim 2
   — locked canvas == follow canvas translated — is true and was measured
   directly (0 of 76,970 court pixels differ at the predicted shift at frame
   605, ~40% at every other shift), but as a *gate* it is not well posed: it
   needs both runs' presented indices to agree and they do not reliably. The
   form that works asks each run the same question of the *same stock frame*
   using *its own* recorded anchor, which has no alignment assumption at all.
3. **The shift is per band, not per canvas.** Comparing all 240 rows at the
   court's shift reports ~910 differing pixels on a healthy run, every one a
   held-band pixel read four columns off.
4. **A probe window is not a fix for an index ambiguity.** The gate's first
   version probed ±4 and reported placement errors that were not there — the
   kind of error a reader believes. `tools/widescreen/anchor_gate.py` says so at
   the constant.

### A pre-existing asymmetry found on the way, and FIXED

`--widescreen` on the command line never enabled the clipped-column reveal:
`sdb_ws_set_overscan` was called from `sdb_ws_enable` (the mod activation path)
and **not** from the `--widescreen` option handler in `sdb_option`. So a bare
`--widescreen 21:9` presented an 8-column black gutter at the block edge — the
exact artefact widescreen exists to remove, and the reason `overscan` was folded
into this feature at all — while the same feature through the launcher revealed
it. AGENTS.md's documented gate command uses `--widescreen`, so its "no black
gutter" expectation was being measured with the reveal **off**.

Fixed, and the obvious one-line fix was the wrong shape twice:

1. **Stamping the reveal from the `--widescreen` handler** makes the two flags
   order-dependent: `--overscan 0 --widescreen 21:9` would reveal and
   `--widescreen 21:9 --overscan 0` would not. `--overscan` is a bare flag and
   cannot carry a default of its own, so the default has to be *asked for*
   rather than pushed.
2. **Putting the `SDB_OVERSCAN` read in the activation plugin** regresses the
   documented A/B on the CLI path, because the plugin does not run there:
   measured **280 of 701 frames differing** between `SDB_OVERSCAN=0 --widescreen
   21:9` and the same with `--mods-root`. Same class of bug as the one being
   fixed — two paths to one feature answering differently.

So the reveal now has **one** resolver, `sdb_ws_reveal_ask`, consulted by
`sdb_ws_apply`: an explicit `--overscan`, then `SDB_OVERSCAN`, then widescreen's
own default of revealing. Stock width resolves to 0 (no compositor owns those
columns). Five measurements, 700 frames of the sweep at 21:9:

| comparison | frames differing |
|---|---|
| CLI vs the launcher path (**the bug**) | **0 of 701** |
| `SDB_OVERSCAN=0`, CLI vs launcher (the regression) | **0 of 701** |
| reveal default vs `SDB_OVERSCAN=0` (the A/B must still bite) | 280 of 701 |
| `--overscan --widescreen` vs `--widescreen --overscan` | **0 of 701** |
| `SDB_OVERSCAN=0` vs explicit `--overscan` (must differ) | 280 of 701 |

Every one of those is a *comparison*, not a single file, which is the point:
three of the five are 0 and two are non-zero, so the instrument distinguishes
"the paths agree" from "the switch still works". The camera lock was re-gated
after the change, at both aspects, with `--reveal`: still 339/339.

### What is NOT established

- **How it looks in play.** Every measurement here is scripted. AGENTS.md's
  standing rule — a played session is the only way to find the band bugs —
  applies with full force, because a locked camera turns the margin from "where
  the camera happens to be looking" into "where the players are, all the time".
  Every one of §26's eleven margin bugs gets a larger audience.
- **Bean Ball.** Its world is not the 512-px stadium, and the margin is already
  black there. Whether the anchor helps, does nothing, or exposes something new
  is unmeasured. It needs a window (`SDB_CAMERA` / the option), not a headless
  replay — see plan item 14.

### Two bugs PLAY found and a scripted run never could

Both were found by playing it, in one session, and both are the anchor's. They
are recorded here because the first is the more interesting lesson: **the anchor
was never only the compositor's business.**

**1. The health bars and player names slid left and right with the camera.** Not
the whole picture — the court sat still, which is the point — but rows 0-47, the
skyline band, drifted 6-7 px per camera step.

Measured, not inferred: at sweep frame 649, where the court's band sits 48 columns
from the held band's slot, the flicker-mod-on run's rows 0-48 are the
flicker-mod-off run's rows 0-48 shifted **exactly +48**, with the court band
aligning at 0. The compositor places the machine's picture at *two* offsets, and
`sdb_present` — the flicker mod's own presentation path, which runs earlier in
the file and had no idea — stamped all 240 rows at the single
`cyc_render_native_origin` returns. So every sprite redraw dragged the HUD with
it.

It took a played session to find because **every scripted check in this project
runs with the flicker mod off**, and with it off the wide path returns the
compositor's canvas untouched: `redraw` is false and `sdb_present` copies
straight through. 701/701 byte-identical frames, two clean aspect gates, and a
339/339 block comparison — none of them touch the code that was broken. The
combination is the bug, and no single-mod gate can see it.

**2. Every menu rendered 132 columns left of centre.** A title screen or a menu
is not world content: nothing tells the PPU to scroll, so `cyc_render_line_scroll_x`
reads 0, the anchor still engaged, and the block went to canvas column 20 where
the engine had centred it at 152. Measured across frames 12-140 — every one of
them `scroll 0, x0 20, xh 152`.

The court's window does not describe a menu, and the game's own clamp is the
test: `[SDB_WS_CAM_MIN, SDB_WS_CAM_MAX]` is `[40, 224]`, the camera cannot leave
it during play, so "is this a court frame?" is answered for free and costs
nothing where it matters. It also catches the locked intro camera that `$DBBB`
sets to 0 for ~204 frames at the start of a course — which the bound's own comment
had already called "a cosmetic cost for a locked intro camera, and the right way
round" without noticing the picture itself was 132 columns off.

**A third path, caught by reasoning rather than by playing**, and it would have
been worse than either: when the compositor *declines* a frame (no background
capture, or a seam-check failure) the host pillarboxes at the **centred** column,
and the anchor state was still saying `x0_court`. `sdb_present` would have
stamped the flicker redraw a camera step from where the host drew the picture —
on exactly the frame-0 and scene-change frames a player notices. Every `return 0`
in `sdb_ws_composite` now calls `sdb_ws_deanchor()` first.

**What the gate says now.** `anchor_gate.py` reads a per-frame `here` flag from
the anchor JSON and treats the two regimes separately: frames the anchor engaged
on must share one anchor, and frames it declined must have `x0 == xh` — which is
the menu fix, stated as a check. A run that examined menus and reported several
anchor values would not be movement, and the gate no longer reports it as such.
Follow mode has exactly one `(x0, xh)` pair per aspect (`152/152`, `299/299`),
which is the proof that the per-row helper is a no-op there and stock is
untouched.

## 29. The margin seam: a character drawn in part at the handover

Reported from play, 2026-10-16, on the locked-camera 21:9 capture
(`tmp/margin-locked`): a player at the right edge of the court is whole, then
missing the top of his head, then whole again, with no input pressed. "Parts of
his body start disappearing since that appears to be the border." A second
player, rightmost in green, from frame 836. The margin-left and margin-right
edges both do it, and the shimmer sweeps across characters as the camera moves.

### What it is not

Three plausible readings were measured and discarded before the cause was found,
and each is a shape this project has paid for before.

**It is not the capture losing parts.** The per-object part count *appears* to
decay — object 3 goes 8, then 4, then 2 — and `sdbb`/`sdbv`/`sdbp` exist because
that decay was the first thing to look at. But `$8483` fires **eight times per
list, every list**, `$02` counting 8 to 1 on frames 800, 804, 808, 812, 816 and
820 without exception. The game culls nothing. The decay is the *handover*: the
margin keeps only what the machine cannot paint (`lost = x < 4 || x > 255`), so
each part transfers from margin to machine as the camera pans, and 8 to 4 to 2 is
a character walking into the viewport, working as designed.

**It is not the boundary detection.** `sdbb` shows exactly one bump per sprite
list, every four frames, `cur=04 < prev=F8/FC`. No spurious boundaries.

**It is not the flicker mod.** The same replay with `remove-character-flicker`
disabled gives `obj 3: 780-807:8  808-811:4  812-880:2` — byte-identical. The
cursor pin is not involved.

### The PPU rule, and a correction

The PPU **clips** sprite pixels past column 255. Nothing wraps. The rule is
`visible = max(0, 256 - X)`, and **X=248 is fully visible** — clipping begins at
249. Confirmed in Mesen (`Core/PPU.cpp:736`, `spriteX + i + 1 < 257`), Mesen2,
Nestopia (`NstPpu.cpp:2015`), FCEUX (`ppu.cpp:2271`, `rasterpos >= x &&
rasterpos < x + 8`), Nintendulator (`PPU.cpp:818`, `Clockticks < 256`), and at
circuit level in `emu-russia/breaks`: the sprite FIFO's down-counter has no
carry path.

The "X ≥ 252 is not displayed" folklore traces to one compressed sentence on the
NESdev PPU_OAM page — "*X-scroll values of $F9-FF results in **parts** of the
sprite to be past the right edge of the screen, thus invisible*". Note it starts
at $F9, not $FC, and says "invisible" while describing a partial effect. The
practical disproof is that games deliberately draw a column of black sprites
across the rightmost 8 columns to mask it, which would be pointless if those
columns could not be drawn.

### The seam, precisely

The game's cull (`$8488` X16 < 4, `$8494` high byte set) admits accumulator
4..255, i.e. **OAM X 0..251**. So the machine's own sprites, drawn into a
256-wide buffer, lose their tail: a sprite starting at OAM X 249, 250 or 251
loses 7, 6 or 5 columns. On stock that is the right edge of the screen and
invisible. **On a wide canvas that column sits in the middle of the court.**

Measured at frame 809, from live OAM (object 3, the reported player):

| band | machine's sprite | paints native | margin's part | paints native | gap |
|---|---|---|---|---|---|
| row 144 | tile 8D @ x=246 | 246–253 | sx 254 | 254–261 | none |
| row 152 | tile 86 @ x=250 | 250–**255** | sx 258 | 258–265 | **256, 257** |
| row 160 | tile 8F @ x=249 | 249–**255** | sx 257 | 257–264 | **256** |
| row 168 | tile 30 @ x=251 | 251–**255** | sx 259 | 259–266 | **256, 257, 258** |

The game holds only the character's **left** halves in OAM and culls the right
ones, so the margin supplies the right. The machine's half stops where the buffer
ends; the margin's starts beyond it; one to three columns × eight rows in
between are painted by nobody. That gap walks down the body as the parts hand
over, and it sweeps across the court as the camera pans. **That is the shimmer.**

### Why three correct fixes did nothing

`sdb_present` replaces the native columns with `s_picture` — a sprite-free
background plus the flicker mod's whole captured list — **after**
`cyc_render_present` has already run the compositor and painted the margin into
the same buffer. Everything the compositor drew inside the native 256 is
therefore discarded. That is correct for its contract ("the native 256 win") and
fatal for a margin part that has to reach them.

So the seam could not be fixed in the compositor at all, and three attempts there
were each measured to be **0 differing pixels over 890 frames**:

- `right_band` from `rx0+256` to `rx0+252` — the machine's columns end at sx 251
  and the two differ by the 4-column OAM bias. Arithmetically exact, inert.
- An ownership map built by walking **OAM**. Also inert: `cyc_render_sprites`
  takes a `CycSpritePlaceFn` `place` callback and the flicker mod moves sprites
  onto their published list's positions, so OAM says where a sprite *is*, not
  where it was *painted*.
- The same map rebuilt from the machine's **rendered frame**. Inert for the
  reason above.

The shape is `mstan/SuperMarioBrosNESRecomp`'s — same engine, same width table
(426/560/854) — whose `docs/WIDESCREEN.md` gives the reason: "Managed enemies
are drawn as complete sprites across both old screen edges; their corresponding
native OAM slots are suppressed for presentation. **This avoids joining half a
host sprite to SMB's column-clipped native sprite.**" Here the native slot
cannot be suppressed — gate 2 wants the native block to stay the machine's — so
the host sprite is drawn *over* it instead, and only into columns the machine
left as background.

### The fix

`sdb_ws_build_owner(native)` marks each native pixel the machine actually filled,
by comparing the rendered frame against the background capture. `sdb_ws_margin_pass()`
then draws margin parts that **straddle the right edge** of the native block,
after the stamp.

Scoping it to the right seam is earned. Letting it repaint every part that merely
touched the block cost object 0 its head at frame 817 (ink 149 → 67 over rows
103–133) by painting the margin's copy over the machine's at a column the
reconcile had shifted, and `margin_track` went from 9 anomalies to 10. The left
seam is left entirely alone: columns 0–7 are the revealed band, which the
compositor owns by construction (`completes`).

A genuine pre-existing bug fell out: `sdb_margin_blit`'s behind-background
priority test used `(width - 256) / 2` — the *centred* native origin — which is
only correct when the compositor could not anchor. With the camera locked it must
be `rx0`. Same class as the anchor split, one function further down.

### Measured

890-frame windowed replay of the recorded vector, 21:9, camera locked:

| | before | after |
|---|---|---|
| object 3's head, frames 805–820 | 9 wrong | **2 wrong** |
| frames 809–811, 813–816 | flickering | **clean** |
| left-margin player, f=817 ink | 149 | **149** |
| object 0 region, f=813 | — | **0 differing px** |
| `margin_track` anomalies | 9 | 10 (the extra is trace-only, 0 px) |
| gate 2, native columns | 1242 px / 54 frames | **1242 px / 54 frames** |
| gate 8, mod-off = no catalog | — | **0 frames, 0 px** |

61 frames and 2299 pixels change against the baseline, ~98.6% of them inside the
native block. The guest is untouched: the margin hooks write no RAM and the pass
writes only the presentation canvas.

### What is still open

**Frames 808 and 812**, both `cam != scr`. Two approaches were tried and both
measured *worse* than leaving them:

- **Capture filter asking the draw site's question** — `csx = x + lag - 4`,
  `lost = csx < 0 || csx > 251`. Fixes 808 and 812, and costs a left-margin
  player 82 of 149 ink pixels at frame 817. The loss occurs on lag-zero frames as
  well as lagged ones, so it is the *timing* of the read and not the lag term:
  `cyc_render_line_scroll_x` is read mid-frame while the emitter runs, and the
  value the draw site uses is the one latched for the frame being presented.
- **Widening the capture** by one camera step (the delta takes 0, 4, 6, 7) and
  letting the draw site discard. 12 wrong frames against this build's 2, and it
  breaks 805–807, which are otherwise clean. Capturing more is **not** safe: the
  margin's copy of a part and the machine's sit a column apart after the
  reconcile, so the ownership test reads those columns as unowned and the margin
  paints a ghost.

### The cause of 808 and 812, found, and why the obvious fix is not shippable

Both frames have the same cause, and it is neither of the two above. The JSON
mirror settles it in one dump. At **trace frame 808** object 3 holds only its
four **right** halves (`sx` 261, 265, 264, 266) while the machine holds neither
half — and `sdbb` says why, on the line above:

```
sdbb f=808 boundary: obj 3 drops 8 parts (build 191 -> 192)
```

`$0617` restarting a list **wipes the object's captured parts**, and the game
rebuilds its list over *two* frames. At the boundary the object has been emitted
in the list that ended and not yet in the one starting. The four left halves were
wiped, and they were not re-captured because they sit **inside** the machine's
columns and so do not pass the capture filter — while the machine's own list was
not yet carrying them either. Neither side drew them.

**Leaving the boundary lazy and expiring parts on a TTL fixes it completely.**
Stamping each part with the frame it was last emitted in, not clearing the slot
on a build bump, and dropping parts not re-emitted within eight frames:

| | committed | lazy boundary |
|---|---|---|
| object 3's head, frames 805–820 | 2 wrong | **0 wrong** |
| `backstop` (per-object part limit) | 22 | **0** |
| `margin_track` | 9 | 9 |

The backstop falls because a slot is no longer emptied and refilled at every
boundary. Eight frames is measured, not chosen: the game leaves OAM alone for
three to six frames between builds, so anything older is not coming.

**And it is still not shippable, because of ghosts.** With the boundary lazy, a
part that stops being emitted survives alongside its replacement, and
`find-or-append` keys on an exact `(dx, row)` while the game's part offset moves
with the camera. The same logical part is re-emitted a pixel or two away each
build and **both** are kept — 125+ `ghost`s on object 8 alone, "two parts on row
160 at columns 289 and 290". That is the documented failure of keeping stale
parts, and it is worse than the two frames it buys.

So a part must be identified by its **row, within one pixel**. That is not the
band anchor §26 removed — that matched on *tile*, which every player shares, and
reported thousands of false hits; this matches on the row and reaches one pixel,
which cannot cross to a neighbour's part (parts of one character are eight apart).
Together the two are clean:

| | committed | lazy + row-tolerant |
|---|---|---|
| object 3's head, 805–820 | 2 wrong | **0 wrong** |
| `margin_track` | 9 (9 `shift`) | 10, **no ghosts** |
| `backstop` | 22 | **0** |
| gate 2, native columns | 1242 px / 54 frames | **1242 px / 54 frames** |
| left-margin player, f=817 ink | 149 | **152** |
| f=813 object 0 region | 0 px | **0 px** |

Every widescreen gate passes. **Gate 8 does not**: with the catalog all off the
new build puts **331 pixels on the stock 256 frame across 42 frames**, against a
reference it must equal. That is not a margin artefact and it is not explained:

- guest state is **byte-identical** over the same 890 frames (`--frame-log`,
  `cmp`), so it is not drift;
- `sdb_ws_margin_pass` is **never called** at stock width (instrumented, and the
  engine does not reach the compositor at 256), and its three consumers are the
  pass, `sdb_json_frame` (trace-only) and `sdb_ws_composite` (not called);
- adding the `long f` field alone, with no logic change, measures **0 px**, so it
  is not struct layout.

What is left is the hook path itself, and unexplained is left as unexplained. The
change was reverted rather than shipped: two frames are not worth a gate that
cannot be accounted for.

### A gate-8 trap worth more than the fix

Measuring that failure took five runs, and the first three were measuring the
wrong thing. **`[feature.values]` are applied whether or not the feature is
`enabled`.** A hand-written all-off `state.toml` carrying `aspect = "fit"` and
`camera = "locked"` still runs a **locked-camera, widescreen-configured** game —
`SDB_TRACE` confirms `camera locked` and the `sdbws` probe shows `s_ws_lock=1`,
`s_ws_aspect=4` with every feature `enabled = false`.

That is the same file AGENTS.md already warns about from the other side (values
attached to the wrong feature are silently dropped). Both halves are needed: the
wrong attachment drops the value, and a *correct* attachment applies it even
though the feature is off. **An all-off catalog must have `aspect = "stock"` and
`camera = "follow"` to be stock at all.** With that corrected, the committed
build measures 0 px against no-catalog, which is gate 8 properly established;
without it, the comparison is between two equally non-stock runs and passes
vacuously.

One thing found along the way is worth recording even though it did not ship:
`sdb_ws_margin_pass`'s only guard is `s_margin_on`, and the `--margin-sprites`
option **defaults on**, so it is 1 even with every feature off. Its correct gate
is the canvas width (`cw <= 256`) — no margin, no seam. Today that is unreachable
rather than harmless: the pass sits behind an early return in `sdb_present` that
the stock-width path takes first, so it is never called at 256. But the guard is
one refactor away from mattering, and nothing else in the function would stop it.

**Also open and predating this:** `backstop 22` on the reported capture — object 0
hitting the 24-part limit at frame 253, a gate-6 violation. The lazy boundary
above clears it, but that change is not shippable for the gate-8 reason.

### A conflict on the record

`mstan/SuperMarioBrosNESRecomp` measures the opposite of research.md §27 on the
scroll basis: "Reading the already-advanced game camera produced a one-frame
offset at the two joins while scrolling; the captured scroll removes that seam.
Measured: the old camera produced 143 misaligned frames; the captured-scroll
version produced none." §27 here measures captured scroll ≈75% correct on
camera-stepping frames against `$C3/$C4` ≈33%. They measure background alignment
at the join and this project measures sprite-object agreement, so they need not
contradict — but it is live, and the capture-filter attempt above is the one that
would have forced the choice.

## 30. Why the seam fix cannot ship: the follow-camera doubling

§29 ends with frames 808 and 812 open, their cause found, and a fix that works.
This section is why that fix does not ship. **It trades two frames for a player
drawn at twenty-four positions**, and it only showed up when someone played it
with the camera following.

### What it looked like

Reported from play, 2026-10-16, on a follow-camera capture (`tmp/follow1`):
player 1 running left is doubled. Not a gap this time — the seam artefact is
gone — a ghost silhouette down the player's left side and a hole in the torso.

Frame 958, cropped and differenced against the committed build, candidate on the
right: **808 differing pixels**, the magenta falling along the left edge and legs
of the body and pooling at the waist. The committed build at the same frame is a
single clean player. So it is not an older regression; the seam fix causes it.

### The cause, and it is not placement

Object 11 is player 1 (`id=01/00`). Its captured part count, per frame:

| | f950 | f954 | f956 | f958 | f962 |
|---|---|---|---|---|---|
| committed | 9 | 8 | 8 | 8 | 9 |
| the §29 fix | 17 | 17 | **24** | **24** | **24** |

Twenty-four is `SDB_MARGIN_PARTS`, the per-object cap, and the capture's
`backstop` fires 3 times — so parts are being *refused*, not just mis-placed.

The mechanism is the part identity, which is `(dx, row)`:

```
f=954   row 158: col -59  row 159: col -60      <- 1 px apart, correct
f=956   row 158: col -59  row 158: col -59      <- impossible: same row
        row 156: col -59  row 158: col -59
f=958   row 156: col -67  row 158: col -67      <- identical column AND tile
```

A **running** player's animation moves the ROW between builds. The replacement
therefore cannot match the part it replaces, appends instead, and the old row-part
survives the eight-frame expiry. The object accumulates until it is pinned at the
cap. `dupes.py` names each one: `obj 11 col -67: row 156/158 tiles 01/01` — the
same sprite at two rows.

### Both knobs are column thresholds, and row drift is not a column drift

`SDB_PART_TTL` and `SDB_PART_NEAR` were each tuned until they worked, and each
works only while the object is still:

| | locked capture | follow capture |
|---|---|---|
| stacked parts, committed | 367 | 37 |
| stacked parts, §29 fix | **3114** | **1668** |

The `NEAR = 7` derivation in §29 is **sound and its scope is narrow**: every
same-row pair the game emits is exactly 8 apart (15717 pairs, one histogram
bucket), so a 7-column window reaches every stale copy and cannot reach a
distinct part. That is a true statement about *column* spacing, measured on a
capture whose objects were largely stationary. Nothing in it bounds the ROW axis,
and for a moving player that is the axis that drifts. **A correct measurement
carried one step too far is the failure, not a wrong constant** — the 1-pixel
version and the 7-pixel version both "worked" and both ship the same bug at a
different magnitude.

So there is no threshold that has both. The lazy boundary is the wrong mechanism
for the seam and cannot be rescued by tuning it.

### Four instruments that were wrong, and one that was not

Every one of these passed on a build that was visibly broken, which is the whole
point of listing them.

1. **`margin_track.py`'s `ghost`** fires only for parts *within one pixel* on a
   row. The locked-mode doubling sat 2–7 px away and was called clean.
2. **`dupes.py`, first version**, widened that to 8 but kept the *same-row*
   restriction, so it reported **0 drifting on the build that doubles here** — the
   artefact is the mirror image, same column and different rows. It now checks
   both shapes and fails the broken build on both.
3. **Part-count gates** were green throughout (`backstop 0` on the locked
   capture, peak and drawn all plausible). Only the per-object count against the
   game's own 8 exposed it.
4. **A recorded-vector replay** said candidate and committed were **byte-identical
   in follow mode — 0 frames, 0 pixels** over 1263 frames. The played capture of
   the same configuration differs on **267 frames / 243912 px**. This is the
   windowed/headless divergence of §29's close, and it is worse than that note
   implies: it is not that a replay is *nearly* faithful, it is that it is
   confidently wrong about the exact thing under test. **Analyse a capture where
   it was made, and do not use a replay to decide whether a margin change is
   safe.**
5. The one instrument that did work: **the per-object part count against the
   game's own**. Eight is a character. Twenty-four is a slot that filled up.

### A host detail worth one line

`NESRECOMP_NO_LAUNCHER=1` skips the launcher, which is what applies
`[feature.values]` — so `aspect` and `camera` must be passed as CLI flags. A
`state.toml` saying `camera = "follow"` was silently ignored and the trace
reported `camera locked`, which is a wasted run if you believe the file.
