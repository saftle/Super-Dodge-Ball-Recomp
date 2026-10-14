# Super Dodgeball NES Recompilation

## Maintaining This File

- Everything here is a present-tense directive, constraint, or fact an agent needs *today*.
- Do NOT add changelog entries, "previously X, now Y" framing, or bug postmortems.
- When a bug is fixed, encode the lesson as a rule below, then delete the narrative.
- Keep entries concise. Prefer "Do X" / "Never do Y" over paragraphs.
- If a pitfall was a one-time infrastructure fix with no lasting constraint, remove it entirely.
- **Ask before installing packages, deleting venv, or committing.** Never assume.
- Use `todowrite` at the start of every session to track what's done and what's pending. Mark items complete as you verify them. Commit when a todo item is verified complete, not when everything feels done.
- Don't re-run tests that already passed — same binary + same inputs = same results.
- Check `git status` once at session start, then work from the known state.
- If a test times out, try a smaller version or accept the partial result. Don't retry with a longer timeout on the same approach.
- Don't re-read files you already read this session — you know the content, move on.

## Overview

NESRecomp recompiles NES 6502 → native C via a two-part system:

- **Recompiler** (`external/nesrecomp/recompiler/`): Per-instruction codegen (`cyc_codegen.c`) — one labeled C block per ROM instruction with every CPU cycle's bus activity spelled out, entered per (PRG bank, CPU address).
- **Cycle backend** (`external/nesrecomp/runner/cyc/`): Cycle-accurate console model (6502, PPU, APU, DMAs, mappers in `hw_*.c`) + host (`cyc_host.c`: input schedules, screenshots, frame-logs, TCP). Built per game into `build-cycle/nes_game`.
- **`src/game.toml`**: Recompiler config — display `name` (window title), a `cycle_seed_file`, plus twelve content-keyed `[[mod_function_hook]]` sites: the ten the character-flicker mod hooks, plus two (`$8483`, `$8302`) that read the game's off-screen sprite cull for the widescreen margins. No discovery tuning; the stock ROM needs none.
- **`src/cycle_seeds.txt`**: Recompiler discovery roots for the sprite emitters. A mod hook site only exists in recompiled code, and `$8040`/`$821E`/`$E32D`/`$966D`/`$A36C` ran on the interpreter — a hook there compiled, bound and never fired. These six lines fix that; total cycle count unchanged.
- **`src/sdb_host_extras.c`**: Our three mods (`HOST_EXTRAS`), the only game-specific code. **(1) character-flicker**: captures every OAM sprite the game emits and presents them host-side, and overrides the game's own object culling (three gates). The truncation half is presentation only (identical cycle count; only `$02FD`/`$02FF` differ); the culling half is **not** — it makes the guest emit sprites it had dropped. **(2) PPU overclocking** (section 5): picks a value for `cyc_set_extra_scanlines` so the guest gets more CPU time per frame and runs faster; re-applied it from `power_on`, because `cyc_power_on()` zeroes the field after mods activate. **(3) widescreen** (section 4): a compositor that presents 426/560/854 columns, filling the margins from the nametable, CHR and the frame's own scroll, with a per-frame seam check against the machine's own background capture; it also draws the sprites the game placed outside its own viewport (research.md §26), which is part of what widescreen means, not a separate feature. No engine patch needed — the engine already ships the widths and the compositor API. See research.md §25 and §26. (VSync started as a mod here and moved to the launcher's Graphics section — patch 007, research §24.)
- **`mods/packages/super-dodgeball.widescreen/`**: The widescreen package (manifest-only like the others), carrying two off-by-default features: `widescreen` (with an `aspect` **choice** option — stock / 16:9 / 21:9 / 32:9 / fit; it draws the players the game culled off-screen as well as the court, research.md §26) and `overscan` (reveal the 8 blank left-hand columns the game hides with PPUMASK).
- **`mods/packages/super-dodgeball.character-flicker/`**: The mod *package* — a `manifest.toml` only, copied beside the executable at build time. Data-only by design; `.nesmod` archives may not carry native code, so the package is a declaration and the implementation is `src/sdb_host_extras.c`. Off by default, toggleable in the launcher's Mods screen.
- **`mods/packages/super-dodgeball.ppu-overclock/`**: The PPU-overclocking package (same data-only design). Integer `[[option]]` so the launcher exposes the scanline count. Off by default because it changes console timing. Requires `patches/006_ppu_overclock_mmc1`; without it the backend refuses the setting on our MMC1 cart.
- **`patches/006_ppu_overclock_mmc1.patch`**: The first active submodule patch — widens `cyc_extra_scanlines_supported()` from MMC3-only to MMC1 too. Strictly additive (consulted only when `extra_scanlines != 0`, so stock timing is unchanged for every game) and on its way upstream.
- **`patches/007_window_vsync.patch`**: The second active patch — VSync as a launcher Display setting (`[Display] VSync` in `config.ini`, `has_vsync` lighting up recomp-ui's Graphics row, renderer built with `PRESENTVSYNC` when on). Default off, so stock behavior is unchanged unless the player opts in; presentation only, never guest timing or save states.
- **`CMakeLists.txt`** (repo root): our game project — includes the submodule's `runner/cyc/project.cmake` but adds `CXX`, `MODS`, `GAME_ID` and `HOST_EXTRAS`, which the submodule's own `runner/cyc/project/CMakeLists.txt` cannot express. This is why we wrap the submodule rather than editing it.
- **`build-cycle/`**: Generated per-instruction C (gitignored, one revision dir per ROM/config) + `nes_game` runner, `config.ini`, and the preloaded `mods/` catalog.
- **`external/recomp-ui/`**: Shared recomp-ui launcher (pre-boot settings dashboard) + in-game menu, compiled into the windowed `nes_game`.

Build: `./build_and_run.sh` (windowed) or pass host flags (`--frames`, `--input`, `--shot-every`) for headless scripted runs. Game config changes re-trigger codegen at configure time (each revision gets its own generated dir).

A headless run does **not** read the mod catalog unless you pass `--mods-root build-cycle/mods`; without it mods stay off and the output is byte-identical to stock. That is the intended way to A/B the mod, and it is what the "mod off" baselines below were measured against.

### The launcher (recomp-ui)

nesrecomp already integrates recomp-ui for the cycle host (`runner/cyc/project.cmake` auto-detects a checkout; the glue is `cyc_ui_launcher.c`/`cyc_ui_menu.c`/`cyc_ui_imgui.cpp`). **The work here is pointing it at our checkout, not writing a launcher.** Nothing game-specific: console identity, bindings, and the picture all come from nesrecomp's adapter.

- Both `build_and_run.sh` and `setup.sh` pass `-DNESRECOMP_RECOMP_UI="$PWD/external/recomp-ui"`. The framework's auto-detect only looks in `${CMAKE_SOURCE_DIR}/recomp-ui`, so passing our real path explicitly is load-bearing, not belt-and-braces.
- `-DNESRECOMP_RECOMP_UI=OFF` builds the window without launcher or menu. `NESRECOMP_NO_LAUNCHER=1` skips the launcher at runtime (for debugging).
- Windowed only. A headless run (`--frames` and friends) sets headless before `cyc_sdl_prelaunch()`, so the launcher is never reached — verified: recomp-ui ON and OFF builds emit byte-identical `--frame-log` output over the 400-frame TAS.
- Settings, bindings and the launcher state live in `build-cycle/config.ini` beside the executable. The Menu shortcut (Escape by default) opens the in-game menu.
- **Pin the submodule.** The launcher's C ABI is append-only and heavily churned; headers and sources must come from one revision. Bump deliberately and rebuild whole. See `NOTICE.md` for provenance.

### ROM Specifications
- **ROM**: `roms/Super Dodge Ball (USA).nes` — iNES 2.0 format
- **Mapper**: 1 (MMC1 / NES-SLROM PCB, MMC1B2 variant)
- **PRG ROM**: 128 KB (8 × 16KB banks) · **CHR ROM**: 128 KB (16 × 8KB banks)
- **Mirroring**: Horizontal · **TV System**: NTSC · **Battery**: None
- **PRG CRC32**: `689971F9` · **SHA-1**: `42f954e9bd3256c011aba14c7e5b400abe35fde3`
- **File size**: 262,160 bytes (16-byte header + 262,144 data)

### Key References
- **`docs/research.md`** — ROM specs, NESRecomp framework details, sprite flickering analysis (§2, §2.1), PPU overclocking (§23), VSync (§24), widescreen and overscan (§25), Zelda/SMB/MesenCE research, input-frontier dossier (§18 pre-fix, §19 resolution)
- **`docs/plan.md`** — Consolidated project plan and todo list (merged from plan.md + todo.md)
- **`README.md`** — Project overview, MesenCE installation options, frame comparison pipeline, TAS differential harness
- **RetroPortingToolKit Docs**: https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs

## Setup & Build

```bash
./build_and_run.sh                                    # windowed, play it
./build_and_run.sh --frames 400 --input tas/cyc400.input --screenshot out.png
```

```bash
./setup.sh    # Handles venv, submodule update, MesenCE, blargg test-ROM checkout, cycle backend build
```

- **Venv**: `venv/` with `nesasm` 0.0.8 — use `venv/bin/python3` for nesasm work only. Venv pip is broken (no installs possible).
- **System `python3`** has Pillow + numpy — use it for `compare_frames.py` and frame analysis.
- **Never recreate `venv/`** — it is pre-configured
- **Never `pip install`, `apt install`, or run any package installer without asking first**
- **New packages go in `pyproject.toml` first** — but venv pip is currently broken, so verify installs work before depending on them
- **Never commit without asking** — always present what changed and ask

### MesenCE installation
Three options (setup.sh uses Option C by default):
- **Download pre-built** binary from MesenCE releases → extract to `external/mesence/Mesen`
- **Build from source**: `cd external/mesence && USE_GCC=true make -j$(nproc)`
- **`./setup.sh`** handles everything (clones submodule, builds MesenCE, builds runner)

### Build scripts
- `./setup.sh` — Full setup (venv, submodules, MesenCE, runner, cycle backend)
- `./build_and_run.sh` — Quick build + run on the cycle backend (windowed; pass `--frames`/`--input`/`--screenshot` for headless scripted runs)

## Running

```bash
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" [--frames N] [--input schedule] [--shot-every N --screenshot shot.png] [--mem-frame N --mem-out mem.txt] [--miss-log seeds.txt] [--align 0-3] [--mods-root DIR] [--present-out FILE --present-every N --present-size WxH] [--ppu-overclock N] [--widescreen MODE] [--overscan]
```

- **Interactive**: bare run — window opens (`[game] name` is the title), keyboard/gamepad input works
- **Headless**: any of `--frames`/`--input`/`--screenshot` implies headless — no display needed
- **Deterministic**: always pass `--ram-init zeros` for comparable runs (default pattern RAM otherwise)
- **ROM** must be at `roms/Super Dodge Ball (USA).nes` (gitignored)
- **Snapshots**: `--frame-log-at mesen --frame-log frames.bin` writes per-frame CYCFRAME-v2 snapshots (CPU RAM, CIRAM, palette indices, OAM, picture indices) for the TAS diff
- **Mods**: the window picks them up via the launcher. Headless needs `--mods-root build-cycle/mods` explicitly, or mods stay off and the run is stock.
- **PPU overclocking**: `--ppu-overclock <0..262>` (blank scanlines before NMI). Needs `patches/006`; refuses with the backend's MMC3 message otherwise. Deliberately changes timing; 0 is stock. The mod's manifest feature does the same thing for the window.
- **VSync**: the launcher's Graphics section (needs `patches/007` or the row is absent). Off by default; persisted as `[Display] VSync` in `build-cycle/config.ini`. Presentation only — guest timing untouched.
- **Widescreen**: `--widescreen stock|16:9|21:9|32:9|fit` (or the Mods screen). Widths come from the engine's `common/nes_video_geometry.h` — 426/560/854 — never hardcode them. Needs no patch.
- **Overscan**: `--overscan` or the Mods screen; only has an effect together with widescreen, because at width 256 the engine never calls the compositor.
- **Presentation**: `--present-out` saves what the *window* would show — the only path that exercises the character-flicker mod. `--screenshot`/`--frame-log` capture the machine's native frame and ignore it.

## Critical Rules

### Never do these without asking the user first:

1. Never install packages (apt, pip, snap, flatpak) without explicit approval
2. Never recreate `venv/`
3. Never run `sudo apt-get install` without asking
4. Never commit without asking — always present what changed first
5. Never remove pip packages without asking
6. Never download .deb files or extract packages without asking
7. Never modify generated C code in `generated/` directly — regenerate via NESRecomp
8. **Never run `git checkout HEAD -- <file>` or `git restore <file>` on modified files** — always back up to `/tmp/` first (e.g., `cp <file> /tmp/<file>.bak`)
9. **Never revert working changes without asking** — `git checkout`, `git restore`, and `git stash` all destroy uncommitted work
10. Never embed ROM-derived bytes in tracked source — no tile/CHR/nametable/attribute/palette arrays, no `.db` dumps, no multi-line disassembly excerpts. Addresses, sizes, and hashes are facts and fine; bulk bytes are not.

### System state:

- **MesenCE binary is prebuilt** at `external/mesence/Mesen` — no build needed. xvfb-run + isolated HOME with `settings.json` for headless `--testRunner`.
- **Available Python packages**: venv holds `nesasm` 0.0.8 only (pip broken — `pyproject.toml` entries like capstone/py65 never installed). System `python3` has Pillow + numpy.
- Build output is in `build-cycle/` (gitignored; windowed SDL build — the vendored SDL2 ships Windows libs only, so configure passes `-DSDL2_DIR=/usr/lib/x86_64-linux-gnu/cmake/SDL2`; see `build_and_run.sh`)
- ROM files are gitignored and must be placed in `roms/` manually
- **MesenCE is the oracle** for this project. Frame reference generation uses MesenCE binary directly with `--testRunner` mode and Lua API (`emu.getScreenBuffer()`). Co-simulation compares cycle-backend state (`--frame-log`) and screenshots against MesenCE references.

### Code and build constraints:

- `external/nesrecomp` is a git submodule — changes there need a separate `git commit`
- `external/mesence` is a git submodule — the MesenCE emulator source, requires .NET 10 SDK + SDL2 dev
- `external/recomp-ui` is a git submodule — never edit it in place, never commit in it. Build it from the pinned revision; do not mix headers across revisions (append-only ABI)
- Adding recomp-ui needs no SDL of its own (nesrecomp provides it) but does pull in OpenGL + Threads at configure time
- Generated C code (`generated/`) is gitignored — do not edit directly
- `make clean && USE_GCC=true make -j$(nproc)` builds the entire MesenCE with UI
- Use venv Python (`venv/bin/python3`) for nesasm work, system `python3` for Pillow/numpy scripts — never mix them up
- `src/` ships **config + our own C only** — `game.toml` and `sdb_host_extras.c`. No game bytes: the cycle project reads the user's ROM at configure time (identity-checked) and the binary takes the ROM path at launch. Hooks identify code by `crc32`, never by `bytes`. Precedents: `tools/rom_to_asm.py`, `tools/frame_gen/oracle_dump.lua`
- Debug byte dumps belong in `tmp/` (gitignored scratch), never in `src/`. Confirm offsets with `tools/dis65.py` plus a python byte-compare against the ROM before coding
- **Every screenshot/frame-dump run gets its own subfolder under `tmp/`** (`tmp/<what-it-is>/`), never the shared `tmp/` top level and never an existing investigation's folder. A per-frame `--present-out`/`--screenshot`/`--frame-log` run writes tens of thousands of files and gigabytes; loose in `tmp/` they are impossible to tell apart and impossible to delete selectively. Delete the folder when the investigation is over, and say what it was before you do
- `tmp/` is scratch, so **anything a reproduction depends on does not live there**: input schedules and vectors go in `tas/vectors/`, analysis scripts that back a research.md claim go in `tools/`
- Pre-commit self-check: `git status --short` and `git ls-files | grep -Ei '\.(nes|png|ppm|bin)$'` must show no game-derived blobs; any new `static const uint8_t …[]` in `src/` needs ROM-provenance justification or gets rejected
- Every third-party addition ships with provenance + license + posture, recorded at add time (never as later cleanup): who made it, under what license (or "no license file found"), and how redistribution is avoided — submodule pointer (own license, never vendored), gitignored + user-fetched (test ROMs, oracle binaries), or credited test fixture (vectors keep in-file + README author credit, link pages instead of re-hosting). Code/deps go in `NOTICE.md`; test data goes in the nearest README
- Installed packages (pip/apt) are not distributed — no attribution needed. Code you write from specs (parsers, harnesses) is yours. Data files you didn't create keep their credits intact — never strip author info
- **nesrecomp submodule tracks pristine `origin/master`** (currently `mstan/nesrecomp` @ `2b4fcdb` — the pin must stay fetchable upstream; never commit in the submodule). The **checked-out tree is expected to be dirty** when a patch is applied: `setup.sh` applies `patches/*.patch` after every checkout, so `git -C external/nesrecomp status` showing a patched file is normal, not a mistake. `git -C external/nesrecomp status` must otherwise be clean. Active patches must be **strictly additive** and are headed for upstream PRs — see `patches/README.md`, `patches/active-patches.md`

## Known Issues (Current State)

### Windowed `--present-out` runs the game in slow motion — pin the capture resolution

**Measured, not inferred.** All figures are 600–900 windowed frames under Xvfb
with the same schedule; ideal is 60.0988 fps (29781 cycles/frame).

| config | fps |
|---|---|
| stock, nothing on | 55.6 |
| + widescreen + overscan + flicker, **no** `--present-out` | 54.5 |
| fullscreen, `WindowScale = 3` (1080p), `--present-every 4` | 41.5 |
| `WindowScale = 3` (1278×720), `--present-every 1` | 29.4 |
| `WindowScale = 2` (852×480), `--present-every 1` | 40.9 |
| **`WindowScale = 1` (426×240), `--present-every 1`** | **54.1** |

**The cost scales with the RENDERER's pixel count, not the file size.**
`present_shot()` (`cyc_sdl.c:1093`) does `SDL_RenderReadPixels` on
`SDL_GetRendererOutputSize`, so `WindowScale` upscaling costs frames and adds no
information — a 1080p PNG of a 426×240 picture is 14.87 MB of the same pixels
stretched. That is also why it *gets worse over a run*: the dirty-page writeback
queue fills.

**Not vsync** — identical with `VSync = 0` and `VSync = 1` (10.19 s vs 10.19 s per
600 frames). **Not fixable by compressing the PNG** — 32.0 fps compressed vs
24.0 stored at scale 3 stride 1, i.e. the readback dominates and the encode is
minor. Do not "fix" it in `cyc_png.c`: upstream `README.md:354` states *pure
C11 with no external dependencies*, and `cyc_png.c` hand-implements CRC-32 and
Adler-32 precisely because zlib is absent, so pulling zlib in contradicts a
documented policy.

Mechanism: the pacing loop's "fell behind, don't try to catch up" fallback
(`cyc_sdl.c:1540`) fires, so the run drops to ~2/3 speed **with the audio**,
because audio is generated per emulated frame. It is not a correctness problem —
the guest state is unaffected — but it makes the game unplayable and makes any
two runs incomparable on wall clock.

**Fix: pin the capture resolution**, by deriving a config rather than
hand-writing one:

```bash
sed -e 's/^WindowScale = .*/WindowScale = 1/' \
    -e 's/^Fullscreen = .*/Fullscreen = 0/' \
    -e 's/^VSync = .*/VSync = 0/' \
    build-cycle/config.ini > tmp/<name>/config.ini
# then pass --config tmp/<name>/config.ini
```

**Keep `Player1Source`.** Deriving from the real `config.ini` (rather than
writing a minimal one) is the point: player 1 is the **keyboard** and player 2 is
the controller (`cyc_bindings_default`, `cyc_input.c:30`), and a config that
loses that records the entire session on port 2 — verified, every press landing
on `2:` while the guest saw nothing.

**Widescreen is `--widescreen 16:9` and is independent of `WindowScale`.**
Scale 1 is still 16:9 at 426×240; the scale only decides how big the window is.

Use `tools/play_record.sh` (`SDB_SCALE`, `SDB_SHOT_EVERY`) rather than assembling
this by hand. Never pass `--present-out` at fullscreen scale.

### Windowed and headless run different guest state — analyse captures in place

Two windowed runs of one schedule are **byte-identical** (same recorded vector,
same trace). A **headless replay of that recorded vector is not**: frames 0–3
match, then it diverges at **frame 4** (`sdbc` scroll 32 vs 256), ending on
different game state — 2666 vs 2121 off-edge captures, 32200 vs 15504 margin
sprites drawn, 769 vs 752 frames composed, seam diff 0 vs 2 px.

**Pacing cannot cause this.** A cycle model runs a fixed cycle count per frame
regardless of how fast the host loop spins, so host speed cannot change what the
guest computed; this is unaffected by the slowness above and is a separate
thing. It is also not the input: the recorded frames are exactly the frames the
schedule fired on, and the divergence starts four frames *before* the first
press. **Cause is unattributed** — do not guess.

Consequence for tooling: **analyse a windowed capture where it was made.**
`tools/widescreen/margin_track.py` reads `trace.log` directly and needs no
replay. `research.md` §22's "windowed == headless" claim was 5 sampled frames
at 256×240 with no mods and does not hold this far in; treat it as superseded.

### A played session is the only way to find the band bugs — `--record-input`

`nes_game --record-input FILE` records the live controller to an `--input` vector
as you play, via `CycHostExtras::input` ("final NES controller bytes, after host
mapping and before latching"). Both host loops call it immediately before
`cyc_session_frame_begin()` (`cyc_sdl.c:1470`, `cyc_host.c:1602`), so `s_frame`
there is the index of the frame about to run — exactly what `input_tick()` matches
steps against. It writes the plain `--input` schedule format, not a new one.
`tools/play_record.sh <name> [frames]` wraps it with `SDB_TRACE=1` and
per-frame screenshots.

**Do not hand-write a court or margin repro vector.** Three have been tried here
and all three were wrong for reasons unreadable in the code: the input window
opens at frame 594 and presses before it are silently dropped; a margin boundary
artifact needs the camera to pan the *whole width*; and the Bean Ball list
degeneration needs the player to *run*, not stand still. None is expressible as a
schedule someone typed. The seventh margin-sprite bug (research §26, the ghost in
the band) survived the sweep, pan, idle and throw vectors and was found by a
played session in about a minute.

**Two windowed runs of one schedule are byte-identical** (same vector, same
trace), which is what makes a played session reproducible. A *headless* replay of
that vector is **not** — see the windowed/headless section above. So replay in a
window (`--virtual-pad` over a converted vector) and never headless.

`Player1Source` is the keyboard for player 1 and the controller for player 2, so
a `--virtual-pad` replay needs `Player1Source = 2` in the config or the guest
never sees the input and the run sits at the title screen — which looks exactly
like "the feature does nothing".

### Three instrumentation bugs that all made a gate silently blind

- **The `sdbw` trace line had no newline before `sdbm`.** The per-sprite entries
  were glued onto the summary line, so `margin_track.py`'s regex matched nothing
  and it reported `no sdbw lines in that log` on exactly the runs where
  `SDB_TRACE_CAPTURES` was turned on to run it. The `n=` closer was moved first.
- **That fix then hit the mirror-image failure.** The same commit also added the
  `oam-band:` line, which ends with its own `\n` — so the closer moved above the
  entries and *below* that block landed on a line of its own. `margin_track.py`
  wants `… n=NN` at the end of the `sdbw` line, so **0 of 701** lines matched and
  gate 3 was inoperable: a healthy run reported "no sdbw lines in that log". The
  closer now prints *before* the `oam-band:` block. Two fixes, each undoing part
  of the other; the order of the two blocks is the whole thing.
- **`margin_track.py`'s `part` check only fires for wholly off-screen objects.**
  An object straddling the band at screen column 5 holding 2 parts instead of 8
  passes clean. That is how the ghost survived an otherwise-green audit.

**Check the gate can run before believing it is green.** Two of these produced a
clean-looking pass over zero examined frames. `python3 tools/widescreen/margin_track.py
LOG --records REC` should print a per-object table; "no sdbw lines in that log"
is a broken instrument, not a clean picture. `SDB_TRACE_JSON` + `json_trace.py`
exists for the same reason — one JSON object per line, parsed with `json.loads`,
which cannot care where a closer was printed.

### Bean Ball: the flicker mod's sprite list degenerates while running — open

From a played capture, game mode "bean ball": as the player runs across the full
map the presented sprite list shrinks monotonically, 53 → 10, and by frame 714 a
single player sprite remains, cut in half. `overflow 0`, `backstop 0` — not the
§2.1 runaway list. The background is perfect (1183 composed, 1 pillarboxed), so
the compositor is not involved; the flicker mod replaces the machine's sprites
with its own list, so a short list is missing sprites. **Not established whether
the game emits fewer or the mod drops them** — opposite fixes. Full detail and the
next measurement in research §26 and plan item 14.

### Court input timings, measured — and input aimed earlier is silently dropped

Three separate repro attempts on the widescreen margin and the judder failed
because of these, so they are recorded rather than re-measured:

- **412** — the court appears (camera 128 shows up, the display records at `$0618`
  start populating at 421).
- **594** — the input window opens. Direction presses before this are **dropped**,
  not queued: pulsing LEFT from frame 380 leaves the camera at 128 until 594 and
  it only then steps 128, 121, 115, 108, 102, 95, 89. A vector that aims a
  repro at frame 500 appears to do nothing at all.
- **617** — a single left tap starts a run.

`tas/vectors/sdb_bar.input` is the vector built on them. Anything aimed at a
court input must start after 594.

### Sprite judder while the camera pans — RESOLVED, and it is the game's

Visible in the **stock** picture with no mods: a player holds still for several
frames then jumps, and the jump is not the same size as the court's, so he
detaches from the floor and snaps back. The sprite steps and the camera steps
**2 frames apart**, both in 6-7 px increments.

**Measured against the oracle 2026-10-13: MesenCE does exactly the same thing,
so this is the game's own cadence and nothing here is broken.** Metric:
`world_x = OAM X + $C3/$C4` for the player sprite (OAM y=143, tile `$20`) — the
player walks left one step while the camera walks right one step, so same-frame
would cancel. Cycle: camera 601/608/615/622, sprite 603/610/617/624, N = 2 four
times. MesenCE: camera 603/610/617, sprite 605/612, N = 2 twice. Same base (207),
same blip. Full write-up in research.md §27; `python3 tools/tas/judder.py RUN`
re-derives it in one command. plan.md item 12.

**Nothing to fix, and specifically not upstream** — a second independent
emulator reproducing it is the proof. **And never in a mod**: it is in the
machine's own 256-column picture, which the compositor must reproduce faithfully,
and smoothing it would break the 0-pixel gate.

Two traps that cost the time, both in §27: the TAS harness had **never** been
logging scroll (five `mesen_tas.lua` state columns are `null` — the keys are
namespaced, `ppu.xScroll`, which is what research §16 warns about), and with the
key fixed **`ppu.xScroll` is still not the court's scroll** (it reads 0-7 through
the whole pan). Do not measure this by correlating pictures either — a column-SAD
over the flat court locks onto the wrong feature.

### Model Notes (cycle backend behavior, verified)

- Sprite palette entry-0 bytes (`$3F10/$3F14/$3F18/$3F1C`) read stale power-on bytes in snapshots — hardware mirrors them to the backdrop `$3F00`. Masked in the TAS diff; rendering unaffected. Upstream-noted, not ours to fix.
- Uninitialized CIRAM differs at power-on (`--ram-init` covers CPU RAM only); the game fills nametables within frames. Boot-frame diffs are init modeling, not divergence.
- Unseeded runs execute partly on the cycle interpreter (still bit-exact, slower). `--miss-log` seed lines feed future `cycle_seed_file` runs; native share is informational, never a gate.
- Boot staging order differs from Mesen (backdrop-green vs stripped title at file 0); content converges from file 1. Same-index comparison stands — no offset search.
- Sprite flickering is **not** emulator sprite-limit removal — the game throttles its own sprites three ways, and "Remove Sprite Limit" cannot help with any of them. (1) The OAM builder has a hard **64-sprite budget** (8-bit `$0617` cursor, `BEQ` bail at `$84B6`) and simply **stops emitting**, so the missing tiles are never computed; characters are 8 sprites each (16×32), so 79% of match frames truncate. (2) `$E32D` builds a display list at `$067C` and refuses to add an object whose state byte `$43` has bits 4-6 equal to `$20`, rotating which object it refuses, so a whole character blinks out 4 frames in 9 while OAM sits at 46-54 of 64. (3) Two emitter shapes no cursor-bump hook can see: the damage-number pair (bank 3, `ADC #$08` step) and the block copier (bank 4, no bump at all) — hooked per-emitter, with prefix carry-over for sprites that sit in OAM. **All fixed** by the opt-in `super-dodgeball.character-flicker` mod (`src/sdb_host_extras.c` + ten content-keyed `[[mod_function_hook]]` sites in `src/game.toml` + `src/cycle_seeds.txt`). Truncation: body-ink stdev 8.7 → 0.8. Culling: box-ink stdev 55.0 → 16.6, 0 frames still culled. Numbers: 108/108 frames steady. Full write-up in research.md §2 and §2.1.
- Three traps in that work, each of which produces a *different* artefact than the one being fixed: a hook site in interpreted code compiles, binds and never fires (seed it); the `$E35F`-`$E36C` id-high-byte rule is inverted if read at a glance and gives restored objects sort key `$FF`, which shows up as a character drawing *in front* of the one above it; and re-adding a culled object changes the guest's work, so the "presentation only" claim stops being true (OAM differs, sprite slot 0 never does, the picture does not).
- **The mod's own sprite-list boundary is `$0617` going backwards, not `$0617 == $04`.** The game sometimes starts a list at a higher slot when the emitters for slots 1-2 have nothing to draw, and a `$04`-only test misses it: `build` never restarts, runs to `SDB_MAX_SPRITES`, and every frame after paints hundreds of stale sprites — a "copies in a trail" smear that looks like a game bug and is not. Backstop at `SDB_LIST_LIMIT` (256 vs a measured max of 77) publishes and warns. `SDB_TRACE=1` prints `frame peak-list overflow published-list saw-$04 backstop-trips` per frame — reach for it first; `SDB_TRACE_CAPTURES=<from>-<to>` dumps every capture. Never leave that boundary on one condition.
- Two traps when touching that mod, both of which produce a *different* artifact than the one being fixed: emit order **differs between builds**, so never blend a half-built list with the previous one index-wise (draws both, out of order) — present the last complete list only on the *first* capture frame, and only if it was published on the immediately previous frame (after a gap the published list predates OAM; research.md §2.1), or the picture goes a full build stale one frame in six. And the blit must walk pixels itself mirroring `cyc_render_sprites`; calling `cyc_render_tile` once per scanline stacks eight overlapping 8×8 copies that overwrite their own pixels, which reads as doubled, wrongly-coloured sprites.
- A restarted list opens with the OAM slots below its cursor copied verbatim (prefix carry-over): persistent sprites like the damage digits live there and the PPU draws them, so omitting them drops sprites the machine shows. Not blending — those slots are outside the new list by construction.
- `$8200` (bank 5) removes an entry from the object list at `$067C` and the SNES port replaces it with `RTS ; disable sprite flickering`. **That was the second flicker.** It is one of three gates on the same culling decision, all now overridden by the mod; research.md §2.1 has the full chain (`$E34A` builder gate → `$828A` record gate → `$8200` crowded-stack gate) and the measurements. The port's other two "flicker" commits (an inverted OAM priority bit, and capping its own OAM translation at 64 sprites with `CPY #$100`) are not ours.

### Current Constraints
- `src/game.toml` carries no discovery tuning (the stock ROM needs none) and no `[[mod_function_hook]]` beyond the ten the character-flicker mod needs: five OAM-emit sites, three object-culling sites, one damage-number site, one block-copy site. Its `cycle_seed_file` (`src/cycle_seeds.txt`) is **not** optional — the culling/number/block sites sit in code static discovery never reaches, and a hook in interpreted code compiles, binds and never fires. **Never add hook content keys as `bytes` unless you intend to commit ROM-derived bytes** — use `length` + `crc32`, which keeps the bytes out of the repo.
- Hook sites declare **no `id`**: they bind to plugins by address. Sharing one `id` across several sites fails `cyc_hooks_validate`.
- No `sram_map` (no-battery MMC1 has no SRAM code)
- **nesrecomp submodule tracks pristine `origin/master`** — the checked-out tree is dirty exactly when an active patch is applied (see above); `setup.sh` re-checkouts the pin on fresh setups

### Additional Known Issues

- **Frame comparison**: images are informational only (cross-renderer RGB always differs by palette emulation). Same-renderer pairs are strict; cross-renderer pairs are palette-relative. State bytes (`diff_tas.py`) decide.
- **`run_roundtrip.py` is trivial**: It only proves `nesasm` preserves bytes through `.db` directives — NOT that recompilation is correct. See `docs/research.md` §12.

### docs/ Directory

- **`docs/research.md`** — ROM specs, NESRecomp framework, NMI dispatcher/`$0100` map (§13), MesenCE oracle (§16), NMI survey (§17), PPU overclocking (§23), VSync (§24), widescreen and overscan (§25)
- **`docs/plan.md`** — Living project plan and todo list

## NESRecomp Submodule Status

**Tracks pristine `origin/master` (`mstan/nesrecomp`, currently `2b4fcdb`).** The pin stays fetchable upstream; never commit in the submodule. The checked-out tree is dirty exactly when an active patch is applied (see the constraint above) — verify any *other* dirt is yours before blaming the framework.

## Codegen Notes

- One labeled C block per ROM instruction, entered per (PRG bank, CPU address); static jumps/branches compile to `goto`s, `cpu.pc` written only when control leaves compiled code
- Frame boundaries are PPU-driven; `--frame-log-at mesen` aligns snapshot records to Mesen's frame end
- The recompiler also supports `[[mod_function_hook]]` (content-keyed subroutine hooks) — used by the character-flicker mod (ten sites); see `docs/plan.md` roadmap for future ones

## Retired: Legacy Runner (2026-10-02)

The function-level runner (`src/extras.c` hooks, `build/`) is deleted. Its game knowledge (NMI dispatcher, tunnel mechanics, input path) lives on in `docs/research.md` §§18–19 and transfers to mod work. `patches/` is back (reopened 2026-10-12 for patch 006): `setup.sh` applies active patches to the submodule after every checkout — see `patches/active-patches.md` — and archived legacy-runner patches stay unapplied in `patches/archived/`. Never re-add game-specific hooks to work around timing again: time it against Mesen, and if the model is wrong, the fix belongs upstream.

## Verification & Testing

### Iterative Testing Procedure

After any `src/game.toml` change (display name, discovery, seeds, **or a `[[mod_function_hook]]` site**):

1. **Rebuild**: `cmake --build build-cycle -j$(nproc)` (configure re-triggers codegen; each revision gets its own generated dir)
2. **TAS**: `python3 tools/tas/run_tas.py --tas tas/sdb_4976.tas.json --frames 400 --out tas/runs/rXXX` + `diff_tas.py` — liveness 400/400 exit 0, state MATCH (mirrors masked)
3. **FAIL criteria**: nonzero exit, logged frames != requested, any state diff outside the masked mirrors

**Known baseline**: cycle 400 TAS frames → team-select, state byte-exact vs Mesen (RAM/OAM/palette/CIRAM); blargg cpu tier 25/25.

### Verifying the character-flicker mod

`--frame-log`/`--screenshot` capture the machine's native frame and never see the mod's
presentation. Check it with `--present-out`, which writes what the window would show:

```bash
# same input both ways: mod on vs off is the whole test (the feature is off
# by default, so mod-on runs pass --remove-character-flicker explicitly)
SDB_TRACE=1 ./build-cycle/nes_game ROM --frames 1100 \
  --input tas/vectors/sdb_idle.input --ram-init zeros --mods-root build-cycle/mods \
  --remove-character-flicker \
  --present-out tmp/mod/on_%05d.png --present-every 1
./build-cycle/nes_game ROM --frames 1100 --input tas/vectors/sdb_idle.input \
  --ram-init zeros --present-out tmp/mod/off_%05d.png --present-every 1
```

**Set `SDB_TRACE=1` on every mod run.** It prints one line per frame and is what turns "the
screen looked wrong somewhere" into a frame number. A healthy frame is tens of sprites; watch
the peak-list column, not the pictures.

Gates:

1. **No runaway list** — peak list size stays under ~100 and the overflow column is 0. A peak
   that climbs frame after frame is the list-boundary failure (Known Issues); it ends in a
   trail of stale sprites. The backstop column must also be 0.
2. **Flicker gone** — pick a character's on-screen box, count non-background pixels per frame:
   stdev must fall well under stock's. Truncation 8.7 → ≤1; culling (Enemy B, frames 700-909)
   55.0 → 16.6, with **0** frames at stock's culled value.
3. **No new artefacts** — inside the affected character's box the mod matches stock where
   stock drew him (frame 866: 0 differing pixels in B's box). Whole-frame sameness is NOT
   the gate: the culling fix shifts guest timing, so blink-phase elements (menu cursor,
   roster scroll) sit a frame apart between runs with identical game flow. Regional ink
   plus eyeballing decides.
4. **Mod off is stock** — byte-identical output with the feature disabled (1100/1100 frames,
   empty catalog vs no catalog).
5. **Play it** — a scripted idle run never reaches the states that break list bookkeeping.
   Throw (`B`; `A` passes, `A+B` jumps) from frame 445 and watch (`tas/vectors/sdb_throw.input`,
   8000-frame stress: max list 77, 0 overflow, 0 backstop). See research.md §2.1.
6. **Numbers present** — damage digits drawn every frame the guest holds them in OAM
   (108/108 steady, throw vector); menu position digits pixel-identical (menu frames 100-400).

Comparison numbers must come from the *same* schedule and `--ram-init`; a different input changes the match and the figures are meaningless.

**Guest state: not a pass/fail gate, but do record it.** The truncation half is presentation
only. The culling half makes the guest emit sprites it had dropped, so OAM differs (sprite slot
0 never does), per-object timers settle at a constant offset, and the picture (CIRAM) is
identical on 899 of 910 frames. Cycle count moves by 1-2 over 910 frames. "Cannot observe" is
not "byte-identical" — do not write that it is.

**One nes_game per mods dir.** `build-cycle/mods/state.toml` is shared mutable state: two
concurrent runs (e.g. a `--miss-log` run beside a mod run) can leave the feature `enabled =
false`, and the whole run silently goes native. Never overlap runs that share a mods dir.

**The TAS gate is not currently trustworthy.** Super Dodge Ball is a random game, so a fixed input schedule does not reproduce the same run: RNG divergence between the cycle backend and Mesen puts the two sides on different paths, and a red `diff_tas.py` may be that rather than a real defect. Until pre-seeding exists (pin both sides to the same RNG state before frame 0), treat TAS diffs as advisory — investigate the first divergent frame, don't assume a regression. Blargg is unaffected and remains the real engine gate.

### Verifying the widescreen mod

`SDB_TRACE=1` prints `widescreen: N composed, M pillarboxed, seam diff D px`, and whenever
widescreen is drawing the margin sprites also
`margin sprites: N captured (M off-edge), D drawn, peak P, backstop B`.
**Pillarboxed rising while composed stays flat means the reconstruction is wrong** — that
counter is the gate, and it is how the four bugs in research.md §25 were found. A healthy
match frame is `composed` with a seam diff of 1-2 px out of 61440. **A missing
`margin sprites:` line means the capture is off** (see the gate list below); **`drawn`
staying at 0 while `off-edge` rises means they are captured but not painted** — check the
`native_x0` mapping.

```bash
SDB_TRACE=1 ./build-cycle/nes_game ROM --frames 900 --input tas/vectors/sdb_throw.input \
  --ram-init zeros --mods-root build-cycle/mods --widescreen 32:9 \
  --present-out tmp/wide/on_%05d.png --present-every 1
```

Gates:

1. **Mod off = stock** — with the catalog present and every feature off, the `--present-out`
   frame is byte-identical to a run with no `--mods-root` at all. Point `--mods-root` at a
   **copy** with an empty `state.toml`; that file is shared mutable state.
   **When hand-writing `state.toml`, `[feature.values]` binds to the `[[feature]]` declared
   immediately above it** — put it after the wrong feature and the option is silently
   dropped, the feature falls back to its manifest default, and nothing warns. (`fit`
   headless also resolves to stock 256, so a lost aspect looks like "the mod did nothing".)
2. **Stock centre** — the presented native 256 columns must be pixel-identical to a stock
   run at every aspect (array-compare; the compositor copies the machine's picture back over
   its own). The one exception is the `overscan` feature's 8 revealed columns, which is the
   point of that feature.
3. **Widths** — 426/560/854 for 240 rows, using the engine's rounding. Truncating turns
   32:9 into 852 and 3.55:1.
4. **Frame 0 pillarboxes** in every mode — the background capture is not available until
   after the first frame, so the compositor declines rather than claiming an unchecked seam.
5. **No black gutter** — measure fully-black column runs in the output. The game's PPUMASK
   left-column clip puts one at the margin boundary unless `overscan` is on.
6. **Play it** — a scripted run never reaches team-select, the roster screens or the
   innings changes, which are exactly the states the seam gate has to hold through.

### Verifying the margin sprites (part of `widescreen`, not a separate feature)

It reads the game's own placement, so its gate is **not** "does it look right" — it is
"does the guest notice". Everything else follows from that being true.

**First check, and it is the one that was failed: does it turn on at all?** This was a
separate `margin-sprites` feature, defaulted off, and the window ran it off — so the
player saw the background-only picture and the feature looked nonexistent, while every
other gate was green because the gates measured the flag rather than the output. It is
now part of `widescreen` and cannot be missed. Verify by running the way the window
runs (`--mods-root`, no CLI flags, a `state.toml` with only `widescreen` ticked) and
checking that `margin sprites:` appears in the trace. **A run with no `margin sprites:`
line means the capture is off, not that there is nothing to capture.**


1. **Guest untouched** — the strictest gate in the project. Two `--frame-log`
   runs, margin sprites off and on, same schedule, `cmp` them: **byte-identical**.
   That covers CPU RAM, CIRAM, palette, OAM and the whole 256×240 picture. The cycle
   count must also match (`mode=native ... cycles=` identical). Both callbacks return
   0, the site's own instruction and the game's own cull still run, and `$0617` is
   untouched because the culled path branches past the cursor bump at `$84B6`. Run
   it on all four vectors and **with and without `overscan`**.
2. **Machine columns untouched — and this is also the no-doubling proof.** Compare
   a `--present-out` run with margin sprites on against the same run with
   `--margin-sprites 0`: the machine's 256 columns must differ by **0 pixels** on
   every frame, while the canvas as a whole differs by a million-odd pixels. At
   every aspect and both overscan states. With `overscan` on, exclude the 8 revealed
   columns from the comparison — revealing them is the point. It holds by
   construction (the blit is clipped to the columns the machine does not own), but
   measure it, do not reason about it.
3. **Per-sprite audit** — `tools/widescreen/margin_track.py`. This is the gate that
   catches the artifacts the others cannot see, and it is the one to reach for when
   something looks wrong near an edge:

   **It has a known blind spot, and it is the shape of research §26's seventh
   bug.** `part` only fires for objects *wholly* outside the viewport. An object
   **straddling** the band — screen column 5, 2 parts instead of 8 — passes with
   `no anomalies`. So a clean audit is necessary, not sufficient: if something is
   visibly wrong *at* the band edge rather than out in the margin, go to the
   `sdbw` trace and the `oam-band:` line, which will say whether the game put a
   sprite in OAM at that column at all. A part with no OAM entry is a ghost —
   the game never emitted it and the margin is drawing something that does not
   exist.

   ```bash
   SDB_TRACE_CAPTURES=500-1200 ./build-cycle/nes_game ROM --frames 1200 \
     --input tas/vectors/sdb_sweep.input --ram-init zeros --widescreen 16:9 \
     --overscan --present-out tmp/mt/w_ --present-every 1 >/dev/null 2>tmp/mt/trace.log
   ./build-cycle/nes_game ROM --frames 1200 --input tas/vectors/sdb_sweep.input \
     --ram-init zeros --frame-log tmp/mt/sweep.bin --frame-log-frames 0:1199
   python3 tools/widescreen/margin_track.py tmp/mt/trace.log --records tmp/mt/sweep.bin
   ```

   It reports per object, per frame: `gone` (live record, off-screen, nothing
   drawn), `ghost` (two parts of one object within a pixel of each other on a row),
   `shift` (the object moved further than its record and the camera did), `part`
   (a wholly off-screen object holding fewer parts than it did). Current baseline:
   **701 sweep frames, eleven objects, no anomalies**, at 16:9 and 32:9, with and
   without `overscan`, and with the character-flicker feature on. Objects 2 and 8 —
   the two the game's display list almost never contains — must read **701/701**.
   **A `ghost` is the one to take seriously**: a character is sixteen columns of two
   eight-wide sprites, so two halves placed on different bases come out one pixel
   apart and the player is drawn at a sixteenth of his width.
4. **Declaring the sites is free** — with margin sprites off, the run must equal the
   committed baseline exactly (cycles *and* native cycles, 0 callbacks). Extra
   `[[mod_function_hook]]` sites must cost nothing when disabled.
5. **They actually fire** — `off-edge` must be non-zero (3951 over 1200 sweep frames
   with the flicker mod on) and `drawn` must follow it (**33726**; **104519** over 3000
   `sdb_throw` frames). A callback count of 4150 with `off-edge` at 0 means the CRC
   key is wrong. The two `drawn` figures were re-baselined 2026-10-13 against this
   binary; the previous 33855 / 104240 did not reproduce and moved in *opposite*
   directions, while `off-edge` 3951 reproduced exactly, so the drift is on the
   presentation side only.
6. **`peak` and `backstop`** — peak around 64 and backstop **0**. `SDB_MARGIN_PARTS`
   is 24, not 8: objects 9 and 11 legitimately emit more than a character's eight, and
   at 8 they lost sprites on 15 frames per 3000. Any non-zero backstop count means
   the per-object slots are not being reset when `$0617` restarts a list.
7. **The revealed columns carry sprites** — with `overscan` on, the 8 columns the
   PPU hides must differ from a `--margin-sprites 0` run on some frames (344 of
   1200, mean 74 px, max 627). Zero means the band is showing the reconstructed
   background only, i.e. the capture's PPUMASK branch is inverted again.
8. **Mod off = stock** — the catalog present with every feature disabled must be
   byte-identical to no `--mods-root` at all. The launcher's real `state.toml` is
   the reference; a hand-written one silently drops `[feature.values]` if it is not
   attached to the feature it belongs to.
9. **Cadence and continuity — the two checks that actually catch things.** This
   game emits its sprite list over two frames and then leaves OAM alone for three
   to six more, so anything assuming a per-frame rebuild is wrong.
   - *Cadence*: compare frame-to-frame churn of the margins against the native
     centre over a match window. The margin must read zero on **exactly** the
     frames the native centre reads zero. If the margins change on a frame the
     centre does not, something is rebuilding per frame. This caught the blink:
     `drawn` went 1300 → 21068 once the list stopped being cleared per frame.
   - *Continuity*: `SDB_TRACE_CAPTURES=<from>-<to>` dumps the list the compositor
     drew, one line per frame. A character's column must march in even steps
     across builds (−6, −18, −30, −41 …). A sprite appearing where the previous
     build did not have it is what the user sees as "appears in a random
     location".
   Both bugs that produced this were boundary/hold errors, not placement errors:
   presenting the *published* list on a quiet frame (it is one build behind by
   construction — on a quiet frame the build in progress IS the complete one),
   and firing the boundary on `cur == SDB_FIRST_SLOT` once per sprite in a run
   of culled ones, since `$0617` only advances for sprites the game actually
   draws. See research.md §26.
10. **Content mismatches are expected and benign here** — these sites report ~386
   mismatches where the flicker sites report 0. `cyc_hooks_due()` tests the address bit
   alone, so a site at `$8483` is entered whenever the PC is `$8483` in **any** MMC1
   bank; `holds()` then rejects on the bytes in the *mapped* bank. Do not "fix" this by
   changing the CRC — a mismatched site is the content key doing its job. It is proven
   benign by gate 1. The flicker sites report 0 only because their addresses happen not
   to be reached with a foreign bank mapped.
11. **Play it** — off-screen players are most of the pitch, so this is visible on every
   match frame; but the *interesting* states are the culls at the very edge of the
   512-pixel world, where a sprite legitimately lands over backdrop.

### Three traps in the margin placement, each producing a *different* artefact

All three are the same mistake wearing different clothes, and all three were made
here before they were measured. Read this before touching the draw site.

1. **There is one basis and it is `cyc_render_line_scroll_x`.** `$C3/$C4` is not
   it. It is what the game subtracted to make OAM X, it looks authoritative, and at
   composite time it already holds the camera for the frame *after* the one being
   presented — one 6-7 px step away on the frames where the camera is stepping.
   Using it puts standalone margin sprites a whole camera step ahead of the court
   under them. The authority is the game's own behaviour: the machine glues its
   sprites to its background (world-aligned frames match to a mean absolute
   difference of 0.00), so scroll, margin background, machine sprites and margin
   sprites are all on the scroll.
2. **Never split parts across the two bases.** It reads like a refinement — "use the
   machine's basis for the part it holds, the scroll's for the part it does not" —
   and it collapses a character from sixteen columns to one, because its two
   eight-wide halves stop being eight apart. Measured at sweep frame 1060: object
   10's rows 184, 168 and 176 one pixel apart instead of eight, correct again at 1063
   when the bases happened to agree. The intermittency is the tell.
3. **`$04/$05` is the accumulator, not a column.** `$84A1: SEC / SBC #$04` makes OAM X
   from it, so the screen column is that minus 4. Leaving the 4 in moves every part
   across the boundary test below, which both *drops* parts the margin owns (a stripe
   missing through a sprite) and *draws* parts the machine owns (a doubled player).

One more, about the data rather than the arithmetic: **do not try to prove
"no doubling" by matching sprites against OAM.** Every player shares the same tiles,
so tile-and-row matching reports thousands of false hits, and matching the OAM slot
cannot work at all because `$0617` only advances for sprites the game draws, so a
run of culled sprites shares one cursor value. Gate 2 proves it structurally instead
— the blit is clipped to the columns the machine does not own, so the machine's
columns differ by 0 px.

**The compositor only runs when something presents the picture** — `--present-out` or the
window. A headless run with neither prints no `widescreen:` line and no `margin sprites:`
line, which looks like the feature did nothing and is not. Always pass `--present-out` when
testing this. Note this bit twice: it is the same class of mistake as the off-by-default
feature — a green run that was measuring nothing.

Reproduce the off-screen population: `python3 tools/widescreen/objects.py LOG --cull`.

### Engine-change procedure (submodule bumps)

Game checks can't isolate engine regressions, so engine changes get both gates:

1. **Game gate**: TAS 400 + diff MATCH per above (must stay green).
2. **Engine gate**: `python3 tools/blargg/run_blargg.py --tier cpu` — 25/25 (20 PASS + 5 screen-confirmed) required; any flip blocks. See `tools/blargg/README.md`.

**Never run blargg without being asked.** It takes many minutes and it is *upstream's* gate, not this project's: blargg tests nesrecomp's CPU/PPU model against blargg's test ROMs and says nothing about Super Dodge Ball. Running it "to be safe", or as an automatic step of a submodule bump, is wasted time here — the owner runs it when they want to know the new engine still passes. Same for TAS: see the advisory note above. Ask before running either.

### Round-trip decompilation (ROM → Assembly → ROM)

The current `run_roundtrip.py` uses `nesasm` to convert ROM bytes to `.db` directives and reassemble. This is a trivial byte-preservation check — it proves the assembler works, NOT that the recompilation is correct. See `docs/research.md` section 12 for proper verification approaches.

### Verification approach:

The project uses **co-simulation** against MesenCE as the primary verification method:
- Compare cycle state snapshots (`--frame-log`: RAM, CIRAM, palette indices, OAM) byte-exact against MesenCE bins per video-frame (`diff_tas.py`; mirrors masked)
- Compare screenshots tolerance-based (`tools/compare_frames.py`): exact RGB differs by palette emulation, structural diff is the metric, state bytes decide
- Disassemble with `tools/dis65.py` (venv pip is broken — no capstone/py65)

**C→x86→ROM comparison is unsound** — the recompiled C compiles to x86-64, making assembly-level comparison to original 6502 code meaningless. No recompilation project uses this approach.

### 6502 disassembly (for analysis, not verification):

```bash
python3 tools/dis65.py "roms/Super Dodge Ball (USA).nes" 831C 4 $((16 + 0*16384 + 0x831C - 0x8000))
# args: ROM file, hex CPU addr, instruction count, file offset of addr
```

Official opcodes only, linear sweep (no resync after data). System `python3` (venv lacks PIL needed for frame work).

### Build & run verification:

```bash
# Rebuild cycle backend
cmake --build build-cycle -j$(nproc)

# Headless scripted run with screenshots + memory snapshot
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 100 \
  --shot-every 1 --screenshot cyc_shots/shot.png --mem-frame 99 --mem-out mem.txt
```

### Frame comparison pipeline:

```bash
# Generate reference frames with MesenCE
./tools/frame_gen/run_frames.sh "roms/Super Dodge Ball (USA).nes" 100 nes_reference

# Generate cycle-backend frames (shot_00000.png scheme; rename to frame_XXXX.png first)
./build_and_run.sh --frames 100 --shot-every 1 --screenshot cyc_shots/shot.png

# Compare
python3 tools/compare_frames.py --ref nes_reference --recomp cyc_shots --frames 100
```

**Critical: Frame 11 is the title screen.** Frame 0 is blank. Pixels never match exactly across renderers (palette emulation differs) — compare structure tolerance-based, and let state bytes (`diff_tas.py`: RAM/OAM/palette/CIRAM byte-exact) decide. Do NOT cite frame 0 as success.

Current: cycle state matches Mesen byte-exact from boot through team-select (400 TAS frames); title pixels are structurally the same screen in both renderers.

### MesenCE testrunner (Lua API — MesenCE 2.2.1):

The MesenCE 2.2.1 Lua API is completely different from older versions. **Never use `emu.frame()`, `video.exportFrame()`, or `input.set()`** — these do not exist. (`os.getenv` works and is used for script parametrization.)

Correct API:
- `emu.addEventCallback(func, emu.eventType.endFrame)` — register callback
- `emu.getScreenBuffer()` — returns table of RGB integers
- `emu.getScreenSize()` — returns `{width, height}`
- `emu.setInput(subport, port, table)` — set controller input
- `emu.stop()` — stops emulation
- **`emu.resume()` and `emu.reset()` crash with exit code 255** in --testrunner mode

When launched with `--testRunner rom.lua`, MesenCE runs frames automatically. The Lua script only registers callbacks — do NOT call `emu.resume()` or `emu.step()`.

Required `settings.json` for headless operation:
- `EnableTestMode: true`
- `Debug.ScriptWindow.AllowIoOsAccess: true`
- `Debug.ScriptWindow.ScriptTimeout: 60`
- `Debugger.BreakOnCpuCrash: false`
- **Remove `mesen.lock`** from `external/mesence/` before running

**MesenCE reads `settings.json` from the directory its binary sits in — NOT from `$HOME`.**
Overriding `HOME` does nothing; the real user config is silently used instead. The sandbox must
own the *executable*: copy `Mesen` + `MesenCore.so` + `lib*.so` + `settings.json` into a temp dir
and run that copy. Verified by control: `Nes.Region=Pal` only moved cycles/frame 29781 → 33248
once `settings.json` sat beside a copied binary. `tools/frame_gen/run_frames.sh` does this.

Two more traps in the same script, both of which look like "Mesen produced nothing":
- **`xvfb-run` word-splits the program path.** It execs via `sh -c`, so a path containing spaces
  (this repo's own path has one) breaks *even when quoted*. `cd` into the sandbox and run `./Mesen`.
- **After `cd`, relative ROM paths stop resolving.** Pass an absolute ROM path.

Also: `PpuExtraScanlinesBeforeNmi` exists under **both** `Nes` and `Snes` in the shipped settings.
A naive `grep` matches whichever comes first in the file and reports the wrong console's value —
read it back with a JSON parser scoped to the section. See `docs/research.md` §23.

### MesenCE as Oracle

Frame reference generation uses the **prebuilt MesenCE binary** (`external/mesence/Mesen`) with `--testRunner` mode and a Lua script (`tools/frame_gen/export_frames.lua`). Deeper oracle work (PPU dumps, per-NMI flag logs) uses `tools/frame_gen/oracle_dump.lua` — see `docs/research.md` §16. Headless runs need `xvfb-run` plus a sandbox that owns the Mesen *binary* and its `settings.json` (see the config-location note above; isolating `HOME` alone does not work).

### Known limitations:
- The NESRecomp toolchain is one-way (ROM → C). No built-in way to compile C back to ROM.
- `nesasm` works via Python API only; CLI `nesasm asm` has a `.db` directive bug
- MesenCE testrunner exits with code 255 when `emu.resume()` or `emu.reset()` is called

## RetroPortingToolKit Rules

Reference: https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs

### House Invariants (from 05_agents/02_house-invariants):
1. **No stubs** — Never create stub functions that return default values
2. **Fix the tool, not generated code** — If generated code is wrong, fix the recompiler or game.toml config
3. **Find first divergence** — When a port breaks, find the FIRST frame where it differs from the oracle
4. **Classify the layer** — Is the divergence in codegen, runner, config, or game logic?
5. **Fix the source** — Fix the root cause, not the symptom
6. **Dispatch misses are blocking** — Every `call_by_address` target must be a recompiled function
7. **Never edit generated/ directly** — Always regenerate via NESRecomp
8. **Align on hardware events** — Prefer VBlank, DMA completion, timer overflow over frame numbers
9. **Use always-on traces** — Ring buffers recording before the bug happened
10. **Unknown is allowed** — Guessing is worse than saying "unknown"

### Verification Rituals (from 05_agents/03-verification-rituals):
- Dispatch misses must be zero
- Co-sim must match (RAM state at checkpoints)
- TCP checks must pass
- Frame hashes must match
- A green build is weak evidence
- Use hardware events, not frame numbers

### Failure Modes (from 05_agents/04-failure-modes):
1. **Stubbing** — Creating fake functions instead of fixing the real issue
2. **Wrong fix** — Fixing the symptom, not the cause
3. **Ignoring first divergence** — Not finding the first frame where things break
4. **Overfitting** — Making changes that work for one frame but break others
5. **Config blindness** — Not checking game.toml for missing directives
6. **Generated code editing** — Editing generated/ files directly (they get overwritten)
7. **Submodule staleness** — Not keeping the nesrecomp submodule up to date
8. **Bank switching bugs** — Overriding MMC1 bit-bang with wrong dispatch override
9. **NMI handler issues** — Calling RTI-based functions as C functions
10. **Input not wired** — Not connecting runner input to game controller reads

## Configuration Files & Locations

| Path | Purpose |
|------|---------|
| `CMakeLists.txt` | Game project (root). Wraps the submodule's `runner/cyc/project.cmake`, adds `CXX` + `MODS` + `GAME_ID` + `HOST_EXTRAS`, copies `mods/` beside the executable |
| `src/game.toml` | Recompiler config: display `name`, a `cycle_seed_file`, plus twelve content-keyed `[[mod_function_hook]]` sites (`length`+`crc32`, never `bytes`) |
| `src/sdb_host_extras.c` | Our mods: the character-flicker mod (OAM sprite capture + host-side presentation + object-culling override), the PPU-overclocking mod (section 5; sets `cyc_set_extra_scanlines`) and the widescreen compositor + margin sprites (section 4). Only game-specific C in the tree. Owns the single `cyc_host_extras()` |
| `src/cycle_seeds.txt` | Recompiler discovery roots for the sprite emitters — a mod hook site only exists in recompiled code |
| `tas/vectors/sdb_idle.input` | Reproduction vector for the flicker fixes (idle into a match; B blinks at 865-905) |
| `tas/vectors/sdb_throw.input` | Stress vector for the mod (throw from frame 446; exposes runaway bookkeeping + missing emitters) |
| `tas/vectors/sdb_pan.input` | 24-frame LEFT/RIGHT alternation from frame 500; a still camera hides every margin boundary artefact |
| `tas/vectors/sdb_sweep.input` | 72-frame holds alternating, i.e. the full 512-pixel world each way. Every margin artefact found so far needed a character to walk clean across an edge |
| `tas/vectors/sdb_bar.input` | Court input repro (research §27): a left tap at 617 starts a run, then right, then down ×4. Built on the measured 412/594/617 timings — **input aimed before 594 is dropped** |
| `tas/vectors/sdb_beanball.input` | **Recorded**, not written: a played Bean Ball run across the full map, the repro for the open flicker-mod list degeneration (research §26, plan item 14). Replay in a window, not headless |
| `tas/vectors/sdb_probe.input` | Diagnostic, not a test vector: LEFT in 4-frame pulses from frame 600, which is how **594** was measured as the frame the court starts accepting input (research §27 timings above). Keep it — it is the reproduction for the timing facts, and the facts cost three failed repros to establish |
| `tools/flicker/` | Analysis scripts behind the research.md §2/§2.1 claims (OAM/record dumps, ref-finder, memdiff) + toolkit README |
| `tools/widescreen/` | Analysis behind research.md §25-26 (`nametable.py` — the court's 512-px looping world and the `$23C0` attribute region; `blank_extent.py` — the per-row/per-column ink measurement the overscan crop is chosen from; `objects.py` — the per-object display records at `$0618`, and how many objects the game culls off-screen; `margin_track.py` — the per-sprite, per-frame audit of the margin sprites, and the gate to reach for when something looks wrong near an edge; `band_fault.py` — bar discontinuity across the revealed band, split into reconstruction vs margin-sprite; **`json_trace.py` — reads the `SDB_TRACE_JSON=<from>-<to>` stream, one JSON object per line, which is what plan item 14 needs (`flicker` puts live OAM and the mod's published list side by side in one frame)**; **`cycframe.py` — the shared `--frame-log` reader the other four sit on, since four copies of the header parser meant a format change would break three tools silently**. **`band_fault.py`'s `BAR` colour is a placeholder and must be sampled before its output means anything** — a wrong colour reports "continuous" on every frame) |
| `tools/play_record.sh` | Play the game in a window and record the session as a replayable `--input` vector plus `SDB_TRACE=1` and per-frame screenshots, into `tmp/<name>/`. Pins `WindowScale`/Fullscreen/VSync, or the screenshots make the game run in slow motion. The only way to find the band bugs — see the recorded-playback section above |
| `tools/overclock/` | PPU-overclocking oracle harness behind research.md §23 (MesenCE sandbox + stock-vs-OC comparison). **Its self-check matters: if cycles/frame stays 29781 for a non-zero `Before NMI`, the setting never applied and the run is worthless** |
| `mods/packages/super-dodgeball.character-flicker/` | Mod package — `manifest.toml` only (data-only by design; implementation is `src/sdb_host_extras.c`). Off by default, toggleable in the launcher's Mods screen; PolyForm NC, same as the project |
| `mods/packages/super-dodgeball.ppu-overclock/` | Mod package for PPU overclocking (research.md §23). Manifest-only like the above; has an integer `[[option]]` so the launcher exposes the scanline count. **Needs `patches/006_ppu_overclock_mmc1`** or the backend refuses on mapper 1 |
| `mods/packages/super-dodgeball.widescreen/` | Mod package for widescreen (research.md §25). Manifest-only; two features — `widescreen` (an `aspect` **choice**: stock/16:9/21:9/32:9/fit) and `overscan`. Needs **no patch**: the engine already ships the widths and the compositor API |
| `external/nesrecomp/` | NESRecomp submodule (recompiler + runners) — pinned `origin/master`; **dirty tree is expected** when `patches/*.patch` is applied by `setup.sh` |
| `external/mesence/` | MesenCE emulator source/binary |
| `external/recomp-ui/` | recomp-ui submodule (launcher + in-game menu) — **pinned, never floated**; MIT, own license |
| `patches/` | Active runner patches applied to the submodule by `setup.sh` (`*.patch`, strictly additive, documented in `active-patches.md`); `archived/` holds the retired legacy-runner ones `setup.sh` ignores |
| `tools/dis65.py` | Dependency-free 6502 disassembler for entry verification (ROM + addr + count + file offset) |
| `tools/frame_gen/memwatch.lua` | MesenCE oracle probe: who-writes (with PC) for RAM addrs + optional NMI flag log (`WATCH`, `MAXF`, `START0/1`, `NMI_LOG`) |
| `tools/frame_gen/oracle_dump.lua` | MesenCE `--testrunner` oracle: frame-N PPU dump + per-NMI `$0100/$0106` log (see `docs/research.md` §16) |
| `tools/frame_gen/` | Frame export Lua script and wrapper |
| `tools/blargg/` | blargg test-ROM harness on the cycle backend (`run_blargg.py` + `manifest.json`); per-ROM builds in `build-cyc-blargg/` (gitignored), ROMs via `--fetch` into `external/nes-test-roms/` (gitignored) |
| `tools/tas/` | TAS differential harness on the cycle backend (bk2/fm2/`.input` converters, `run_tas.py` + `diff_tas.py`, `mesen_tas.lua`, and **`judder.py` — re-derives research §27's pan-judder attribution in one command**); vectors in `tas/vectors/`, runs in `tas/runs/` (gitignored) — see `tools/tas/README.md`, research §21 |
| `build-cycle/` | Cycle backend build (gitignored; `./setup.sh` builds it, `./build_and_run.sh` runs it) |
| `config/settings.json` | MesenCE runtime configuration (gitignored) |
| `pyproject.toml` | Python project configuration |
| `roms/` | ROM files (gitignored) |
| `venv/` | Python virtual environment |
| `build/` | Build output (gitignored) |
| `config/` | MesenCE settings (gitignored) |

## Keybinds

- Player 1: A=Z, B=X, Select=\, Start=Return, Up=Up, Down=Down, Left=Left, Right=Right
- Gamepad 1: A=a,b; B=x,y; Select=back; Start=start; Up/Down/Left/Right=d-pad

## MesenCE Lua API Quick Reference

### Memory
- `emu.read(addr, memType, debug)` — read bytes
- `emu.write(addr, value, memType)` — write bytes
- `emu.read16(addr, memType, debug)` — read 16-bit
- `emu.write16(addr, value, memType)` — write 16-bit
- `emu.read32(addr, memType, debug)` — read 32-bit
- `emu.write32(addr, value, memType)` — write 32-bit
- `emu.convertAddress(addr, memType)` — convert address
- `emu.getLabelAddress(label)` — get label address
- `emu.getMemorySize(memType)` — get memory size

### CPU
- `emu.getCpuState(cpuType)` — get registers
- `emu.setCpuState(state, cpuType)` — set registers
- `emu.getCpuCycleCount()` — get cycle count
- `emu.getMasterClock()` — get master clock

### Events/Callbacks
- `emu.addEventCallback(func, eventType)` — register event callback
- `emu.removeEventCallback(ref, eventType)` — remove callback
- `emu.addMemoryCallback(func, callbackType, addr1, addr2, cpuType, memType)` — memory watch/breakpoint
- `emu.removeMemoryCallback(ref, callbackType, addr1, addr2, cpuType, memType)` — remove watch

### Video
- `emu.getScreenSize()` — returns `{width, height}`
- `emu.getScreenBuffer()` — returns table of RGB integers
- `emu.setScreenBuffer(buf)` — set screen buffer
- `emu.getPixel(x, y)` — get pixel color
- `emu.takeScreenshot(path)` — save screenshot

### Input
- `emu.setInput(buttons, port, subport)` — set controller input
- `emu.getInput(port, subport)` — get current input
- `emu.isKeyPressed(key)` — check key
- `emu.getPressedKeys()` — get all pressed keys

### State
- `emu.getState()` — full serialized console state
- `emu.setState(state)` — restore state
- `emu.createSavestate(path)` — create save state
- `emu.loadSavestate(path)` — load save state
- `emu.reset()` — **crashes in --testrunner mode (exit 255)**
- `emu.stop(reason)` — stop emulation
- `emu.breakExecution()` — pause execution
- `emu.resume()` — **crashes in --testrunner mode (exit 255)**
- `emu.step(stepType)` — step one instruction

### Debug
- `emu.getCdlData(memType)` — Code/Data Logger coverage
- `emu.getAccessCounters()` — get access counters
- `emu.resetAccessCounters()` — reset counters
- `emu.getRomInfo()` — ROM metadata
- `emu.getScriptDataFolder()` — script folder path

### Event Types
- `emu.eventType.inputPolled` — input poll time (safe for `emu.setInput`)
- `emu.eventType.startFrame` / `emu.eventType.endFrame`
- `emu.eventType.Nmi` / `emu.eventType.Irq` / `emu.eventType.Reset`
- `emu.eventType.StateLoaded` / `emu.eventType.StateSaved`
- `emu.eventType.CodeBreak` / `emu.eventType.HaltStarted` / `emu.eventType.HaltEnded`

### Memory Types (NES)
- `emu.memType.nesInternalRam` — $0000-$07FF
- `emu.memType.nesPrgRom` — PRG ROM
- `emu.memType.nesChrRom` — CHR ROM
- `emu.memType.nesNametableRam` — $2000-$2FFF
- `emu.memType.nesPaletteRam` — $3F00-$3F1F
- `emu.memType.nesSpriteRam` — $0000-$00FF (OAM)
- `emu.memType.nesSecondarySpriteRam` — secondary sprite RAM

### CPU Types
- `emu.cpuType.nes` — NES 6502

### Settings for Headless
- `EnableTestMode: true`
- `Debug.ScriptWindow.AllowIoOsAccess: true`
- `Debug.ScriptWindow.AllowNetworkAccess: true`
- `Debug.ScriptWindow.ScriptTimeout: 60`
- `Nes.RamPowerOnState: 1` (AllZeros)
- `Nes.RandomizeMapperPowerOnState: false`
- `Nes.RandomizeCpuPpuAlignment: false`
- Remove `mesen.lock` from `external/mesence/` before running