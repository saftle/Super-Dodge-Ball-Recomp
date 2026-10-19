# patches/

Active patches (`*.patch`) are applied to the NESRecomp submodule by
`setup.sh`, in filename order, after every submodule checkout, so the
submodule tree can stay pinned to pristine `origin/master` while a change that
still needs upstreaming is usable here.

Dormant ones live in `archived/` and are deliberately **not** applied — they
target the retired legacy runner. See `archived/archived-patches.md`.

Rules:

- **Active patches must be strictly additive.** They may enable something that
  was refused; they must not change behaviour for a game that did not ask. They
  are headed for upstream PRs, so a reviewer has to be able to read the diff and
  see that no existing title moves.
- Each one documents what, why, blast radius, evidence, and what happens if
  upstream takes it.
- The submodule tree itself stays pinned and clean between setup runs; a patch
  never gets committed inside `external/nesrecomp`.

## Active patches

**None.** All three that this project carried landed upstream, and the
submodule pin is now past them — see the table below.

## Landed upstream

| Patch | Upstream PR | What it is now |
|---|---|---|
| `006_ppu_overclock_mmc1` | [#66](https://github.com/mstan/nesrecomp/pull/66) | `cyc_extra_scanlines_supported()` accepts mapper 1 as well as 4. Upstream took it and went further: the `--extra-scanlines` refusal now reads *"requires an MMC1 or MMC3 cartridge"*, and `runner/cyc/PROJECTS.md` documents both boards. |
| `007_window_vsync` | [#67](https://github.com/mstan/nesrecomp/pull/67) | `[Display] VSync` in `config.ini`, `gi.has_vsync` lighting up the launcher's Graphics row. **Upstream rewrote the implementation** rather than taking ours: the renderer is created without `SDL_RENDERER_PRESENTVSYNC` and the setting is applied at run time with `SDL_RenderSetVSync()` (SDL 2.0.18+), so fast-forward and uncapped runs can turn blocking presents off. It also decoupled the guest's frame deadline from the display (`runner/cyc/cyc_frame_pacing.h`, with a unit test). Ours set the flag at renderer creation, which cannot be undone. |
| `008_rom_read_substitution` | [#69](https://github.com/mstan/nesrecomp/pull/69) | `cyc_mod_set_rom_read_override()` and a table consulted on every ROM-space CPU read, from `hw_bus_read()` and from `hw_read_rom()`. Our `cyc_mod.c`/`cyc_mod.h` are byte-identical to upstream's. Upstream also carried a hunk **our patch did not have**: `runner/cyc/cpu6502.h` had to start using `hw_read_rom()`'s return value. |

**This project's upstream PRs are five, not three.** PRs
[#21](https://github.com/mstan/nesrecomp/pull/21) and
[#22](https://github.com/mstan/nesrecomp/pull/22) — the NES-to-runner PPUMASK
bit translation layer, and forcing rendering on when it is enabled — were
merged upstream in September 2026, before this project carried any patch at
all, and they touch `runner/src/runtime.c` (the legacy function-level runner,
retired here in 2026-10) rather than the cycle backend. Upstream later revised
#22's forcing (commit `a671d53`). The full attribution is in `NOTICE.md`.

### The lesson from 008, and why it is worth a line here

`hw_read_rom()` returns the substituted value, and the *folded* read path
(`cpu_read_rom()` in `runner/cyc/cpu6502.h`) was calling it for its side
effect only — `hw_read_rom(addr, value);` — and then returning the original
folded constant. So a substitution applied to a **folded** read was computed
and thrown away, while the same substitution on a **run-time** read (through
`hw_bus_read()`) landed. Over Super Dodge Ball's generated code that is 1405
`cpu_read()` against 3327 `cpu_read_rom()`, so 70% of PRG reads were blind.

The symptom is not "the cheat does nothing". It is **the same cheat producing
two different games** depending on which path executed the read: native and
`--interp-only` runs of one vector disagreed. Measure it that way, not by
asking whether the cheat changed anything.

### What to do when a patch lands upstream

1. Delete the patch file (the upstream commit is the durable record).
2. Bump the submodule pin past the merge commit.
3. **Diff the upstream implementation against the patch, hunk by hunk.** A
   merge that "looks like" our patch can carry a fix ours lacked — 008's
   `cpu6502.h` hunk is the example, and it was the one that mattered.
4. Re-run the gates the patch was written for, on the upstream build.
5. Update the references in `AGENTS.md` and `docs/research.md`.

### A patch for the engine that is not carried here

The **frame-state capture**: an opt-in snapshot of the frame's own nametables,
palette and pattern window, taken at dot 1 of visible line 0, with accessors
that read it (`cyc_render_capture_frame_state()`,
`cyc_render_frame_nametable/chr/palette/color`). The problem it solves is
real: the live `cyc_render_nametable()`, `cyc_render_chr()` and
`cyc_render_palette()` return the **next** frame's state at a host callback on
a frame boundary, because a game writes them in vblank and no callback sits
inside that gap. In another cartridge the live reads made the widescreen
reconstruction disagree with the machine's own background on more than half the
picture.

**It applies cleanly to our pin** (the pre-image blob hashes are unchanged
upstream) and it is **upstreamable as-is** — but it is **not needed here**,
and that is measured rather than assumed: switching this project's margin
reconstruction and its flicker-mod blit to the frame-state accessors changes
**0 of 1201** presented frames on `sdb_sweep` and **0 of 3001** on `sdb_throw`,
at 16:9 and at 32:9. The reason, the instrument and the numbers are in
`docs/research.md` §25 ("The reconstruction reads LIVE state, and that is
correct here — measured"). Carrying it would be carrying a dependency this
cartridge does not need, and an engine change that is not upstreamed is a
patch `setup.sh` has to keep applying.

Retired with the cycle switch (2026-10-02), kept as a record:

| Patch | What | Fate |
|---|---|---|
| `002_interp_ram_dummy` | RAM-trampoline interp RTS pops live-caller bytes; pre-push dummy pair | Legacy runner; **proposed upstream** (research §19.1, draft in `docs/plan.md`) |
| `003_interp_rom_clamp` | ROM-miss pop guard (002's companion) | Legacy runner; lapsed |
| `004_nmi_tunnel_unwind` | Start-tunnel longjmp unwind | Game-specific; lapsed |
| `005_hang_watchdog_default` | 90 s idle auto-kill default | Taste call; lapsed |
| `001_nmi_tail_unwind` | NMI-tail unwind for the `$FCB3` spin | Dormant, never fired |
