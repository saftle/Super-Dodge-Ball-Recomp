# Research Notes — Super Dodgeball NES Recompilation

> Living notes. RetroPortingToolKit docs reference: https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs

## Summary of Findings

Cycle backend is the only play path; legacy runner retired (§§18–19 keep its game knowledge).
Team-select verified byte-exact vs Mesen from boot (RAM/OAM/palette/CIRAM, 400-frame TAS); blargg
cpu tier 25/25.

**Character flicker: SOLVED, twice** (§2, §2.1). Neither cause was emulator sprite-limit
removal; the game throttles its own sprites twice over.

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
`super-dodgeball.ppu-overclock` (§23) and `super-dodgeball.widescreen` (§25, which also carries
an `overscan` feature). Only the flicker fix changes guest behaviour, and only in its culling
half; overclocking changes console *timing*; **widescreen changes nothing at all** — it reads
the nametable the frame already wrote and re-presents the picture.

**Widescreen: the margins are already in the nametable** (§25). Presenting 16:9 / 21:9 / 32:9
(426 / 560 / 854 columns) needed **no engine patch** — the widths and the compositor API are
already upstream. The court's world is a 512-pixel looping stadium that is byte-identical across
a whole match, so margin pixels are resolved the way the PPU resolves them rather than
remembered the way wideNES does; wideNES can only ever show what the guest already drew, and
cannot draw sprites at all. Reconstruction agrees with the machine's own background capture to
1-2 px in 61440, and the stock 256-column centre is copied back so it stays pixel-identical.
Overscan is measured, not assumed: the left 8 columns are blank in all 2112 measured frames,
while the right 8 carry the court and the top/bottom bands are used by a transition frame.

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

**No Super Dodgeball disassembly exists publicly.** The SNES port by Rumbleminze is the closest reference.

---

## 9. Related Resources

- NESdev Wiki: https://www.nesdev.org/wiki/Tools
- RetroReversing (NES): https://www.retroreversing.com/nes
- NESdev Forum: https://forums.nesdev.org/
- Romhacking.net game page: https://www.romhacking.net/games/814
- StrategyWiki: https://strategywiki.org/wiki/Super_Dodge_Ball_(NES)
- SNES Port repo: https://github.com/rumbleminze/super-super-dodgeball
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
- **Config**: `src/game.toml` carries the display name plus ten `[[mod_function_hook]]` sites and a `cycle_seed_file`; no `extra_func`, no `force_interp`, no discovery tuning needed for the stock ROM
- **Mods**: `super-dodgeball.character-flicker` and `super-dodgeball.ppu-overclock`, both off by default; packages preloaded into `build-cycle/mods/` at build time; window picks them up via the launcher, headless needs `--mods-root build-cycle/mods`. VSync is a launcher Graphics setting (patch 007), not a mod. Reproduction vector for both flicker fixes: `tas/vectors/sdb_idle.input`
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
and of the three mods only the truncation half is presentation-only (§2, §22, §25).
Schedulable
input (`<frame> NAMES` absolute state, same bit map).

- **Mods**: all three are opt-in (off by default). They never appear in
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
- Windowed == headless (2026-10-02): `--virtual-pad` playback vs
  `--input` headless, same TAS/schedule/RAM-init, pixel-identical
  (`--present-out` vs `--screenshot`, 256x240) at frames 1/101/…/501.
  The window adds no divergence of its own; the only input-path
  difference anywhere is 9 opposing-dpad frames (first at 5619) the
  windowed bindings cancel to neutral.
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

## 25. Widescreen and overscan: the margins are already in the nametable

Goal: present 16:9 / 21:9 / 32:9, and offer the "remove overscan" option
emulators ship. **No engine patch is needed** — unlike §23, where the feature
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

**The collision that had to be resolved first:** the character-flicker mod's
`present()` returned a non-NULL 256x240 buffer, and both the window
(`cyc_sdl.c:306`) and `--present-out` (`cyc_host.c:886`) take `present()` over
`cyc_render_present()`. A compositor would have been silently bypassed whenever
flicker was on. `present()` now always asks `cyc_render_present()` first — it is
the only place the compositor runs and the only place the width is decided — and
returns the engine's buffer untouched when there is nothing else to do.

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
guest calls, no world cache, no `[[mod_function_hook]]` sites, no seeds.

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
`overscan` feature instead draws the real background in those columns and leaves
them alone when copying the machine's picture over the centre. The whole canvas
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
the backdrop, which for this game is black and is already the colour of the
world's own outermost columns (`C0`), so the bar continues the court's real edge
rather than stopping at it. At 21:9 and 32:9, on camera positions that put the
world's edge inside the view, this leaves a black band at that end. That is the
honest limit: the stadium is 512 pixels wide and a 854-column frame does not fit
inside it.

### One more trap, in the mod state file

Hand-writing `state.toml` to test the package: **`[feature.values]` binds to the
`[[feature]]` block declared immediately above it.** With two features in the
package, a values table written after `overscan` but meant for `widescreen` is
silently dropped — `overscan` has no `aspect` option, so nothing is written
back and `widescreen` falls back to its manifest default (`fit`). Headless,
`fit` resolves to the stock aspect, the width stays 256, and the engine does not
call the compositor at 256 — so the feature appears to do nothing at all, with
no error anywhere. The launcher never hits this because it writes the file; only
a hand-built test file does.

### Not sprites in the margins

The flicker mod already captures every sprite the game emits, and the object
display list at `$067C` is camera-invariant across a pan (measured: the records
at `$0618` do not change while OAM X shifts by 7 px with the court), so
off-screen players are in principle placeable from memory — which wideNES
structurally cannot do. That is **not** implemented. OAM X is a byte and
camera-relative, so the sprite list alone cannot place anything outside the
viewport; it needs the display records' world-space X, which is not yet
identified. Background only for now, and the seam check plus `cyc_render_sprites`
are the machinery if it is picked up.
