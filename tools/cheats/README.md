# tools/cheats/

Analysis behind the cheat work (`src/sdb_cheats.c`, `cheats/`, and the
engine's ROM read substitution — upstream PR #69).

## `ramcensus.py`

Which RAM bytes behave like a counter, over a `--frame-log`.

There is no shortcut to "the byte that advances the stage": no vector in
`tas/vectors/` crosses a stage boundary — checked, not assumed — so the address
has to come out of a recorded run. A `--frame-log` already carries CPU RAM for
every frame, so the hard part is not collecting it but reading 2048 columns of
2600 rows without eyeballing a diff. This scores every byte and prints the
shortlist.

```
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 2600 \
  --input tas/vectors/sdb_sweep.input --ram-init zeros \
  --frame-log tmp/cheats/log/sweep.bin --frame-log-frames 0:2599
python3 tools/cheats/ramcensus.py tmp/cheats/log/sweep.bin --top 20
```

**What counts as a counter**, and why the obvious test is not the one used: not
"changed" — RAM is full of bytes that change (timers, health bars, cursors,
animation). A stage index or a score takes *small* values, changes *rarely*, and
very often moves *by one*. The first two tests alone are not enough: a two-valued
flag that toggles scores well on both, and only the by-one test throws it out.
So the ranking is built so a toggle cannot win it, and the by-one fraction is
printed for every entry so you can see the reasoning rather than trust a score.

**What it is not.** A shortlist, not an answer. A per-shot counter looks
identical from here to a stage counter, and one that never wraps in the window is
invisible to a "small value set" test. `--all` drops the ranking and lists
everything that moves at all, which is the honest fallback when the shortlist
comes back empty.

Exit 0 always: this is an instrument, not a gate. It reads RAM and nothing else,
so a run of it says what these bytes did and nothing about the machine.

## `gg_selftest.py`

The Game Genie decoder against the **published scheme**, not against itself.

`src/sdb_cheats.c` implements the Code Masters / Galoob device's code encoding
from the technical notes at <https://tuxnes.sourceforge.net/gamegenie.html>,
rather than from any emulator's source — deliberately, since this project is
PolyForm Noncommercial and the obvious reference implementations are GPL. That
choice is only worth something if the implementation demonstrably matches the
*specification*, so this drives the real binary over TCP and asks the real C
decoder what it made of three codes: the notes' own two worked examples
(`GOSSIP` -> `$D1DD`/`$14`, `ZEXPYGLA` -> `$94A7`/`$02`/compare `$03`) and
`NYGEAE` for `$8048` = `$FF`, which is independently cross-checked by running it
against the raw `8048:FF` spelling and comparing a full run.

A Python copy of the algorithm would pass even if the C had drifted, which is
why it asks the binary instead.

Start a game with the three codes in a `.cht`, then:

    python3 tools/cheats/gg_selftest.py 9099

Exit 0 if all three match, 1 otherwise.

## `toggle_probe.py`

Arms a RAM freeze over TCP, checks the byte is being held, disarms it, and
checks the byte came back. The disarm is the half that shipped broken once, and
the in-game menu is a mouse-driven widget that no script can drive, so this is
the only automated coverage of it.

## `ram_tap.py`

Samples CPU RAM from a **running windowed session**, one CSV row per sample. See
its docstring for why: the windowed host has no `--frame-log`, and a headless
replay of a recorded vector is a different machine from frame 4, so a diff taken
there answers the wrong question.

## `ui_shot.py`

Opens and drives the in-game menu over the host's TCP commands and screenshots
what the window actually presents (`screenshot ... layer=ui`, which is the only
read-back that sees the menu). This is how the cheat section was confirmed
present rather than assumed.