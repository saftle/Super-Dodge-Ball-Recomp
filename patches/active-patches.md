# Runner patches

`setup.sh` auto-applies `patches/*.patch` (sorted order) after every
nesrecomp submodule checkout. `patches/archived/` is deliberately ignored
by that glob — it holds dormant patches with revival conditions. The pin in
`.gitmodules`/`external/nesrecomp` is always pristine upstream (`origin/master`)
precisely so fresh clones can fetch it; patch state lives only in these files
plus the submodule working tree (never committed in the submodule — an
unfetchable pin breaks every fresh checkout, including CI).

## Active (applied by setup.sh)

## 002_interp_ram_dummy.patch (ACTIVE in working tree, 2026-10-01)

**What:** in `nes_interp_dispatch_bank`'s RAM/SRAM path, pre-push a dummy
6502-stack pair before `interp_run`, and strip it on exit if untouched.
Native JSR/JMP sites are C calls that push no return address, but RAM code
routinely ends in RTS (Super Dodgeball's `$07B4` STA-trampoline:
`JMP $07B4` executes `STA $40xx` + RTS). Without the pair the terminal RTS
pops two bytes of live caller data (+2 S per call).

**Why:** the sound tick (`$FA00` → bank-0 cluster → `$86C2 JMP $07B4`,
3 executions/frame) lifted S by +6/NMI. S wrapped into the `$0100` page
within ~18 frames, stack pushes clobbered `$0106` to `$90`, the `$EF9F`
BMI-skip self-sustained, controller polls stopped, and the title froze with
Start dead. Follow-on damage (all downstream of the S drift): intermittent
`$8003` bank-8 dispatch miss (MMC1 sequence entered with corrupted A),
`BRK $0001`/`$00A2`/`$0600` slides, `$90` in `$0100`, C-stack growth from
skipped-RTS control flow.

**Evidence:** `func_FA00` no-op experiment → S perfectly balanced (EFAE/F9 =
EFD0/F9 every frame); per-instruction S-delta trace → +2 single-steps at
`$812A` (post-`JSR $84A8`) with no intermediate push/pop = interpreter RTS
after `call_by_address_tail(0x07B4)` → `nes_interp_dispatch` → RAM.
Post-fix: `$0106` clean at frame 24 (was `$90`), 0 dispatch misses, 0 BRKs,
title 98.9% palette-idx vs Mesen 11.

**Blast radius:** RAM/SRAM interp entries only; ROM dispatch untouched.
Dummy bytes are consumed by a terminal RTS (stack-lift exit, S exactly
restored) or stripped if the run exits another way. Other games using RAM
trampolines benefit; all others unaffected (their RAM entries get 2 transient
stack bytes). No `$0001`-wander: the stack-lift floor is captured after the
push, so a consuming RTS exits via lift immediately instead of falling
through to a dummy-derived return address.

## 003_interp_rom_clamp.patch (ACTIVE in working tree, 2026-10-01)

**What:** one-sided S-clamp on the ROM-miss fallback path in
`nes_interp_dispatch_bank`: snapshot S at interp entry; on a handled exit,
erase net pops (`S = entry` iff S rose), never touches S when the run
pushed more than it popped. Companion to 002 (same phantom-RTS class, ROM
targets instead of RAM).

**Why:** undiscovered ROM targets reached from native JSR sites (which push
nothing) hit the same phantom-RTS class: a match-transition state machine
(`$8E00` bank 6, since discovered) lifted S +2 per call (`$FF`→`$01` in one
dispatch), cascading into `A=$10` stack garbage → `FF08($10)` → bank thrash
→ `$C000` BRK → hang.

**Evidence:** dispatch-trace S column (`C $8e00 S=$FF` → next transfer
`S=$01`); post-fix 240/240 frames clean. Verified no-$0001 detour: the
stack-lift rule already exits at the consuming RTS (floor includes any
pushed pair), so the clamp only ever fires on genuinely unbalanced exits.

**Blast radius:** fallback-miss path only; balanced callees (net-0) and
net-push runs are untouched. Mirrors the native `PLA PLA RTS` abort-idiom
guard philosophy. Handoff-island/force paths (`interp_run_ex` direct
callers) bypass it.

## 004_nmi_tunnel_unwind.patch (ACTIVE in working tree, 2026-10-01)

**What:** opt-in NMI-tunnel unwind. Generic mechanism, game-specific arming:
new `g_tunnel_site`/`g_tunnel_armed`/`g_tunnel_buf` (`nes_runtime.h`),
`runtime_get_guest_pc()` (`runtime.c`), and a check at the top of both
`coroutine_scheduler_setjmp()` implementations (`coroutine.c`): a TXS-site
marker reached while armed, at the registered site, inside NMI
(`depth > 0`) resets vblank depth and longjmps to the main-context landing.
Zero effect unless a game arms it (site 0 = disabled).

**Why:** Start tunnels main into `$F09F` from inside NMI
(`$EFBC: JSR $FE8A, LDX #$FF, TXS, JMP $F09F`). C-calling `$F09F` never
returns, so the outer frame callback never completes: frames freeze, no
render/present/count. The landing (`game_run_main` + `setjmp`) runs `$F09F`
natively on the main context; NMIs then interrupt normally (hardware
behavior). The landing also calls `runtime_prepare_guest_resume` — longjmp
skips frame/vblank callback-depth decrements, tail-detector/dispatch-depth
resets, and C-stack tracking cleanup, all of which must be restored or
frames stay frozen and telemetry leaks. Companion game-side pieces (in `src/extras.c`, not this patch):
`NESTED_NMI_RUN_HANDLER` (nested `$C0`/`$EFD9` must run to release `$F09F`'s
`$F1A2` handshake wait), nested gate (only `$C0` runs nested — blocks
re-tunnel), Start-edge inhibit until `$F0C3` (`$0100=$C0`).

**Evidence:** `[TUNNEL] NMI-tunnel unwind at $EFC1 (frame 10)`, then 30/30
(and 40/40, 60/60) frames, 0 misses, 0 BRKs, title→city→team-lineup screens.
Note: `depth > 1` inside `game_run_nmi` is the *top-level* NMI (trigger 1 +
firing 1); truly-nested is `> 2`. A `> 1` gate silently skips every handler
(title still renders main-thread-only, polls never run).

**Revive/applicability:** any NMI-tunnel game (`TXS; JMP <main>` out of the
handler) arms with its site address. Upstream-worthy if a second title needs
it.

## 005_hang_watchdog_default.patch (ACTIVE in working tree, 2026-10-01)

**What:** the hang watchdog fires after N seconds with no completed frame
(default 90; `NESRECOMP_HANG_TIMEOUT_S` overrides, `0` disables) instead of
only via env. Previously env-only (off by default), so interactive hangs
needed a manual kill with zero diagnostics.

**Why:** a frozen frame counter is always a hang (frames complete
continuously in every non-hung state, even idle title). The dump (C stack +
dispatch ring + RAM image) it prints on exit is the primary hard-hang
diagnostic. Requested after an interactive gameplay freeze required
manual kill.

**Blast radius:** none when frames advance (the common case checks one
counter per ~1M instructions). Debugger/SIGSTOP users should set
`NESRECOMP_HANG_TIMEOUT_S=0`.
