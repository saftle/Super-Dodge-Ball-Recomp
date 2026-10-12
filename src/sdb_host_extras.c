/* sdb_host_extras.c - Super Dodge Ball's additions to the cycle host
 * (cyc_host_extras.h).
 *
 * "Remove character flicker" mod. Super Dodge Ball builds OAM in place at $0200 (the
 * $4014 DMA source page, see $FE34) with an 8-bit byte cursor in $0617. Each
 * emitter writes a complete 4-byte sprite at $0200+cursor, then runs
 *
 *     $84B6: LDA $0617 / CLC / ADC #$04 / STA $0617 / BEQ $84E5
 *
 * Characters are 8 sprites each (16x32 = four 2-tile rows), so a busy match
 * needs more than OAM's 64 slots. When the cursor wraps $FC -> $00 that BEQ
 * fires and the character emitter stops emitting: the game never computes the
 * dropped tiles, so the character renders half a body and no emulator
 * sprite-limit toggle can restore it. Measured over a 466-frame match: 370
 * frames (79%) truncate, and the cut position moves as sprites come and go --
 * that movement is the flicker. See docs/research.md section 2.
 *
 * The mod does two things, and changes nothing else:
 *
 *   1. Capture. At each emit site (src/game.toml [[mod_function_hook]]) the
 *      sprite just written is appended to a host-side list in OAM-slot order,
 *      and the cursor is pinned so it never wraps to zero -- so the game emits
 *      every sprite it intends instead of giving up. Only $0617 is touched.
 *
 *   2. Redraw. The window is presented from that list over the PPU's exact
 *      background capture (cyc_render_capture_background), so every sprite
 *      draws regardless of the hardware 8-per-scanline limit. Native sprite
 *      evaluation, sprite-0 hit, the overflow flag and mapper IRQ all still
 *      happen: the machine runs completely stock, this is presentation only.
 *
 * The game builds its sprite list across two frames, not one: it resets the
 * cursor to slot 1 to start a list, then emits a slice per frame until the whole
 * list is in $0200. Lists are therefore assembled in `build` and the previous
 * one kept in `done`.
 *
 * Because every build emits in the same order, index i of a list is always the
 * same sprite's tile i -- the OAM slot it would occupy. So a frame mid-build
 * presents index i from `build` when the build has reached it and from `done`
 * when it has not. That is exactly what the game's own OAM page does (newer
 * slots written, not-yet-reached slots still holding last build's sprites), so
 * the picture tracks the native one exactly -- except that indices past 63
 * exist, which is the whole point: the character that OAM had no room for is
 * drawn instead of dropped.
 *
 * Enabled by default through the "super-dodgeball.character-flicker" package
 * (mods/packages/.../manifest.toml), toggleable from the launcher's Mods screen
 * or the runtime menu. With the feature off, present() returns NULL and the
 * host presents the native picture exactly as before.
 */
#include "cyc_host_extras.h"
#include "cyc_core.h"
#include "cyc_hooks.h"
#include "cyc_mod.h"
#include "cyc_render.h"

#include "mod_function_hooks.h"
#include "mod_runtime.h"

#include <stdio.h>
#include <string.h>

/* ---- guest constants (facts about the ROM, not ROM bytes) ---- */

#define SDB_CURSOR      0x0617u   /* OAM byte cursor into the $0200 page */
#define SDB_OAM_BASE    0x0200u
#define SDB_OAM_LAST    0xFCu     /* byte offset of sprite slot 63 */
#define SDB_FIRST_SLOT  0x04u     /* cursor value the game starts a list at */
#define SDB_PIN         0xF8u     /* cursor value whose ADC #$04 lands on $FC */

/* A character is 8 sprites; a full 6-player match wants ~72. Sized for the
 * worst case the game can reach, with headroom. */
#define SDB_MAX_SPRITES 512

typedef struct { uint8_t y, tile, attr, x; } SdbSprite;

static SdbSprite s_build[SDB_MAX_SPRITES];   /* list being assembled */
static int s_build_count;
static SdbSprite s_done[SDB_MAX_SPRITES];    /* last list that finished */
static int s_done_count;
static int s_overflow;
static int s_captured;              /* a hook fired this frame */
static int s_cap_run;                /* consecutive frames with captures */
static int s_on;                             /* feature active */

static uint32_t s_picture[256 * 240];

/* ------------------------------------------------------------------ *
 * 1. Capture
 * ------------------------------------------------------------------ */

/* Runs at the instruction boundary before each cursor bump: the 4 bytes of the
 * sprite just written are already at $0200+cursor. */
static int sdb_capture(uint16_t addr)
{
    (void)addr;
    uint8_t cur = cyc_mod_peek(SDB_CURSOR);
    uint16_t at = (uint16_t)(SDB_OAM_BASE + cur);

    /* A cursor back at slot 1 means the game is starting a NEW list, so the one
     * we have been assembling is finished: publish it before starting over. */
    if (cur == SDB_FIRST_SLOT && s_build_count > 0) {
        memcpy(s_done, s_build, (size_t)s_build_count * sizeof(SdbSprite));
        s_done_count = s_build_count;
        s_build_count = 0;
    }

    if (s_build_count < SDB_MAX_SPRITES) {
        SdbSprite *s = &s_build[s_build_count++];
        s->y    = cyc_mod_peek(at);
        s->tile = cyc_mod_peek((uint16_t)(at + 1));
        s->attr = cyc_mod_peek((uint16_t)(at + 2));
        s->x    = cyc_mod_peek((uint16_t)(at + 3));
    } else {
        s_overflow++;
    }

    s_captured = 1;
    if (cur >= SDB_OAM_LAST) {
        /* The cursor is about to wrap $FC -> $00. That slot is already copied
         * out above, so pin the cursor to a value whose ADC #$04 lands on $FC
         * instead of $00: the game keeps emitting (overwriting slot 63, which
         * we hold) and the BEQ at $84BF never sees zero. Slot 0 stays intact,
         * so the game's own frontmost sprite still reaches the DMA. */
        cyc_mod_poke(SDB_CURSOR, SDB_PIN);
    }
    return 0;   /* never take over the routine: the original bump runs */
}

/* ------------------------------------------------------------------ *
 * 2. Redraw
 * ------------------------------------------------------------------ */

/* One sprite. Mirrors the engine's own sprite blit (runner/cyc/cyc_render.c,
 * cyc_render_sprites) row for row, because that loop is the reference: it
 * resolves 8x8 vs 8x16, the per-line sprite pattern table, H/V flip, palette
 * select and the leftmost-column clip exactly as the PPU does.
 *
 * This walks pixels itself on purpose. cyc_render_tile() paints a whole 8x8
 * tile per call, so calling it once per scanline would stack eight overlapping
 * copies and overwrite each other's pixels -- which shows up as doubled,
 * wrongly-coloured sprites. */
static void sdb_blit(const SdbSprite *s, uint32_t *out, int width, int height)
{
    int y = s->y + 1;                       /* OAM Y+1 is the first drawn line */
    if (s->y >= 0xEF || y >= 240) return;   /* off screen / the game's hidden slot */
    unsigned attr = s->attr;
    int ly = y < 240 ? y : 239;
    if (!(cyc_render_line_mask(ly) & 0x10)) return;   /* sprites off on this line */
    int tall = cyc_render_line_sprite16(ly);
    uint16_t table = cyc_render_line_sprite_table(ly);
    int rows = tall ? 16 : 8;
    /* Behind-background priority: SDB does not use OAM attr bit 5 (measured over
     * a full match), so there is no coverage buffer to consult. */
    for (int r = 0; r < rows; ++r) {
        int py = y + r;
        if (py < 0 || py >= height || py >= 240) continue;
        int row = (attr & 0x80) ? rows - 1 - r : r;
        uint16_t pattern = tall
            ? (uint16_t)(((s->tile & 1) ? 0x1000 : 0) + (s->tile & 0xFE) * 16 + (row >= 8 ? 16 : 0))
            : (uint16_t)(table + s->tile * 16);
        uint8_t lo = cyc_render_chr((uint16_t)(pattern + (row & 7)));
        uint8_t hi = cyc_render_chr((uint16_t)(pattern + (row & 7) + 8));
        for (int c = 0; c < 8; ++c) {
            int px = s->x + c;
            if (px < 0 || px >= width) continue;
            /* the left-column clip is in native columns */
            if (s->x + c < 8 && !(cyc_render_line_mask(py) & 0x04)) continue;
            int bit = (attr & 0x40) ? c : 7 - c;
            int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            if (!v) continue;
            out[(size_t)py * (size_t)width + (size_t)px] =
                cyc_render_color(16 + (int)(attr & 3) * 4 + v);
        }
    }
}

/* The picture the window shows. Returns NULL (host presents the native frame)
 * whenever the feature is off or there is nothing to draw from. */
static const uint32_t *sdb_present(void *ctx, int *width, int *height)
{
    (void)ctx;
    if (!s_on) return NULL;
    if (s_build_count == 0 && s_done_count == 0) return NULL;
    if (!cyc_render_background(s_picture)) return NULL;
    /* A build takes two frames: the first capture frame leaves the list still
     * half-assembled, the second finishes it. Emit order differs between
     * builds, so a half-built list must never be blended with the previous one
     * (that draws both, out of order). Present the last COMPLETE list only
     * while the build is genuinely still in progress -- that is, on the first
     * capture frame. From the second capture frame on the build is whole. */
    int run = s_captured ? s_cap_run + 1 : 0;   /* includes this frame */
    int stale = run == 1 && s_done_count;
    const SdbSprite *list = stale ? s_done : s_build;
    int count = stale ? s_done_count : s_build_count;

    /* OAM slot order is priority order, slot 0 frontmost, and the capture is in
     * emission (slot) order -- so paint back to front. Slot 0 is not part of
     * the capture (the cursor starts at slot 1); it is the game's frontmost
     * sprite and is still intact in OAM because the cursor never wraps onto it. */
    for (int i = count - 1; i >= 0; i--)
        sdb_blit(&list[i], s_picture, 256, 240);

    const uint8_t *oam = cyc_render_oam();
    SdbSprite front = { oam[0], oam[1], oam[2], oam[3] };
    sdb_blit(&front, s_picture, 256, 240);

    *width = 256;
    *height = 240;
    return s_picture;
}

/* ------------------------------------------------------------------ *
 * Frame + activation plumbing
 * ------------------------------------------------------------------ */

static void sdb_frame_begin(void *ctx)
{
    (void)ctx;
    s_cap_run = s_captured ? s_cap_run + 1 : 0;
    s_captured = 0;
    s_overflow = 0;
}

static void sdb_power_on(void *ctx)
{
    (void)ctx;
    cyc_render_capture_background(true);
    s_build_count = 0;
    s_done_count = 0;
    s_overflow = 0;
}

/* ---- mod activation ---- */

static const char *const SDB_HOOK_IDS[] = {
    "super-dodgeball.oam.b5-8116",
    "super-dodgeball.oam.b5-8338",
    "super-dodgeball.oam.b5-84b6",
    "super-dodgeball.oam.b4-a2f7",
    "super-dodgeball.oam.b4-a330",
};

static void sdb_enable(void)
{
    for (unsigned i = 0; i < sizeof SDB_HOOK_IDS / sizeof SDB_HOOK_IDS[0]; i++)
        nes_mod_set_function_hook_enabled(SDB_HOOK_IDS[i], 1);
    s_on = 1;
}

static void sdb_disable(void)
{
    nes_mod_disable_all_function_hooks();
    s_on = 0;
}

static const CycHostOption SDB_OPTIONS[] = {
    { "--remove-character-flicker", false, "draw every OAM sprite host-side, so characters are never truncated" },
};

static bool sdb_option(void *ctx, const char *name, const char *value)
{
    (void)ctx; (void)value;
    if (strcmp(name, "--remove-character-flicker")) return false;
    for (unsigned i = 0; i < sizeof SDB_HOOK_IDS / sizeof SDB_HOOK_IDS[0]; i++)
        nes_mod_set_function_hook_enabled(SDB_HOOK_IDS[i], 1);
    s_on = true;
    return true;
}

static const CycHostExtras SDB_EXTRAS = {
    .present = sdb_present,
    .power_on = sdb_power_on,
    .frame_begin = sdb_frame_begin,
    .options = SDB_OPTIONS,
    .option_count = 1,
    .option = sdb_option,
};

const CycHostExtras *cyc_host_extras(void) { return &SDB_EXTRAS; }

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
static void __cdecl register_sdb_mods(void);
__declspec(allocate(".CRT$XCU")) void (__cdecl *register_sdb_mods_constructor)(void) = register_sdb_mods;
#pragma comment(linker, "/include:register_sdb_mods_constructor")
static void __cdecl register_sdb_mods(void)
#else
static void register_sdb_mods(void) __attribute__((constructor));
static void register_sdb_mods(void)
#endif
{
    static const uint16_t addrs[] = { 0x8116, 0x8338, 0x84B6, 0xA2F7, 0xA330 };
    if (!nes_mod_register_activation_plugin("super-dodgeball.character-flicker", sdb_enable) ||
        !nes_mod_register_reset_callback(sdb_disable))
        fprintf(stderr, "sdb: mod registration failed\n");
    for (unsigned i = 0; i < sizeof addrs / sizeof addrs[0]; i++)
        if (!nes_mod_register_function_entry_plugin(SDB_HOOK_IDS[i], addrs[i], sdb_capture))
            fprintf(stderr, "sdb: hook registration failed for $%04X\n", addrs[i]);
}