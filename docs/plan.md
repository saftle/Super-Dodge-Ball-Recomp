# Super Dodgeball NES Recompilation — Project Plan

> Cycle backend is the default and only play path. Team-select verified byte-exact vs Mesen; blargg 25/25. Flicker fix **shipped** for all causes (opt-in mod until a full playthrough is tested). PPU overclocking **shipped** (opt-in mod; deliberately changes timing). Widescreen **shipped** (opt-in mod, 16:9 / 21:9 / 32:9), and its margins now carry sprites as well as court. Margin sprites found by a *played* session rather than a script — `--record-input` + `tools/play_record.sh` (item 4, research §26) — and the last of them, a duplicated sprite at the seam that flickered while the camera panned, turned out to reproduce **headlessly on `sdb_pan` all along** (research §26). The sprite judder while the camera pans is **resolved and it is the game's** — MesenCE reproduces the same two-frame offset, so nothing here is broken (item 12, research §27). **Cheats shipped** (item 17, research §32): RetroArch `.cht` files with all four code kinds including Game Genie, which needed a new engine primitive (`patches/008`). The **instant-win cheat is not solved** (item 18) and four attempts at it are refuted by measurement — read that item before spending time on it; save states already deliver what it was wanted for. **One** open question left unattributed: the Bean Ball sprite-list degeneration (item 14). Item 15 is **fixed**, and so is the seam offset it left behind. Frontier: full-movie TAS vectors (level-switch screens).

## Where Things Stand

- **Backend**: cycle-accurate (`./build_and_run.sh` → `build-cycle/nes_game`, upstream `runner/cyc`). Stock ROM on accurate timing. Three presentation/timing mods live in `src/sdb_host_extras.c`: the character-flicker mod (truncation half presentation-only; the culling half makes the guest emit sprites it had dropped, so its timing shifts slightly), the PPU-overclocking mod (deliberately changes console timing so the guest runs faster) and the widescreen compositor (presentation only; the guest is never touched). A **fourth**, `super-dodgeball.cheats`, lives in its own `src/sdb_cheats.c` because it is a different kind of thing: it *writes* guest RAM. It reaches the player through the in-game menu (Escape → Cheats), not the Mods screen — the Mods entry only says the game has cheats. VSync is a launcher Graphics setting (patch 007), not a mod. The legacy runner is retired (files removed 2026-10-02; the pre-squash history survives in this repo's `refs/stash` and in `~/Documents/backups/superdodgeball-private-backup.bundle`; game knowledge preserved in research §§18–19).
- **Verification**: TAS 400 frames reach team-select with RAM/OAM/palette/CIRAM byte-exact vs Mesen from boot (`tools/tas/`); blargg cpu tier 25/25 (20 RAM-text PASS + 5 screen-confirmed).
- **Upstream**: maintainer is moving to cycle-default; legacy goes antiquated. Our migration is done early. Patch 006 (PPU overclocking for MMC1) is the live upstream proposal, and patch 008 (ROM read substitution) is the second — see `patches/active-patches.md`. The legacy 002 S-latch guard is drafted below.
- **Frontier**: court BG past team-select is unverified on cycle.

## Open Items
1. Court BG past team-select — substantially verified on cycle: `tas/vectors/sdb_throw.input` plays matches end-to-end (throws land, digits show, menus render correctly, 8000-frame stress clean). What remains is the full-movie TAS path (TASVideos #4976 actually *wins* rounds — needed for level-switch screens), still open.

2. Propose 002 S-latch guard upstream (draft below), and 006 PPU-overclock MMC1 gate (see `patches/active-patches.md`).

3. ~~Widescreen~~ — **DONE.** `super-dodgeball.widescreen`, opt-in, off by default. No engine patch: the cycle backend already ships the widths (`common/nes_video_geometry.h` — 426/560/854) and the compositor API. One compositor in `src/sdb_host_extras.c` fills the margins from the nametable, CHR, palette and the frame's own per-line scroll; the stock 256 columns are copied back over the centre so they stay pixel-identical to the unmodded game, and a per-frame seam check against `cyc_render_background()` pillarboxes anything that fails (so title screens and menus fall back for free). Reconstruction agrees with the machine to 1-2 px of 61440. Enabling it also reveals the 8 blank left-hand columns the game hides with PPUMASK (blank in 2112/2112 measured frames); vertical is not offered because one wipe-transition frame uses those bands. `overscan` was a second feature and was **folded in**: the same compositor draws all three parts, so the combination the toggle permitted — widescreen on, reveal off — is a wide frame with a black band at its edge, which is the artefact widescreen exists to remove. Same call already made for margin sprites (item 4). `SDB_OVERSCAN=0` in the environment still forces the clip back for the gate 5 measurement. Margins past the court's own bounds are backdrop, and **those bounds are `[40,480)` — read out of the game, not fitted to it**. `$C3/$C4` is the camera (`$823E/$8246: SEC / SBC $C3 / SBC $C4`), clamped at `$E6AB` to `[$28, $E0]` = `[40,224]`, so the machine can only ever display `[40, 224+256)`. Two immediate operands, **no per-level table** — the first attempt measured the nametable's end-wall band instead, which was right on the stadium by luck and gave the London course *no* bound at all. That band's tiles are not the backdrop either: `$C0` is 70 set bits in CHR and renders an arch, which is what put a row of blue arches floating on black at both ends. The four bugs the seam check caught, the wideNES comparison, the overscan measurement and the camera-clamp bound are in research.md §25.
3b. **Camera lock** — **built, default off, needs playing** (research §28). `widescreen` gained a `camera` choice: `follow` (stock, and what every other baseline in this file was measured against) and `locked`, which makes the canvas a fixed window on the court instead of one centred on the game's camera. **An option, not a new package**, for the reason `overscan` and `margin-sprites` were folded in: the manifest format has no feature dependency (`MOD_PACKAGES.md` — features are independently enabled), so a separate feature would be enabled on its own and do nothing at stock width, which is verbatim the "feature looks like it did nothing" failure this project already paid for twice. The court is `[40,480)` = 440 columns and the camera is clamped to `[40,224]`, so at 21:9 and 32:9 the whole court is **already on screen at every camera position play reaches** (measured 102..180) — the panning adds motion and no information, and that is the whole argument. `native_x0` became per row because the game splits its scroll mid-frame (rows 0-47 hold scroll 0 and carry the skyline/scoreboard), so anchoring the whole canvas would slide the HUD 6-7 px a step. Gate `tools/widescreen/anchor_gate.py`: **339/339** at both aspects, one anchor value, 0 off-canvas frames, machine's own 256 columns identical on every frame; 1800 frames of `sdb_throw` give one anchor over 1752 composed frames; follow is byte-identical to the pre-change build on 701 frames. **Played once, and playing found two bugs no scripted gate could**: every menu rendered 132 columns left of centre (a menu's scroll is 0, which is outside the game's camera window, so the court bound does not describe it — the test is now `[40,224]`), and the health bars and names slid with the camera (the flicker mod's own `sdb_present` stamped all 240 rows at the court's single x0, while the compositor places the court and held bands at two). Both fixed and measured; the second took an alignment scan to pin down — at sweep frame 649, where the bands are 48 apart, the flicker-on run's rows 0-48 were the flicker-off run's shifted exactly +48, and are now **shift 0, 0 bytes differ**. **Lesson: every scripted gate here runs with the flicker mod off, and with it off the wide path copies the compositor's canvas straight through, so no single-mod gate can see a compositor bug that only shows with a second mod on.** **Open: it has been played once**, and a locked camera promotes the margin path from edge case to main screen, so §26's eleven margin bugs all get a bigger audience. A pre-existing asymmetry turned up on the way and is **fixed**: `--widescreen` on the CLI never enabled the clipped-column reveal, so it presented an 8-column black gutter the launcher path did not — the exact artefact widescreen exists to remove. The reveal now has one resolver (`sdb_ws_reveal_ask`), precedence **explicit `--overscan` → `SDB_OVERSCAN` → on**, stock width off. Both obvious shapes of that fix are wrong and both were measured: stamping the reveal from the `--widescreen` handler makes the two flags order-dependent, and reading `SDB_OVERSCAN` in the activation plugin regresses the A/B on the CLI path because the plugin does not run there (280 of 701 frames). Now **0 of 701** CLI-vs-launcher, 280/701 default-vs-`SDB_OVERSCAN=0`, and the camera lock re-gated after the change at both aspects (339/339).

4. **Sprites in the margins** — **DONE** (was the one part of widescreen not done), and then **audited to destruction**: eleven boundary/placement bugs, ten of them found only by recording and replaying a played session (`--record-input`, research §26). Part of the `widescreen` feature itself, not a separate toggle: a margin with no players in it is not a wider game. The `$0618` records were the wrong place to look: the game places every sprite in a **16-bit** screen coordinate (`$04/$05`, `$06/$07`) and *then* hides anything outside `[4, 255]` by writing Y=$F8, so the tile, flips, palette and both halves of the position are all computed and only the byte truncation at `$84A1` discards it. Two hook sites (`$8483` the strip walk, `$8302` the three single-sprite emitters — same cull, different tile source), read at the game's own decision, plus `$8231` for the object index. **40% of objects are off-screen in every measured frame.** Presentation only, proven: a `--frame-log` with the feature on is byte-identical to it off (RAM/CIRAM/OAM/palette/picture) at an unchanged cycle count on all four vectors, with and without overscan, and the presented machine columns differ from a `--margin-sprites 0` run by **0 pixels** on every frame at 16:9, 21:9 and 32:9. No new `cycle_seed_file` line was needed — `$8483` is in the routine `$821E` already seeds.

   What took the work was not the capture but **placement**. Six artifacts, all
   boundary artifacts, all needing the camera to move: players blinking out (most
   off-screen objects are not on `$067C` at all, so nothing measured in emissions
   survives as a lifetime), a stripe missing through a sprite (the accumulator's
   bias of 4 applied twice), doubled players, a headless body at the edge (the PPU
   hides eight columns the game never culls, and with the reveal drawing them
   the band has to draw them), a fragment instead of a character (one `$8231`
   visit emits four of a character's eight sprites), and an older animation frame
   pinned in the band. The basis is `cyc_render_line_scroll_x` — but *not* only
   that, and getting that wrong cost the eighth bug: on about a quarter of the
   camera-stepping frames a whole object sits **exactly one camera step** from the
   machine's copy of it, which is the seam doubling, and `margin_track.py` cannot
   see it because it audits the margin against itself. Fixed by moving such an
   object onto the machine's columns. The gate went through two shapes: first the
   disagreement had to be exactly the camera step (which admitted a crowd
   mismatch and refused the real d=0 case), now any object sitting wholly outside
   the machine's columns is corrected and anything straddling them is left alone.
   Three more edge bugs followed in the same area — a whole character blinking on
   the culling flag the identity check mistook for a recycled slot, a 4 px join
   with the camera still, and a 1 px row leapfrog — all in research §26. The audit
   is `tools/widescreen/margin_track.py` — **one `cshift` on the sweep (eyes-on,
   exit 0), none on the pan vector** — and the gate that actually found the first
   of these is the whole-object delta table in research §26. Research.md §26.

5. ~~Flicker fix~~ — **DONE, all causes.** `super-dodgeball.character-flicker`, opt-in from the launcher's Mods screen (off by default: the culling half shifts guest timing and the level-switch screen shows ghosts with it on — needs a full playthrough test). (a) Hard 64-sprite OAM budget with a `BEQ` bail on `$84B6` — stops emitting. (b) Display-list culling at `$067C` (`$43` bits 4-6 `$20`), three gates overridden. (c) Two sprite classes no bump hook could see: floating damage numbers (bank-3 pair emitter, `ADC #$08` step) and team-select position numbers (bank-4 block copier, no bump at all) — hooked per-emitter plus prefix carry-over and done-freshness for sitters. Ten content-keyed `[[mod_function_hook]]` sites + host-side redraw + six `cycle_seed_file` roots (hook sites need recompiled code; all three emitters were on the interpreter). Truncation: body-ink stdev 8.7 → 0.8 (truncation-only build). Culling: B's box-ink stdev 55.0 → 16.6 with 0 frames still culled (re-verified current binary). Numbers: 108/108 frames steady. Traps and the guest-cost caveat in research.md §2 and §2.1.

6. **Run the gates on patch 006 before it lands.** A submodule change is exactly when both are wanted: TAS 400 + `diff_tas.py` (must stay MATCH) and blargg cpu 25/25. Not yet run — AGENTS.md says ask first. The patch is strictly additive and the 600-frame stock run is byte-identical with and without it, but that is not the same as the full gates.

7. ~~PPU overclocking~~ — **DONE.** `super-dodgeball.ppu-overclock`, opt-in, off by default (it changes console timing, so hardware parity must stay the default). Integer `[[option]]` so the launcher exposes the count. Needs patch 006; without it the backend refuses on mapper 1. Measured: 2.0x cycles/frame, and the guest populates its display list on 565 of 800 frames vs 316 stock. Research section 23.

8. ~~VSync~~ — **DONE as a launcher setting** (it started as a `super-dodgeball.vsync` mod and moved: vsync is host-global, and recomp-ui already had the Graphics row waiting behind `has_vsync`). Pre-boot Graphics → VSync toggle, persisted per-player in `config.ini` `[Display]`; needs patch 007. Presentation only — guest clock untouched. Windowed `--exit-after 60` exit 0 both ways via the config key, and the launcher path is confirmed (Graphics → VSync → Play prints the vsync line). Research section 24.

9. Resolution honesty: wider canvas only — no upscaling path exists upstream (CHR PNG replacement at most). Don't promise HD.

10. Docs reconciled to cycle (README, AGENTS.md, plan, research §§21–25, harness READMEs); research §§18–19 stay as game-knowledge history.

11. **Flicker follow-ups** (not blocking): the mod draws sprites with no scanline evaluation at all, so it also drops the NES 8-per-scanline rule — real hardware behaviour some may want kept, which would be an `[[option]]` on the same feature rather than a second package. ~~The blit also ignores OAM attribute bit 5; this ROM never sets it.~~ **That was wrong and it is fixed**: OAM slot 0 carries attr `$23`, bit 5 set, and the blit was painting it over the court the machine leaves showing background through — 3 px a frame. It now resolves bit 5 against `cyc_frame_bg_opaque()` the way `cyc_render_sprites` does. The multiplexer above is *also* real hardware behaviour: blitting all of OAM over the sprite-free background reproduces the machine's own picture to 4 px over 387 live frames, every one of them the machine dropping a lower-priority slot under a transparent one.

12. ~~**Sprite judder while the camera pans**~~ — **RESOLVED, and it is the game's.** (research §27.) Visible in the **stock** picture, no mods: a player holds still for several frames then jumps, and the jump is not the same size as the court's, so he detaches from the floor and snaps back. The metric is `world_x = OAM X + $C3/$C4` for the player sprite (OAM y=143, tile `$20`): the player walks left one 6-7 px step and the camera walks right one 6-7 px step, so if the game did both on the same frame they would cancel and `world_x` would be constant. Measured, with the oracle run and one command:

    | | camera steps | sprite steps | N | world_x base | blips |
    |---|---|---|---|---|---|
    | cycle backend | 601, 608, 615, 622 | 603, 610, 617, 624 | **2, 2, 2, 2** | 207 | 15/34 |
    | MesenCE 2.2.1 | 603, 610, 617 | 605, 612 | **2, 2** | 207 | 6/20 |

    Same N, same base, same blip. **A second, independent emulator reproduces it exactly, so the two-frame offset is the game's own cadence and nothing here is broken.** Nothing to fix upstream and nothing to fix in a mod; §27 keeps the reasoning, and `tools/tas/judder.py` re-derives it in one command so nobody re-chases it.

    Two traps that cost most of the time, both recorded in §27. The harness had **never** been logging scroll — five of `mesen_tas.lua`'s state columns are `null` because the keys are namespaced (`ppu.xScroll`, not `ScrollX`), which is exactly what research §16 warns about; fixed in `7cfa35a`. And with the key fixed, **`ppu.xScroll` is still not the court's scroll** — it reads 0-7 through the whole pan while `$C3/$C4` goes 128→154, so treating it as the scroll gives a confidently wrong answer. The metric avoids both by using the game's own camera variable, which both sides already dump in RAM. Do not measure this by correlating pictures either: a column-SAD over the flat court locks onto the wrong feature.

    The sides agree at frame 600 (camera 134 on both) and diverge by 650, which is the known RNG divergence — so the usable window is 597-618, not the whole pan.
14. **Bean Ball: the flicker mod's sprite list degenerates while running across the full map** (research §26, **OPEN — shelved at the owner's request, not closed**). Everything needed to resume is below, including what the owner said in their own words and how to get the capture again without referring to any scratch folder.

    ### How it presents, from the owner's description

    Game mode **"bean ball"**, which lets you run across the **entire map** rather
    than a half-width court. It works in general — the failure is specific to
    **running from one end of the screen to the other**, and it looks like the
    implementation "plays catch up and screws up". The owner's frame-by-frame
    account, verbatim in substance:

    | frame | what they saw |
    |---|---|
    | **658** | everything normal |
    | —→ **679** | progressively: **two sprites on the left are completely covered** |
    | —→ **692** | a **third** sprite is completely covered as well |
    | —→ **708** | a sprite **all the way across the screen to the left** is covered, **leaving just the player sprite visible** |
    | —→ **714** | **the player sprite itself gets cut in half** |

    Two properties matter and are the owner's, not inferred. **The order is
    random** — which things get hidden differs on a different playthrough, so a
    future agent must not expect the same sequence or the same frame numbers.
    And **"progressively"** is the tell: sprites are lost one at a time over ~56
    frames, not all at once, which rules out a single bad transition and points at
    something degrading.

    ### What the trace shows (measured, from the owner's capture)

    Flicker mod per-frame line `sdb <frame> <peak-list> <overflow> <published-list>
    <saw-$04> <backstop>`, frames 658→714. The list **decays monotonically as the
    player runs**:

    ```
    658:54  662:51  666:44  670:37  674:34  678:30  682:25  686:22  690:22
    698:21  702:17  706:13  710:10  714:14          (peak-list)
    ```

    **53 → 10, then a small recovery.** `overflow` is 0 and `backstop` is 0 on
    every one of those frames. So it is **not** §2.1's runaway-capture-list bug
    running backwards, and it is not the `SDB_LIST_LIMIT` backstop firing.

    Ruled out by the same run:

    - **Not the widescreen compositor.** `widescreen: 1183 composed, 1 pillarboxed,
      seam diff 2 px` — the background is essentially perfect, and the screenshots
      at 658 and 714 show the building, tree and fence fully drawn and correctly
      scrolled in both. The margins are black in Bean Ball (its world is not the
      512-px stadium) which is expected, not the fault.
    - **Not the margin sprites.** `margin sprites: 1207 captured (872 off-edge),
      2018 drawn, peak 9` — and `sdbm` is **0** on almost every frame in the whole
      655–716 window. The margin draws essentially nothing here, so it cannot be
      what is covering anything.
    - **Not the camera.** `scr == cam` for 660–690 (`scr=40` throughout); the
      camera starts moving only around 696. The loss begins at 658, *before* the
      pan.

    The mechanism is straightforward once stated: **the flicker mod replaces the
    machine's own sprites with its own captured list** (`cyc_render_capture_background`
    + `sdb_blit`). So a short list *is* missing sprites, whatever caused the list
    to be short.

    ### The one question that has to be answered first

    **Not established: whether the game emits fewer sprites in Bean Ball, or the
    mod drops them.** These need opposite fixes — one is a fact about the ROM to be
    documented, the other is our boundary detection to be corrected — and guessing
    wrong wastes the whole investigation. **Do not pick a branch and start editing
    before measuring this.**

    `--frame-log` cannot answer it, and the reason is worth not re-learning:
    `--frame-log` sets `headless = true` (`cyc_host.c:1100`), and the window and
    headless hosts **do not run the same guest state** (frames 0–3 match, frame 4
    diverges — research §26). An OAM dump from a headless run describes a
    different game.

    **The instrument now exists: `SDB_TRACE_JSON`.** `src/sdb_host_extras.c`
    emits one JSON object per line in two kinds — `f` at frame begin, carrying
    the flicker counters *and every live OAM entry for that frame*, and `m` at
    composite. `python3 tools/widescreen/json_trace.py flicker LOG` prints live
    OAM against the mod's published list in the same frame, which is exactly
    this question, and only offers the interpretation on a ≥30% collapse so it
    does not cry wolf on a normal run's jitter. It exists because the *human*
    trace had twice lied to `margin_track.py` — a missing newline and then a
    misplaced line closer, each leaving the audit reporting "no lines" on a
    healthy run — so this is plan item 14's instrument reading a structured
    stream instead of a regex. See `tools/widescreen/README.md`.

    **So: replay the vector in a WINDOW with that flag on**, and read
    `flicker`. The decision rule, both branches spelled out so the next agent
    cannot rationalise either way:

    - **OAM holds ~50 sprites while the mod's list holds 10 → it is ours.** The
      game emitted them and the mod lost them. Go to the list bookkeeping: the
      `$0617`-going-backwards boundary, the prefix carry-over, and the
      done-freshness rule (`s_frame - s_done_frame <= 1`), all of which are in
      `sdb_restart`/`sdb_publish`/`sdb_present` in `src/sdb_host_extras.c`. The
      prior art is §2.1's runaway list, which was the same code failing the other
      way.
    - **OAM holds 10 too → the game is not emitting them** in this mode at this
      camera position, the mod is faithful, and the honest outcome is a recorded
      finding rather than a fix. Check whether it is the display-list culling of
      §2.1 reasserting itself (all three gates overridden — so that would be a
      *fourth* gate nobody has found), whether it is Bean Ball legitimately holding
      fewer objects in a list, or whether it is the two-frame build cadence
      interacting with a longer camera travel.

    ### Getting the capture again — do not refer to scratch

    The original capture was 1.4 GB of **scratch, gitignored, and not a source of
    truth**. Do not cite a capture folder and do not expect one to exist — cite the
    vector and the number instead (AGENTS.md, "Never cite a `tmp/` capture as the
    source of a number"). **Ask the owner to re-record it** — they are the only one
    who can play the route; it is a specific mode plus a specific run:

    ```bash
    SDB_SCALE=2 SDB_UNLIMITED=1 tools/play_record.sh bar-artifact-2
    ```

    `SDB_SCALE=2` because they read the artifact at that scale, `SDB_UNLIMITED=1`
    so there is no frame cap to land on (they stop when they have it — and closing
    the window still flushes the vector's frame count, because that line is written
    from the destructor; SIGKILL does not), `SDB_SHOT_EVERY=1` to present every
    frame. **`SDB_SHOT_EVERY` is a PRESENT interval, not a screenshot interval** —
    the compositor only runs on a presented frame, so any stride above 1 silently
    skips it and every `drawn`/`composed` count drops. A capped alternative is
    `SDB_FRAMES=1400`, chosen to cover the ~658–714 window with margin.
    **Make the request rather than re-deriving the state**: the symptom is
    *intermittent and order-random*, so a vector built by trial and error will not
    reproduce it, and a headless replay will not reproduce it either.

    If a replay is wanted for instrumentation, `tas/vectors/sdb_beanball.input` is
    the owner's recorded 40-step vector from this very session and is committed.
    Two things it needs, both of which have bitten this session already:
    **replay it in a window, never headless**, and give the config
    **`Player1Source = 2`** — player 1 is the keyboard and player 2 the controller,
    so the default sends every press nowhere and the run sits at the title screen,
    which looks exactly like "the feature does nothing". Check `sdbc` for
    `cam≈128` by frame 600 to confirm the run is live.

    ### Examples from last time — the closest things we have

    No committed vector reproduces this, and it is worth being explicit about that
    rather than rediscovering it. `sdb_sweep` runs the full 512-px world but only
    in the **stadium** mode; `sdb_pan` alternates direction from a still camera;
    `sdb_throw` stresses the flicker mod's bookkeeping for 8000 frames in the
    stadium and holds `max list 77, 0 overflow, 0 backstop`; `sdb_idle` and
    `sdb_bar` are stadium matches. **None enters Bean Ball, and none makes the
    player run the length of the map.** The one thing that resembles it —
    §2.1's runaway capture list, where the presented picture smeared hundreds of
    stale court copies in a trail — is the *same code failing the other way* and
    was fixed by the backwards-`$0617` boundary plus the `SDB_LIST_LIMIT`
    backstop. If this bug turns out to be ours, start from that fix and ask what
    is different about a camera in constant motion.

15. **The presented sprite layer is one build old, and the seam offset it shared a symptom with — BOTH FIXED.** With the flicker mod on the character used to render 1-2 px from where OAM put it, and 7-11 px on a build boundary: the captured list is published only once a build completes, so an entry the game has since moved was drawn where it *was*.

    Measured for the first time with `json_trace.py flicker`'s new positional column: over the 1200-frame sweep, **42514 of 54479 unambiguous (y, tile, attr) matches agreed exactly and 11965 (22.0%) were off-position** — and every one of those sat on a frame that saw no build reset, i.e. mid-build with OAM already carrying the next list. 640 of 1201 frames carried at least one.

    The fix is to MOVE a matched entry rather than reject it: if a list entry is the unique (y, tile, attr) match of a live OAM sprite, draw it at **that sprite's x**. Ambiguous entries keep their captured x, so this can only move a sprite toward the machine, never invent one. Result: **22.0% -> 0.0% off-position**, all 54479 matches agreeing. 33 frames of a played capture change, all inside canvas columns 93-150 — the seam.

    Rejecting instead is what four earlier attempts did and each cost more than it fixed; moving it is a different decision, because the sprite is still drawn and it is drawn where the machine has that exact sprite.

    **And at the widescreen seam it was fixed too, by a different mechanism — which is why this item closes.** A character straddling the edge of the 4:3 viewport is drawn by TWO sources at once: the machine paints the half inside its own picture, the margin paints the band half. Two separate defects sat on that join. The reconciliation above fixed the flicker side's one-build lag. The **margin's** side was not that at all — on about a quarter of the camera-stepping frames the margin placed a whole object on `$C3/$C4` while the machine was on the latched scroll, so the halves registered a whole camera step apart and the character read as doubled, worse the more the view panned. Reported as a duplicated ball at the left seam and as a sliver left behind at it; measured at research §26, "The margin and the machine are on different cameras". `sdb_object_delta` moves such an object onto the machine's columns, gated on the disagreement being exactly one camera step. The band HOLES that used to sit on top of all this were fixed earlier by removing `sdb_oam_has` (research 26).

    What remains is not this but the list's own age: it is still a build old for every entry that cannot be matched unambiguously, which is most of them once several players share a tile. That is what `json_trace.py`'s positional column measures, and it is why the two sides of the seam needed *different* corrections rather than one. Row matching has since been widened to ±2 for the same reason columns were: an entry whose sprite both moved and was re-emitted has a new row too, and exact-row matching left 771 such entries unmatched (zero new ambiguity; ±3 collides).

    **Item closed.** Both halves of the seam problem are fixed; the second was a basis disagreement rather than this one-build lag, and the two had been reported as one symptom for several passes. What structurally remains is that a straddling character is drawn by two sources at all.

16. ~~**The menu ghost: a stale sprite list outliving its screen**~~ — **DONE** (research §26, "The menu ghost"). Fixed by `sdb_page_clear()`: when all 64 OAM slots are hidden, the machine is painting nothing, so the mod presents nothing. The signal the four failed rules lacked was not *the screen changed* but *is anything on screen at all*. Measured on a played level-1 session into the between-rounds screen (captured with `tools/play_record.sh`): the page goes from 3 hidden to 64 in one frame at window 868 and stays at 64 for **90 frames** while the mod's 60-sprite list is frozen — that is the ghost. Gates: the match (window 605-866) is **262/262 frames bit-identical** to the pre-fix build; `hid == 64` with a list held happens once in 3000 frames on each of throw / sweep / idle / pan, at frame 40 where the machine is empty; margin parts withheld on a match: **0**. Two traps cost a real bug each and are written up: read **`cyc_render_oam()`**, not `$0200` (the DMA source runs a frame ahead and blanked 6 frames of title screen), and do **not** make it a per-entry test on the entry's own slot (fires on slot 63 alone for 107 play frames — the mod's own cursor pin — and takes the legs off a player). The **margin** had the same ghost from the record-liveness side and is gated by the same test.

17. ~~**Cheat support**~~ — **DONE** (research §32). RetroArch/libretro `.cht` files load, with all four code kinds: `AAAA:VV`, `AAAA?CC:VV`, 8-hex Pro Action Replay, and 6/8-character Game Genie. Game Genie needed a new engine primitive (`patches/008`, a ROM read substitution) because the cycle backend had no way to substitute a value the CPU *reads*. Reached through the in-game menu — `Escape` → `Cheats`, one toggle per entry, applied on the spot — plus `--cheat` / `--cheats` / `--no-cheats` for headless work and a `cheat` TCP command for scripts. A freeze restores the byte it displaced when switched off, and a one-shot kind (`cheatN_once`) exists for event-shaped codes. libretro-database's 13 codes for this ROM load unchanged and **are largely not doing what they claim** — see research §32.

18. **An instant win / skip-stage cheat** (research §32, **OPEN — and read the section before spending time on it**). Four hypotheses were tried and **all four refuted by measurement**: freezing all six P2 energy bytes at 0; one-shotting those same bytes instead; freezing the match clock `$0754` at 0; forcing the stage counters `$007A`/`$018A`. One measurement explains all four at once: with all input stripped from frame 700, `$0754` still expires at **frame 2809, the same frame**, so the match is a fixed ~37-second scripted sequence with no early exit, and a freeze removes the transition any comparison would watch for. The win check is still real — in that same run `$018A`/`$007A` never move, so the game decides at the end whether you advance. RAM poking cannot reach it because `$0754` and `$0532` have **zero occurrences as operands in the ROM**: the game reaches that state through an indirect pointer, so "find the code that touches `$0754`" fails at step one. **What is needed:** resolve the pointer, read the comparison at the code that uses it, and then patch 008's substitution is the right tool (a comparison has an immediate operand, read at run time). A 756-candidate brute-force search was costed at three hours and **deliberately not run**, because no strictly monotonic countdown exists in any of the 2048 bytes and the apparent ones are wrap-around counters.

    **Note before picking this up:** save states already deliver the stated need — jump to any stage instantly, with no cheat, no guest state, and nothing to go stale. This item is only worth doing to have it *as a cheat*, and that is the expensive version of the problem, not the easy one.

## What Other Successful NESRecomp Projects Do Differently

### Zelda 1 (mstan/LegendOfZeldaNESRecomp) — 100% playable
- Uses `bank_switch = [0xFFAC]` (proper MMC1 bit-bang routine)
- Has `sram_map` for bank-1 code mapped to SRAM
- Has `inline_dispatch` at `0xE5E2`
- Has ~100+ `extra_label` entries for bank 1 discovery
- Has `data_region` declarations in `game.discovery.toml`
- `game_dispatch_override()` only handles SRAM remapping (`0x6C90`-$7FFF), NOT controller polling
- Has TCP debug server, verify mode, entity diagnostics
- Keeps `nesrecomp` submodule at `origin/master` (commit `27b281e`)
- Has `game.discovery.toml` with pointer scan exclusions

### Super Mario Bros (mstan/SuperMarioBrosNESRecomp) — Fully playable
- Uses NROM (mapper 0) — no bank switching complexity
- `game_dispatch_override()` returns 0 (not needed)
- Recompiled code handles `ReadJoypads`/`ReadPortBits` natively
- Has extensive `[[mod_function_hook]]` and `[[ram_read_hook]]` sections
- Has TCP debug server on port 4370
- Has verify mode, entity diagnostics
- Has `game.discovery.toml` and `symbols.sym`
- Generated C code is committed to repo (not gitignored)

### Yoshi's Cookie (mstan/nesrecomp reference) — 100% playable, zero oracle divergence
- MMC3 mapper (more complex than MMC1)
- Uses `[[trampoline]]`, `[[inline_dispatch]]`, `[[data_region]]`
- Has proper `game.toml` with all directives

### RetroPortingToolKit — Key Lessons
- **House Invariants** (05_agents/02_house-invariants): "No stubs, fix the tool not generated code, find first divergence"
- **Co-simulation** (02_concepts/05_co-simulation): Run port beside oracle, compare at guest-time checkpoints
- **Verification Rituals** (05_agents/03-verification-rituals): Dispatch misses must be zero, co-sim must match, TCP checks pass
- **Failure Modes** (05_agents/04-failure-modes): 10 common AI failure shapes — stubbing, wrong fixes, ignoring first divergence
- **When You Cannot Run the Game** (05_agents/07-when-you-cannot-run-the-game): Partial checks without game/BIO/oracle

---

## RetroPortingToolKit Key Rules

### House Invariants (from 05_agents/02_house-invariants)
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

### Verification Rituals (from 05_agents/03-verification-rituals)
- Dispatch misses must be zero
- Co-sim must match (RAM state at checkpoints)
- TCP checks pass
- Frame hashes must match
- A green build is weak evidence
- Use hardware events, not frame numbers

### Failure Modes (from 05_agents/04-failure-modes)
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

### When You Cannot Run the Game (from 05_agents/07-when-you-cannot-run-the-game)
1. Check if the ROM loads (CRC verification)
2. Check if the title screen renders (PPU works)
3. Check if dispatch misses are zero (all functions discovered)
4. Check if input is wired (controller buttons change)
5. Check if RAM state progresses (state machine advances)
6. Check if NMI fires (frame counter increments)
7. Check if audio works (APU register writes)

### Port-a-Game 6-Step Workflow
1. Identify the exact game (title, region, revision, hashes)
2. Fetch the framework (pin matters)
3. Teach the recompiler about the game
4. Generate the code (check for dispatch misses)
5. Build and run
6. Fix the right layer

### What Does Done Mean?
Booting is not done. A useful bring-up checklist: file identity works, title screen reaches, input works, gameplay reachable, saves/loads work, audio behaves, menus work, dispatch misses resolved, compared against reference.

---

### Iterative Testing Procedure

After any `src/game.toml` change (display name, discovery, seeds):

1. **Rebuild**: the cyc project reconfigures on ROM/config change — `cmake --build build-cycle -j$(nproc)` (each revision gets its own generated dir; old ones can be discarded with the build dir).
2. **TAS**: `python3 tools/tas/run_tas.py --tas tas/sdb_4976.tas.json --frames 400 --out tas/runs/rXXX` + `diff_tas.py` — liveness 400/400 exit 0, state MATCH (mirrors masked), images informational.
3. **Engine changes** (submodule bumps): add `python3 tools/blargg/run_blargg.py --tier cpu` — 25/25 (20 PASS + 5 screen-confirmed) required; any flip blocks.

**Known baseline**: cyc 400 TAS frames → team-select, state byte-exact vs Mesen (RAM/OAM/palette/CIRAM, mirrors masked); blargg 25/25.

---

## Backend Decision Record (2026-10-02: cycle default, legacy retired)

The cycle backend runs the stock ROM flawlessly (team-select matching Mesen, 25/25 blargg) with zero game-specific work. The legacy function-level runner is retired: `src/extras.c`, `src/CMakeLists.txt`, `patches/`, `tools/input/`, `tools/blargg/{blargg_extras.c,selftest}`, `tools/tas/{emit_script.py,edgewatch.lua}` removed (pre-squash history: this repo's `refs/stash`, plus `~/Documents/backups/superdodgeball-private-backup.bundle` which holds four further commits). Game knowledge (NMI dispatcher, tunnel mechanics, input path) lives on in research §§18–19 and transfers to mod work. `setup.sh` no longer applies patches or builds legacy; `./build_and_run.sh` targets cycle.

## Upstream Proposal: 002 S-latch guard (to send, then lapse the rest)

- **Symptom**: interpreter RTS after native JSR lifted S +6..+48/call, clobbered `$0106` to `$90` (polls died), corrupted A into the MMC1 bit-bang (bank-8 `$8003` miss), BRK slides (`$0001`/`$00A2`/`$0600`).
- **Root**: native C calls push no 6502 return address; the `$86C2 JMP $07B4` RAM trampoline runs via interp dispatch whose terminal RTS pops 2 live-caller bytes per trampoline (3/frame).
- **Fix** (`002_interp_ram_dummy`): in `nes_interp_dispatch_bank`'s RAM/SRAM path, pre-push a dummy 6502-stack pair before `interp_run`, strip on exit if untouched — terminal RTS consumes dummies, S restored exactly.
- **Evidence**: post-fix `$0106` clean, balanced S, 0 misses, 0 BRKs (research §19.1). Patches 003 (companion clamp), 004 (game-specific tunnel), 005 (watchdog taste) lapse.
- **Status**: [ ] proposed upstream; on acceptance or legacy-drop, delete local copies (already removed from tree).

## Enhancement Roadmap

Principle from SMB (reference implementation): rendering never changes guest edges — game logic runs stock, the host re-draws. All items build on `HOST_EXTRAS`; only the flicker fix needed `[[mod_function_hook]]` sites (content-keyed, no game bytes in repo).

1. ~~**Widescreen**~~ — **SHIPPED** (`super-dodgeball.widescreen`, off by default, opt-in). Custom compositor, 256 → 426/560/854, stock centre copied back so it stays byte-identical, per-frame seam check pillarboxes anything that fails. **The SMB pattern was not what shipped**: SMB re-runs the game's own area parser in isolation and caches the world, because SMB's world does not fit in its nametables. This game's world *does* — a 512-pixel looping stadium that is byte-identical across a whole match — so the margins are read straight out of the nametable, with no world cache, no isolated guest call and no new hook sites. wideNES is the same story from the other side: it warps remembered pixels, so it can only show what the guest already drew and cannot do sprites at all. Research.md §25.
2. **Flicker fix — SHIPPED** (`super-dodgeball.character-flicker`, off by default, opt-in). Three measured mechanisms, none of them the emulator sprite limit: (a) hard 64-sprite OAM budget with a `BEQ` bail on `$84B6` (79% of match frames truncate); (b) display-list culling, three gates; (c) two emitter shapes no bump hook sees (damage-number pair step, block copier with no bump). Ten content-keyed `[[mod_function_hook]]` sites + host-side redraw + six seed roots. Truncation: body-ink stdev 8.7 → 0.8. Culling: 55.0 → 16.6, 0 frames culled. Numbers: 108/108 steady. Guest machine: truncation half untouched (same cycle count; `$02FD`/`$02FF` only); culling half shifts NMI phasing slightly by construction. Traps in research.md §2/§2.1 and AGENTS.md.
3. **Resolution** (honest limit): wider canvas, same chunky pixels. No upscaling path upstream (CHR PNG replacement only). Don't promise HD.
4. ~~**Sprites in the widescreen margins**~~ — **SHIPPED** (part of the `widescreen` feature, not a separate toggle). Not an inference and not a re-derivation: the game places every sprite in a **16-bit** screen coordinate and then culls anything outside `[4, 255]` by writing Y=$F8 into the OAM slot, so the tile, flips, palette and position are all computed and one byte of truncation loses it. Two hook sites capture that at the game's own decision. 40% of objects are off-screen in every measured frame, so this is most of the field of play at 16:9. Presentation only, and the gate is strict: a 700-frame `--frame-log` with it on is byte-identical to it off, and the native 256 differ by 0 pixels at both 16:9 and 32:9. This is the capability wideNES names as the reason it never shipped. Research.md §26.

## Removed Legacy Action Items (retired with the backend — kept as a struck record)

- ~~Mode-menu confirm divergence (legacy transition pacing)~~ — moot on cycle.
- ~~`func_DB4E` load-bearing, `func_D98A` NMI path, `func_FE34`/`FE8A` checks, OAM-fix extension, TCP server in `extras.c`~~ — legacy-code questions; extras.c deleted.
- ~~C-stack lap growth / `merge_range` dedup~~ — legacy codegen shape; cycle blocks don't lap.
- ~~`game.discovery.toml`, `known-issues.md` creation~~ — superseded (seeds replace discovery tuning; this plan is the issues list).
- Kept: TAS full-19k storage policy; `fm2` vectors if any appear; `nestest` entry-point work (all backend-agnostic).
