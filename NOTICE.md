# Super Dodge Ball Recomp

## Attribution

**Super Dodge Ball recompilation and integration work**

- Copyright 2026 Saftle
- Licensed under the PolyForm Noncommercial License 1.0.0, see [LICENSE.md](LICENSE.md)

**"Remove Character Flicker" mod** (`src/sdb_host_extras.c`,
`mods/packages/super-dodge-ball.character-flicker/`)

- Copyright 2026 Saftle
- Licensed under the PolyForm Noncommercial License 1.0.0, the same terms as
  the rest of this project — see [LICENSE.md](LICENSE.md)
- Not separately licensed, and the package under `mods/` cannot be. A `.nesmod`
  archive carries data only and may not contain native code, so the package is
  just a manifest declaring a feature whose implementation is
  `src/sdb_host_extras.c`, statically linked into `nes_game` and selected by a
  stable id. Licensing the manifest differently from the code it points at would
  imply reusable terms the implementation does not carry

**NESRecomp**, which this project is built on

- Copyright 2026 Matthew Stanley and contributors
- <https://github.com/mstan/nesrecomp>
- Licensed under the PolyForm Noncommercial License 1.0.0

`external/nesrecomp` is a separate repository included as a submodule and
carries its own license and third-party attribution. `external/mesence`
(MesenCE oracle, submodule pointer only — binary/data fetched at setup time,
never committed) likewise carries its own license; see that repository.

**recomp-ui**, the launcher and in-game menu for the windowed build

- Copyright 2026 Matthew Stanley and contributors
- <https://github.com/RetroPortingToolKit/recomp-ui>
- Licensed under the MIT License
- Pinned as a submodule at `external/recomp-ui/` (commit `41c88ff`); its own LICENSE and
  third-party attribution (Dear ImGui, stb_*, tinyfiledialogs — each retaining
  its own license) travel with that checkout

Built from source into `nes_game`. There is no prebuilt recomp-ui binary and
none is redistributed here.

**SDL2** is a system dependency resolved at configure time
(`-DSDL2_DIR` pointing at the system SDL2 CMake config for the cycle build)
and is not shipped here. SDL2 itself is zlib-licensed,
Copyright Sam Lantinga and contributors.

**Archived runner patches** (`patches/archived/*.patch`) targeted the retired
legacy runner and apply to nothing; they are used under the same PolyForm
Noncommercial terms. See `patches/README.md`.

**Engine changes this project contributed.** Five upstream pull requests in
`mstan/nesrecomp` came from this project. All of them live in
`external/nesrecomp` and carry that repository's license and attribution — the
same terms as the rest of the engine.

- **PR #21** — *runner: Add NES-to-runner PPUMASK bit translation layer*
  (`nes_ppumask_to_runner()`, `g_ppumask_translate`). Opt-in: existing titles
  keep the original behaviour unchanged. Still upstream.
- **PR #22** — *runner: Force rendering enabled when `g_ppumask_translate` is
  set*. **Upstream later revised this** (commit `a671d53`, 2026-10-18,
  *"Preserve disabled PPUMASK rendering bits in compatibility mode"*), which
  removed the forced `g_ppumask |= 0x1E`. The translation layer from #21
  remains; the forcing does not.
- **PR #66** — *Allow PPU overclocking on MMC1 carts*
- **PR #67** — *Add a VSync display setting to the windowed cycle host*
- **PR #69** — *cyc: a ROM read override, so a trusted mod can patch what the
  CPU reads* (the primitive Game Genie cheat codes need)

#21 and #22 touch `runner/src/runtime.c`, the **legacy function-level runner**,
which this project retired in 2026-10 — so neither reaches the cycle backend
this project plays on. The other three were originally carried here as local
submodule patches (006, 007, 008) and are now upstream; `patches/README.md`
records what upstream changed while taking them.

**libretro-database**, the cheat-code collection
<https://github.com/libretro/libretro-database>. Its
`Super Dodge Ball (USA).cht` supplied the thirteen entries in
`cheats/Super Dodge Ball (USA).cht`; ours also carries codes written for this
project, and each entry says which is which. **The upstream project's licence is
CC BY-SA 4.0** — its only licence file, and not GPL — recorded here as a fact
about *that project*. It is not a claim over our file: a cheat code is a short
functional instruction ("hold this address at this value") with no expressive
content of its own, and the file is ours in part. Nothing else from that
repository is used or re-hosted, and no file of theirs is imported into `src/`.

Its own project notes the cheat files are auto-generated and untested, which is
why the entries here are labelled unverified rather than presented as working
codes.

**Cheat formats, decoders, and why no GPL source is in the tree.** The obvious
references for this work are GPL — RetroArch (`cheat_manager.c`, GPL-3.0) and
libretro-fceumm (`src/cheat.c`, GPL-2.0) — and this project is PolyForm
Noncommercial, so a transcribed GPL routine would be a problem for the whole
repository rather than for one file. `src/sdb_cheats.c` therefore implements
published *specifications*, not those implementations:

- the **Game Genie** decoder implements the Code Masters / Galoob **device's code
  encoding**, per the published technical notes at
  <https://tuxnes.sourceforge.net/gamegenie.html> (the alphabet and the three
  formulas are quoted in the source, along with their provenance);
- the **Pro Action Replay / GameShark** decoder implements that code layout's own
  convention of discarding the leading byte;
- the **`.cht` parser** implements the documented RetroArch file layout.

A file format and a code encoding are not copyrightable, and implementing a
published scheme is not a derivative work of whichever project also implements
it. Behaviour is verified rather than assumed: the Game Genie decoder's output is
compared against the technical notes' own worked examples (`GOSSIP` →
`$D1DD`/`$14`, `ZEXPYGLA` → `$94A7`/`$02`/`$03`), and against the raw `AAAA:VV`
spelling for the same address, over a full run.

## Game content and rights holders

This repository contains no Super Dodge Ball ROM and no assets extracted from
one. The recompilation configuration describes addresses and layout only.
Super Dodge Ball and all related material belong to their respective rights
holders. You must supply your own legally obtained copy of the ROM.

Super Dodge Ball (熱血高校ドッジボール部, *Nekketsu Kōkō Dodgeball Bu*) was developed
and originally published by Technōs Japan Corp., with CSG Imagesoft publishing the
North American release. Rights later passed to entities including Million and
Arc System Works. *Kunio-kun*, *Renegade* and the related titles are covered by the
same rights.

**This project is not affiliated with, endorsed by, sponsored by, or approved by
Technōs Japan, CSG Imagesoft, Million, Arc System Works, Miracle Kidz, Nintendo,
or any other company holding rights in the game, nor by any of their parents,
subsidiaries, affiliates, successors or assignees.** It is an independent
non-commercial fan project. Trade, brand and product names are used only to
identify the hardware and software recompiled here and to credit the tools it
builds on, and remain the property of their respective owners. *Super Dodge Ball*
is a trademark of its respective owner.

The license in this repository covers the source code here. It grants no rights
to the game itself.

A build of this project produces a statically recompiled — machine-translated —
build of the original 6502 code. That is inherent to static recompilation and is
the distribution model used by other recompilation projects. **The repository
itself carries no prebuilt binary**: the generated C, the built executable and
the build directory are gitignored, and the game's code and assets are not
committed in any form.

## What a release contains, and what it does not

The releases page publishes built binaries. Each one carries:

| | |
|---|---|
| the **recompiled game code** | the cartridge's 6502 instructions, translated to C and compiled — this is what a recompilation *is*, and it is the same model as every other recompilation project |
| the **launcher assets, the mod manifests and the cheat file** | ours, or third-party code under its own licence (see above) |
| **no ROM** | the packaging refuses to write one: a `.nes` in the stage fails the build |
| **no game assets** | no graphics, no music, no text, no palettes |

The point of that last row is what makes the distribution defensible rather
than merely customary: **the release cannot substitute for the cartridge.** The
graphics, the music and the text are read out of the player's own ROM while
the game runs — a release with the ROM's CHR zeroed renders a blank screen.
There is nothing in the artifact that plays Super Dodge Ball without a copy of
the original, and no way to obtain one from us.

**Where the ROM goes, and where it does not.** The release is built on the
maintainer's own machine, from a copy of the cartridge they own. The
cartridge is read from `roms/`, checked against the cartridge's identity,
and **never leaves that machine** — it is not committed, not stored in a
secret store, not sent to a CI runner, not put in a build artifact and not
in any published file. There is no release workflow, and there is no secret
to unlock. A rights holder asking where a copy of the game might be found
in this project's infrastructure has the answer: nowhere.

**The translation's source form is not published.** The build generates the
translated game as C (`game_cyc*.c`, 8 MB) and that file is where the game's
*data* lives in readable form — the codegen folds constant ROM bytes into
immediates, so roughly 28% of the cartridge's PRG appears in it as literal
constants alongside the translated instructions. The release carries the
compiled translation only; the source form stays in the build tree, is
gitignored, and is not published in any artifact.

That is a deliberate line, and the reason is not squeamishness. The
*verification* is ours and is published — the tools, the frame-log comparisons,
the byte-exact diffs against MesenCE — so a reader can check that this is the
same game, translated honestly, without ever seeing a line of the
translation. The translation itself is the game's, and the less of it that
travels, the better. (Other recompilation projects do commit their generated
C; that is their call, not a precedent that makes it risk-free.)

The project is independent, non-commercial and unaffiliated. Trade and brand
names are used nominatively, to identify the hardware and software being
recompiled and to credit the tools this is built on.

## Requests from rights holders

If you hold rights in anything covered here and want it taken down, open an
issue on the repository (or contact the maintainer directly through the
repository's own contacts). Verified requests from a rights holder are
honoured: the release is withdrawn and the affected material removed, without
argument and without asking for anything in return. This project exists
because someone loved a game; nothing about it is worth a fight with the
people who made it.

## AI disclosure

This project was developed with the assistance of AI coding agents, which were
used for the game-specific mods, the analysis and verification tooling, the mod
manifests, and substantial parts of the documentation. The human maintainer
directs the work, reviews the output, decides what ships, and is responsible for
the contents of this repository.

For the full statement, and for what that means for verification here, see the
[AI disclosure in README.md](README.md#ai-disclosure).

The upstream projects this builds on — NESRecomp, recomp-ui, MesenCE and the RPTK
documentation — are not covered by this statement. Each carries its own authorship,
its own licensing and its own account of how it was built, published by its own
authors; the attribution is recorded above.
