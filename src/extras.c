#include "game_extras.h"
#include "nes_runtime.h"
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/types.h>

extern void func_RESET(void);
extern void func_FF3A(void);
extern void func_FE34(void);
extern void func_F700(void);
extern void func_DB7A(void);
extern void func_DF58(void);
extern void func_DB4E(void);
extern void func_E4E5(void);
extern void func_F09F(void);
extern uint8_t g_ppumask_translate;
extern uint8_t g_ppuctrl;
extern uint8_t g_ram[0x0800];

/* NMI-tunnel state: the title→menu tunnel ($EFBC: JSR $FE8A, LDX #$FF, TXS,
 * JMP $F09F) relocates main into $F09F from inside NMI. The runner unwinds
 * the NMI C-stack via longjmp to game_run_main's landing (see
 * patches/004_nmi_tunnel_unwind). s_tunnel_inhibit suppresses Start
 * re-detection while the tunnel window is open ($0100 still $80, $F0C3 not
 * yet run) so held Start can't re-tunnel mid-init. */
static volatile int s_tunnel_inhibit = 0;
/* Last frame whose outer (top-level, depth<=2) NMI ran to completion. A
 * nested NMI that finds its frame already serviced skips: without this the
 * deferred budget-cap NMI runs a second $EFD9 per wall-frame (double
 * $F700 staging, double $E49C strips, double $0106 clear → up to 2 main
 * laps/frame), running menu timers ($069E) ahead of hardware. The tunnel
 * frame never completes its outer NMI, so transition laps still get their
 * nested $EFD9. Set AFTER func_NMI returns. */
static uint64_t s_nmi_done_frame = (uint64_t)~0ULL;

const char *game_get_name(void) { return "Super Dodge Ball (USA)"; }
const char *g_rom_path_for_extras = "";

/* Options — set to 1 to enable */
static int s_oam_flicker_fix = 1;
static int s_save_screenshot = 0;
static const char *s_screenshot_dir = NULL;

static uint8_t s_oam_backup[0x100];
static uint8_t s_oam_x16_backup[64];
static int s_first_frame = 1;
static uint8_t s_ppuctrl_backup = 0x10;

/* Read game bytes from the user's ROM at run time (never embedded in
 * tracked source). iNES header is 16 bytes; a 512-byte trainer follows iff
 * header[6] bit 2 is set. PRG bank N starts at base + N*16384. */
static int read_prg_bytes(uint32_t prg_offset, uint8_t *out, size_t len) {
    FILE *f;
    uint8_t hdr[16];
    long base;
    size_t n;
    if (!g_rom_path_for_extras || !g_rom_path_for_extras[0])
        return 0;
    f = fopen(g_rom_path_for_extras, "rb");
    if (!f)
        return 0;
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "NES\x1a", 4) != 0) {
        fclose(f);
        return 0;
    }
    base = 16 + ((hdr[6] & 0x04) ? 512 : 0);
    if (fseek(f, base + (long)prg_offset, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    n = fread(out, 1, len, f);
    fclose(f);
    return n == len;
}

void game_on_init(void) {
    /* Nested NMI must RUN the handler (hardware behavior): after Start the
     * game tunnels main into $F09F, whose $0106 handshake wait is released
     * only by a real NMI ($EFD9 clears $0106). The default skip-shim would
     * deadlock $F09F at $F1A2. This also matches the title phase (extra
     * polls are edge-safe: FD5D debounces via $F5/$F7). */
    g_nested_nmi_policy = NESTED_NMI_RUN_HANDLER;
    /* Arm the NMI-tunnel unwind for the title→menu tunnel TXS site $EFC1
     * (see game_run_main landing). Site-specific: no other TXS can fire it. */
    g_tunnel_site = 0xEFC1;
    g_tunnel_armed = 1;
    /* 20c9b28-era setup (palette, PPUCTRL shadow, CHR, attr). No mask toggle. */
    /* Initialize NES palette mapping. g_ppu_pal[0] = background index,
     * g_ppu_pal[1..3] = sub-palette 0 colors, etc. */
    g_ppu_pal[0] = 0x0F;  /* background = index 15 (black) */
    g_ppu_pal[1] = 0x00;  /* sub-palette 0, color 1 = index 0 */
    g_ppu_pal[2] = 0x00;  /* sub-palette 0, color 2 = index 0 */
    g_ppu_pal[3] = 0x00;  /* sub-palette 0, color 3 = index 0 */
    g_ppu_pal[4] = 0x00;  /* sub-palette 1, color 0 = index 0 */
    g_ppu_pal[5] = 0x01;  /* sub-palette 1, color 1 = index 1 */
    g_ppu_pal[6] = 0x02;  /* sub-palette 1, color 2 = index 2 */
    g_ppu_pal[7] = 0x03;  /* sub-palette 1, color 3 = index 3 */
    g_ppu_pal[8] = 0x00;  /* sub-palette 2, color 0 = index 0 */
    g_ppu_pal[9] = 0x04;  /* sub-palette 2, color 1 = index 4 */
    g_ppu_pal[10] = 0x05; /* sub-palette 2, color 2 = index 5 */
    g_ppu_pal[11] = 0x06; /* sub-palette 2, color 3 = index 6 */
    g_ppu_pal[12] = 0x00; /* sub-palette 3, color 0 = index 0 */
    g_ppu_pal[13] = 0x07; /* sub-palette 3, color 1 = index 7 */
    g_ppu_pal[14] = 0x08; /* sub-palette 3, color 2 = index 8 */
    g_ppu_pal[15] = 0x09; /* sub-palette 3, color 3 = index 9 */

    g_ppumask_translate = 1;
    g_ppumask = 0x1E; 
    g_ram[0xFF] = 0x10;  /* Initialize PPUCTRL shadow */
    nes_write(0x2000, 0x10);  /* Force BG pattern table at $1000 */
    g_ppuctrl = 0x10;

    /* Force MMC1 to 4KB CHR mode (control register = 0x1E) via bit-bang.
     * The original func_FE96 bit-bang at startup is not correctly recompiled.
     * 0x1E = 0b00011110: 4KB CHR mode, PRG mode 3, single-screen lower mirror.
     * Shift in bits LSB-first: 0,1,1,1,1 (bits 0-4 of 0x1E = 11110). */
    nes_write(0x8000, 0x00);  // bit 0 = 0
    nes_write(0x8000, 0x01);  // bit 0 = 1
    nes_write(0x8000, 0x01);  // bit 0 = 1
    nes_write(0x8000, 0x01);  // bit 0 = 1
    nes_write(0x8000, 0x01);  // bit 0 = 1 (5th bit)

    /* Verify CHR mode is now 4KB by forcing PPUCTRL=0x10 (pattern table $1000) */
    nes_write(0x2000, 0x10);
    g_ppuctrl = 0x10;
    g_ram[0xFF] = 0x10;

    /* Initialize attribute table for nametable 0 ($23C0-$23FF) from the
     * user's ROM (PRG bank 0 offset 0x073C, 64 bytes). Without this,
     * attribute table is all zeros -> 2-color display. */
    nes_write(0x2006, 0x23);  // PPUADDR high = $23
    nes_write(0x2006, 0xC0);  // PPUADDR low = $C0 (attribute table for nametable 0)
    {
        uint8_t attr_table[64];
        int i;
        if (!read_prg_bytes(0x073C, attr_table, sizeof(attr_table))) {
            fprintf(stderr, "[extras] WARNING: attr table ROM read failed — zeros used (expect 2-color title)\n");
            memset(attr_table, 0, sizeof(attr_table));
        }
        for (i = 0; i < 64; i++) {
            nes_write(0x2007, attr_table[i]);
        }
    }
}

void game_on_frame(uint64_t fc) {
    (void)fc;
    if (s_oam_flicker_fix) {
        memcpy(s_oam_backup, g_ppu_oam, 0x100);
        memcpy(s_oam_x16_backup, g_oam_x16, 64);
    }
}
void game_post_nmi(uint64_t fc) {
    /* Pure real-NMI path: no tile buffer init (the old 512-byte memcpy to
     * $0108 clobbered the 6502 stack page $0100-$01FF and OAM buffer,
     * smashing return addresses → padding jumps → BRK $0001), no manual
     * VRAM writes. $F700 stages $0108; $E49C writes it. */
    if (s_oam_flicker_fix && !s_first_frame) {
        memcpy(g_ppu_oam, s_oam_backup, 0x100);
        memcpy(g_oam_x16, s_oam_x16_backup, 64);
    }
    s_first_frame = 0;

    /* Post-NMI scroll/PPUCTRL apply. Whether this call is still load-bearing
     * for frame advancement is untested (see plan.md open items). */
    { uint16_t wb = g_code_window_base; g_code_window_base = 0xE000; func_DB4E(); g_code_window_base = wb; }

    (void)fc;
}
int game_handle_arg(const char *k, const char *v) {
    if (strcmp(k, "--save-screenshot") == 0) {
        s_save_screenshot = 1;
        s_screenshot_dir = v;
        if (s_screenshot_dir && s_screenshot_dir[0] == '-') {
            /* Next arg is another flag, not a directory */
            s_screenshot_dir = NULL;
        }
        return 1;
    }
    return 0;
}
const char *game_arg_usage(void) { return ""; }
uint32_t game_get_expected_crc32(void) { return 0x689971F9u; }
int game_dispatch_override(uint16_t a) {
    if (a == 0xFE8A) {
        /* func_FE8A: PPUCTRL update - clears NMI enable bit (bit 7) */
        uint8_t ppuctrl = nes_read(0xFF);
        ppuctrl &= 0x7F;  /* Clear bit 7 (disable NMI) */
        nes_write(0xFF, ppuctrl);
        nes_write(0x2000, ppuctrl);
        return 1;  /* Handled */
    }
    return 0;  /* Not handled */
}
uint8_t game_ram_read_hook(uint16_t pc, uint16_t a, uint8_t v) { (void)pc; (void)a; return v; }

/* Replacement for $FCA0 (see [[replace_func]] in game.toml): NMI-enable +
 * PPUMASK apply. Pre-menu (boot/title) the $FCB3 spin is skipped — instant
 * return keeps the title pixel-perfect (98.9%). Post-tunnel (menu phase,
 * s_menu_phase set at the tunnel landing) it parks faithfully: mirror the
 * two hardware pushes (so a park-NMI tail balances), wait one NMI with
 * boundaries flowing, then apply the mask. The park paces the menu loop
 * (F403/F42A FCA0-syncs) to ~1-2 laps/frame like hardware; instant FCA0
 * lets it lap ~30×/frame (menu timers race, input uncontrollable). A full
 * faithful park (also at boot) was tried and reverted — it garbles boot
 * strip staging (23%); the split preserves both. */
static volatile int s_menu_phase = 0;
static volatile int s_nmi_seen = 0;
/* Replacement for $8393 bank 6 (see [[replace_func]] in game.toml): shared
 * tail (STX $1C, PLA, PLA, LDA #$00, STA $1D, LDA $1A, ORA #$80, STA $1A).
 * The PLA PLA consumes 2 ancestor-pushed arg bytes absent under C calls;
 * dummies stand in (values discarded by the body anyway). */
void func_8393_b6(void) {
    g_ram[0x1C] = g_cpu.X;
    g_ram[0x100 + g_cpu.S] = 0; g_cpu.S--;
    g_ram[0x100 + g_cpu.S] = 0; g_cpu.S--;
    g_cpu.S++; g_cpu.A = g_ram[0x100 + g_cpu.S];
    g_cpu.N = (g_cpu.A >> 7) & 1; g_cpu.Z = (g_cpu.A == 0);
    g_cpu.S++; g_cpu.A = g_ram[0x100 + g_cpu.S];
    g_cpu.N = (g_cpu.A >> 7) & 1; g_cpu.Z = (g_cpu.A == 0);
    g_cpu.A = 0x00;
    g_cpu.N = 0; g_cpu.Z = 1;
    g_ram[0x1D] = g_cpu.A;
    g_cpu.A = g_ram[0x1A];
    g_cpu.A |= 0x80;
    g_cpu.N = (g_cpu.A >> 7) & 1; g_cpu.Z = (g_cpu.A == 0);
    g_ram[0x1A] = g_cpu.A;
}

void func_FCA0(void) {
    /* Menu phase parks faithfully (see header comment): mirror the two
     * hardware pushes, NMI-enable, park till one NMI with boundaries
     * flowing (a park-NMI tail then balances), then fall through to the
     * mask apply. Order matters: the park must run with NMI enabled (as
     * hardware does after its ORA $80), or no NMI can release it. */
    if (s_menu_phase) {
        g_ram[0x100 + g_cpu.S] = g_cpu.A; g_cpu.S--;
        g_ram[0x100 + g_cpu.S] = g_ram[0x100]; g_cpu.S--;
        g_ram[0x100] = 0x00;
        g_cpu.A = g_ram[0xFF];
        g_cpu.A |= 0x80;
        g_cpu.N = (g_cpu.A >> 7) & 1; g_cpu.Z = (g_cpu.A == 0);
        g_ram[0xFF] = g_cpu.A;
        nes_write(0x2000, g_cpu.A);
        s_nmi_seen = 0;
        { unsigned long spins = 0;
          while (!s_nmi_seen) {
              nes_cpu_instruction_boundary(0xFCB3, 2);
              if (++spins > 100000000ul) {
                  fprintf(stderr, "[FCA0] park without NMI (S=%02X) — continuing\n", g_cpu.S);
                  break;
              }
          } }
    } else {
        g_cpu.A = g_ram[0xFF];
        g_cpu.A |= 0x80;
        g_cpu.N = (g_cpu.A >> 7) & 1; g_cpu.Z = (g_cpu.A == 0);
        g_ram[0xFF] = g_cpu.A;
        nes_write(0x2000, g_cpu.A);
    }
    /* $FCB3 spin skipped (see above). */
    g_cpu.A = g_ram[0xFE];
    g_cpu.A &= 0xE7;
    g_cpu.N = (g_cpu.A >> 7) & 1; g_cpu.Z = (g_cpu.A == 0);
    g_ram[0xFE] = g_cpu.A;
    nes_write(0x2001, g_cpu.A);
}

void game_run_nmi(void) {
    /* Park release for menu-phase $FCA0 syncs (set even when gated/skipped:
     * the NMI vectored, which is what the hardware park waits for). */
    s_nmi_seen = 1;
    /* Gate on NMI-enable like hardware: without the interrupt frame the
     * RTS-tail misfires and corrupts the main stack. */
    if (!(g_ppuctrl & 0x80))
        return;
    /* Tunnel window: $F0C3 ($0100=$C0) not yet run — mask the Start edge so
     * held Start can't re-tunnel mid-init. Live $4016 still holds the
     * button, so menu polls see the continued hold. $0100=$C0 observed
     * means the tunnel completed: drop the inhibit. */
    if (g_ram[0x100] == 0xC0)
        s_tunnel_inhibit = 0;
    else if (s_tunnel_inhibit && g_ram[0x100] == 0x80)
        g_ram[0xF5] &= (uint8_t)~0x10, g_ram[0xF7] &= (uint8_t)~0x10;
    /* Truly-nested (depth>2: trigger 1 + firing 1 + nested 1) handlers run
     * only for the $C0 title-NMI, and at most once per wall-frame (see
     * s_nmi_done_frame): after Start tunnels main into $F09F, its
     * $F19C/$F1A2 handshake wait is released solely by a real $EFD9 pass,
     * and only nested callbacks can run it while the tunnel is in flight.
     * Other nested modes are skipped: nested $80 would re-detect held
     * Start and re-tunnel ($EFC1 TXS + $F09F) inside the in-flight tunnel,
     * nesting without bound; nested $40/$00 are outer-covered or
     * park-shaped. The runner undoes its own interrupt-frame push on skip,
     * so skipping is S-safe. $EFD9 touches only fixed-bank code and never
     * switches banks, but the code-window var is still scoped across the
     * call for safety. */
    if (runtime_get_vblank_depth() > 2) {
        if (g_ram[0x100] != 0xC0)
            return;
        if (s_nmi_done_frame == g_frame_count)
            return;
    }
    { uint16_t wb = g_code_window_base; func_NMI(); g_code_window_base = wb; }
    if (runtime_get_vblank_depth() <= 2)
        s_nmi_done_frame = g_frame_count;
}
void game_run_main(void) {
    /* NMI-tunnel landing: the runner longjmps here from the tunnel TXS site
     * ($EFC1), unwinding the NMI C-stack. Run $F09F natively on the main
     * context so frame callbacks complete and NMIs interrupt normally. */
    if (setjmp(g_tunnel_buf) != 0) {
        /* Full runner-state reset for the unwound frames (vblank + frame
         * callback depths, tail detector, dispatch depth, interp context,
         * C-stack tracking): longjmp skipped all of their cleanups. */
        runtime_prepare_guest_resume(0xF09F, 0);
        g_tunnel_armed = 1;
        s_tunnel_inhibit = 1;
        s_menu_phase = 1;
        func_F09F();
        for (;;) { /* $F09F returned (never on hardware); park servicing NMIs */
            nes_cpu_instruction_boundary(0xF3B9, 2);
        }
    }
    func_RESET();
}
void game_post_render(uint32_t *fb) {
    (void)fb;
    if (s_save_screenshot) {
        char path[320];
        if (s_screenshot_dir) {
            mkdir(s_screenshot_dir, 0755);
            snprintf(path, sizeof(path), "%s/frame_%04llu.png", s_screenshot_dir, (unsigned long long)g_frame_count);
        } else {
            snprintf(path, sizeof(path), "frame_%04llu.png", (unsigned long long)g_frame_count);
        }
        runner_screenshot(path);
    }
}
