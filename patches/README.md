# patches/

Active patches (`*.patch`) are applied to the NESRecomp submodule by
`setup.sh`, in filename order, after every submodule checkout, so the
submodule tree can stay pinned to pristine `origin/master` while a change that
still needs upstreaming is usable here.

Dormant ones live in `archived/` and are deliberately **not** applied — they
target the retired legacy runner. See `active-patches.md` and
`archived/archived-patches.md`.

Rules:

- **Active patches must be strictly additive.** They may enable something that
  was refused; they must not change behaviour for a game that did not ask. They
  are headed for upstream PRs, so a reviewer has to be able to read the diff and
  see that no existing title moves.
- Each one documents what, why, blast radius, evidence, and what happens if
  upstream takes it.
- The submodule tree itself stays pinned and clean between setup runs; a patch
  never gets committed inside `external/nesrecomp`.

| Patch | What | Fate |
|---|---|---|
| `006_ppu_overclock_mmc1` | `cyc_extra_scanlines_supported()` also accepts mapper 1, so the existing PPU-overclocking enhancement works on MMC1 | **Propose upstream** (generalisation, not a game hack — `active-patches.md`) |

Retired with the cycle switch (2026-10-02), kept as a record:

| Patch | What | Fate |
|---|---|---|
| `002_interp_ram_dummy` | RAM-trampoline interp RTS pops live-caller bytes; pre-push dummy pair | Legacy runner; **proposed upstream** (research §19.1, draft in `docs/plan.md`) |
| `003_interp_rom_clamp` | ROM-miss pop guard (002's companion) | Legacy runner; lapsed |
| `004_nmi_tunnel_unwind` | Start-tunnel longjmp unwind | Game-specific; lapsed |
| `005_hang_watchdog_default` | 90 s idle auto-kill default | Taste call; lapsed |
| `001_nmi_tail_unwind` | NMI-tail unwind for the `$FCB3` spin | Dormant, never fired |