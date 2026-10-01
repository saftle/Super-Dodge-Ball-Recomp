# Super Dodgeball Recomp

## Attribution

**Super Dodge Ball recompilation and integration work**

- Copyright 2026 Saftle
- Licensed under the PolyForm Noncommercial License 1.0.0, see [LICENSE.md](LICENSE.md)

**NESRecomp**, which this project is built on

- Copyright 2026 Matthew Stanley and contributors
- <https://github.com/mstan/nesrecomp>
- Licensed under the PolyForm Noncommercial License 1.0.0

`external/nesrecomp` is a separate repository included as a submodule and
carries its own license and third-party attribution. `external/mesence`
(MesenCE oracle, submodule pointer only — binary/data fetched at setup time,
never committed) likewise carries its own license; see that repository.

**SDL2** is a system dependency resolved at configure time
(`find_package(SDL2)` in `src/CMakeLists.txt`) and is not shipped here.
`src/third_party/SDL2/`, if present on a machine, is a local gitignored
build artifact only — never committed. SDL2 itself is zlib-licensed,
Copyright Sam Lantinga and contributors.

**Runner patches** (`patches/*.patch`) target the NESRecomp runner above and
are used under the same PolyForm Noncommercial terms.

## Game content

This repository contains no Super Dodge Ball ROM and no assets extracted from
one. The recompilation configuration describes addresses and layout only.
Super Dodge Ball and all related material belong to their respective rights
holders. You must supply your own legally obtained copy of the ROM.

The license in this repository covers the source code here. It grants no rights
to the game itself.
