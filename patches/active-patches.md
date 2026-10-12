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