# Research Notes — Super Dodgeball NES Recompilation

> Living notes. RetroPortingToolKit docs reference: https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs

## Summary of Findings

TITLE DONE: 99.8% structural match vs Mesen frame 11 from frame 1, `smoke 30` exit 0. Unlocked by `[[replace_func]]` for `$FCA0` (NMI-wait spin removed game-side). Current: Start tunnels to mode/skill/team-select (100+ frames green); active runner patches 002/003/004/005 (`patches/`, auto-applied by `setup.sh`). Full story: §§13-19.

---

## 1. ROM Technical Specifications

- **Format**: iNES 2.0 header (`4E 45 53 1A 08 10 10 08 00 00 00 00 00 00 00 01`)
- **Mapper**: 1 (MMC1 / NES-SLROM PCB, MMC1B2 variant)
- **PRG ROM**: 128 KB (8 × 16KB banks) · **CHR ROM**: 128 KB (16 × 8KB banks)
- **Mirroring**: Horizontal · **Battery**: None · **TV System**: NTSC
- **CRC32**: `689971F9` · **SHA-1**: `42f954e9bd3256c011aba14c7e5b400abe35fde3`
- **CIC chip**: 6113B1 · **File size**: 262,160 bytes

---

## 2. Sprite Flickering Analysis

### The Mechanism

Super Dodgeball uses **Method 2 Software Sprite Removal**:
- When more than 8 sprites appear on a scanline, the game **strips sprites from the OAM list entirely** rather than reordering them
- "Remove Sprite Limit" in emulators is **non-functional** — the game has already removed those sprites from OAM
- The flickering specifically targets the **ball target indicator** (player/enemy being targeted for a throw)
- Trigger threshold: approximately 6+ characters on screen

### SNES Port Fix

Rumbleminze's SNES port (`rumbleminze/super-super-dodgeball`) fixes this via hardware advantage (SNES has 128 OAM slots, 32 sprites/scanline vs NES 64/8).

### What This Means for PC Port

On PC, there is **no hardware sprite limit**. The fix requires identifying the OAM-culling routine in the recompiled C code and overriding it to write all sprites without the removal logic. Approaches: `nop_jsr` to skip culling code, `extra_func` to replace it, or `force_interp` with custom C code.

---

## 3. NESRecomp Framework

### Architecture

NESRecomp is a **static 6502 recompiler** — not an emulator. It translates NES ROM machine code to C at build time, compiled to native x64.

**Three main components:**
- **Recompiler** (`recompiler/src/`): Function discovery via BFS from RESET/NMI/IRQ vectors, C code generation. Key files: `code_generator.c`, `function_finder.c`, `game_config.c`, `cpu6502_decoder.c`
- **Runner** (`runner/src/`): NES hardware emulation — PPU, APU, mapper, input, save states. Key files: `runtime.c`, `main_runner.c`, `ppu_renderer.c`, `mapper.c`, `interp.c`, `apu.c`, `controller.c`, `debug_server.c`
- **Game project** (`src/`): `extras.c` (hooks), `game.toml` (config), `generated/` (auto-generated C)

**How function discovery works:**
1. Start from vectors (RESET, NMI, IRQ) in the fixed bank (bank 7, `$C000-$FFFF`)
2. BFS: follow JSR targets, branch targets, JMP targets
3. On JSR: add target as new function
4. On RTS/RTI/BRK/JMP: stop current function
5. Register state propagation tracks A/X/Y constants to detect bank switches
6. Pointer table scanner finds dynamically-dispatched functions (4+ consecutive 16-bit LE values where each target decodes as valid code for 7+ instructions)
7. RTI hijack detection: scans for `STA $01NN,X` writes followed by `LDA #imm` pushes

**How the dispatch table works:**
- `call_by_address(uint16_t addr)` — basic dispatch, returns 1 if recompiled function exists
- `call_by_address_cb(uint16_t addr, int caller_bank)` — with bank fallback
- `nes_dispatch_call(addr, caller_bank)` — depth-counted, for JSR sites
- `call_by_address_tail(addr, caller_bank)` — deferred JMP tails
- On miss: falls through to interpreter (if enabled) or dispatch-miss policy

**Supported game.toml directives (full list):**
- `[game]`: output_prefix, push_all_jsr, disable_ptr_scan, disable_secondary, deduplicate_functions, symbol_file
- `[mapper] bank_switch`: addresses of bank switch routines
- `[[trampoline]]`: bank-switch trampoline config
- `[[inline_dispatch]]`: indexed dispatch via inline address table
- `[[inline_pointer]]`: JSR reads inline bytes into zero page
- `[[nop_jsr]]`: skip JSR entirely
- `[[extra_func]] bank, addr`: force-create a function entry
- `[[extra_label]] bank, addr`: secondary entry point within existing function
- `[[data_region]] bank, start, end`: exclude from pointer scanning
- `[[sram_map]] sram_start, rom_start, bank, size`: SRAM-to-ROM mapping
- `[[replace_func]] addr, bank, scope`: replace function with extras.c
- `[[mod_function_hook]] addr`: mod callback hook
- `[[ram_read_hook]] addr, indexed`: hook for RAM reads
- `[functions] fixed = [...], bankN = [...]`: fixed native functions
- `[force_interp] fixed = [...], bankN = [...]`: force interpreter mode
- `[[stack_bail_func]]`: stack-bail function addresses
- `[[cond_bail_func]]`: conditional bail functions
- `[[merge_func]]`: merge two function bodies
- `[[merge_range]]`: merge all functions in a range
- `[[indirect_continuation]]`: indirect continuation targets
- `[[push_jsr]]`, `[[push_jmp]]`: JSR/JMP needing dummy push

**Runtime frame flow:**
1. `game_run_main()` is called (never returns)
2. CPU executes native code, calling `nes_instruction_boundary()` per instruction
3. `maybe_trigger_vblank()` checks cycle budget and fires NMI
4. `nes_vblank_callback()` runs `game_run_nmi()` then `game_post_nmi()`
5. `ppu_render_frame()` renders PPU to framebuffer
6. `game_post_render()` does post-render compositing
7. SDL presents frame; `g_frame_count++`

**Nested NMI policy:**
- `NESTED_NMI_POKE_SPIN_FLAGS` (default): skips handler, sets $1A/$20 spin-wait bytes
- `NESTED_NMI_RUN_HANDLER`: runs handler nested — used by SMB3

**Controller input:**
- `g_controller1_buttons` / `g_controller2_buttons` hold 8-bit button state
- Button bitmask: bit7=A, bit6=B, bit5=Select, bit4=Start, bit3=Up, bit2=Down, bit1=Left, bit0=Right
- NES controller uses serial shift register: strobe pin starts shift, each clock pulse shifts out one bit
- `$4016` returns `0x40 | (g_controller1_buttons >> 7)` when strobe is set; otherwise shifts out bit 7 first

**Key runtime globals:**
- `g_ram[0x0800]`: 2KB work RAM
- `g_sram[0x2000]`: 8KB battery SRAM
- `g_ppu_oam[0x100]`: 64 sprites × 4 bytes
- `g_ppu_pal[0x20]`: Palette
- `g_ppu_nt[0x1000]`: Nametable RAM
- `g_controller1_buttons` / `g_controller2_buttons`: Controller state
- `g_ppuctrl` / `g_ppumask` / `g_ppustatus`: PPU registers
- `g_code_window_base`: Code window base for bank switching
- `s_vblank_depth`: Current NMI nesting depth
- `s_ops_count`: Per-frame CPU cycle accumulator
- `s_frame_budget`: ~29781 cycles (NTSC dot-accurate)

### Proven MMC1 Support

- **Yoshi's Cookie** (MMC3): 100% playable, zero oracle divergence
- **Zelda 1** (MMC1): Fully playable
- **Metroid** (MMC1): Fully playable
- **Dr. Mario** (MMC1): Fully playable
- **Faxanadu** (MMC1): Fully playable
- **Super Mario Bros** (NROM/0): Fully playable

### Pipeline

```
NES ROM (.nes) + game.toml → recompiler binary → generated/<game>_full*.c + _dispatch.c
→ CMake + SDL2 → native executable
```
Regen command (repo root): `./external/nesrecomp/recompiler/build/NESRecomp "roms/Super Dodge Ball (USA).nes" --game src/game.toml --output-prefix "Super_Dodge_Ball_(USA)"`

### Limitations

- Generated C code is **not human-editable** (auto-generated, overwritten on recompile)
- Audio is basic (APU register writes captured, full mixing is WIP)
- Undocumented 6502 opcodes (DCP, ISC, SLO, RLA, SRE, RRA, SAX, +90 more) emitted as sized NOPs
- `BRK` treated as comment/stop, not real software interrupt
- Cycle accuracy: base cycle counts only; page-cross and branch penalties mostly not modeled
- Mappers 2, 3, 7, 9 not yet supported

### game_dispatch_override Deep Analysis

**Critical finding**: `game_dispatch_override` is ONLY checked in the **interpreter fallback path**, NOT in the native C dispatch chain.

- Native JSR/JMP to `func_FF08`: calls `func_FF08()` directly via dispatch table (`case 0xFF08: func_FF08(); break;`) — `game_dispatch_override` is NOT consulted
- Interpreter fallback for `$FF08`: `nes_interp_dispatch_bank()` calls `game_dispatch_override(addr)` — if returns 1, the real function never runs
- `nes_log_dispatch_miss()` also calls `game_dispatch_override(addr)` — if returns 1, no dispatch miss is recorded

This means `game_dispatch_override(0xFF08)` creates a **split-brain**:
- **Native code sees**: `func_FF08()` runs the MMC1 bit-bang (writes to `$8000-$FFFF`)
- **Interpreter sees**: Controller read sequence (strobes `$4016`, reads 8 bits)
- **Dispatch miss logging**: `$FF08` never appears as a miss (override swallows it)

**Runner's `$4016` handling is correct**: The runner (`runtime.c`) properly implements the NES controller strobe/shift register:
- `$4016` WRITE 0x01: sets `s_ctrl1_strobe = true`
- `$4016` WRITE 0x00 (after strobe): latches `g_controller1_buttons` into `s_ctrl1_shift`
- `$4016` READ: shifts out bits from `s_ctrl1_shift`, returns `0x40 | bit`
- `g_controller1_buttons` defaults to 0 in headless/smoke mode

**Working `game.toml` state**: `bank_switch=[0xFF08]`, minimal `fixed`, dis65-verified `extra_func` (banks 0/1/6), two `[[replace_func]]` (`$FCA0`, `$8393` bank 6), empty `[force_interp]`. Historical note: the override was removed from `extras.c`; the old `force_interp` entries never loaded (TOML didn't parse, §14).

**Historical hang (resolved, see §15)**: with the ppumask toggle, 3 frames then hang. Without it, frames complete.

### NMI Handler and func_NMI() Pattern

All successful NESRecomp games follow a standard pattern for NMI handling:

1. **The code generator auto-generates `func_NMI()`** (`code_generator.c` lines 4920-4978). It:
   - Resets `g_rti_target = 0`
   - Sets `g_code_window_base = 0xE000` (bank 7 for NES)
   - Calls the NMI vector handler (e.g., `func_FF3A`)
   - If `g_rti_target != 0` (RTI hijack), dispatches to the hijacked target via `call_by_address`

2. **The runner sets up the interrupt stack frame** before calling `game_run_nmi()`:
   - Pushes PCH placeholder, PCL placeholder, P (status flags) onto 6502 stack
   - Sets `g_cpu.I = 1` (interrupt disable)

3. **All games call `func_NMI()` from `game_run_nmi()`**:
   ```c
   // Dr. Mario: simplest case
   void game_run_nmi(void) { func_NMI(); }
   // Zelda, Yoshi, etc: wraps with verify/oracle sync
   void game_run_nmi(void) { verify_mode_run_nmi(); }
   ```

4. **RTI is now handled correctly** by both the code generator and interpreter. The codegen emits RTI as:
   ```
   g_rti_target = (PCH << 8) | PCL;  // stored from stack
   ```
   The runner checks `g_rti_target` after `game_run_nmi()` returns:
   - If `0`: normal RTI - restore registers from pre-interrupt snapshot
   - If non-zero: RTI hijack - post-NMI code runs via `call_by_address(g_rti_target)`

5. **No `force_interp` needed**: `force_interp` stays empty — `0xFF08` in it garbles graphics. All NMI paths run natively via `func_NMI()` gated on PPUCTRL bit 7 in `game_run_nmi()`.

**Debunked**: earlier notes claimed coroutine-scheduler code (`coroutine_scheduler_setjmp`, fibers) on the NMI path. No such symbol exists in the tree; `func_NMI()` runs cleanly. Do not re-add workarounds for it.

## 4. Zelda NES Recomp — Detailed Comparison

**Repository**: https://github.com/mstan/LegendOfZeldaNESRecomp
**Status**: Believed 100% playable — overworld, dungeons, bosses, items, caves, inventory all working
**NESRecomp submodule**: commit `27b281e` (pinned)
**Two builds**: stock (CRC `0x3FE272FB`) + HD Remastered (CRC `0xFD9C577F`)

### game.toml Configuration

```toml
[game]
output_prefix = "zelda"
symbol_file = "symbols.sym"

[mapper]
bank_switch = [0xFFAC]    # Proper MMC1 bit-bang routine at $FFAC

[[inline_dispatch]]
addr = 0xE5E2             # ASL A; TAY; PLA×2; LDA ($00),Y ×2; JMP ($0002)

[[sram_map]]
sram_start = 0x6C90
rom_start = 0xA500
bank = 1
size = 0x1370             # Zelda copies bank 1 ROM $A500 -> SRAM $6C90 during init

[functions]
fixed = [...]             # ~80+ entries
bank0 = [0x8300, 0xBFAC, 0xBF98]
bank1 = [0xA69D, 0xB3F9, 0xBFAC, 0xBF98, ...]   # ~100+ extra labels
bank4 = [0xBFAC, 0xBF98, ...]                    # Enemy AI dispatch targets

[[extra_label]]           # ~100+ entries for bank 1
addr = 0xA500, bank = 1
addr = 0xA570, bank = 1
...
```

### extras.c Architecture

- **SRAM persistence**: Uses shared `save_ram` backend
- **TCP debug server** on port 4370 (gated by `debug.ini`)
- **`game_on_init()`**: Debug server init, voxel init, SRAM legacy path migration
- **`game_on_frame()`**: Voxel hotkeys, debug server polling, input override from debug server
- **`game_post_nmi()`**: Debug frame recording
- **`game_run_nmi()`**: Delegates to `verify_mode_run_nmi()`
- **`game_run_main()`**: Supports `RUN_MODE_EMULATED` and `RUN_MODE_VERIFY`
- **`game_dispatch_override()`**: **Only handles SRAM remapping** (`0x6C90`-$7FFF → bank 1 `$A500`+). **Does NOT handle controller polling.**
- **Entity diagnostics**: `game_fill_frame_record()`, `game_handle_debug_cmd()` with `entity_snapshot`, `entity_slot` commands

### Key Differences from Super Dodgeball

| Aspect | Zelda | Super Dodgeball |
|--------|-------|-----------------|
| `bank_switch` | `[0xFFAC]` (proper MMC1 routine) | `[0xFF08]` (proper MMC1 routine) |
| `game_dispatch_override` | SRAM remapping only | Narrow `0xFE8A` PPUCTRL-shadow arm only |
| `sram_map` | `0x6C90`-`0x7FFF` from ROM `0xA500` | None (no-battery MMC1) |
| `extra_label` | ~100+ entries for bank 1 | None (add only with proof) |
| `data_region` | In `game.discovery.toml` | None |
| `inline_dispatch` | `0xE5E2` | None (add only with proof) |
| `force_interp` | Not present | Empty (stays empty) |
| Debug server | TCP on port 4370 | None |
| `game_run_nmi()` | Delegates to `verify_mode_run_nmi()` | `func_NMI()` gated on PPUCTRL bit 7 |
| Input | Recompiled code handles natively | Recompiled code handles natively (`$4016` served by runner; title waits for Start) |

### Zelda Issues (from ISSUES.md)

- **Black overworld background** — SRAM functions bank-switched but code generator hardcoded JSR/JMP to bank 1. Fixed by modifying `code_generator.c` to emit `call_by_address()` for SRAM-sourced functions.
- **Cave soft lock** — Entity spawning broken
- **Enemies don't spawn** — Smoke clouds play but enemies never fully spawn
- **Link only has upper body** — 8x16 sprite missing lower body tiles
- **New area renders as all-zeros briefly** — PPU nametable update timing
- **HUD "DCE" garbled** — Tile mapping for counter digits

---

## 5. Super Mario Bros NES Recomp — Detailed Comparison

**Repository**: https://github.com/mstan/SuperMarioBrosNESRecomp
**Status**: Fully playable
**249 commits**, mapper 0 (NROM) — no bank switching

### Key Differences from Super Dodgeball

| Aspect | SMB | Super Dodgeball |
|--------|-----|-----------------|
| **Mapper** | NROM (0) — no bank switching | MMC1 (1) — working via `bank_switch=[0xFF08]` |
| **game.toml** | Has `[[mod_function_hook]]`, `[[ram_read_hook]]`, `[[inline_dispatch]]` | Minimal: `fixed` + bank-0 `extra_func` + `replace_func $FCA0` |
| **extras.c complexity** | Moderate (delegates to mods) | Low (family-shape hooks + one replacement) |
| **NMI handling** | Native, no override | Native `func_NMI()` gated on PPUCTRL bit 7 |
| **Input** | Recompiled `ReadJoypads`/`ReadPortBits` handles natively | Recompiled code + runner `$4016` emulation |
| **Bank switching** | None needed | Working (`$FF08` routine, `$A220`-table entries verified) |
| **Debug server** | TCP on port 4370 | None |
| **game_dispatch_override** | Returns 0 (not needed) | Narrow `0xFE8A` arm only |
| **`game_run_nmi()`** | `verify_mode_run_nmi()` | `func_NMI()` gated |

### SMB extras.c Key Features

- **No game-specific initialization** in `game_on_init()` — just watchdog, widescreen, voxel, mod init, debug server
- **`game_run_nmi()`** simply calls `verify_mode_run_nmi()` — the recompiled code handles NMI natively
- **`game_run_main()`** supports `RUN_MODE_EMULATED` and `RUN_MODE_VERIFY`
- **Input handling**: Via `game_smash64_update_input()`, `game_sonic_update_input()`, etc.
- **Audio**: Mod-specific (Sonic uses YM2612 via ymfm)
- **TCP debug server** on port 4370 with full RAM inspection commands
- `game_dispatch_override()` returns 0 (not needed for NROM)
- `game_ram_read_hook()` delegates to `game_smash64_ram_read_hook()`

### SMB Commit History Insights

Key commits showing what made it work:
1. Started with working NESRecomp base, then layered on widescreen, character replacements, voxel, debug server
2. `Merge pull request #6 from vibecodekun/feat/sonic-s3k-accuracy` — Latest merge
3. `Vendor ymfm for YM2612 emulation` — Audio dependency
4. `Pin merged widescreen engine master` — "Build passed; 120/120 stock smoke hashes match"

---

## 6. RetroPortingToolKit Documentation

### Source

Fetched from https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs — 54+ markdown files across 7 sections:
- **01_start**: What is static recompilation, how a port is made, is this emulation, quickstart
- **02_concepts**: Recompiler and runtime, code discovery, HLE vs LLE, code you can't see ahead of time, co-simulation, accuracy and burndowns, timing models, determinism, the game file you supply, glossary, start with original game
- **03_platforms**: NES, SNES, PlayStation, GBA, Genesis, Master System, DS, Virtual Boy, CD-i
- **04_guides**: Build a toolchain, port a game, write a mod, translate a game, add widescreen, debug a divergence, set up co-simulation, release a port
- **05_agents**: Start here, house invariants, verification rituals, failure modes, machine surfaces, contributing as an agent, when you cannot run the game
- **06_reference**: CLI, TCP protocol, TCP port registry, mod manifest, configuration, catalog schema, status vocabulary, errors and exit codes, site tools
- **07_fleet**: Repositories, lineage and credit, licenses, provenance

### House Invariants (from 05_agents/02_house-invariants)

1. **No stubs** — Never create stub functions that return default values
2. **Fix the tool, not generated code** — If generated code is wrong, fix the recompiler or game.toml config
3. **Find first divergence** — When a port breaks, find the FIRST frame where it differs from the oracle
4. **Classify the layer** — Is the divergence in codegen, runner, config, or game logic?
5. **Fix the source** — Fix the root cause, not the symptom
6. **Dispatch misses are blocking** — Every `call_by_address` target must be a recompiled function
7. **Never edit generated/ directly** — Always regenerate via NESRecomp
8. **Align on hardware events** — Prefer VBlank, DMA completion, timer overflow over frame numbers
9. **Use always-on traces** — Ring buffers and history queries recording before the bug happened
10. **Treat dispatch misses as blocking** — Can skip a whole subroutine without crashing
11. **Unknown is allowed** — Guessing is worse than saying "unknown"
12. **Prove the change** — Build output, test output, dispatch-miss status, coverage, oracle comparison

### Failure Modes (from 05_agents/04-failure-modes)

| What you see | What it may mean |
|---|---|
| One behavior never happens | A dispatch miss skipped a subroutine |
| A fix disappears later | The agent edited generated output |
| The fix only works for one game | A framework bug was hidden in game config |
| Tests pass but the game is wrong | The test did not cover the route |
| Native and oracle diverge at different places each run | Route not deterministic, or input timing changed |
| Two implementations agree on a wrong value | They share the same bug |
| A visual bug appears after a timing tweak | Timing tweak changed real game behavior |
| A release archive is too large or suspicious | May include generated junk or forbidden inputs |

### Verification Rituals (from 05_agents/03-verification-rituals)

- Build proves the compiler accepted code, not that the game is correct
- Dispatch misses must be empty or unchanged for a known reason
- Coverage must not regress
- Co-simulation: both sides must stop at same guest-time checkpoints
- TCP input: good for menus and simple button presses, not replacement for skilled gameplay
- Use hardware events (VBlank, DMA, timer overflow) not frame numbers
- A green build is weak evidence — a green test suite is stronger

### Port-a-Game 6-Step Workflow (from 04_guides/02_port-a-game)

1. Identify the exact game (title, region, revision, hashes)
2. Fetch the framework (pin matters — newer framework can change everything)
3. Teach the recompiler about the game (functions, data, banks, indirect calls)
4. Generate the code (check for dispatch misses)
5. Build and run (expect game to ask for game file)
6. Fix the right layer (problem → belongs in: recompiler, runtime, config, discovery, mod)

### What Does Done Mean?

Booting is not done. A useful bring-up checklist:
- Does the file identity check work?
- Does the game reach title screen?
- Does input work?
- Can you reach gameplay?
- Do saving and loading work?
- Does audio behave?
- Do menus and transitions work?
- Are dispatch misses resolved?
- Has the port been compared against a reference?

---

## 7. MesenCE API Research

### MesenCE as Oracle

Frame reference generation uses the **MesenCE binary directly** (`external/mesence/Mesen`) with `--testRunner` mode and a Lua script (`tools/frame_gen/export_frames.lua`). The Lua script registers an `endFrame` callback using `emu.addEventCallback()` and captures frames via `emu.getScreenBuffer()` and `emu.getScreenSize()`.

The co-simulation compares recompiled game screenshots (via `--save-screenshot`) against MesenCE reference frames. See `tools/frame_gen/run_frames.sh` and `tools/frame_gen/export_frames.lua` for the implementation.

### MesenCE Lua API (Full Reference)

**Memory:** `emu.read()`, `emu.write()`, `emu.read16()`, `emu.write16()`, `emu.read32()`, `emu.write32()`, `emu.convertAddress()`, `emu.getLabelAddress()`, `emu.getMemorySize()`

**CPU:** `emu.getCpuState()`, `emu.setCpuState()`, `emu.getCpuCycleCount()`, `emu.getMasterClock()`

**Events/Callbacks:** `emu.addEventCallback()`, `emu.removeEventCallback()`, `emu.addMemoryCallback()`, `emu.removeMemoryCallback()`

**Video:** `emu.getScreenSize()`, `emu.getScreenBuffer()`, `emu.setScreenBuffer()`, `emu.getPixel()`, `emu.takeScreenshot()`

**Input:** `emu.setInput()`, `emu.getInput()`, `emu.isKeyPressed()`, `emu.getPressedKeys()`

**State:** `emu.getState()`, `emu.setState()`, `emu.createSavestate()`, `emu.loadSavestate()`

**Control:** `emu.stop()`, `emu.breakExecution()`, `emu.step()`, `emu.rewind()`
- **`emu.reset()` and `emu.resume()` crash with exit code 255 in --testrunner mode**

**Debug:** `emu.getCdlData()`, `emu.getAccessCounters()`, `emu.resetAccessCounters()`, `emu.getRomInfo()`

**Info:** `emu.getScriptDataFolder()`, `emu.getLogWindowLog()`

### Event Types
- `emu.eventType.inputPolled` — safe point for `emu.setInput()`
- `emu.eventType.startFrame` / `emu.eventType.endFrame`
- `emu.eventType.Nmi` / `emu.eventType.Irq` / `emu.eventType.Reset`
- `emu.eventType.StateLoaded` / `emu.eventType.StateSaved`
- `emu.eventType.CodeBreak` / `emu.eventType.HaltStarted` / `emu.eventType.HaltEnded`

### Memory Types (NES)
- `emu.memType.nesInternalRam`, `nesPrgRom`, `nesChrRom`, `nesNametableRam`, `nesPaletteRam`, `nesSpriteRam`, `nesSecondarySpriteRam`

### CPU Types
- `emu.cpuType.nes` — NES 6502

### Headless Settings
- `EnableTestMode: true`, `Debug.ScriptWindow.AllowIoOsAccess: true`, `Debug.ScriptWindow.AllowNetworkAccess: true`, `Debug.ScriptWindow.ScriptTimeout: 60`
- `Nes.RamPowerOnState: 1` (AllZeros), `Nes.RandomizeMapperPowerOnState: false`, `Nes.RandomizeCpuPpuAlignment: false`
- Remove `mesen.lock` from `external/mesence/` before running

### mesen-for-ai Project (Reference — evaluated, rejected)

**URL**: https://github.com/paulomanrique/mesen-for-ai

MCP server wrapping the Lua API (stdio JSON-RPC + TCP bridge, xvfb, per-session Mesen). Rejected: every read it does is one `emu.read()` line in plain Lua; the raw `--testRunner` + file-writing script (§16, `tools/frame_gen/oracle_dump.lua`) covers all oracle needs with no extra processes.

---

## 8. Existing NES Decompilation Projects (Reference)

| Game | Stars | Approach | Status |
|------|-------|----------|--------|
| SMB (nukep) | — | C/C++ translation | Complete |
| Zelda 1 (aldonunez) | 262 | Full disassembly | Complete |
| Contra (vermiceli) | 226 | Annotated disassembly | Complete |
| Tetris (CelestialAmber) | 106 | Byte-for-byte rebuild | Complete |
| Final Fantasy (Entroper) | 99 | Full disassembly | Complete |
| SMB (mstan/nesrecomp) | 18 | Static recompile | Playable |
| Zelda (mstan/nesrecomp) | 11 | Static recompile | 100% playable |
| Bomberman (emu-russia) | 30 | Decompiled source | Complete |
| Tecmo Super Bowl | — | Exhaustive disassembly | 2024 |

**No Super Dodgeball disassembly exists publicly.** The SNES port by Rumbleminze is the closest reference.

---

## 9. Related Resources

- NESdev Wiki: https://www.nesdev.org/wiki/Tools
- RetroReversing (NES): https://www.retroreversing.com/nes
- NESdev Forum: https://forums.nesdev.org/
- Romhacking.net game page: https://www.romhacking.net/games/814
- StrategyWiki: https://strategywiki.org/wiki/Super_Dodge_Ball_(NES)
- SNES Port repo: https://github.com/rumbleminze/super-super-dodgeball
- NESRecomp docs: `/docs/` directory in repo, Discord: `discord.gg/Ad9BwSzctP`
- RetroPortingToolKit Docs: https://github.com/RetroPortingToolKit/RetroPortingToolkit.com/tree/main/data/docs
- mesen-for-ai: https://github.com/paulomanrique/mesen-for-ai
- Zelda NES Recomp: https://github.com/mstan/LegendOfZeldaNESRecomp
- SMB NES Recomp: https://github.com/mstan/SuperMarioBrosNESRecomp
- NESRecomp: https://github.com/mstan/nesrecomp

---

## 10. NESRecomp Submodule Status

**Tracks `origin/master` (currently `3ef948c`) plus active `patches/002,003,004,005` (auto-applied by `setup.sh`; a bare checkout wipes them — `patches/archived/001` stays dormant).**

Upstream history includes the PPUMASK rendering fix (PR #22) and register-propagation/8x16-sprite fixes. Diagnostic builds enable rings + stack tracking (`build-diag/`, gitignored); prod keeps them off.

---

## 11. Current Project Build State

- **nesrecomp submodule**: `3ef948c` (origin/master) + active `patches/002,003,004,005` (auto-applied by `setup.sh`; see §10 and `patches/active-patches.md`)
- **Generated C code**: ~40 part files in `generated/`, gitignored, regenerated from valid `game.toml`
- **Builds**: `build/` (prod, rings off) + `build-diag/` (gitignored, rings + stack tracking)
- **Coverage**: ~4,399 functions analyzed, 30/30 `extra_func` resolved (check `[Eval] Coverage` each regen)
- **Dispatch misses**: 0 (benign `$801B`/`$A3C7` interp skips only, LOG_RETURN-safe)
- **Frame comparison**: frame 1+ ~99.8% structural match vs Mesen frame 11; `smoke 30` exit 0, static title hash `a84564b5`; menus to team-select verified to 330+ frames
- **Venv**: `nesasm` 0.0.8 only (pip broken); system `python3` has Pillow + numpy
- **MesenCE binary**: `external/mesence/Mesen` (83.5MB pre-built)
- **keybinds.ini**: Player 1 keyboard and gamepad mappings configured

---

## 12. Verification Approaches & Disassembly Tooling

### Why C→x86→ROM Comparison Is Unsounded

The NESRecomp toolchain produces C source code compiled to native x86-64. Comparing generated x86 assembly against original 6502 machine code is a category error — they are different ISAs with no meaningful mapping. No recompilation project uses this approach. The verification target is C source behavior, not its compiled form.

### The Gold Standard: Co-Simulation

The entire RetroPortingToolkit ecosystem uses **co-simulation** for verification:
- Run the recompiled binary alongside a trusted emulator (MesenCE) as oracle
- Feed both the same game and same input
- Stop both at the same guest-time checkpoint (VBlank, DMA, etc.)
- Compare CPU registers, RAM, VRAM, cycle counts
- "Self-agreement is NOT accuracy" — both sides can be identically wrong
- See `COSIM.md` and `NES_ACCURACY_BURNDOWN.md` in the upstream nesrecomp repo

### Available Disassembly Tools

| Tool | Location | Purpose |
|------|----------|---------|
| **`tools/dis65.py`** | repo | Dependency-free 6502 disassembler (official opcodes, linear sweep). Verified `$82BD`-family, `$A220`, `$FF3A`/`$FF4C`/`$FCA0`. Use this. |
| **nesasm** | venv (`nesasm` 0.0.8) | Assembler only, **no disassembly mode** |
| **capstone/py65** | Listed in `pyproject.toml` but **never installed** (venv pip broken) | Unavailable — do not reference as usable |

**py65** would need an MPU device instance anyway; capstone has no NES memory-map awareness. Neither is worth fixing while `dis65.py` covers entry verification.

### Practical Verification Pipeline

What actually works for this project:
1. **Co-simulation** against MesenCE (primary method)
2. **Dispatch miss monitoring** — 0 misses means all `call_by_address` targets are recompiled
3. **Symbol files** — Use ca65 disassembly or capstone to generate `.sym` files for `game.toml` to verify function boundaries
4. **Frame comparison** — MesenCE reference vs recompiled screenshots (frame 1+ ~99.8% structural match vs Mesen 11; metric is tolerance-based, exact RGB never matches across renderers)
5. **`run_roundtrip.py`** — Trivial byte-preservation check via `.db` directives (not meaningful for correctness)

### The "True" Round-Trip Problem

A true C→6502→ROM round-trip would require decompiling the generated C back to 6502 assembly, then assembling it back to ROM. This is the inverse of the recompiler — a hard research problem nobody solves automatically. Not feasible as a verification tool today.

---

## 13. NMI Dispatcher, RESET Sequence, Title writer (from SNES Port Fixed-Bank Bytes)

Source: `rumbleminze/super-super-dodgeball` `src/bank7.asm` (verbatim NES fixed-bank bytes with annotations). No public full disassembly or RAM map exists elsewhere.

### NMI dispatcher (`$FF3A`) — 4-way decode of `$0100` (N=bit7, V=bit6)

```
BIT $0100
BPL → NMI-off tail     ($0100 = $00)
BVC → JMP $EF9F        ($0100 = $80, gameplay/menu NMI)
JMP $EFD9              ($0100 = $C0, title NMI)
; at $FF47: BVC $FF4C else JMP $EF80  ($0100 = $40, third path)
; $FF4C tail: PPUCTRL NMI-off, 5×PLA, STA $0100, PLA, RTS
```

- `$EFD9` (title): `JSR $FE34` (OAM DMA) → `JSR $F700` (title staging) → if `$0106` bit0: `JSR $E49C` + `JSR $DB7A/$DF58/$DB4E` → clear `$0106` → RTI
- `$EF9F` (gameplay): set `$0106` bit7 → `JSR $FD5D` → Start check (`$F7 & $F5 & #$10`) → on Start: `JSR $FE8A` + stack reset (`LDX #$01FF; TXS`) + `JMP $F09F`, else sound/scroll work → clear `$0106` → RTI. **Never C-call `$F09F`** — it must be entered via the NMI stack-reset path.
- `$0100=$80` at NMI time runs `$EF9F`, which never calls `$E49C`. Note: the working build shows the title with `$0100=$80/$91` persistent — the strips arrive via main-thread setup (`$F378` chain), not the `$E49C` gate, so `$C0`-or-bust is not the whole story; `$0106` stays `$00` throughout.

### Writers of `$0100`

- RESET: `LDA #$C0 STA $0100`, `LDA #$50 STA $0101`
- `$F378` (title setup): `LDA #$80 STA $0100`, `LDA #$00 STA $0106`
- `$FCA0`: saves `$0100`, writes `$0100=$00` (NMI-off critical section), re-enables NMI
- NMI exit tail: `STA $0100` (lock handoff from stack)

### RESET (`$FF61`, via `$FFE8` stub: `INC $FFF1; NOP×3; JMP $FF61`)

`SEI; LDA #$10 STA $2000/$FF; CLD`; 2× vblank wait; `STX $FD/$FC/$FB`; `STX $4016`; `$F9=$FF`; `$4017=$40`; `$4015=$0F`; `LDA #$1E JSR $FE96` (MMC1 init = PRG mode 3, 4KB CHR); `$0100=$C0, $0101=$50`; soft-reset magic (`$0102==$35 && ($0103==$53||$AC)` → warm via `JSR $FCDF`, else cold clears `$0001–$00F8`); `CLI; JMP $F036`. FDS-BIOS `$0100/$0101/$0102` convention — see https://www.nesdev.org/wiki/FDS_BIOS.

### Title writer (`$E49C`)

PPU addr `$20:($40+X)` with `$5F`-clamp, `X=$0107`, up to `$20` bytes/frame from `$0108,X → $2007`, `STX $0107`; if `$70` V-flag: `$0168→$20A2`, `$0169→$20BD`. Staged by `$F700`, gated by `$0106` bit0. Menu indices `$06B1` (mode) / `$06EA` (difficulty).

## 14. game.toml Never Parsed — All Prior Runs Used Default Config

`addr = 0x82BD, bank = 0` on one line is invalid TOML ("extra chars after value"). The committed `game.toml` had this shape, so every regen fell back to default config: no `bank_switch` modeling, no `fixed`/`bankN` lists, no `force_interp`, no `extra_func`. Correct shape:

```toml
[[extra_func]]
addr = 0x82BD
bank = 0
```

Current working config: `bank_switch = [0xFF08]`, minimal `fixed` (verified entries only), dis65-verified bank-0/1/6 `extra_func` (30/30 resolved), empty `[force_interp]`. Validate every regen with the `[Eval] Coverage` line (must show resolved entries, no "could not load game config").

### Verified bank-0 function entries (hand-disassembled with `tools/dis65.py`)

`$82BD, $82D4, $82DB (tail JMP $81B5), $82E6, $82ED, $831C, $8323, $81B5` + `$B0D1` — small `LDA ($61),Y → STA $07xx,X` writers dispatched from `$A220` (bank 6) with X-index. `$AC89` (bank 6) is zero padding — never add `extra_func` for `ZERO_FILLED` targets. `$31D1`-style sub-`$8000` addresses are PPU mirrors, never codegen targets.

## 15. Hang Cause Verified; Cheat Obsolete With Working Bank Switch

- The `g_ppumask = 0x00` direct write in `game_post_nmi()` (commit `3194d28`) is the 3-frame-hang cause: it bypasses the runner's `|= 0x1E` translation and breaks VBlank timing. Removal → `smoke 15` finishes, exit 0, 0 dispatch misses.
- `force_interp` on `0xFF08` (MMC1 bit-bang) garbles graphics — the bit-bang must run natively. Bisected: empty `force_interp` restores baseline pixels exactly.
- With `game.toml` loading, `bank_switch=[0xFF08]` works, so real game code runs and owns the nametable (`func_FF61` zeroes it per frame). Manual tile writes cannot survive: orange-background frame (21k orange px vs 49k black) proves real code overwrites cheat tiles. Title must come from the real NMI path, not `game_post_nmi`.
- Stripped build (family-shape hooks) hangs in frame 0 at `$FCB3: BNE $FCB3` (fixed bank, after NMI-enable). The only exit is the NMI-off tail (`$FF4C`: NMI-off, `PLA×5`, `STA $0100`, `PLA`, `RTS`), which ends in `RTS` so `g_rti_target==0` and the runner restores registers+stack, rewinding the unwind. Dispatch trace goes silent after 5 events (last: NMI dispatch from `$FCB3`). Candidate submodule-level fix: NMI-tail unwind semantics for RTS-terminated handlers. Debug with `NESRECOMP_DISPATCH_TRACE=8000:FFFF` + `NESRECOMP_DISPATCH_TRACE_FILE` (needs a diagnostic build — prod compiles tracing out).
- `tools/dis65.py`: minimal dependency-free 6502 disassembler (ROM file + hex addr + count + file offset). Use for entry verification; venv has no capstone/pip.
- An NMI-off spin freezes smoke: 40M+ boundary ticks at `$FCB3` with ops sawtoothing (budget consumed, no completions; temporary tick-rate sampler). Separately, entering `func_NMI` with NMI disabled corrupts the main stack (no hardware frame was pushed for the tail to pop) — `game_run_nmi` must gate on PPUCTRL bit 7. NMI-off branch behavior (`nes_cosim_emit_boundary` vs callback) is as-read in `runtime.c`; the exact counting interplay when PPUCTRL flickers mid-frame is unverified.
- Native RTS is a C return: the `$FF4C` tail's g_ram unwind is ignored for control flow, so execution resumes inside the spin. `game_run_nmi` must gate on `g_ppuctrl & 0x80` (ungated NMI-off entry corrupts the main stack — no hardware frame was pushed). Forcing `$0100=$C0` keeps NMI enabled and frames advancing (15, static wrong screen) but strands main in the spin.
- Runner unwind idea archived, not shipped (`patches/archived/001_nmi_tail_unwind.patch` + `archived-patches.md`): pop the skipped RTS return and resume the guest there at frame end. Never fired successfully — the game-side `replace_func` bypassed the tail path first. Evidence kept: NMI survey (§17), SMB3 A/B (byte-identical), pixel-identical title with/without. Revival conditions in the archive README.
- RESOLVED via `[[replace_func]]` for `$FCA0` instead: the `$FCB3` spin is removed game-side (NMI-enable + PPUMASK apply retained). Current form is split — instant pre-menu, faithful NMI-waiting park post-tunnel (`s_menu_phase`) pacing the menu loop (see §19.6). Result: title 99.8% structural match vs Mesen frame 11 from frame 1, `smoke 30` exit 0, 0 dispatch misses.
- Open (updated 2026-10-01): `$8003` bank-8 miss resolved (S-latch fallout); BRK skips benign/transient; `func_DB4E` load-bearing untested; input tunnels past title (see §19).

## 16. MesenCE Oracle: Raw --testrunner + Lua, Not mesen-for-ai

mesen-for-ai (MCP server) adds TCP/xvfb/session machinery for zero extra PPU visibility — every read it does is one `emu.read()` line in plain Lua. Use raw `--testrunner` + file-writing Lua.

- Mapped reads (what the game sees, MMC1 applied): `emu.read(addr, emu.memType.nesPpuMemory)` for `$0000–$1FFF` (live CHR), `$2000–$2FFF` (nametables, mirroring applied). Raw ROM: `nesChrRom` (131072 bytes), `nesPrgRom`. RAM: `nesInternalRam`; PPU regs: `nesPaletteRam` (32B), `nesSpriteRam` (256B), `nesNametableRam` (CIRAM 2K pre-mirroring). Prefer `*Debug` memTypes (`nesDebug`, `nesPpuDebug`) — plain reads have side effects (`$2002` clear, `$4016` advance).
- No mapper-state API. Live CHR banks via `emu.convertAddress(ppuAddr, emu.memType.nesChrRom, emu.cpuType.nes)` per frame, or fingerprint the 4K mapped window against CHR-ROM slices.
- Per-NMI flags: `emu.eventType.nmi` callback + `emu.getCpuState(emu.cpuType.nes)` + RAM reads (`$0100/$0106/$0107/$0108/$0168`). Scroll/video-latch keys come from `emu.getState()` (key names vary by build — enumerate, never hardcode).
- Frame counting in `endFrame`; exit with `emu.stop(0)`; output via files (`io.open`) or `emu.takeScreenshot` — testrunner stdout does not carry logs. Never call `emu.reset()`/`emu.resume()` in testrunner (exit 255). `createSavestate` only works inside `exec` callbacks.
- Headless needs `xvfb-run` + isolated HOME with `settings.json` (`AllowIoOsAccess`, `ScriptTimeout: 60`, `RamPowerOnState: 1`, no mapper/CPU-PPU randomization); delete `mesen.lock`.
- Reference oracle script: `tools/frame_gen/oracle_dump.lua` (frame-N PPU dump + per-NMI flag log + Start injection + mapper-key enumeration).
- Who-wrote-it queries: `emu.addMemoryCallback(fn, emu.callbackType.write, start, end, emu.cpuType.nes, memType)` + `pc` from `getCpuState` inside; CDL tables are 0-indexed in a 1-indexed language.

---

## 17. NMI Termination Survey (Patch Blast-Radius Evidence)

Question: which games would behave differently if post-NMI rewind is skipped whenever `S != entry S`? Surveyed via disassembly repos + nesdev wiki.

| Game | Verdict | Detail |
|---|---|---|
| SMB3 | RTI | `IntNMI` pushes PHP/PHA/TXA/PHA/TYA/PHA + temps; all paths converge to PLA/PLP/RTI epilogue. Nested re-entry only DECs `VBlank_Tick`, same RTI. Only TXS is reset. |
| SMB1 | RTI | `NonMaskableInterrupt` ends `rti`. RTS routines are NMI-called subroutines, not top level. |
| Zelda 1 | RTI | `IsrNmi` ends PPUCTRL-restore + `RTI`. |
| Dr. Mario | RTI | NMI falls through into single-`rti` IRQ stub. |
| Metroid | RTI | Ends `LC113: RTI`, `NMIStatus` + `WaitNMIEnd` protocol. |
| Faxanadu | RTI | Pops into `OnHardwareInterrupt: RTI`. RTS-dispatch (`JSR $F859`) exists but only in main code. |
| Yoshi's Cookie | Likely RTI | No public disassembly; zero-divergence report under rewind runner implies balanced. |
| FDS BIOS | RTS (tail only) | `BIT $0100` dispatch; NMI-off path: 3×PLA discard + `PLA/STA $0100`, `PLA, RTS`. Only retail-FDS shape; cart ports (Metroid/Zelda) were rewritten to RTI. |

No surveyed cart leaves the NMI stack unbalanced; every TXS found is in reset/init. No nesrecomp issue covers NMI RTS-vs-RTI. Conclusion: the unwind branch is dormant for all surveyed titles (zero expected blast radius); it activates only for FDS-tail shapes. Caveats: Faxanadu's in-NMI RTS thunks could unbalance S via recompiler artifacts; SMB3 nested re-entry is safe by construction (each level snapshots its own entry S). Gate narrowly + log S-delta histograms before enabling globally.

---

## 18. Input Frontier (Title → Menu Transition)

Status: input flows but the game never reacts; Start press derails. All verified by execution (`--script` HOLD/RELEASE/WAIT_RAM8/DUMP_RAM + `NESRECOMP_TRACE_CTRL4016` + MesenCE oracle + diag-build chains).

> Superseded by §19 (all items below root-caused and fixed); kept as the pre-fix evidence trail.

- **Runner input path is correct**: scripted `HOLD START` shows `btn=10` on `$4016` reads frame 1. `$FDC2` is a textbook strobe/shift reader; runner latch/shift verified bit-for-bit. `g_controller1_buttons` fills from script/TCP/keyboard as designed.
- **Polls stop ~frame 18, deterministically**: `$4016` traffic (strobe writes + reads) ends; `$0100=$90` stable, `$0106` latches `$90` from ~20. EF9F's `BMI` skip then self-sustains (skip path never clears bit7; nothing in menu clears it). Clearing bit7 pre-NMI does not restore polls; clearing all of `$0106` hangs boot (staging bits load-bearing) — both reverted.
- **Hardware idles the same way**: oracle `nmi_log` shows every NMI from frame 14 taking the tail path (`$0100=$00`, `$0106=$80` stuck). Title/menu idle with silent NMI is authentic. Hardware Start must therefore arrive via **main-thread polls**: `JSR $FD5D` sites exist in bank1 `$BA6A` and bank4 menu code (`$8EE9/$9732/$999D/$99BA/$99E8/$9A04/$9ECE/$9F1E`). Our main evidently never reaches them (menu state frozen).
- **Start-during-poll derails**: EF9F Start branch (`JSR $FE8A`, `LDX #$FF; TXS`, `JMP $F09F`) → BRKs at `$0001`/`$0091`/`$0092` (RAM zeros), `$092D`/`$093D` (bank-3 data), `$0600`, `$00A2` (BRK-slide through zero page from ~frame 18) → hang. BRKCTX chain: `FCBF→FA15→8000(b0)→FA00→8003(b0)→80EF→8466/84A8/86A8` sound-cluster recursion, nested `FF3A` frames.
- **MMC1 bank-8 corruption at frames 14/16** (mapper trace): clean `6↔0` toggling until `$FF08` runs with entry A=`$90` — first write hits the bit7 RESET, the 5-bit sequence loses alignment, later commits land on invalid bank 8 (`$8003` miss, sound tick skipped → frozen tone = user's repeating-tone report). No static `LDA #$08`/`LDA $FF` call site exists; 12 PLA-fed `JSR $FF08` sites exist only in bank-7 file bytes (mapping unclear). Corruption precedes the frame-18 derail.
- **C-stack growth ~1 `func_FF3A`/frame** (diag HANG dump: 511 deep at frame 558, interleaved `EF9F/FA00/sound-cluster`): NMI C-frames accumulate without returning. Ticking bomb for long sessions; likely the same control-flow wrongness as the `$F09F` hang.
- **Frozen tone explained**: `$FA00` tick starved (`$8003` skipped on bank-8 miss) + polls stopped. Same root causes as input, not a separate audio bug.
- Interactive run (347 frames, clean window close): same BRKs plus `$00A2`@18. SDL_QUIT handling works; the unkillable case is headless-only deep hangs.

---

## 19. Input Frontier Resolution (Start Works; Menu Boots; 2026-10-01)

Status: Start tunnels to menus reliably (30/30 through 330/330
frames, 0 misses, 0 BRKs): title → city → lineup → MODE MENU → skill
(animated sprite) → team-select (`CHANGE POSITION?` + rosters + courts,
renders perfectly). Menu navigation verified (`$06B1` moves on dpad,
Start/A confirms advance screens). Fixed along the way: the frame-~182
derail (empty-stack `PLA PLA` in bank-6 `$8393` shared tail — see below).
Remaining: court BG stalls as tile-soup past team confirm (sprites/sound/
logic run; strips drain top rows only), and unbounded lap growth
(~5 C-frames/frame, slow-burn).

### 19.1 S-latch root cause: interpreter RTS after native JSR (RUNNER BUG, FIXED)

Polls stopped ~frame 18 with `$0106=$90` and no `nes_write` to `$0106`
after frame 18. Per-instruction S trace (`nes_cpu_instruction_boundary`
TEMP log of every S delta + PC) showed `func_FA00` exiting with S lifted
+6..+48 per call (entry F9 → exit FF/01/29). Narrowed to single +2 steps at
`$812A` (post-`JSR $84A8`) with no intermediate push/pop: the `$86C2
JMP $07B4` RAM trampoline (`STA $40xx; RTS` written by `$8035`) runs via
`call_by_address_tail` → `nes_interp_dispatch` → interpreter, whose RTS
pops 2 bytes as a return address. But the outer `JSR $84A8` was a NATIVE C
call that pushed nothing — so each trampoline RTS pops 2 bytes of live
caller data. 3 trampolines/frame → +6/frame → S wraps into `$0100`,
clobbers `$0106` to `$90`, `EF9F` BMI-skip self-sustains, polls die. All
follow-on symptoms (bank-8 `$8003` miss from corrupted A at `$FF08`,
`BRK $0001`/`$00A2`/`$0600` slides from interp garbage-wandering,
`$90` in `$0100`, C-stack growth) were downstream of the S drift.

Fix (`patches/002_interp_ram_dummy.patch`, active): in
`nes_interp_dispatch_bank`'s RAM/SRAM path, pre-push a dummy 6502-stack
pair before `interp_run` (standing in for the missing hardware JSR push),
strip it on exit if untouched. Terminal RTS then consumes the dummies and
the stack-lift rule exits with S exactly restored. Post-fix: `$0106`
clean, 0 misses, 0 BRKs. Also verified: `func_FA00` no-op experiment gave
perfectly balanced S (EFAE/EFD0 = F9 every frame), proving the sound
engine was the sole imbalance source.

### 19.2 NMI-tunnel unwind (RUNNER FEATURE + GAME WIRING, ACTIVE)

`EF9F` Start branch (`JSR $FE8A`, `LDX #$FF; TXS`, `JMP $F09F`) relocates
main into `$F09F` from inside NMI. C-calling `$F09F` never returns: the
outer frame callback never completes, frames freeze, no render/count.
Fix (`patches/004_nmi_tunnel_unwind.patch`, active): opt-in
longjmp unwind — game arms `g_tunnel_site = $EFC1`; a TXS-site marker
(`coroutine_scheduler_setjmp`, which already exists at every
`LDX #$FF; TXS`) reached while armed, at the site, inside NMI
(`depth > 0`) resets vblank depth and longjmps to `game_run_main`'s
landing, which runs `func_F09F()` natively on the main context. NMIs then
interrupt normally (hardware behavior). Generic mechanism (site 0 =
disabled, zero effect otherwise), game-specific arming. Companion
game-side pieces in `src/extras.c`: `NESTED_NMI_RUN_HANDLER` (nested
`$C0`/`$EFD9` must run — the transition lap's `$F1A2` wait is released
only by a real `$EFD9`, runnable solely from nested callbacks while the
tunnel is in flight), nested gate (only `$C0` runs nested — blocks
re-tunnel; `$80`/`$40`/`$00` skip S-safely via runner restore), Start-edge
inhibit until `$F0C3` (`$0100=$C0`, held Start can't re-tunnel mid-init),
one-service-per-frame pacing gate (nested `$C0` skips when the outer
already ran — without it the deferred budget-cap NMI double-clears
`$0106` and menu timers run ~2× ahead of hardware), code-window scoping.

Depth subtlety: inside `game_run_nmi`, depth 2 IS the top-level NMI
(trigger 1 + firing 1); truly-nested is > 2. A `> 1` gate silently skips
every handler (title still renders main-thread-only, polls never run).

### 19.3 Post-tunnel hardware behavior (ORACLE)

With Start@11-12, hardware shows: `$0100` oscillates `$80`/`$00` (never
`$C0` past frame 13!), `$0106=$80` stuck, ~1 NMI/frame (tails), `$F5/$F7`
decay to `$00`, `$06B0=$00` steady, `$78=$00` steady, `$69E` counting
~+1/frame, sp steady `$F6`. I.e. main runs everything, NMI is tails, and
the lineup idles indefinitely awaiting input. Our `$06B0` cycling (0-3)
vs oracle steady, and our `$78=$41` (authentic `F5E8`-init code reached
early via instant-`$FCA0` timing) vs oracle `$00`, are pacing divergences:
instant-`$FCA0` (spin removed) blows through NMI-syncs in one frame where
hardware parks ≥1 frame each. A faithful park version (pushes + boundary
spin till next NMI) was tried and REVERTED (title boot garbles to 23% —
park timing vs boot strip staging; cause unisolated). Spin-less `$FCA0`
stands (title 98.9%).

### 19.4 Discovery loop (all entries dis65-verified before adding)

Menu/init writer family (bank 0, `$A220`-table `LDA ($61),Y` writers +
APU-silence + setters): `$8308, $8312, $8381, $8388, $83AD, $83D3, $83E1,
$83FE, $82C1, $83C0, $8295, $82F6, $801B` (sound-engine alternate entry);
menu input checker `$897D` + sibling `$89A9` bank 1
(`BIT/BVS/BMI` + `LDA $27/CMP #$10` Start test, called from `$8809` at the
lineup→match transition). Excluded with proof: `$B183` bank 0 (data soup),
`$31D1` (ZERO_FILLED PPU mirror), `$8024` bank 0 (mid-entry `$8027`
trampoline setup — symptom of bad-vector dispatch, not an entry).
30/30 resolved, 0 suspicious. `$8388` silences the APU at Start
(`STA $4015`); title music verified perfect interactively. State machine
`$8E00` bank 6 (called from fixed `$C0F3`) added with proof. Benign skips
needing no entries: `$801B` via interp path and `$A3C7`
return-continuation (LOG_RETURN-safe, game completes).

### 19.5 Open: frame-~182 derail, lap growth, menu navigation

- Idle path derailed ~frame 182 (FIXED via `[[replace_func]] $8393` bank 6): single +2 S-lift, then `A=$10` stack garbage → `FF08($10)` → bank thrash → `BRK $C000` + `$8024`/`$8003` misses → hang. Root cause: bank-6 shared tail `$8393` reached with an empty stack ate `$0100/$0101`. Post-fix 240/240 frames clean.
- C-stack lap growth: `F1AA BNE $F13A` compiles to `call_by_address`
  (+1 C-frame/lap, depth 72→84 in ring, 509 at hang). `[[merge_range]]`
  over `F09F`/`F13A` ranges converts laps to gotos (verified in codegen)
  but CANNOT ship: merged entries double-emit across bank-7 codegen parts
  sharing one TU (`_full.c`) and break the build (recompiler limitation,
  documented in `game.toml`). Needs a recompiler-side dedup fix.
- Menu navigation: VERIFIED working — dpad moves `$06B1`, Start/A confirms advance mode → skill → team-select (`CHANGE POSITION?` + rosters + courts, renders perfectly). Confirm is Start-edge driven (`$F5&$10` at `$F445`/`$F63A`); A navigates nothing in `$F49A` (`AND #$2F` excludes it). Cursor/progress tracked via `$06B1` DUMP_RAM, not just pixels.
- User-reported (interactive): sprites glitchy in the two post-title
  menus (since improved — menus render cleanly paced), music perfect
  throughout; gameplay-time freeze reports predate the `$8393`/tunnel fixes
  and need re-testing against current build.

### 19.6 Menu loop + pacing (FCA0 park split, 2026-10-01)

Oracle who-writes trace (Start@10-12, frames 12-60) shows the hardware menu
loop is exactly 4 writes/frame: tail `STA $0100=$80` (interrupted PC low
byte `$80`), `INC $069E` @ `$F424`, `FCA0 STA $0100=$00` + park. Main runs
`$F403`-loop (FCA0-sync, FD5D polls, Start-edge check at `$F445`,
mode tables at `$F494/$F497`); `$06B0`/`$0106`/`$78` untouched, `$69E`
+1/frame, `$70` evolves. The game NEVER runs F09F-dispatch post-tunnel.

Recomp consequences: (a) instant-`$FCA0` lets the menu loop lap ~30×/frame
(`W69E` trace: 29 INCs in frame 10) — timers race, input uncontrollable.
(b) A full faithful park (also at boot) garbles boot strips (23%).
Fix: SPLIT `$FCA0` — instant pre-menu (title stays 98.9%), faithful park
(pushes + NMI-enable + boundary spin till one NMI) post-tunnel
(`s_menu_phase`, set at the tunnel landing). Park order matters: park must
run NMI-enabled (hardware enables before spinning); parking with NMI off
deadlocks till the iteration cap. With parks: title → city → lineup →
MODE MENU (`GAME MODE ... SKILL LEVEL ...`) → skill confirm animates.
Start/A/confirm inputs all process (hashes advance, 0 misses).

Caveat: the tunnel longjmp skips frame/vblank callback-depth decrements
(and tail/dispatch/stack-tracking cleanup). The landing must call
`runtime_prepare_guest_resume` (not just vblank reset) or frames freeze
(post_nmi/render/count skipped as "nested") and NMI never fires in C
spins. Symptom of the stuck depth: `[PARK] depth=0` yet no NMI for 100M
iterations (NMI-off path also implicated: park entry with ppuctrl=$00).

Transition tears: skill-confirm leads through 1-2 garbled frames (partial
redraw) that resolve into the next clean screen; not a stuck state.
`BRK $0001` seen once mid-transition (frame 135, skipped, no fallout).

### 19.7 Court BG stall (current frontier, 2026-10-01)

Past team confirm the BG stalls as tile-soup (uniform cleared nametable +
fragments) while sprites animate, sound ticks, input processes, and frames
advance with 0 misses to 330+ frames. Evidence: PPU-write trace during the
stall shows ONLY scroll (`$2005` via `DB4E`) — zero `$2006`/`$2007`; strips
drain top rows only (`$2060/$2080/$2040` via `E49C`, repeating); uploaders
(`$BA80` reader, `$BACC`, `$BB56`) never fire. Prep (`$B922-$B993`,
`$B9D9` 6-round `$90` cycle) and `$BD6A` waits (incl. 128-frame `$80`) run
repeatedly; `$017A` stream pointer never reaches its post-wait value, so the
long waits never complete (suspect: per-lap re-entry resets them, or the
draw phase is gated behind unmet state). `$0100=$00` (tails, no NMI
services), `$0106=$80` stuck, `$FF=$80` (BG pattern `$0000`).
`$B8C8`-region draw entries have no static callers (fallthrough/computed
only). Next: find the draw sequence's per-lap caller, or the gate that
withholds the content phase.

---

## 20. Engine validation via blargg harness (2026-10-01, `tools/blargg/`)

Recompiling blargg CPU singles against the current tree (patch-neutral
extras, wall-clock `$6000` poller — tests complete with 0 frames and park
in the shell's `forever` loop, so frame-boundary polling alone false-HANGs
everything). Baseline (runner = pristine `origin/master` + `patches/*.patch`): PASS 2 (`01-basics`, `16-special`),
FAIL 14, HANG 4 (`cpu_timing_test6`, `branch_1/2/3`), SKIP 5 (banked).

Durable backend-gap evidence (all reproduced on pristine `origin/master`,
so upstream's, not ours):
- JSR/RTS-template checksums over S and pushed return addresses mismatch:
  native JSR/RTS are C calls pushing nothing (`12-jmp_jsr`, `13-rts`;
  cf. upstream `NES_ACCURACY_BURNDOWN.md` Axis 4/6).
- `11-stack` → `internal_error` code 255: PLA/PLA stack-introspection pops
  live data (same class as the `$8393` tail, §19.5).
- `15-brk` → BRK skip-stub (BRK vectoring unimplemented, upstream-known).
- HANGs: APU-length-counter `$4015` spins (branch family) and periodic-NMI
  waits (`cpu_timing_test6`).
- Lap-growth corroboration: `instr_01` template helpers (`E442`/`E458`/
  `E8CF`) stack 510 deep like Dodgeball's `F1AA`/`F13A` (§19.5) — same
  `call_by_address` lap class, independent ROM.
Patches exonerated twice over (construction + pristine A/B with identical
verdicts and HANG stall frames); details in `tools/blargg/README.md`.

---

## 21. TAS-integrated differential harness (2026-10-01, `tools/tas/`)

Shared-input co-sim closing the loop from §§12/16: one TAS drives both the
MesenCE oracle and the recomp; diffs land on the first divergence.

- **Vectors**: TASVideos #4976 (ShesChardcore & Dasrik 2022, BizHawk 2.8,
  19692 frames, hardest difficulty) in `tas/vectors/sdb_4976.bk2` —
  headerless MD5 matches our ROM (`6e44871b…`), first Start at frame 11
  (matches `oracle_dump.lua` Start@11-12). No USA Dodgeball `.fm2` exists
  anywhere (only `.fcm` #742 + `.bk2` #4976); `fm2_to_tas.py` covers text +
  binary logs and is validated on an SMB2 `.fm2` sample (1016 frames as
  listed; `RLDUTSBA→0xFF` unit-checked). Manufacturing a USA `.fm2` =
  FCEUX `Tools → Convert FCM` on the #742 `.fcm` (sync not guaranteed).
- **Design**: bk2/fm2 → canonical `tas.json` (recomp mask bits) → MesenCE
  `mesen_tas.lua` (per-frame state.jsonl with regs/`$0100`-family/scroll/
  cycles/input echo + NMI log; RAM/CHR/NT/pal/OAM/ciram bins + PPM at
  stride; input injected in `inputPolled` only) and recomp `--script`
  (RLE HOLD/RELEASE/WAIT + `WAIT 1000000` tail so the script outlives
  `--smoke`; full 19k TAS emits 2361 cmds < 4096 cap) + env traces
  (WRAM/PPUMEM/cosim/APU) + smoke JSON → `diff_tas.py` gates liveness
  (`frames_run == N`, 0 misses, no `[HANG]`) → renderer-independent state
  (delta traces replayed, byte-compared, stack masked, joined on
  video-frame index) → images informational only. Frame 0 is black warmup
  (title from frame 1); no offset search — same-index flags real drift.
- **Metric lesson**: `compare_frames.py`'s palette was mostly `[0,0,0]`
  rows, collapsing all dark colors to one index (matched solid-green vs
  solid-black at "100%"). Table now equals the runner's own
  `g_nes_palette` (exact for recomp, nearest for Mesen). Cross-renderer
  RGB is still palette-relative by construction — state bytes are the
  cross-side signal, pixels are secondary.
- **Boot datum**: oracle shows backdrop-green at file 0, recomp stages
  strips over ~10 more files (`$00FF` PPUCTRL-shadow NMI-bit timing differs)
  — systematic staging lag, filed as timing gap, not failure. Neighbor test
  (same-index 9 diffs vs 20–110 off-by-one) proves the join is sound.
- **Honesty**: `NESRECOMP_COSIM_INJECT=5:ram:0x10:0xff` moves `chain`
  exactly at f=5 (r30inj); A/A byte-identical both sides (r400 vs r400b
  recomp incl. all 400 shots; r400 vs r400c Mesen incl. bins/PPMs).
- **First finding**: 400-frame TAS run green on liveness (400/400, 0 misses)
  but mode-menu confirm diverges ~TAS frame 44 — hardware advances to
  team-select by f≈50–60, recomp idles on the mode/skill menu (cursor moves,
  confirms don't land). Suspect: 1–2-frame TAS taps vs edge-driven confirms
  (`$F5&$10`, `AND #$2F` quirks, §19.5). Next: `$06B1`/`$F5` edge trace
  around f=44, memwatch vs WRAM-watch.
