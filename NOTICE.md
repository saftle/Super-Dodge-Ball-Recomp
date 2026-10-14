# Super Dodgeball Recomp

## Attribution

**Super Dodge Ball recompilation and integration work**

- Copyright 2026 Saftle
- Licensed under the PolyForm Noncommercial License 1.0.0, see [LICENSE.md](LICENSE.md)

**"Remove Character Flicker" mod** (`src/sdb_host_extras.c`,
`mods/packages/super-dodgeball.character-flicker/`)

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
the distribution model used by other recompilation projects. No prebuilt binary is
distributed here: the generated C, the built executable and the build directory are
gitignored, and the game's code and assets are not included in any form.

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
