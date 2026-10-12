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
 * A third thing, sdb_unsuppress() below, covers a SECOND and independent
 * flicker: the per-object emitter skips a whole object on 4 frames out of
 * every 9, on a flag the game raises on one rotating object at a time. That is
 * a flag, not a budget -- OAM has room when it happens. See section 2.1 of
 * docs/research.md.
 *
 * The game builds its sprite list across two frames, not one: it resets the
 * cursor to slot 1 to start a list, then emits a slice per frame until the whole
 * list is in $0200. Lists are therefore assembled in `build` and the previous
 * one kept in `done`.
 *
 * A half-built list is never blended index-wise with the previous one: emit
 * order differs between builds, so that draws characters twice, out of order.
 * Instead the first frame of a build presents the last complete list whole,
 * and later frames present the new build whole -- except right after an
 * emission gap, where the published list predates what sits in OAM now and the
 * new build (with its carried-over prefix) wins regardless of completeness.
 * Indices past 63 exist in these lists, which is the whole point: the
 * character that OAM had no room for is drawn instead of dropped.
 *
 * Shipped as the opt-in `super-dodgeball.character-flicker` package
 * (mods/packages/.../manifest.toml), toggleable from the launcher's Mods screen
 * or the runtime menu, headless via `--remove-character-flicker`. It stays off
 * by default until a full playthrough is tested (the level-switch screen shows
 * sprite ghosts with it on -- open issue, research.md section 2.1). With the
 * feature off, present() returns NULL and the host presents the native picture
 * exactly as before.
 */
#include "cyc_host_extras.h"
#include "cyc_core.h"
#include "cyc_hooks.h"
#include "cyc_mod.h"
#include "cyc_render.h"
#include "cyc_video.h"

#include "mod_function_hooks.h"
#include "mod_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- guest constants (facts about the ROM, not ROM bytes) ---- */

#define SDB_CURSOR      0x0617u   /* OAM byte cursor into the $0200 page */
#define SDB_OAM_BASE    0x0200u
#define SDB_OAM_LAST    0xFCu     /* byte offset of sprite slot 63 */
#define SDB_FIRST_SLOT  0x04u     /* cursor value the game starts a list at */
#define SDB_PIN         0xF8u     /* cursor value whose ADC #$04 lands on $FC */
#define SDB_PIN_WIDE    0xF4u     /* cursor value whose ADC #$08 lands on $FC */
#define SDB_OAM_HIGH    0xF8u     /* cursor value whose ADC #$08 would wrap */

/* The per-object emitter $821E loads the object's 7-byte display record and
 * keeps byte 0 in $4E. Bit 5 of it means "draw nothing": $828A is
 * `LDA $4E / AND #$20 / BNE $8293` and $8293 is the RTS, so the whole object's
 * sprite strip is skipped. */
#define SDB_OBJ_FLAG    0x4Eu
#define SDB_OBJ_HIDE    0x20u

/* The display-list builder, $E32D in the fixed bank. It takes the object's
 * display record pointer in $65/$66 -- $0618 + 7*index, via the table at
 * $E3A2, the same one $80B8 mirrors in the switchable bank -- appends the
 * object to the list at $067C (count $067A, byte cursor $067B), and gates the
 * append on the object's state byte $43. */
#define SDB_REC_PTR     0x0065u   /* $65/$66: this object's display record */
#define SDB_REC_BASE    0x0618u
#define SDB_REC_STRIDE  7u
#define SDB_STATE       0x0043u   /* the object's state byte, and the gate's input */
#define SDB_STATE_MASK  0x70u     /* $E34C: the bits the gate looks at */
#define SDB_STATE_HIDE  0x20u     /* $E34E: the one value that means "do not add" */
#define SDB_LIST_CNT    0x067Au
#define SDB_LIST_CUR    0x067Bu
#define SDB_LIST_ID     0x067Cu
#define SDB_ID_HI       0x0048u   /* high byte of the appended id */
#define SDB_ID_HI_SIGN  0x0049u   /* its sign byte, per $E35F-$E36C */
#define SDB_LIST_MAX    32u       /* 64 slots / 2 bytes per id */

/* A character is 8 sprites; a full 6-player match wants ~72. Sized for the
 * worst case the game can reach, with headroom. */
#define SDB_MAX_SPRITES 512
/* How many sprites one list may hold before the mod stops trusting its own
 * boundary detection and publishes what it has. Measured maximum over a match
 * is 77, so this only ever fires on a detection miss. */
#define SDB_LIST_LIMIT 256
/* SDB_TRACE_CAPTURES=<from>-<to> dumps every capture in that frame range, to
 * watch the cursor step through a list. SDB_TRACE=1 alone gives the per-frame
 * summary, which is what usually localises a problem. */

typedef struct { uint8_t y, tile, attr, x; } SdbSprite;

static SdbSprite s_build[SDB_MAX_SPRITES];   /* list being assembled */
static int s_build_count;
static int s_build_peak;              /* largest s_build_count seen this frame */
static SdbSprite s_done[SDB_MAX_SPRITES];    /* last list that finished */
static int s_done_count;
static int s_overflow;
static long s_frame;                   /* frame counter, for the trace */
static int s_trace;                    /* SDB_TRACE=1: per-frame trace */
static long s_cap_from = -1, s_cap_to; /* SDB_TRACE_CAPTURES=<from>-<to> */
static uint8_t s_prev_cursor;          /* previous capture's cursor value */
static int s_list_lost;                /* lists that hit the backstop */
static int s_saw_reset;              /* a capture saw the cursor at slot 1 */
static int s_captured;              /* a hook fired this frame */
static int s_culled_restored;       /* objects put back on the display list */
static long s_done_frame = -100;    /* frame the published list was finished */
static int s_cap_run;                /* consecutive frames with captures */
static int s_on;                             /* feature active */

/* Requested PPU-overclock lines, re-applied at power_on (see section 5). */
static int s_oc_lines;

static uint32_t s_picture[256 * 240];

/* The presented picture when it is wider than 256 (widescreen) and/or shorter
 * than 240 (the overscan crop). One buffer, repacked: a cropped picture cannot
 * be handed back as a pointer into the engine's canvas, because its rows are
 * canvas_width apart while the host copies it as a dense width*height block. */
static uint32_t s_canvas[NES_VIDEO_MAX_WIDTH * 240];

/* ------------------------------------------------------------------ *
 * 1. Capture
 * ------------------------------------------------------------------ */

/* A finished list is published to `done`, which is what the first frame of the
 * next build presents while the new one is still half-assembled. */
static void sdb_publish(void)
{
    memcpy(s_done, s_build, (size_t)s_build_count * sizeof(SdbSprite));
    s_done_count = s_build_count;
    s_done_frame = s_frame;
}

/* One OAM slot copied into the list being assembled. Slots the game hid
 * (y=$F0 and up) ride along harmlessly: the blit skips them exactly the way
 * the PPU does. */
static void sdb_take(uint16_t at)
{
    if (s_build_count < SDB_MAX_SPRITES) {
        SdbSprite *s = &s_build[s_build_count++];
        s->y    = cyc_mod_peek(at);
        s->tile = cyc_mod_peek((uint16_t)(at + 1));
        s->attr = cyc_mod_peek((uint16_t)(at + 2));
        s->x    = cyc_mod_peek((uint16_t)(at + 3));
        if (s_build_count > s_build_peak) s_build_peak = s_build_count;
    } else {
        s_overflow++;
    }
}

/* A new sprite list. The game normally says so by putting the cursor back
 * at slot 1 ($04) before it starts one, and that is the common case. It is
 * NOT the only one: the same frame can begin a list at a higher slot when
 * the emitters that fill slots 1-2 have nothing to draw (measured: a list
 * starting at $0C right after one that ended at $F4). Testing only $04 let
 * that one run on: the list never restarted, grew ~57 sprites every five
 * frames, and at SDB_MAX_SPRITES the presentation painted several hundred
 * stale sprites -- the "copies in a trail" smear.
 *
 * So a cursor that has gone BACKWARDS is a new list too. Nothing inside one
 * list moves it backwards: it only ever steps +4, and once the pin below
 * engages the tail reads $FC, $FC, $FC... The game's save/restore around
 * $DBDB re-reads the same value, which is not a decrease.
 *
 * When the new list starts above slot 1, the slots below it keep whatever
 * OAM holds -- persistent sprites like the floating damage numbers live
 * there -- and the native picture draws them. So the restarted list opens
 * with those slots copied verbatim, in slot order. That is not blending two
 * builds: it is reading the machine's own OAM page for slots this list does
 * not cover, which is exactly what the PPU will draw there. */
static void sdb_restart(uint8_t cur)
{
    if (s_build_count > 0 && (cur == SDB_FIRST_SLOT || cur < s_prev_cursor)) {
        sdb_publish();
        s_build_count = 0;
        for (uint8_t slot = 1; slot < cur / 4; slot++)
            sdb_take((uint16_t)(SDB_OAM_BASE + (uint16_t)slot * 4u));
    }
    s_prev_cursor = cur;
}

/* Runs at the instruction boundary before each cursor bump: the 4 bytes of the
 * sprite just written are already at $0200+cursor. */
static int sdb_capture(uint16_t addr)
{
    (void)addr;
    uint8_t cur = cyc_mod_peek(SDB_CURSOR);
    uint16_t at = (uint16_t)(SDB_OAM_BASE + cur);

    if (s_cap_from >= 0 && s_frame >= s_cap_from && s_frame <= s_cap_to)
        fprintf(stderr, "sdbc f=%ld cur=%02X n=%d\n", s_frame, cur, s_build_count);

    sdb_restart(cur);

    /* Backstop. No real list comes close to this (the measured maximum over a
     * match is 77), so reaching it means the boundary test above missed
     * something again. Publish rather than let the list run to the array's
     * end, and say so -- a silently wrong picture is the failure mode this
     * whole mod exists to avoid. */
    if (s_build_count >= SDB_LIST_LIMIT) {
        sdb_publish();
        s_build_count = 0;
        s_list_lost++;
        if (s_list_lost < 8)
            fprintf(stderr, "sdb: frame %ld: sprite list hit the %d limit; "
                            "published it and restarted\n", s_frame, SDB_LIST_LIMIT);
    }

    sdb_take(at);

    s_captured = 1;
    if (cur == SDB_FIRST_SLOT) s_saw_reset = 1;
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

/* The two-sprite emitter at $96B9 (bank 3): the floating damage numbers. When
 * a player is hit, a little number floats above him showing the lost energy,
 * and this routine writes that digit pair at $0200+cursor, advancing the cursor
 * by 8 (`LDA $0617 / ADC #$08 / STA $0617` -- note the different step, and that
 * this shape has no wrap bail). It is the only emitter with this shape, and it
 * was never hooked, so the numbers were never captured and never drawn: the one
 * thing on screen that told you a hit landed was missing with the mod on.
 *
 * Same boundary rule as above (a new list starts at $04 or anywhere the cursor
 * went backwards), then both sprites. The pin differs: it must survive an
 * ADC #$08, so a cursor that would wrap is pulled back to $F4, which lands on
 * $FC. Only slot 63 is ever overwritten, same as the single-sprite pin. */
static int sdb_capture2(uint16_t addr)
{
    (void)addr;
    uint8_t cur = cyc_mod_peek(SDB_CURSOR);

    if (s_cap_from >= 0 && s_frame >= s_cap_from && s_frame <= s_cap_to)
        fprintf(stderr, "sdbn f=%ld cur=%02X n=%d [%02X %02X %02X %02X] [%02X %02X %02X %02X]\n",
                s_frame, cur, s_build_count,
                cyc_mod_peek((uint16_t)(SDB_OAM_BASE + cur)),
                cyc_mod_peek((uint16_t)(SDB_OAM_BASE + cur + 1)),
                cyc_mod_peek((uint16_t)(SDB_OAM_BASE + cur + 2)),
                cyc_mod_peek((uint16_t)(SDB_OAM_BASE + cur + 3)),
                cyc_mod_peek((uint16_t)(SDB_OAM_BASE + cur + 4)),
                cyc_mod_peek((uint16_t)(SDB_OAM_BASE + cur + 5)),
                cyc_mod_peek((uint16_t)(SDB_OAM_BASE + cur + 6)),
                cyc_mod_peek((uint16_t)(SDB_OAM_BASE + cur + 7)));

    sdb_restart(cur);

    if (s_build_count >= SDB_LIST_LIMIT) {
        sdb_publish();
        s_build_count = 0;
        s_list_lost++;
        if (s_list_lost < 8)
            fprintf(stderr, "sdb: frame %ld: sprite list hit the %d limit; "
                            "published it and restarted\n", s_frame, SDB_LIST_LIMIT);
    }

    for (int k = 0; k < 2; k++)
        sdb_take((uint16_t)(SDB_OAM_BASE + cur + (uint16_t)(k * 4)));

    s_captured = 1;
    if (cur >= SDB_OAM_HIGH) {
        /* $F8 + 8 and $FC + 8 both wrap to $00. Pull back to $F4 instead, whose
         * ADC #$08 lands exactly on $FC. */
        cyc_mod_poke(SDB_CURSOR, SDB_PIN_WIDE);
    }
    return 0;   /* never take over the routine: the original bump runs */
}

/* The block-copy emitter at $A36C (bank 4): menu position numbers and friends.
 * It streams up to 8 sprites from a static table ($A3DC) into OAM, stepping an
 * X register it loaded from the cursor -- there is no per-sprite cursor bump,
 * just one `STX $0617` when the loop is done. So no bump hook can ever see
 * these sprites, and the mod never drew them: the position numbers around the
 * team-select court diagrams (tiles $05-$08, $22) were missing with the mod on.
 *
 * The hook sits at $A394, the first instruction after a loop iteration has
 * written all 4 bytes of its sprite (the `STA $0203,X` just ran), and only on
 * the copy path -- iterations the $A3A1 gate skips jump straight to the loop
 * bottom and never come here. X is the sprite's slot offset, read from the
 * CPU registers; the $0617 byte itself is untouched until the loop's own final
 * store commits it, so there is nothing to pin. Boundary, prefix and backstop
 * handling are shared with the bump hooks: the loop continues whatever list
 * the cursor emitters were building. */
static int sdb_copy(uint16_t addr)
{
    (void)addr;
    CycModRegs r;
    cyc_mod_regs(&r);
    uint8_t cur = r.x;

    sdb_restart(cur);

    if (s_build_count >= SDB_LIST_LIMIT) {
        sdb_publish();
        s_build_count = 0;
        s_list_lost++;
        if (s_list_lost < 8)
            fprintf(stderr, "sdb: frame %ld: sprite list hit the %d limit; "
                            "published it and restarted\n", s_frame, SDB_LIST_LIMIT);
    }

    sdb_take((uint16_t)(SDB_OAM_BASE + cur));

    s_captured = 1;
    return 0;   /* never take over the routine: the original loop runs */
}

/* The second flicker: the game's own object culling, and a completely separate
 * mechanism from the budget truncation above.
 *
 * A character used to vanish completely for 4 frames out of every 9 -- OAM at
 * 46-54 of 64 throughout, so not a budget -- while every other character kept
 * drawing. It is the game declining to add one object to the display list, and
 * rotating which one it declines. There are three gates between that decision
 * and the screen, and the mod has to open all three:
 *
 *   1. $E32D, the display-list builder, never appends an object whose state
 *      byte $43 has bits 4-6 equal to $20.       -> sdb_add_hidden()
 *   2. $821E skips any object whose display record carries bit 5 in byte 0,
 *      which is that same state byte.             -> sdb_unsuppress()
 *   3. $8200 shifts an entry out of the list outright when four or more are
 *      stacked within $20 pixels.                 -> sdb_no_cull()
 *
 * Nothing about a culled character changes while it is hidden -- same record,
 * same position, same tiles as the frames either side -- so this is the game
 * hiding something it drew a frame earlier, not a transitional state being
 * skipped. Over a 905-frame match the flag is set on 229 frames, never on two
 * objects at once, and the object it names rotates.
 *
 * The only guest-visible consequence of overriding all three is that more
 * sprites reach OAM -- the same consequence the cursor pin above already has.
 * See docs/research.md section 2.1.
 */
/* Gate 1: re-add the object the list builder left out.
 *
 * $E32D is the display-list builder. It computes this object's record pointer
 * into $65/$66, then:
 *
 *     $E34A: LDA $43
 *     $E34C: AND #$70
 *     $E34E: CMP #$20
 *     $E350: BEQ $E378        <- bits 4-6 of $43 are exactly $20: do not add
 *     $E352..$E377           <- otherwise append: id into $067C/$067D, bump
 *                               $067A, advance the $067B cursor by 2
 *     $E378:                 <- and either way, write the record from $43/$41/$42
 *
 * So an object in that state never reaches the list and $821E never sees it --
 * which is why no hook at the emitter could reach it. The game keeps that bit
 * on one object at a time and rotates which object, so a character is whole on
 * 5 frames out of every 9 and gone for the other 4, with nothing about it
 * changed in between.
 *
 * The site here is the landing pad, so the guest is left exactly as it found
 * itself: this callback only does the append the taken branch skipped. Every
 * input is already in RAM ($65/$66, $067B, $48, $49), so nothing has to be
 * guessed from registers.
 *
 * The guards matter: $E378 is reachable from elsewhere in the fixed bank, and
 * the record pointer is only this game's. Anything unexpected falls through. */
static int sdb_add_hidden(uint16_t addr)
{
    (void)addr;
    uint8_t state = cyc_mod_peek(SDB_STATE);
    if ((state & SDB_STATE_MASK) != SDB_STATE_HIDE) return 0;

    uint16_t rec = (uint16_t)(cyc_mod_peek(SDB_REC_PTR)
                            | (cyc_mod_peek(SDB_REC_PTR + 1) << 8));
    if (rec < SDB_REC_BASE
        || ((rec - SDB_REC_BASE) % SDB_REC_STRIDE) != 0
        || (rec - SDB_REC_BASE) / SDB_REC_STRIDE > 12u)
        return 0;                       /* not one of the thirteen records */

    uint8_t at = cyc_mod_peek(SDB_LIST_CUR);
    if (at + 2u > SDB_LIST_MAX) return 0;

    /* The id is the record index, high byte per $E35F-$E36C:
     *
     *     $E35F: LDA $49 / BPL $E368 / LDA #$00 / STA $067D,X
     *     $E368: BEQ $E36F       / LDA #$FF / STA $067D,X
     *
     * so $49 negative forces $00, $49 zero leaves $48 alone, and anything else
     * positive forces $FF. The high byte is the sort key $8040 orders the list
     * by, so getting this wrong does not lose the object -- it puts it at the
     * wrong depth and the character above it draws in front. */
    uint8_t hi = cyc_mod_peek(SDB_ID_HI);
    uint8_t sgn = cyc_mod_peek(SDB_ID_HI_SIGN);
    if (sgn & 0x80)      hi = 0x00;
    else if (sgn != 0)   hi = 0xFF;

    cyc_mod_poke(SDB_LIST_ID + at, (uint8_t)((rec - SDB_REC_BASE) / SDB_REC_STRIDE));
    cyc_mod_poke(SDB_LIST_ID + at + 1, hi);
    cyc_mod_poke(SDB_LIST_CNT, (uint8_t)(cyc_mod_peek(SDB_LIST_CNT) + 1));
    cyc_mod_poke(SDB_LIST_CUR, (uint8_t)(at + 2));
    s_culled_restored++;
    return 0;
}

/* The other half of the cull, at the point of use.
 *
 * $821E reads the display record and keeps byte 0 in $4E. Then:
 *
 *     $827A: BIT $4E      bit 6 -> extra path ($82B9)
 *     $8281: AND #$10     bit 4 -> $82DD
 *     $828A: AND #$20 / BNE $8293   bit 5 -> RTS, the object emits nothing
 *     $8290: JMP $8348               bit 5 clear -> emit normally
 *
 * The record's byte 0 is the same $43 the list builder gated on, so an object
 * put back on the list is still filtered here. The hook runs one instruction
 * after `LDA ($61),Y / STA $4E`, so $4E still holds this object's flag and
 * nothing has run since it was loaded. Clearing the bit leaves the record in
 * RAM alone -- it holds whatever the game put there, and the game rebuilds it
 * next frame.
 *
 * Not OAM pressure: OAM holds 46-54 of its 64 slots throughout, and over a
 * 905-frame match the flag is set on 229 frames, never on two objects at once.
 * See docs/research.md section 2.1. */
static int sdb_unsuppress(uint16_t addr)
{
    (void)addr;
    uint8_t flag = cyc_mod_peek(SDB_OBJ_FLAG);
    if (flag & SDB_OBJ_HIDE)
        cyc_mod_poke(SDB_OBJ_FLAG, (uint8_t)(flag & (uint8_t)~SDB_OBJ_HIDE));
    return 0;   /* never take over the routine: the original body runs */
}

/* Gate 1. $8200 is a plain subroutine (`JSR $8200` at $816F), so returning
 * nonzero makes the host pop the pushed address and carry on at $8172 --
 * byte for byte what an `RTS` there would do, and what the SNES port's
 * `RTS ; disable sprite flickering` did. The routine's own body never runs, so
 * $067A keeps its count and $067C keeps its entry. */
static int sdb_no_cull(uint16_t addr)
{
    (void)addr;
    return 1;
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
static void sdb_blit(const SdbSprite *s, uint32_t *out, int width, int height, int x0)
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
            int px = x0 + s->x + c;
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

/* Section 4's compositor state, needed before it is defined. */
/* The picture the window shows.
 *
 * The widescreen compositor, when it is installed, runs inside
 * cyc_render_present() -- that is the only place it runs, and the only place
 * the presented width is decided. So this callback must always ask for that
 * picture first, even when the character-flicker mod has nothing to say: ask
 * for nothing and the host presents the native frame, which would quietly
 * switch a 854-column window back to 256.
 *
 * Three shapes come out of here:
 *   - no flicker, no crop, stock width  : the engine's buffer, returned as is
 *   - flicker only, stock width         : the stock 256x240 redraw
 *   - wide and/or cropped               : a repacked copy we may draw sprites into
 *
 * Returns NULL (the host presents the native frame) whenever the flicker mod
 * has nothing to draw and nothing about the picture needs repacking. */
static const uint32_t *sdb_present(void *ctx, int *width, int *height)
{
    (void)ctx;
    int cw, ch;
    const uint32_t *canvas = cyc_render_present(&cw, &ch);
    if (!canvas || cw <= 0 || ch <= 0) return NULL;

    int redraw = s_on && (s_build_count || s_done_count) && cyc_render_background(s_picture);

    if (!redraw && cw == 256) {
        *width = cw;
        *height = ch;
        return canvas;
    }

    /* A build takes two frames: the first capture frame leaves the list still
     * half-assembled, the second finishes it. Emit order differs between
     * builds, so a half-built list must never be blended with the previous one
     * (that draws both, out of order). Present the last COMPLETE list only
     * while the build is genuinely still in progress -- that is, on the first
     * capture frame. From the second capture frame on the build is whole.
     *
     * "Genuinely still in progress" needs the freshness check, not just the
     * frame count: after an emission gap (no captures for a frame or more) the
     * published list is older than the build that just restarted, and showing
     * it would hold the picture several frames stale -- measured: the floating
     * damage numbers sit untouched in OAM slots 1-2 across a gap while the game
     * emits nothing, and the pre-gap list predates them. So the first-capture
     * frame falls back only to a list published on the immediately previous
     * frame; anything older loses to the new build, partial or not, because the
     * new build (with its carried-over prefix) is what the machine's OAM page
     * actually holds. */
    int run = s_captured ? s_cap_run + 1 : 0;   /* includes this frame */
    int fresh = (s_frame - s_done_frame) <= 1;
    int stale = run == 1 && fresh && s_done_count;
    const SdbSprite *list = stale ? s_done : s_build;
    int count = stale ? s_done_count : s_build_count;

    if (cw == 256) {
        if (!redraw) return NULL;
        goto blit;
    }

    /* Wide: repack into s_canvas, which the character-flicker mod then draws
     * its sprite list into. The engine's buffer is reused and generation-cached,
     * so it is read, never written. */
    for (int y = 0; y < ch; y++)
        memcpy(s_canvas + (size_t)y * cw, canvas + (size_t)y * cw,
               (size_t)cw * sizeof(uint32_t));
    *width = cw;
    *height = ch;
    if (!redraw) return s_canvas;

    /* Sprites are in native columns. On a wide canvas they belong at the same
     * offset the native picture itself sits at, which the compositor centres;
     * a cropped canvas is only ever shorter, never offset, so this covers both
     * cases. cyc_render_native_origin is the compositor's own declaration of
     * where native column 0 went, falling back to the centred position. */
    {
        int x0 = cyc_render_native_origin(cw);
        if (x0 < 0 || x0 + 256 > cw) x0 = (cw - 256) / 2;
        for (int i = count - 1; i >= 0; i--)
            sdb_blit(&list[i], s_canvas, cw, *height, x0);
        const uint8_t *oam = cyc_render_oam();
        SdbSprite front = { oam[0], oam[1], oam[2], oam[3] };
        sdb_blit(&front, s_canvas, cw, *height, x0);
        return s_canvas;
    }

blit:
    /* OAM slot order is priority order, slot 0 frontmost, and the capture is in
     * emission (slot) order -- so paint back to front. Slot 0 is not part of
     * the capture (the cursor starts at slot 1); it is the game's frontmost
     * sprite and is still intact in OAM because the cursor never wraps onto it. */
    for (int i = count - 1; i >= 0; i--)
        sdb_blit(&list[i], s_picture, 256, 240, 0);

    {
        const uint8_t *oam = cyc_render_oam();
        SdbSprite front = { oam[0], oam[1], oam[2], oam[3] };
        sdb_blit(&front, s_picture, 256, 240, 0);
    }

    *width = 256;
    *height = 240;
    return s_picture;
}

/* ------------------------------------------------------------------ *
 * Frame + activation plumbing
 * ------------------------------------------------------------------ */

/* SDB_TRACE=1 prints one line per frame:
 *
 *     sdb <frame> <peak-list> <overflow> <done-list>
 *
 * `peak-list` is the largest number of sprites assembled for one list that
 * frame. A healthy frame is tens (OAM holds 64, and the cursor pin lets the
 * game emit past that). A value that climbs frame after frame is the runaway
 * capture: the game never returns the cursor to $04, so no new list starts and
 * the list being assembled grows without bound. That is what smears a trail of
 * stale sprites across the picture, so it is worth being able to see in a
 * headless run rather than only in the window. */
static void sdb_frame_begin(void *ctx)
{
    (void)ctx;
    if (s_trace)
        fprintf(stderr, "sdb %ld %d %d %d %d %d\n", s_frame, s_build_peak, s_overflow,
                    s_done_count, s_saw_reset, s_list_lost);
    s_frame++;
    s_cap_run = s_captured ? s_cap_run + 1 : 0;
    s_captured = 0;
    s_overflow = 0;
    s_build_peak = 0;
    s_saw_reset = 0;
}

static void sdb_power_on(void *ctx)
{
    (void)ctx;
    cyc_render_capture_background(true);
    /* Re-apply the requested CPU-budget enhancement: cyc_power_on() cleared
     * hw_extra_timing after the mod activated. 0 is the stock default, so with
     * the feature off this changes nothing. */
    cyc_set_extra_scanlines((unsigned)s_oc_lines);
    s_build_count = 0;
    s_done_count = 0;
    s_overflow = 0;
    s_build_peak = 0;
    s_saw_reset = 0;
    s_prev_cursor = 0;
    s_list_lost = 0;
    s_done_frame = -100;
    s_frame = 0;
    const char *t = getenv("SDB_TRACE");
    s_trace = t && t[0] == '1';
    const char *c = getenv("SDB_TRACE_CAPTURES");
    if (c && c[0]) {
        long a = 0, b = 0;
        if (sscanf(c, "%ld-%ld", &a, &b) == 2) { s_cap_from = a; s_cap_to = b; }
    }
}

/* ---- mod activation ---- */

static const char *const SDB_HOOK_IDS[] = {
    "super-dodgeball.oam.b5-8116",
    "super-dodgeball.oam.b5-8338",
    "super-dodgeball.oam.b5-84b6",
    "super-dodgeball.oam.b4-a2f7",
    "super-dodgeball.oam.b4-a330",
    "super-dodgeball.obj.b5-8231",
    "super-dodgeball.cull.b5-8200",
    "super-dodgeball.list.f7-e378",
    "super-dodgeball.num.b3-96b9",
    "super-dodgeball.copy.b4-a394",
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
    { "--ppu-overclock", true, "extra blank scanlines before NMI (0-262); 0 = stock timing" },
    { "--widescreen", true, "present 16:9 / 21:9 / 32:9 / fit instead of stock 4:3" },
    { "--overscan", false, "drop the blank scanlines a CRT hid at the top and bottom" },
};

/* ------------------------------------------------------------------ *
 * 4. Widescreen and overscan (super-dodgeball.widescreen)
 * ------------------------------------------------------------------ */

/* PRESENTATION ONLY. The guest is never touched: no cycle added or removed,
 * no RAM written, no PPU register changed. The compositor runs after the frame
 * has finished, from cyc_render_present(), and reads state the frame left.
 *
 * The extra columns are produced by READING the nametable rather than by
 * warping the framebuffer the way wideNES does. wideNES remembers pixels it
 * has already seen and slides them by the $2005 delta, so it can only ever
 * show what the guest has already drawn, it has no idea where a nametable
 * boundary is, and its own maintainer lists "cannot handle off-screen sprites"
 * as the reason it never shipped (docs/research.md 25).
 *
 * That machinery is not needed here, because this game's world is entirely
 * resident in CIRAM before the frame starts: measured over a match the court's
 * nametable is byte-identical from frame 450 to frame 900 and spans the full
 * 64-tile / 512-pixel space of both nametables --
 *
 *   C0 C0 C0 C0 | 4E 4D 4E 4D ... | 04 03 04 03 ... | 2C 2B 2C 2B ... | C0 C0 C0 C0
 *   backdrop     left sideline     crowd             right sideline     backdrop
 *
 * so a margin column is resolved the way the PPU would have resolved it:
 * scroll -> world column -> tile -> attribute -> palette. Nothing is inferred
 * and nothing is remembered, so there is no warm-up and no scene bookkeeping.
 *
 * The world is 512 wide and the canvas at most 854, so at 21:9 and 32:9 the
 * requested column falls outside it and is filled with the backdrop colour.
 * The court already has blank backdrop at both of its own edges, so that reads
 * as the dark arena around the court rather than a second copy of it -- the
 * same choice SMB's widescreen makes with room_left / room_right.
 */
#define SDB_WS_WORLD_W   512     /* two nametables, which horizontal mirroring gives */

/* Overscan: 8/8 is the number every emulator lands on (NESdev's 256x224
 * "action safe"; Mesen, RetroArch and the libretro Mesen core all offer 8 as
 * their first step). For this ROM it is also exactly lossless: measured over
 * 551 non-blank match frames and 96 title/menu frames, rows 0-15 and 232-239
 * carry 0% non-backdrop pixels while rows 224-230 carry ~90% (the court), so 8
 * is the most that can go and 12 cannot. */
/* The game clears PPUMASK bit 1, so the PPU paints the backdrop in the
         * leftmost 8 columns. Measured over 2112 frames on both vectors those 8
         * columns carry no ink in ANY frame -- they are the NES's left
         * overscan, which the game hides and which a CRT would crop.
         *
         * "Overscan removal" here therefore REVEALS them rather than cutting
         * them: the compositor draws the real background there instead of the
         * backdrop. Cutting them would splice the world -- the margin shows
         * world columns up to sx-1 and the picture would resume at sx+8, a
         * visible 8-column skip in the court -- and revealing them makes the
         * margin and the picture one continuous run of world columns with no
         * black gutter between them.
         *
         * Left only, and measured, not assumed: the right-hand 8 columns carry
         * the court on 1372 of 2112 frames, and the blank top and bottom bands
         * are used by one wipe transition (frame 420), so neither may be cut. */
#define SDB_WS_CLIP 8
static int s_ws_reveal;
static int s_ws_aspect = NES_VIDEO_STOCK;
static int s_ws_fixed;           /* width we asked for by hand, 0 = by mode */
static long s_ws_composed, s_ws_pillarboxed, s_ws_last_diff;
static int s_ws_sx_lo = 9999, s_ws_sx_hi = -1;   /* camera scroll seen, SDB_TRACE=3 */

/* Past the edge of the 512-pixel world the canvas is filled with the backdrop,
 * which for this game is black -- the same colour the world's own outermost 32
 * columns already are (the blank tile C0), so the margin continues the
 * court's real edge instead of stopping at it.
 *
 * Wrapping or mirroring was tried and rejected: the world's outer band holds
 * the scoreboard and the Statue of Liberty skyline, so mirroring renders the
 * text backwards and wrapping shows the same skyline two or three times. Both
 * invent picture the game never had and both look like a bug at 32:9, where
 * the margin is 299 columns wide and therefore most of the screen. Black is
 * the honest answer: past the stadium's end there is nothing to show. */
static uint32_t sdb_ws_pixel(int wx, int y)
{
    if (wx < 0 || wx >= SDB_WS_WORLD_W) return cyc_render_color(0);
    int nt_x = (wx >> 8) & 1;
    int tile_x = (wx & 255) >> 3;
    int sub_x = wx & 7;

    int sy = cyc_render_line_scroll_y(y);      /* 0-479, two screens stacked */
    int nt_y = (sy >= 240);
    int tile_y = (sy - nt_y * 240) >> 3;
    int sub_y = sy & 7;

    unsigned base = (unsigned)(nt_x * 0x400 + nt_y * 0x400);
    uint8_t tile = cyc_render_nametable((uint16_t)(0x2000u + base + tile_y * 32u + tile_x));
    /* Attributes sit at the END of each 1 KiB nametable page, $23C0 / $27C0 --
     * NOT at $2800. This cart is horizontally mirroring, which makes $2800
     * alias $2000 and $2C00 alias $2400, so an attribute address built the
     * usual way reads tile bytes. Measured: the byte at page offset 0x3C2 is
     * 0x55, and 0x55 is palette 1 in all four quadrants -- exactly the palette
     * the machine paints there and this code did not.
     *
     * The mirroring folds $2C00 onto the right physical page by itself, so
     * nt_y can stay in the base here just as it does for the tile address. */
    uint8_t attr = cyc_render_nametable((uint16_t)(0x2000u + base + 0x3C0u +
                                                   (tile_y >> 2) * 8u + (tile_x >> 2)));

    /* This game does NOT use the standard 16x16-metatile attribute layout: the
     * index that matches its picture is (tile >> 2), one byte per 4x4 TILES, so
     * each byte's four palettes cover 2x2 tiles each and the quadrant is
     * (tile >> 1) & 1. Measured both ways against the machine's own background
     * capture: (tile>>2) leaves 2613 pixels disagreeing out of 61440,
     * the textbook (tile>>1)&7 leaves 18420. The bytes are at $23C0/$27C0 --
     * page offset 0x3C0 -- because horizontal mirroring aliases $2800 onto
     * $2000, which is this cart's mirroring.
     *
     * Bits: 0-1 top-left, 2-3 top-right, 4-5 bottom-left, 6-7 bottom-right. */
    unsigned qy = (unsigned)((tile_y >> 1) & 1);
    unsigned qx = (unsigned)((tile_x >> 1) & 1);
    unsigned pal = (attr >> ((qy * 4u) + (qx * 2u))) & 3u;

    uint16_t pattern = (uint16_t)(cyc_render_line_bg_table(y) + tile * 16 + sub_y);
    uint8_t lo = cyc_render_chr(pattern);
    uint8_t hi = cyc_render_chr((uint16_t)(pattern + 8));
    unsigned bit = 7u - (unsigned)sub_x;       /* bit 7 is the leftmost pixel */
    unsigned v = ((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1);
    return cyc_render_color(v ? (int)(pal * 4 + v) : 0);
}

/* The canvas the engine hands the compositor, plus the picture the machine
 * really produced. Reconstructing the whole canvas and then comparing the
 * native 256 columns against the machine's own is the seam gate: if the two
 * disagree, the scroll, nametable or palette state being read is not what the
 * PPU used, and the margins would be drawn from the same wrong state. A frame
 * that fails returns 0, which tells the engine to pillarbox the native picture
 * exactly as it does today -- so title screens, menus and scene changes fall
 * back for free instead of growing garbage.
 *
 * Only pixels the background owns are compared; sprite pixels legitimately
 * differ, because this pass does not draw sprites. */
/* Which palette RAM entry a drawn colour came from, for diagnostics. Two
 * entries can hold the same colour, so the first match is reported; that is
 * enough to tell a wrong quadrant from a wrong tile. */
static int sdb_ws_pal_index(uint32_t argb)
{
    for (int e = 0; e < 32; e++)
        if (cyc_render_color(e) == argb) return e;
    return -1;
}

/* The seam check compares our reconstruction against the machine's BACKGROUND,
 * not against its picture. The picture has sprites composited on top -- a
 * character standing in the court would otherwise count as a reconstruction
 * failure on every pixel it covers, which is what a first pass of this check
 * did: ~9000 "failures" a frame, all of them players, none of them background.
 * cyc_render_background() is the engine's exact capture of the background
 * before the sprite priority mux (see cyc_render.h), which is the thing this
 * pass is actually reproducing. */
static uint32_t s_ws_native_bg[256 * 240];

static int sdb_ws_composite(uint32_t *out, int width, int height, int native_x0,
                            const uint32_t *native, void *user)
{
    (void)user;
    if (width <= 256 || height != 240) return 0;
    if (!(cyc_render_line_mask(239) & 0x08)) return 0;   /* background off */

    const uint8_t *opaque = cyc_frame_bg_opaque();
    for (int y = 0; y < height; y++) {
        int sx = cyc_render_line_scroll_x(y);
        /* PPUMASK bit 1 is "show background in the leftmost 8 pixels" -- SET means
         * shown, so the clip exists when the bit is CLEAR. (wideNES's own write-up
         * states the opposite of its code here; the code is right.) This game
         * clears it, and getting the polarity backwards paints real court in
         * the eight columns the PPU painted with the backdrop. The clip is in
         * screen columns, so it lands on the native 256, not on the world. */
        int clip = s_ws_reveal ? 0 : ((cyc_render_line_mask(y) & 0x02) ? 0 : SDB_WS_CLIP);
        uint32_t *row = out + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            int off = x - native_x0;
            if (off >= 0 && off < clip) { row[x] = cyc_render_color(0); continue; }
            /* cyc_render_line_scroll_x is 0-511, $2000 bit 0 already folded in,
             * so a canvas column's world column is that line's scroll plus its
             * own offset from native column 0. */
            int wx = sx + off;
            row[x] = sdb_ws_pixel(wx, y);
        }
    }

    /* The revealed columns are meant to differ, so they are not compared. */
    int first = s_ws_reveal ? SDB_WS_CLIP : 0;
    long diff = 0;
    if (cyc_render_background(s_ws_native_bg)) {
        for (int y = 0; y < 240; y++) {
            const uint32_t *row = out + (size_t)y * width + native_x0;
            const uint32_t *bg = s_ws_native_bg + (size_t)y * 256;
            for (int x = first; x < 256; x++) {
                if (opaque && !opaque[(size_t)y * 256 + x]) continue;
                if (row[x] != bg[x]) diff++;
            }
        }
    } else {
        /* No capture available: nothing to verify against, so do not claim a
         * seam we did not check. */
        return 0;
    }
    /* 2% of the frame's background pixels. Generous enough that a handful of
     * genuinely ambiguous edge pixels do not force a fallback, tight enough
     * that reading the wrong nametable cannot pass. */
    if (diff * 100 > 2 * 256 * 240) {
        s_ws_pillarboxed++;
        s_ws_last_diff = diff;
        return 0;
    }

    /* The machine's own 256 columns win, so the middle of a wide frame is
     * pixel-for-pixel the stock picture at every aspect ratio -- with one
     * deliberate exception: when the overscan feature is revealing the clipped
     * columns, those are left as reconstructed, or the copy would put the black
     * gutter straight back. */
    int keep = s_ws_reveal ? SDB_WS_CLIP : 0;
    for (int y = 0; y < height; y++)
        memcpy(out + (size_t)y * width + native_x0 + keep, native + (size_t)y * 256 + keep,
               (256 - keep) * sizeof(uint32_t));
    s_ws_composed++;
    s_ws_last_diff = diff;
    return 1;
}

/* The width a ratio needs for a picture `rows` rows tall. Without a crop this
     * is whatever the engine's aspect mode says (426 / 560 / 854 at 240 rows).
     * With one, the picture is 224 rows tall and the same RATIO needs a different
     * width -- 16:9 -> 398, 21:9 -> 522, 32:9 -> 796. Using the stock widths with
     * a cropped height would silently deliver 34:9 instead of what was asked for. */
static int sdb_ws_width_for(int mode, int rows)
{
    double aspect;
    switch (mode) {
    case NES_VIDEO_16_9: aspect = 16.0 / 9.0; break;
    case NES_VIDEO_21_9: aspect = 21.0 / 9.0; break;
    case NES_VIDEO_32_9: aspect = 32.0 / 9.0; break;
    default: return 0;         /* stock and fit use the engine's own numbers */
    }
    /* The engine's own rounding (nes_video_geometry_width_for_aspect): round to
     * the nearest even column. Truncating instead turns 32:9's 853.3 into 852,
     * which is 3.55:1 rather than 3.556:1 -- a half-pixel of aspect ratio is
     * nobody's idea of exact. */
    int w = 2 * (int)floor(((double)rows * aspect) / 2.0 + 0.5);
    if (w < 256) w = 256;
    if (w > NES_VIDEO_MAX_WIDTH) w = NES_VIDEO_MAX_WIDTH;
    return w & ~1;              /* even, so the stock 256 stay centred */
}

static void sdb_ws_apply(void)
{
    int wide = s_ws_aspect != NES_VIDEO_STOCK;
    cyc_render_set_compositor(wide ? sdb_ws_composite : NULL, NULL);

    if (!wide) {
        s_ws_fixed = 0;
        cyc_video_set_mode(NES_VIDEO_STOCK);
        return;
    }
    s_ws_fixed = sdb_ws_width_for(s_ws_aspect, 240);
    if (s_ws_fixed > 0) {
        /* A fixed width rather than a named mode: the mode computes its width
         * from 240 rows, and a cropped picture is shorter than that. Note
         * request_width() clears the mode, so a mode can only be replaced by
         * re-asserting it, never by "clearing" the width. */
        cyc_video_set_mode(NES_VIDEO_STOCK);
        cyc_video_request_width(s_ws_fixed);
    } else {
        cyc_video_set_mode(s_ws_aspect);
    }
}

static void sdb_ws_set_aspect(int mode)
{
    s_ws_aspect = mode;
    sdb_ws_apply();
}

static void sdb_ws_set_overscan(int on)
{
    s_ws_reveal = on ? SDB_WS_CLIP : 0;
    sdb_ws_apply();
}

/* Reset callbacks run before every activation pass, so a disabled feature
 * reliably restores the stock 256-column picture. */
static void sdb_ws_disable(void)
{
    s_ws_aspect = NES_VIDEO_STOCK;
    s_ws_reveal = 0;
    s_ws_fixed = 0;
    cyc_render_set_compositor(NULL, NULL);
    cyc_video_set_mode(NES_VIDEO_STOCK);
}

/* SDB_TRACE=1: one line at exit, so a headless run says whether the margins
 * were actually drawn or quietly pillarboxed away every frame. */
static void sdb_ws_report(void)
{
    if (s_trace && s_ws_composed + s_ws_pillarboxed)
        fprintf(stderr, "widescreen: %ld composed, %ld pillarboxed, seam diff %ld px\n",
                s_ws_composed, s_ws_pillarboxed, s_ws_last_diff);
}

static int sdb_ws_aspect_from(const char *env, const char *option)
{
    char buf[32] = "";
    int mode = NES_VIDEO_STOCK;
    if (env && env[0] && nes_video_geometry_parse(env, &mode)) return mode;
    if (nes_mod_option_value("super-dodgeball.widescreen", "widescreen",
                             "aspect", buf, sizeof buf) &&
        nes_video_geometry_parse(buf, &mode))
        return mode;
    (void)option;
    return NES_VIDEO_STOCK;
}

static void sdb_ws_enable(void)
{
    sdb_ws_set_aspect(sdb_ws_aspect_from(getenv("SDB_WIDE"), "aspect"));
}

static void sdb_ov_enable(void)
{
    const char *e = getenv("SDB_OVERSCAN");     /* headless A/B */
    sdb_ws_set_overscan(e && e[0] ? atoi(e) != 0 : 1);
}

/* ------------------------------------------------------------------ *
 * 5. PPU overclocking (super-dodgeball.ppu-overclock)
 * ------------------------------------------------------------------ */

/* The game feels slower than it should because its main loop is an NMI-synced
 * spin: `$FCA0` parks waiting for the next NMI (research.md 19.3, 19.6), so the
 * guest only gets 262 scanlines of CPU per frame. Hand it more and it completes
 * more of its own work before parking again, so the game runs faster.
 *
 * That is exactly what the cycle backend already offers as
 * `cyc_set_extra_scanlines` -- "blank scanlines before NMI, APU paused"
 * (runner/cyc/PROJECTS.md) -- the same mechanism as MesenCE's
 * `Nes.PpuExtraScanlinesBeforeNmi` and FCEUX's post-render scanlines. Upstream
 * scopes it to MMC3; patches/006 widens the gate to MMC1, whose IRQ is
 * write-triggered at $8000/$8001 and never clocked by the PPU. Measured against
 * MesenCE at 300 lines: +12.0 work-ticks per frame, and the lead grows
 * (research.md section 23).
 *
 * The added lines are BLANK -- rendering is off above scanline 239 and the APU
 * is paused across them -- so no extra picture appears and the guest's 256x240
 * output is unchanged. This mod only picks a value for the backend setting.
 *
 * It deliberately changes console timing, so it is OFF by default and a player
 * who wants hardware parity can leave it off.
 */
#define SDB_OC_MAX     262    /* what cyc_set_extra_scanlines accepts */
#define SDB_OC_DEFAULT 262    /* one extra frame's worth, which doubles the CPU
                               * budget per frame; fceumm's "2x-Postrender"
                               * preset uses 266 for the same reason. MesenCE
                               * allows 1000, our backend caps at 262. */

static void sdb_oc_apply(int lines)
{
    if (lines < 0) lines = 0;
    if (lines > SDB_OC_MAX) lines = SDB_OC_MAX;
    s_oc_lines = lines;
    if (!cyc_set_extra_scanlines((unsigned)lines))
        fprintf(stderr, "ppu-overclock: backend refused %d extra scanlines\n", lines);
    else
        fprintf(stderr, "ppu-overclock: %d extra scanlines before NMI%s\n", lines,
                lines ? "" : " (stock timing)");
}

/* Runs when the launcher or a saved selection enables the feature. */
static void sdb_oc_enable(void)
{
    const char *t = getenv("SDB_PPUSCAN");
    if (t && t[0]) { sdb_oc_apply(atoi(t)); return; }   /* headless A/B */
    sdb_oc_apply(nes_mod_get_option_int("super-dodgeball.ppu-overclock",
                                        "ppu-overclock", "scanlines",
                                        SDB_OC_DEFAULT));
}

/* Reset callbacks run before active plugins, so a disabled feature reliably
 * restores stock timing on every launch. */
static void sdb_oc_disable(void)
{
    s_oc_lines = 0;
    cyc_set_extra_scanlines(0);
}

static bool sdb_option(void *ctx, const char *name, const char *value)
{
    (void)ctx;
    if (!strcmp(name, "--remove-character-flicker")) {
        for (unsigned i = 0; i < sizeof SDB_HOOK_IDS / sizeof SDB_HOOK_IDS[0]; i++)
            nes_mod_set_function_hook_enabled(SDB_HOOK_IDS[i], 1);
        s_on = true;
        return true;
    }
    if (!strcmp(name, "--ppu-overclock")) {
        if (!value || !value[0]) {
            fprintf(stderr, "--ppu-overclock needs a value: 0..%d\n", SDB_OC_MAX);
            return false;
        }
        char *end = NULL;
        long v = strtol(value, &end, 10);
        if (!end || *end || v < 0 || v > SDB_OC_MAX) {
            fprintf(stderr, "--ppu-overclock: value must be 0..%d\n", SDB_OC_MAX);
            return false;
        }
        sdb_oc_apply((int)v);
        return true;
    }
    if (!strcmp(name, "--widescreen")) {
        int mode;
        if (!value || !nes_video_geometry_parse(value, &mode)) {
            fprintf(stderr, "--widescreen: stock, 16:9, 21:9, 32:9 or fit\n");
            return false;
        }
        sdb_ws_set_aspect(mode);
        return true;
    }
    if (!strcmp(name, "--overscan")) {
        /* Declared as a bare flag, so the host passes no value at all (a NULL
         * here means "flag given", not "flag not given"). */
        sdb_ws_set_overscan(value == NULL || value[0] == '\0' || atoi(value) != 0);
        return true;
    }
    return false;
}

static const CycHostExtras SDB_EXTRAS = {
    .present = sdb_present,
    .power_on = sdb_power_on,
    .frame_begin = sdb_frame_begin,
    .options = SDB_OPTIONS,
    .option_count = sizeof SDB_OPTIONS / sizeof SDB_OPTIONS[0],
    .option = sdb_option,
};

const CycHostExtras *cyc_host_extras(void) { return &SDB_EXTRAS; }

/* One report per run, on the way out. SDB_TRACE=1 turns it on. */
__attribute__((destructor)) static void sdb_report_at_exit(void)
{
    sdb_ws_report();
}

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
    /* The five OAM cursor bumps share one callback; the sixth site is the
     * per-object emitter's flag load and has its own. */
    static const struct { uint16_t addr; int (*fn)(uint16_t); } sites[] = {
        { 0x8116, sdb_capture }, { 0x8338, sdb_capture }, { 0x84B6, sdb_capture },
        { 0xA2F7, sdb_capture }, { 0xA330, sdb_capture },
        { 0x8231, sdb_unsuppress },
        { 0x8200, sdb_no_cull },
        { 0xE378, sdb_add_hidden },
        { 0x96B9, sdb_capture2 },
        { 0xA394, sdb_copy },
    };
    if (!nes_mod_register_activation_plugin("super-dodgeball.character-flicker", sdb_enable) ||
        !nes_mod_register_activation_plugin("super-dodgeball.ppu-overclock", sdb_oc_enable) ||
        !nes_mod_register_activation_plugin("super-dodgeball.widescreen", sdb_ws_enable) ||
        !nes_mod_register_activation_plugin("super-dodgeball.overscan", sdb_ov_enable) ||
        !nes_mod_register_reset_callback(sdb_disable) ||
        !nes_mod_register_reset_callback(sdb_oc_disable) ||
        !nes_mod_register_reset_callback(sdb_ws_disable))
        fprintf(stderr, "sdb: mod registration failed\n");
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0]; i++)
        if (!nes_mod_register_function_entry_plugin(SDB_HOOK_IDS[i], sites[i].addr, sites[i].fn))
            fprintf(stderr, "sdb: hook registration failed for $%04X\n", sites[i].addr);
}