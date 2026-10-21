# cheats/

`Super Dodge Ball (USA).cht` is a **RetroArch / libretro cheat file**. It is
copied beside `nes_game` at build time and loaded when the
`super-dodge-ball.cheats` feature is on, or with `--cheats <path>`.

## What the format is, and where it came from

Nothing here is invented. The file layout (`cheats = N`, `cheatN_desc`,
`cheatN_code`, `cheatN_enable`) and the four code spellings are the ones
RetroArch and the NES libretro cores settle on between them. That is what makes
the format worth supporting: a `.cht` from **libretro-database**
loads here unchanged, and a `.cht` that works in FCEUmm works here.

| spelling | meaning | mechanism here |
|---|---|---|
| `AAAA:VV` | freeze `AAAA` to `VV` | `cyc_mod_poke`, every frame |
| `AAAA?CC:VV` | write `VV` while `AAAA` still reads `CC` | `cyc_mod_peek` + poke |
| `AAAA:VV` at `$8000+` | substitute `VV` for the byte read at `AAAA` | ROM read override |
| 8 hex digits | Pro Action Replay / GameShark; **leading byte discarded** | as above |
| 6 or 8 chars | Game Genie (`APZLGITYEOXUKSVN`); 8-char form carries a compare | ROM read override |

Several codes may share one line, joined by any of `+ , ; . _` or a space —
`--cheat 058B:FF;0553:FF` is one cheat string holding two codes.

## Every entry here ships disabled

That is how libretro-database ships its files: a cheat file is a **menu**, not a
decision. A player picks. Nothing is armed until you set `enable = true`, or
pass the code on the command line, so ticking the feature in the Mods screen and
seeing no effect is a true report rather than a broken toggle.

## What a Game Genie code cannot do here

A recompiled machine has already turned some bytes into C. An **opcode** is a
`goto`; an **address operand** is a compile-time constant
(`recompiler/src/cyc_codegen.c`). Those bytes are not on the bus any more, so no
read hook can reach them. Immediates and data operands *are* read at run time
and do respond — for this ROM, 3327 folded operand reads and 1405 run-time reads
both reach the substitution, which is why the engine hooks both sites rather
than one.

So a substitution changes the constants the game compares against and the data it
reads out of a ROM table. It cannot retarget an instruction. `src/sdb_cheats.c`
has the long version; the engine half is upstream now
(`cyc_mod_set_rom_read_override()`, PR #69) — see `runner/cyc/cyc_mod.h`.

## Provenance

The thirteen entries came from
[libretro-database](https://github.com/libretro/libretro-database)'s
`cht/Nintendo - Nintendo Entertainment System/Super Dodge Ball (USA).cht`, with
all thirteen still disabled. They are unverified: that project's own notes call
the files auto-generated and untested, and four of these are labelled
"Possibility 1–4" upstream. They are here to be measured, not because they are
known to work — see `docs/research.md` for what was measured.

Codes we write go below those, with a note saying which are which, so the origin
of each entry stays visible as this file grows. No licence claim is made over
the codes themselves: a cheat code is a short functional instruction — "hold this
address at this value" — with no expressive content of its own. `NOTICE.md`
records the upstream project's licence as a fact about *them*, not as a claim
about this file, which will be ours in part.

**The code that reads this file is ours, and implements published
specifications rather than GPL source.** The Game Genie decoder follows the
Code Masters / Galoob device's code encoding as documented at
<https://tuxnes.sourceforge.net/gamegenie.html>; the Pro Action Replay /
GameShark decoder follows that layout's own convention; the `.cht` parser
follows the documented file layout. RetroArch (`GPL-3.0`) and libretro-fceumm
(`GPL-2.0`) implement the same formats, and transcribing either would be a
copyleft problem for the whole repository, so neither was. The decoder is
checked against the technical notes' own worked examples by
`tools/cheats/gg_selftest.py`, which asks the real binary rather than a copy.

Any code we add is ours, and carries the same provenance note in this file.