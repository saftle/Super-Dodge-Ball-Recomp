# Active patches

Patches in `patches/*.patch` are applied to the NESRecomp submodule by
`setup.sh`, in filename order, after every submodule checkout. They exist so
the submodule tree can stay pinned to pristine `origin/master` while a change
that still needs upstreaming is usable here. `patches/archived/` holds dormant
ones that `setup.sh` deliberately ignores — see `archived/archived-patches.md`.

Every active patch must be **strictly additive**: it may enable something that
was refused, but it must not change behaviour for a game that did not ask for
it. These are meant to become upstream PRs, so a reviewer should be able to
read the diff and see that no existing title moves.

## 008_rom_read_substitution.patch (active 2026-10-17)

**What:** `cyc_mod_set_rom_read_override(addr, value)` and a table consulted on
every ROM-space CPU read. `hw_rom_read_override()` is called from `hw_bus_read()`
and from `hw_read_rom()`.

**Why written:** a Game Genie code changes the *value* the CPU reads at a ROM
address — that is how it patches a game's own code without rewriting the image.
The cycle backend could not do that at all: `cyc_mod_peek()` reads *without*
intercepting, and `hw_code_watch` sees stores only. So a game that wanted to load
a `.cht` file had no way to honour the Game Genie codes in it.

**Two sites, and one of them is the whole reason.** `hw_bus_read()` covers a
byte the recompiler left computed at run time; `hw_read_rom()` covers one it
folded to a constant. Over Super Dodge Ball's generated cycle code that is **1405
`cpu_read()` against 3327 `cpu_read_rom()`** — 70% of PRG reads are folded, so a
hook on `hw_bus_read()` alone would be blind to most of the ROM and would look
like it worked. Both sites already called `hw_cart_cpu_read_snoop()`, which is why
these are the two lines to touch.

**What it cannot reach, and why no read hook ever could:** what the recompiler
has already turned into C. An opcode is a `goto`, an address operand is a
compile-time constant (`recompiler/src/cyc_codegen.c`). So a substitution changes
the constants a game compares against and the data it reads out of a ROM table,
and cannot retarget an instruction. Retargeting needs a codegen change, not this.

**The patch's own message names no file in this repository.** It is written to
stand alone for an upstream reviewer, so everything it cites
(`cyc_mod_peek`, `hw_code_watch`, `hw_bus_read`, `hw_read_rom`,
`hw_cart_cpu_read_snoop`, `cyc_mod_set_ppu_write_hook`,
`recompiler/src/cyc_codegen.c`) exists in the submodule, and its verification is
described against the API rather than against this project's CLI flags or
vectors. Anything game-specific lives here instead — which is why this entry
duplicates the detail, deliberately.

**Blast radius: none for any game without a cheat.**
`cyc_mod_rom_read_overrides` is NULL until something calls the setter, and
`hw_rom_read_override()` returns on it, so the added cost on the hottest path in
the machine is one load and a branch. Overrides are host policy, not machine
state, and are not serialized into a save state — the same position as
`cyc_mod_set_ppu_write_hook`, and the right one: a state records the machine, and
a cheat applies to whatever machine is loaded.

**Verified** (in the game that needed it; `tools/cheats/README.md`, `src/sdb_cheats.c`):
- cheats **off** is byte-identical to the build before this patch — 600/600
  frames over `sdb_throw.input`, `cycles=17865922 native_cycles=13225160
  (74.0%)`, every frame's trace/mem/PC/register hash equal, re-checked after
  rebuilding the tree from pristine + 006 + 008.
- the RAM path lands exactly: `--cheat 058B:FF` leaves `$058B = $FF` at frame
  900, `--cheat 0019:8F` leaves `$0019 = $8F`, with 110 downstream bytes differing
  because the guest then computes something different.
- the substitution reaches a **folded** read: `--cheat 8048:FF` changes guest
  state against a stock control over the same 300 frames.
- the decoder agrees with the raw spelling: the Game Genie code for `$8048=$FF`
  gives output byte-identical to `--cheat 8048:FF`.

The substitution is applied *after* `hw_cart_cpu_read_snoop()`, so a cheat wins
over the board. A cheat the mapper can undo is not a cheat.

**Afterlife: propose upstream.** Cheat support is not game-specific and the
API is deliberately game-agnostic. Upstream would likely also want the compare
byte honoured per-read rather than once at arm time, and a way to reach a folded
*opcode* — but the latter is a codegen question, not a bus one.

## 007_window_vsync.patch (active 2026-10-12)

**What:** VSync as a launcher Display setting. `CycSettings` gains `[Display]
VSync` (default off, persisted in `config.ini`); `cyc_ui_launcher.c` sets
`gi.has_vsync = 1` — lighting up recomp-ui's existing Graphics row, the same
one PSX uses — and transfers the value both ways (Adaptive counts as on:
SDL2 has no adaptive swap API on the 2D renderer); `cyc_sdl.c` builds its
renderer with `SDL_RENDERER_PRESENTVSYNC` when set, falling back to tearing
rather than dying if the driver refuses.

**Why written:** stock pacing presents on an `SDL_Delay` + busy-wait timer
with no vsync, so presents jitter and the 60.0988 Hz guest drifts against a
fixed-60 Hz panel — choppiness no guest speedup fixes. Vsync paces presents
to the display's refresh at any rate (60/120/144 Hz); the guest keeps its own
clock, so this is presentation only.

**Why a setting, not the mod it started as:** vsync is host-global, not
game-specific — a mod would make every NESRecomp title reinvent it, and the
launcher row, the `has_vsync` gate and the 1/2/3 encoding already exist
upstream waiting for a console to opt in. The first version *was* a
`super-dodgeball.vsync` mod (activation plugin + renderer rebuild on
runtime toggles); it worked, but the in-game menu has no vsync row and
cannot gain one without editing the pinned recomp-ui, so the setting won
and the mod was deleted.

**Why safe:** default off, so `renderer_flags()` equals the old
`SDL_RENDERER_ACCELERATED` and the launcher row shows Off unless the player
opts in. Guest timing, save states and headless (which never reaches the
renderer code) are untouched.

**Three traps worth recording:**
- SDL2 allows one renderer per window: the mod-era rebuild path created the
  new renderer before destroying the old and failed every toggle with
  "Renderer already associated with window" (plus, the menu holds textures
  on the old renderer, so it needs a shutdown/reinit around the swap).
  Deleted with the mod; the setting applies once at startup.
- A `state.toml` entry for a deleted mod is not inert: boot refuses with
  "The executable does not provide this trusted plugin" plus "Resolve the
  highlighted mod conflicts", and the game never starts. The vsync entries
  were removed by hand; the launcher drops unknown entries on its next
  commit.
- Deleting a mod package from `mods/packages/` does not remove it from the
  build: the post-build step is `copy_directory`, which only adds, so the
  deleted mod keeps showing in the Mods screen until its
  `build-cycle/mods/packages/<id>/` dir is removed by hand.

**Verified:** `cmake --build build-cycle` clean; windowed `--exit-after 60`
under xvfb exit 0 both ways — stock config prints no vsync line,
`VSync = 1` in `config.ini` prints `vsync on: presenting at display
refresh`, and the launcher path is confirmed on a real display (Graphics →
VSync → Play prints the same line).

**Afterlife: propose upstream.** Any game can opt in with one
`has_vsync` line; the request API is deliberately game-agnostic.

## 006_ppu_overclock_mmc1.patch (active 2026-10-12)

**What:** one line. `cyc_extra_scanlines_supported()` returns
`hw_cart.mapper == 1 || hw_cart.mapper == 4` instead of `hw_cart.mapper == 4`.

**Why written:** the cycle backend already implements MesenCE's PPU
overclocking exactly — `cyc_set_extra_scanlines()` holds the PPU at scanline
240 for N extra blank lines before vblank/NMI and pauses the APU across them
(`runner/cyc/PROJECTS.md`; the same clock policy as Mesen2's
`NesPpu::UpdateApuStatus`). Upstream scopes it to MMC3. Super Dodge Ball is
mapper 1, so `--extra-scanlines` refused with *"requires an MMC3 cartridge"* and
the `super-dodgeball.ppu-overclock` mod could not arm.

**Why MMC1 is safe:** the added lines are blank (rendering is off above scanline
239) and the APU is paused, so they are inert for any mapper whose IRQ is not
clocked by the PPU. In this engine only MMC3 counts PPU A12 rises
(`mmc3_clock_irq`), which is why it is the one board the extra lines could
disturb. MMC1 triggers its IRQ from writes to `$8000`/`$8001` bit 7
(`mmc1_write`) and never from the PPU.

**Blast radius: none at stock timing.** The predicate is consulted only when
`extra_scanlines` is non-zero — `cyc_set_extra_scanlines()` short-circuits on 0,
and `cyc_state.c` validates `(!timing.extra_scanlines || supported())`. At the
default 0 every game behaves exactly as before.

**Verified:**
- `nes_game --frames 600 --input tas/vectors/sdb_throw.input --ram-init zeros`
  → `cycles=17865922 native_cycles=13225160 (74.0%)`, byte-identical to the
  same command on the unpatched build.
- `--extra-scanlines 262` → ~2× cycles/frame (59,523 vs 29,740 at 60 frames),
  i.e. the enhancement actually engages.
- The mod then arms from either its manifest feature or `--ppu-overclock 262`.

**Second effect, worth knowing:** `cyc_state.c` validates a loaded state's
`extra_scanlines` against this same predicate. Before the patch a state written
while overclocking would have been **rejected on load** for a mapper-1 cart
("invalid CPU budget enhancement state"). After it, such states load. So the
patch is what makes the mod save-state-safe; without it the feature would have
had to be refused whenever saves were enabled.

**Afterlife: propose upstream.** This is a generalisation of an existing
feature to the most common NES mapper, not a game-specific hack. Upstream should
probably go further than `mapper == 1 || mapper == 4` — the principled rule is
"any mapper whose IRQ is not PPU-clocked" — but that needs a per-mapper table
this patch does not introduce.

**Revive/drop:** if upstream lands the change, delete this file; `setup.sh`
skips patches that no longer apply and the pristine submodule then builds the
game correctly on its own.