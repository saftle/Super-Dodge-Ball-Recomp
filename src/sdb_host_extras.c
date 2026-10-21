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
#include "sdb_cheats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- guest constants (facts about the ROM, not ROM bytes) ---- */

/* The widest disagreement, in world columns, that still counts as a whole
 * object sitting beside the machine's copy of itself rather than as two
 * unrelated sprites. A camera step plus one: the step itself, plus the frame of
 * ball travel that separates a fresh margin capture from a 3-frame-stale OAM
 * (edge replay f856: 9 against a step of 7). The n>=2 modal vote in
 * sdb_object_delta is what keeps a wide window from matching a neighbouring
 * player, not this number. */
#define SDB_MATCH_SLOP 12

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
/* The bits of record byte 0 (`$43`) that section 2.1's culling gates read. They
 * change while the object is still the same object -- the flicker mod exists to
 * override them -- so they are not part of its identity. See the draw site. */
#define SDB_CULL_BITS    0x70u
/* How far a presented-list entry's row may sit from OAM's and still be called
 * the same sprite. See sdb_reconcile; +-2 is measured, +-3 is not free. */
#define SDB_RECONCILE_ROW 2
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

/* ------------------------------------------------------------------ *
 * 1b. Margin sprites: the 16-bit positions the game computes and throws away
 * ------------------------------------------------------------------ */

/* $8483 is one instruction after the emitter's accumulator loop
 *
 *     $8470: LDA $04,X / CLC / ADC $0A,X / STA $04,X      (twice: X and Y)
 *
 * has folded the object's camera-relative X ($0A/$0B) and Y ($0C/$0D) into the
 * per-part position ($04/$05, $06/$07), and before the game's own cull:
 *
 *     $8488: LDA $04 / CMP #$04 / BCC $849A     X < 4         -> hide
 *     $848E: LDA $06 / CMP #$04 / BCC $849A     Y < 4         -> hide
 *     $8494: LDA $05 / ORA $07 / BEQ $84A1      high byte set  -> hide
 *     $849A: LDA #$F8 / STA $0200,X             hide (Y = $F8)
 *     $84A1: LDA $04 / SEC / SBC #$04 / STA $0203,X   OAM X = X16 - 4
 *     $84A9: LDA $06 / CLC / ADC #$04 / EOR #$FF / SEC / SBC #$10
 *           STA $0200,X                                 OAM Y = $EB - Y16
 *
 * Both measured against OAM on the current binary: a sprite at X16 = 208
 * lands at OAM X = $CC and one at Y16 = 82 at OAM Y = $99, which is what the
 * two lines above compute.
 *
 * This is why the widescreen margins can hold sprites at all. The game places
 * every sprite in a 16-bit SCREEN coordinate and then hides anything outside
 * [4, 255] by writing Y = $F8 into the OAM slot it would have used -- so the
 * tile, its flips and its palette are all computed, and only the final byte
 * truncation loses it. OAM X cannot express a column outside the viewport; the
 * accumulator can. Capturing here therefore reads the game's OWN placement
 * rather than inferring one, which is the same authority rule the rest of this
 * mod follows.
 *
 * $0617 is untouched at this site (the culled path branches to $84C1, past the
 * cursor bump at $84B6), so this never perturbs the cursor pin above, and it
 * fires for sprites the game emitted -- visible or not -- without changing
 * which of them reach OAM. */
typedef struct {
    int16_t dx;         /* part column RELATIVE to its object's record anchor */
    /* THE OBJECT'S WORLD COLUMN AT THE EMISSION THAT PRODUCED THIS PART.
     *
     * The margin places a part at `this + part_offset - scroll - 4`, and the
     * anchor used to be the record's X read LIVE at presentation time. The
     * machine's own copy of the same sprite is FROZEN at the emission that
     * produced it -- the game rewrites the record every frame and emits an
     * object's sprites every four -- so the two were on different time bases
     * the moment the object moved, and a character straddling the machine's
     * 256-column edge came apart.
     *
     * MEASURED on a played 16:9 capture, object 5 at frames 549-552: the game
     * emitted the character at frame 548 with accumulators 259/251/257/249/
     * 257/249/256/248, so the machine drew the LEFT half at OAM X 247/245/245/
     * 244 and the game culled the RIGHT half (accumulator > 255), which the
     * margin drew. The record's X moved 355 -> 359 at frame 550, so the
     * margin's half moved +4 to 259/257/257/256 while the machine's half
     * stayed at 247: a four-column GAP straight through the character. At 552
     * the two halves overlapped instead.
     *
     * So the anchor is taken at the capture, not at the draw, and the part
     * stands where the object stood when the game last emitted it -- the same
     * time base the machine's copy of it is on.
     */
    uint16_t rec_x;     /* the record's X at the emission this part came from */
    int16_t y;          /* screen row of the sprite's FIRST line, from $06/$07 */
    uint8_t tile;
    uint8_t attr;
    uint8_t own;        /* the GAME emitted this $4D inside its own cull, so the
                         * machine owns it and the margin must not draw it */
    int16_t id;         /* $4D, the emitter's index into THIS object's sprite
                         * list -- a stable part identity. Only meaningful when
                         * `keyed`; see the site split below. */
    uint8_t keyed;      /* 1 = identified by `id`, 0 = by (dx,row) */
    long b;             /* the sprite LIST that last emitted this part */
} SdbMargin;

/* The two emitter sites, which cull identically but store their tile and
 * attribute differently. See SdbMargin. */
#define SDB_MARGIN_STRIP 0x8483u   /* the per-object strip walk, from $8348 */
#define SDB_MARGIN_SOLO  0x8302u   /* the single-sprite emitters, from $82B9/$82DD/$8294 */

/* The game's Y accumulator to a screen row: first line = $EC - Y16. See
 * sdb_margin_capture. */
#define SDB_MARGIN_Y0 236

/* The camera the emitter subtracts ($821E: SEC / SBC $C3 / SBC $C4). */
#define SDB_CAM_LO 0x00C3u
#define SDB_CAM_HI 0x00C4u

/* The eight columns the PPU's PPUMASK bit 1 hides, declared here rather than
 * with the rest of the widescreen state because sdb_margin_capture needs it:
 * it is the width of the hardware's own left clip, which loses sprites just as
 * surely as the game's cull and is half of what this feature recovers. */
#define SDB_WS_CLIP 8

/* The world's own bounds, moved up from the compositor because the post-stamp
 * margin pass in sdb_present needs them and runs before it. */
#define SDB_WS_CAM_MIN 40          /* $A6AF: LDA #$28 -- where the camera RESTS */
#define SDB_WS_CAM_MAX 224         /* $A6B9: LDA #$E0 */
#define SDB_WS_VIEW_W  256         /* the viewport's width, in world columns */
/* How far past [CAM_MIN, CAM_MAX] the camera may actually reach, and so how far
 * the anchor's "is this a court frame" test has to tolerate before standing the
 * anchor down. NOT a fudge: the game's own clamp at $A6AF is one-sided (BCC skips
 * the store when $61 is already below 40), so $C3 receives a lower target
 * verbatim, and the camera really does go left of 40 -- measured on a court-cam
 * played session, it holds $C3 = 32 for 32 frames while a ball is thrown left,
 * and standing the anchor down there re-centres the whole canvas, which is a
 * 93-column jump. See the note at the test.
 *
 * 8 is measured, not chosen. Over 3457 in-match samples -- the played session
 * plus four vectors -- $C3 spans 32..216, i.e. 8 past CAM_MIN at the left and
 * never past CAM_MAX at the right. A slack of 8 therefore admits the whole
 * observed range while leaving the menu screens (scroll 0) comfortably outside,
 * which is what the test is for. Widening it to cover CAM_MAX + slack as well
 * was measured and is WRONG: it lets the block run 10 columns off the right edge
 * at the camera's far end (16 of 1085 sweep frames), because the interval it
 * has to fit in is already empty at 16:9 and every column of slack spent at one
 * end is a column stolen at the other. */
#define SDB_WS_CAM_SLACK 8

/* The range the camera ACTUALLY reaches, as distinct from the clamp it is given.
 * Measured over 3457 in-match samples -- a court-cam played session plus four
 * vectors -- $C3 spans [32, 216], not the nominal [40, 224]. Both ends matter.
 * The left one is the slack above; the right one matters because the anchor's
 * interval is ALREADY EMPTY at 16:9 (the canvas holds 170 columns and the
 * camera spans 184), so every column admitted at one end is a column stolen at
 * the other. Using the nominal 224 there admitted 8 columns the game never asks
 * for and pushed 10 columns of the block off the canvas's right edge -- measured
 * 16 of 1085 sweep frames, a hard vertical seam in the court at f=267. */
#define SDB_WS_CAM_LO_SEEN 32       /* measured, not nominal */
#define SDB_WS_CAM_HI_SEEN 216

#define SDB_WS_LO (SDB_WS_CAM_MIN)
#define SDB_WS_HI (SDB_WS_CAM_MAX + SDB_WS_VIEW_W)
static int s_ws_reveal;         /* overscan: the clip is being revealed */
/* What --overscan / SDB_OVERSCAN asked for, or -1 for "nobody has an opinion,
 * follow widescreen". Resolved to s_ws_reveal by sdb_ws_apply, which is the
 * only place that decides -- see the note there for why it cannot be stamped by
 * whichever handler happened to run. */
static int s_ws_reveal_explicit = -1;

/* THE CAMERA ANCHOR. Declared HERE, with the rest of the widescreen state,
 * because sdb_present -- the flicker mod's own presentation path, which runs
 * BEFORE the compositor's function in this file -- has to agree with it.
 *
 * It did not, and the symptom was the health bars and the player names sliding
 * left and right with the camera while the court sat still: the compositor
 * places the machine's own picture at TWO offsets (the court band and the held
 * band), and sdb_present stamped all 240 rows at the single one
 * `cyc_render_native_origin` returns. Measured at frame 649 of the sweep, where
 * the two are 48 columns apart: the flicker-on run's rows 0-48 are the
 * flicker-off run's rows 0-48 shifted exactly +48, and the court band aligns at
 * 0. Those 48 rows are the skyline, the names and the health bars -- the HUD the
 * game holds still in screen space, which is the entire reason there are two
 * offsets.
 *
 * See sdb_ws_anchor_frame and sdb_ws_row_x0, below, for the rest. */
static int s_ws_lock;            /* the setting: 0 = follow, 1 = anchored */
static int s_ws_court;           /* the modal line scroll: the court's camera */
static int s_ws_x0_court;        /* canvas column of native column 0, court band */
static int s_ws_x0_held;         /* ... on the HELD band, which is not the court */
static int s_ws_anchor;          /* world column the canvas's column 0 holds */
static int s_ws_anchored;        /* frames presented with the anchor engaged */
static int s_ws_framed;          /* frames where it deliberately did not */
static int s_ws_here;            /* this frame: did the anchor engage? */

/* Where native column 0 sits, for one screen row. Forward-declared for
 * sdb_present; defined with the anchor arithmetic. */
static int sdb_ws_row_x0(int y);
static int sdb_ws_band_native(int rx0);
static int sdb_ws_sprite_origin(void);
static int sdb_ws_margin_shift(int prow, int origin);

/* The frame counter and capture-trace range live here (rather than with the
 * flicker state below) because the object-index callback above needs them. */
static long s_frame;                     /* frame counter, for the trace */
static long s_cap_from = -1, s_cap_to;   /* SDB_TRACE_CAPTURES=<from>-<to> */
static int s_trace;                    /* SDB_TRACE=1: per-frame trace */

/* SDB_TRACE_CAPTURES NARROWS A TRACE. IT DOES NOT START ONE.
 *
 * The per-sprite lines used to be gated on the capture range alone, so an
 * inherited `SDB_TRACE_CAPTURES=0-6000` printed `sdbw f=478 x0=158: n=0` and
 * `oam-band:` twice a frame for an entire session on a plain
 * `./build_and_run.sh` -- twice, on 2026-10-17, because an exported shell
 * variable outlives the capture it was set for and nothing in the tree objected.
 *
 * Every trace in this project is switched on by SDB_TRACE=1 (see the s_trace
 * read at session start), so the range is a MODIFIER of that, exactly as the
 * name says: it selects which frames a running trace reports. Reading it as an
 * enabler made an environment variable silently turn on thousands of lines per
 * frame in normal play, and made it look like a bug in the margin when it was a
 * stale shell. */
#define SDB_CAP_TRACE (s_trace && s_cap_from >= 0 && s_frame >= s_cap_from \
                       && s_frame <= s_cap_to)
static int s_ws_aspect = NES_VIDEO_STOCK;  /* moved up: sdb_frame_begin's trace reads it */
/* SDB_TRACE_JSON=<from>-<to>: one JSON object per line, two kinds. See
 * sdb_json_frame / sdb_json_margin for why it is a separate stream and not a
 * reformatting of the sdb/sdbc/sdbw/sdbm lines. */
static long s_json_from = -1, s_json_to;

/* Trace counters. With per-object slots there is no single list to measure, so
 * these are per-frame tallies and the SDB_TRACE=1 line reports the totals. */
static long s_margin_drawn;         /* sprites actually painted */
static long s_margin_lost;          /* captures dropped on the per-object limit */
static long s_margin_off_edge;      /* sprites the game culled */
static long s_margin_cleared;       /* parts withheld: the game's OAM page was cleared */
static long s_margin_stale;         /* objects stood down: the game stopped emitting them */
static long s_margin_frames;        /* frames the trace counted */
static int  s_margin_on;            /* feature active */
static int  s_margin_captured;      /* a capture fired this frame */
static int  s_margin_run;           /* consecutive frames with captures */
static int  s_margin_count;         /* sprites painted this frame (trace only) */
static int  s_margin_peak;          /* most sprites painted in one frame */

/* Keyed by OBJECT, not by list. The object index comes from the display record
 * pointer $821E leaves in $61/$66 (see sdb_margin_object), so a capture always
 * knows which object it belongs to and nothing has to be reassembled.
 *
 * One slot per record ($0618 + 7*index, thirteen of them) holding that object's
 * off-screen sprite parts. A character is eight sprites, so eight slots per
 * object is the exact bound; the single-sprite emitters put one there.
 *
 * WHAT A SLOT HOLDS is the part's column RELATIVE to its object's record anchor,
 * not an absolute one, and that is the whole reason this feature does not blink.
 * `$821E` computes `$0A/$0B` as (record X - camera) and `$8470` adds it to the
 * running position, so `part_column = record_X + part_offset` in world terms --
 * the camera cancels. Storing the absolute screen column instead means it goes
 * stale the moment the camera moves, and the earlier TTL on "when was this last
 * emitted" was a patch over the resulting gap rather than a fix: the game's
 * display list (`$067C`) does not even CONTAIN most off-screen objects. Measured
 * over frames 600-700 of the pan vector, `$8231` fires for records 0,1,3,5,6,7,
 * 9,10,11 on 14-16 of every 100 frames but for record 2 only 4 times and record
 * 8 ONCE -- and 2 and 8 are precisely the two objects sitting in the margins. So
 * any lifetime measured in emissions blinks those two out and only those two.
 *
 * Presentation therefore re-reads the RECORD, which the game refreshes every
 * frame for every object whether or not it is on the display list (verified:
 * records 0,1,4,10 all walk across the run while 2, 8 and 9 stand still, and
 * they are all being written), and recomputes the part's world column from it.
 * Liveness is the record itself: byte 0 non-zero means the slot is in use, and
 * the current column being outside the viewport means the machine is not drawing
 * it. Nothing expires, so nothing blinks, and there is no TTL to tune.
 *
 * `id0`/`id1` are the record's first two bytes as they were at capture. Records
 * are recycled as a match goes on, and a reused slot still holds the OLD
 * object's tiles; if the identity no longer matches, the slot is stale and is
 * not drawn until the game emits the object again and refreshes it. */
#define SDB_MARGIN_OBJECTS 13
/* 40, not 24: parts are now CARRIED across a list boundary rather than
 * wiped, because the game's own sprite index says which of them are
 * still owed (see sdb_margin_capture). Two lists of a 16-sprite object
 * have to fit, and 16 + slack is 40. */
#define SDB_MARGIN_PARTS 40
/* HOW LONG THE GAME MAY BE SILENT ABOUT AN OBJECT before the margin stops
 * drawing it. This is the menu-ghost rule, and the number is measured rather
 * than guessed, because a threshold is only worth having if the two states
 * are actually separated.
 *
 * Liveness was already read from the display record rather than from OAM
 * (research 26: the game's display list does not contain most off-screen
 * objects, so anything measured in list membership blinks objects 2 and 8).
 * The record is the wrong thing for this question, though: the between-rounds
 * screens leave every record standing while the game stops emitting the whole
 * roster, so the margin keeps repainting whatever stood off the right edge of
 * the pitch. Measured on a played level-1 session (1905 frames, captured with
 * `tools/play_record.sh`), the object list
 * goes quiet between frames 1041-1205 and resumes at 1557 -- objects 6, 7 and
 * 8 stand in the right margin at screen columns 271-330 for that whole
 * stretch, drawn from a capture the game made 504 frames earlier and has not
 * touched since.
 *
 *     largest gap between two emissions of one object, in play:
 *       sdb_sweep / sdb_throw / sdb_idle / sdb_pan, 3000 frames each    140
 *       the played level-1 session, 1905 frames                       144
 *     smallest gap that spans a menu in that session                   352
 *
 * Every one of those in-play maxima is object 8 -- the object research 26
 * already names as emitted least often and as one of the two that stand in the
 * margins -- so this stands down exactly the objects whose silence is hardest
 * to read, which is why it is measured on all four vectors and not on one.
 * 240 sits 71% above the in-play maximum and 32% below the menu minimum.
 *
 * This is NOT the freshness threshold research 26 warns about, which was about
 * the flicker mod's whole published list and its age in frames: that quantity
 * cycles every three or four and a sweep publishes at ages 4-8, so it had no
 * separation to find. This one does. */
#define SDB_MARGIN_STALE 240

typedef struct {
    SdbMargin parts[SDB_MARGIN_PARTS];
    int n;
    long seen;         /* frame the parts were last written (trace/debug) */
    uint8_t id0, id1;  /* record bytes 0-1 at capture: the object identity */
    long build;        /* the sprite list these parts belong to */
    long pruned;       /* the build sdb_ws_prune_margin last retired against */
} SdbMarginObj;

static SdbMarginObj s_margin_obj[SDB_MARGIN_OBJECTS + 1];  /* +1 = discard bin */
static int s_margin_cur_obj = -1;   /* the object $821E is emitting right now */
static uint16_t s_margin_rec;       /* its $0618-relative display record */
static long s_margin_build;         /* bumped when $0617 restarts a list */
static uint8_t s_margin_prev_cursor;

/* $8231, one instruction after $821E stores this object's display record into
 * $4E. $61/$66 still holds the record pointer, so `(ptr - $0618) / 7` is the
 * object index and the captures that follow belong to that object.
 *
 * This is a SECOND plugin on the site the flicker mod already uses, and it
 * touches nothing: no RAM is written, so enabling it with the margin sprites on
 * and the flicker mod off is still byte-for-byte stock. The flicker mod's own
 * callback at the same address clears the hide bit, which IS a guest change, so
 * it cannot be reused here. */
static int sdb_margin_object(uint16_t addr)
{
    (void)addr;
    uint16_t rec = (uint16_t)(cyc_mod_peek(0x61) | (cyc_mod_peek(0x62) << 8));
    int idx = (rec >= SDB_REC_BASE && (rec - SDB_REC_BASE) % SDB_REC_STRIDE == 0)
              ? (rec - SDB_REC_BASE) / SDB_REC_STRIDE : -1;
    /* Not one of the thirteen records: file the captures that follow in the bin
     * past the end, so a stray pointer cannot be charged to a real object. */
    s_margin_cur_obj = (idx >= 0 && idx < SDB_MARGIN_OBJECTS) ? idx : SDB_MARGIN_OBJECTS;
    /* The record itself, kept because the captures run later and $61/$66 will
     * have moved on: its X is the anchor every part's offset is measured from,
     * and it is also what presentation re-reads. */
    s_margin_rec = rec;
    /* New sprite list? $0617 only ever moves forwards while a list is being
     * emitted -- including past sprites the game culled, because other objects
     * in the same list do bump it -- and it starts again near zero for the next
     * one. Measured over frames 618-645 of the pan vector it walks 04, 24, 40,
     * 60, 80, 84, 94, C8, E8, 00 and then restarts at 04, never backwards in
     * between. A backwards step is therefore a build boundary, and it is the
     * only boundary this feature can see. */
    uint8_t cur = cyc_mod_peek(SDB_CURSOR);
    if (SDB_CAP_TRACE)
        fprintf(stderr, "sdbv f=%ld obj=%d rec=%02X/%02X x=%d cur=%02X "
                        "held=%d seen=%ld\n",
                s_frame, s_margin_cur_obj,
                cyc_mod_peek(s_margin_rec), cyc_mod_peek((uint16_t)(s_margin_rec + 1)),
                (int)(cyc_mod_peek((uint16_t)(s_margin_rec + 3))
                    | (cyc_mod_peek((uint16_t)(s_margin_rec + 4)) << 8)),
                cur,
                (s_margin_cur_obj >= 0 && s_margin_cur_obj < SDB_MARGIN_OBJECTS)
                    ? s_margin_obj[s_margin_cur_obj].n : -1,
                (s_margin_cur_obj >= 0 && s_margin_cur_obj < SDB_MARGIN_OBJECTS)
                    ? s_margin_obj[s_margin_cur_obj].seen : -1L);
    if (cur < s_margin_prev_cursor) {
        s_margin_build++;
        if (SDB_CAP_TRACE)
            fprintf(stderr, "sdbb f=%ld bump: cur=%02X < prev=%02X -> build=%ld, "
                            "obj %d had %d parts\n",
                    s_frame, cur, s_margin_prev_cursor, s_margin_build,
                    s_margin_cur_obj,
                    (s_margin_cur_obj >= 0 && s_margin_cur_obj < SDB_MARGIN_OBJECTS)
                        ? s_margin_obj[s_margin_cur_obj].n : -1);
    }
    s_margin_prev_cursor = cur;
    return 0;
}

static SdbSprite s_build[SDB_MAX_SPRITES];   /* list being assembled */
static int s_build_count;
static int s_build_peak;              /* largest s_build_count seen this frame */
static SdbSprite s_done[SDB_MAX_SPRITES];    /* last list that finished */
static int s_done_count;
static int s_overflow;
static uint8_t s_prev_cursor;          /* previous capture's cursor value */
static int s_list_lost;                /* lists that hit the backstop */
static int s_saw_reset;              /* a capture saw the cursor at slot 1 */
static int s_captured;              /* a hook fired this frame */
static int s_culled_restored;       /* objects put back on the display list */
static long s_done_frame = -100;    /* frame the published list was finished */
static int s_cap_run;                /* consecutive frames with captures */
static int s_on;                             /* feature active */
/* List entries withheld at presentation because the game's OAM page was cleared.
 * Per frame for the JSON record, cumulative for the exit line: a healthy match
 * withholds nothing, and a non-zero total is the gate reporting it fired. */
static int s_retired_frame;
static long s_retired_total;
static long s_cap_total;             /* entries captured, for the same line */
static int s_hidden_slots;           /* OAM slots hidden at the last present */

/* Requested PPU-overclock lines, re-applied at power_on (see section 5). */
static int s_oc_lines;

static uint32_t s_picture[256 * 240];

/* The presented picture when it is wider than 256 (widescreen) and/or shorter
 * than 240 (the overscan crop). One buffer, repacked: a cropped picture cannot
 * be handed back as a pointer into the engine's canvas, because its rows are
 * canvas_width apart while the host copies it as a dense width*height block. */
static uint32_t s_canvas[NES_VIDEO_MAX_WIDTH * 240];

/* Moved up: `sdb_present` needs both, and it runs BEFORE the compositor. See
 * the note on why the margin has to be drawn there too. */
static uint32_t s_ws_native_bg[256 * 240];

/* WHICH NATIVE PIXELS THE MACHINE ACTUALLY PAINTED. 255 = it did not.
 *
 * Derived from the machine's OWN RENDERED FRAME against its own background
 * capture, not from OAM. That distinction is the whole of it, and getting it
 * wrong is why an earlier attempt at this failed: the map was built by walking
 * OAM, and `cyc_render_sprites` takes a `place` callback
 * (CycSpritePlaceFn) that the character-flicker mod uses to move each sprite
 * onto the position its published list gives it. So OAM says one thing and the
 * frame says another, the map described where sprites were rather than where
 * they were painted, and it marked columns 252-253 as owned that the machine
 * in fact left as background -- which is the opposite of what was needed and
 * made the change a measured no-op on the artefact it was written for.
 *
 * Comparing the two pictures has no such gap: `native` IS what the machine
 * drew, reconcile and all. A pixel that differs from the background capture is
 * a pixel the machine painted, and that is the only question the margin has.
 *
 * The leftmost eight columns are never painted (PPUMASK bit 2 is clear for this
 * game), so they are left unowned and the margin supplies them -- which is the
 * whole point of revealing them.
 *
 * The shape is `smb_ws_actors.c`'s `native_priority[]`, from the sibling recomp
 * on this same engine, whose WIDESCREEN.md gives the reason the margin cannot
 * simply clip at the block's edge: "Managed enemies are drawn as complete
 * sprites across both old screen edges; their corresponding native OAM slots
 * are suppressed for presentation. This avoids joining half a host sprite to
 * SMB's column-clipped native sprite." */
static uint8_t s_ws_native_owner[256 * 240];

static void sdb_ws_build_owner(const uint32_t *native)
{
    for (int y = 0; y < 240; y++) {
        const uint32_t *n = native + (size_t)y * 256;
        const uint32_t *b = s_ws_native_bg + (size_t)y * 256;
        uint8_t *o = s_ws_native_owner + (size_t)y * 256;
        for (int x = 0; x < 256; x++)
            o[x] = (x < SDB_WS_CLIP || n[x] == b[x]) ? 0xFF : 0;
    }
}

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
        s_cap_total++;
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

    if (SDB_CAP_TRACE)
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

    if (SDB_CAP_TRACE)
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

/* One sprite the game computed and then culled for being off-screen, kept with
 * the 16-bit column it would have had. Runs at $8483 or $8302 (see SdbMargin).
 *
 * Returns 0 always: the site's own instruction and the cull that follows it both
 * run, and $0617 is not touched (the culled path branches past the cursor bump
 * at $84B6), so the guest's OAM is exactly what it would have been without us.
 * That is what keeps this feature presentation-only. */
/* If live OAM holds this exact part, the deltas to the machine's copy.
 * Returns nonzero on a match, with the column delta in `*col` and the row
 * delta in `*rowd`.
 *
 * `world` is the part's world column as the margin re-placed it (`rec_x + dx`);
 * the machine's is `oam_x + scroll + 4` -- the same quantity measured through the
 * guest's own `$84A1: SEC / SBC #$04`. Both carry the same OAM bias, so the
 * difference needs no correction. The row window is SDB_RECONCILE_ROW, the same
 * measured +-2 the flicker reconcile uses: a re-emitted sprite that also moved
 * has a new row as well as a new column.
 *
 * Matched on WORLD, never on tile alone. Every player shares tiles, so a
 * tile-and-row match is ambiguous by construction, and this project has been
 * bitten by that twice already: the `sdb_oam_has` filter (research 26) and the
 * band anchor that replaced it. Two objects are never at the same world column,
 * so this names one sprite and cannot name the wrong one.
 *
 * An exact column agreement (delta 0) IS a match and is reported as one: it
 * votes for 0 rather than abstaining, so a split object -- some parts agreeing
 * with the machine, some a step away -- resolves toward leaving it alone
 * instead of moving the agreeing parts off the machine. */
static int sdb_world_match(int world, int row, uint8_t tile, int scroll,
                           int *col, int *rowd)
{
    const uint8_t *oam = cyc_render_oam();
    if (!oam) return 0;
    for (int j = 0; j < 64; j++) {
        if (oam[j * 4] >= 0xEF) continue;          /* the game's hidden slot */
        if (oam[j * 4 + 1] != tile) continue;
        int dr = (int)oam[j * 4] + 1 - row;
        if (dr > SDB_RECONCILE_ROW || dr < -SDB_RECONCILE_ROW) continue;
        int dc = (int)oam[j * 4 + 3] + scroll + 4 - world;
        if (abs(dc) > SDB_MATCH_SLOP) continue;
        *col = dc;
        *rowd = dr;
        return 1;
    }
    return 0;
}

/* WHERE A MARGIN PART GOES.
 *
 * `record_X + part_offset - scroll - 4`, with the anchor taken AT THE EMISSION
 * rather than at the draw.
 *
 * The machine's own copy of a sprite is FROZEN at the emission that produced
 * it: the game writes an OAM entry and the PPU draws it at that column until
 * the game rewrites it, and the game rewrites an object's sprites every four
 * frames while it rewrites the display RECORD every frame. So the record runs
 * ahead of the picture, and a part placed from the LIVE record ends up on a
 * different time base from the machine's copy of the same sprite.
 *
 * MEASURED on a played 16:9 capture, object 5 at frames 549-552: the game
 * emitted the character at frame 548 with accumulators 259/251/257/249/257/
 * 249/256/248, so the machine drew the LEFT half at OAM X 247/245/245/244 and
 * the game culled the RIGHT half (accumulator > 255), which the margin drew.
 * The record's X moved 355 -> 359 at frame 550, so the margin's half moved +4
 * to 259/257/257/256 while the machine's half stayed at 247: a four-column GAP
 * straight through the character, and at 552 the two halves overlapped
 * instead. With the anchor taken at the emission the margin's half stays at
 * 255/253/253/252, which is the machine's half plus eight columns, and the
 * character is whole.
 *
 * WHAT THIS IS NOT. It is not "the game's own placement" (the accumulator the
 * emitter computed): that is camera-relative and frozen at the emission, so a
 * character the game stopped emitting drifts away from where it is as the
 * camera moves -- measured on the same capture, object 3, emitted at frame 544
 * with the camera at 108 and not emitted again for 880 frames while the
 * camera walked to 40, and on the accumulator it was drawn 68 columns from
 * where the object was. It is also not safe on its own: placing a part where
 * the game put it puts it on a column the machine may have painted with a
 * STALE sprite, and the ownership test then stands the margin down and the
 * sprite's head goes missing.
 *
 * The anchor at the emission keeps the part where the OBJECT was when the game
 * last emitted it, on the frame's own scroll -- which is the machine's basis
 * for the copy of it that the PPU is drawing, without giving up the margin's
 * own scroll-relative placement for the objects only it draws. */
static int sdb_margin_col(const SdbMargin *m, int scroll)
{
    return (int)m->rec_x + m->dx - scroll - 4;
}

/* The one correction to make to this object's record anchor this frame, or 0.
 *
 * THE PROBLEM. The margin places a part with the latched scroll
 * (`cyc_render_line_scroll_x`); the guest makes OAM X from `$C3/$C4`. Those two
 * cameras differ by one step, and only while the camera is moving. On the frames
 * where they differ, a part the machine is also painting gets painted a second
 * time, one character-width away: the doubled ball at the left seam and the
 * sliver left behind by it (research 26). Measured on the pan vector over 1581
 * whole-object comparisons, the disagreement is 0 or exactly one step, and
 * nothing else:
 *
 *     d = cam - scr     -7      -6       0      +6      +7
 *     delta            0,+7    0,+6      0      0,-6    0,-7
 *
 * Swapping the basis for `$C3/$C4` outright is NOT the fix and measures worse:
 * it puts 75% of stepping frames right where the scroll had 33%.
 *
 * THE GATE, and why it is what makes this safe. Live OAM cannot be trusted as
 * "where is this sprite now" -- the game leaves OAM alone for 3-6 frames between
 * builds, so on a quiet frame it answers for an older one, and placing parts by
 * it unconditionally cuts characters in half (the sweep at 759-761, where OAM was
 * three columns out and following it moved a player's head off its body).
 *
 * But OAM IS authoritative about one thing: it is what the machine paints this
 * frame, stale or not. So the correction is applied only when the disagreement
 * has exactly the shape of a basis error -- a whole object sitting a CONSTANT
 * distance from the machine's copy of it, and that distance being the camera
 * step. A stale OAM disagrees by an arbitrary amount and is ignored; a
 * coincidental match with a neighbouring player cannot hold one offset across an
 * object's parts. Both are measured facts, not hopes: the stale case reads 3,
 * which is no multiple of anything, and the gate rejects it.
 *
 * WHOLE OBJECT, never per part. A character is sixteen columns of two eight-wide
 * sprites; moving one half onto the machine's column and leaving the other on
 * the record collapses the eight pixels between them to one, and `margin_track`
 * reads that as a `ghost`. Done per part it measured 22 of them on the sweep. */
/* The one row correction to make to this object's parts this frame, or 0.
 * Same vote as the columns, on the row deltas from the same matched slots:
 * two parts must agree the machine's copy sits N rows away. The vote window
 * itself bounds it (|N| <= SDB_RECONCILE_ROW), so a genuinely different
 * animation cel -- rows further out than that -- is never touched. */
static int sdb_object_delta(const SdbMarginObj *ob, int *row_corr)
{
    /* Only objects the machine cannot be painting. The correction moves parts
     * onto the machine's columns; for a part the machine already paints that
     * risks trading a good join for a modal mismatch in a crowd (sweep 618:
     * an 8-part character straddling band and viewport was moved -7 onto a
     * neighbour, jogging head against body). A part outside [8,251] can never
     * overlap anything the machine paints, so moving it can only fix a join or
     * defer to the machine -- never double. This replaces the old basis-shape
     * gate, which admitted that case and refused the d=0 intra-frame advance
     * (flicker-2 f718: 4px with the camera still). */
    int seen[2 * SDB_MATCH_SLOP + 1];
    int rseen[2 * SDB_RECONCILE_ROW + 1];
    memset(seen, 0, sizeof seen);
    memset(rseen, 0, sizeof rseen);
    int parts = 0;
    for (int i = 0; i < ob->n; i++) {
        const SdbMargin *m = &ob->parts[i];
        int prow = m->y < 0 ? 0 : (m->y >= 240 ? 239 : m->y);
        /* THE PART'S SCREEN COLUMN, on whichever basis this object is on.
         * See sdb_margin_col: the game's own placement when the machine drew
         * part of this character, the live world position when it did not. */
        int scroll = cyc_render_line_scroll_x(prow);
        int sx = sdb_margin_col(m, scroll);
        if (sx >= 8 && sx <= 251) { *row_corr = 0; return 0; }
        int dc, dr;
        if (sdb_world_match(sx + scroll + 4, m->y, m->tile, scroll, &dc, &dr)) {
            seen[dc + SDB_MATCH_SLOP]++;
            rseen[dr + SDB_RECONCILE_ROW]++;
        }
        parts++;
    }
    if (parts < 2) { *row_corr = 0; return 0; }   /* one part is not a disagreement */

    int best = 0, n = 0;
    for (int k = 0; k < 2 * SDB_MATCH_SLOP + 1; k++)
        if (seen[k] > n) { n = seen[k]; best = k - SDB_MATCH_SLOP; }
    if (n < 2) { *row_corr = 0; return 0; }   /* no two parts agree: not our object */
    int best_r = 0, rn = 0;
    for (int k = 0; k < 2 * SDB_RECONCILE_ROW + 1; k++)
        if (rseen[k] > rn) { rn = rseen[k]; best_r = k - SDB_RECONCILE_ROW; }
    *row_corr = (rn >= 2) ? best_r : 0;
    /* No basis gate: any constant whole-object offset is acted on. The gate used
     * to require the disagreement to be the camera step, but d=0 disagreements
     * are real too (flicker-2 f718: margin 4px from the machine with the camera
     * still -- intra-frame cam advance, not a basis), and leaving them draws a
     * second copy of a sprite the machine is painting. Moving the margin onto
     * the machine's columns can only ever put it where the machine paints, so
     * the worst case is stock-faithful; the modal vote plus the slop bound keep
     * a neighbouring player from ever supplying the offset. */
    return best;
}

static int sdb_margin_capture(uint16_t addr)
{
    uint8_t cur = cyc_mod_peek(SDB_CURSOR);

    /* For the strip walk, the tile and attribute the emitter already wrote into
     * the slot it was about to use: $8419-$8427 stores them at $0200+cursor+1 and
     * +2 before the position test runs. $0200+cursor (the Y byte) is stale at
     * this point -- the game has not written it yet, which is exactly why the
     * cull writes $F8 there -- so Y is taken from the accumulator instead. */
    uint16_t at = (uint16_t)(SDB_OAM_BASE + cur);
    int16_t x = (int16_t)(cyc_mod_peek(0x04) | (cyc_mod_peek(0x05) << 8));
    int16_t y = (int16_t)(cyc_mod_peek(0x06) | (cyc_mod_peek(0x07) << 8));

    /* Every ENTRY, before the cull below, so "the emitter did not fire" and "the
     * emitter fired and the part was rejected" are different lines. $02 is the
     * emitter's per-object sprite counter ($84C1: DEC $02 / BEQ) and $4D the
     * index it is walking ($8483: INC $4D), which is what says whether the game
     * emitted fewer sprites for this object or emitted the same ones and the
     * capture threw them away. */
    if (SDB_CAP_TRACE)
        fprintf(stderr, "sdbp f=%ld obj=%d at=$%04X $02=%d $4D=%d x=%d y=%d "
                        "cur=%02X build=%ld held=%d\n",
                s_frame, s_margin_cur_obj, addr,
                (int)cyc_mod_peek(0x02), (int)cyc_mod_peek(0x4D), x, y, cur,
                s_margin_build,
                (s_margin_cur_obj >= 0 && s_margin_cur_obj < SDB_MARGIN_OBJECTS)
                    ? s_margin_obj[s_margin_cur_obj].n : -1);

    /* `seen` IS "THE FRAME THE GAME LAST EMITTED THIS OBJECT", and it is stamped
     * HERE, before any of the cull branches below.
     *
     * It used to be stamped at the bottom of this function, after the not-culled
     * branch and the vertical-cull branch had already returned, so it only ever
     * advanced on a part the game CULLED. An object standing on the pitch,
     * emitting eight sprites every four frames with all eight on screen, never
     * moved it at all. Measured on the sweep vector: object 0 is emitted at frames
     * 382, 386, 390 ... 622 and `seen` was still reading 382 on frame 623, because
     * every one of those emissions took the `!lost` exit.
     *
     * That is invisible while `seen` is only a trace field, and it is why the
     * liveness gate built on it (SDB_MARGIN_STALE) stood objects down 26,676 times
     * over 3000 frames of an ordinary match. The `sdbp` line prints ahead of all
     * of these branches, so the emission-gap measurement taken from it was the
     * true one all along; only the field the code kept was wrong. */
    if (s_margin_cur_obj >= 0 && s_margin_cur_obj < SDB_MARGIN_OBJECTS)
        s_margin_obj[s_margin_cur_obj].seen = s_frame;

    /* A DIFFERENT OBJECT NOW HOLDS THIS SLOT: throw away what the previous one
     * left behind.
     *
     * The identity is written at the bottom of this function, on every capture,
     * and the three draw sites compare the live record against it and skip the
     * object when they disagree. That is what keeps a recycled slot from being
     * drawn wearing the previous occupant's tiles -- and it is exactly what
     * RESURRECTS them, because the disagreement is the only thing hiding them.
     * The moment the game emits the slot's new occupant, the capture stamps the
     * NEW identity, the gate now agrees, and every part the OLD occupant left in
     * `o->parts` becomes drawable again.
     *
     * Measured on a played stage-2 session (captured with
     * `tools/play_record.sh`). Object slot 0:
     *
     *     f=96     n=8   identity 00/48    the menu's object
     *     f=1081   n=19  identity 00/48    11 more parts arrive, same identity
     *     f=1676   n=19  identity 02/31    stage 2's object takes the slot
     *
     * From 641 to 1675 the live record read 02/18 or 02/31 against a recorded
     * 00/48, so nothing of slot 0 was drawn at all. At 1676 the game emitted the
     * new occupant's eight sprites, the identity was refreshed, and the eleven
     * parts captured at 1081 -- 595 frames earlier, for a different object, on a
     * different screen -- became visible. They are what the player sees slide out
     * of the left edge from 1676 and settle by 1693: eleven tiles in two rows at
     * screen rows 152 and 160, columns 72..112, drawn in a strip the game has not
     * emitted since 1081 and cannot paint here, so nothing ever covers them.
     *
     * The comparison MASKS `$70` out of byte 0 for the reason research 26 already
     * records: the game flips bits 4-6 of the state byte to signal its own
     * culling, so an unmasked test reads that flip as a new object and drops a
     * character mid-match. Byte 1 is compared whole, exactly as the draw sites
     * compare it, so a slot the game has genuinely re-identified is dropped and
     * rebuilt from this frame's captures. */
    if (s_margin_cur_obj >= 0 && s_margin_cur_obj < SDB_MARGIN_OBJECTS) {
        SdbMarginObj *slot = &s_margin_obj[s_margin_cur_obj];
        uint8_t n0 = cyc_mod_peek(s_margin_rec);
        uint8_t n1 = cyc_mod_peek((uint16_t)(s_margin_rec + 1));
        if ((n0 & ~SDB_CULL_BITS) != (slot->id0 & ~SDB_CULL_BITS) || n1 != slot->id1) {
            if (slot->n && SDB_CAP_TRACE)
                fprintf(stderr, "sdbR f=%ld obj %d re-identified %02X/%02X -> "
                                "%02X/%02X: discarding %d parts from the previous "
                                "occupant\n", s_frame, s_margin_cur_obj,
                        slot->id0, slot->id1, n0, n1, slot->n);
            slot->n = 0;
            slot->build = s_margin_build;
            slot->pruned = s_margin_build;
        }
    }

    /* Which of the two emitters this is. They place a sprite the same way (the
     * same 16-bit accumulators, the same [4,255] cull) but take the tile and
     * attribute from different places: the strip walk at $8348 has already
     * written them into the OAM slot it is about to use ($8419-$8427), while
     * the single-sprite emitters landing at $8302 still hold them in zero page
     * and only store them after the cull passes ($8326/$832B). Reading
     * $0200+cur unconditionally would get the strip walk's bytes and the wrong
     * bytes for the other three. */
    int strip = addr == SDB_MARGIN_STRIP;

    /* Two different things lose a sprite, and only one of them is the game's.
     *
     * The GAME's cull is `X in [4,255] AND Y in [4,255]` ($8488/$848E/$8494):
     * anything outside it never reaches OAM.
     *
     * The PPU's own left clip is the other one, and it is easy to miss because
     * the game does not cull those sprites -- it writes them to OAM and the
     * hardware declines to draw them. PPUMASK bit 1 is clear for this game, so
     * SCREEN columns 0-7 are never painted, and OAM X is the sprite's column
     * (`$84A1: SEC / SBC #$04`), which puts that band at accumulator X = 4..11.
     * With overscan revealing those eight columns from the nametable, a sprite
     * standing in them is lost exactly as surely as one the game culled, and
     * nothing draws it: the machine skips it and the old filter skipped it too.
     * That is a player sliced in half at the screen edge while the camera pans,
     * and getting the sense of this test backwards -- dropping the band instead
     * of keeping it -- reproduces it precisely.
     *
     * A sprite culled VERTICALLY is never captured, because there is no vertical
     * margin to put it in -- the camera scrolls horizontally only, so an
     * off-screen row is off-screen for good. */
    /* THE GAME'S OWN CULL, AND NOTHING ELSE.
     *
     * An earlier version asked the DRAW SITE's question here instead --
     * `csx = x + lag - 4`, `lost = csx < 0 || csx > 251` -- on the theory that the
     * two filters disagree by the camera lag and that asking the same question in
     * the same space would fix that. It does fix the artefact it was written for
     * and it costs a character its head elsewhere: `cyc_render_line_scroll_x` is
     * read HERE, mid-frame, while the emitter runs, and the value the DRAW site
     * will use is the one latched for the frame being presented. Those are not
     * the same number at the same moment. Measured on the margin-locked capture,
     * a left-margin player loses 82 of 149 ink pixels at frame 817, and the loss
     * occurs on lag-zero frames as well as lagged ones, so it is the timing of
     * the read and not the lag term.
     *
     * So the two filters are left disagreeing, deliberately: this one is a cull
     * of what the GAME emitted, asked in the game's own accumulator, and the
     * other is a placement decision made at presentation time. The seam is closed
     * where it belongs -- in `sdb_ws_margin_pass`, after `sdb_present` has
     * stamped the native block -- not by trying to make a mid-frame read agree
     * with a presentation-time one. */
    /* THE GAME'S OWN CULL, EXACTLY. Do not widen it.
     *
     * `$8488` hides a sprite with X16 < 4 and `$8494` hides one whose high byte is
     * set, so the game only ever emits accumulator 4..255 -- OAM X 0..251.
     *
     * Both ways of reconciling that with the screen column the answer is consumed
     * in were tried here and both are worse than leaving it alone:
     *
     *  - Asking the DRAW site's question, by reading `cyc_render_line_scroll_x`
     *    mid-frame and computing `x + lag - 4`: fixes frames 808 and 812 and
     *    costs a left-margin player 82 of 149 ink pixels at frame 817. The loss
     *    occurs on lag-zero frames as well as lagged ones, so it is the timing of
     *    the read -- the value read here while the emitter runs is not the value
     *    the draw site will use at presentation -- and not the lag term.
     *  - Widening by one camera step (the delta takes 0, 4, 6, 7), capturing
     *    more and letting the draw site discard: 12 bad frames against this
     *    build's 2, and it broke 805-807 which are otherwise clean. Capturing
     *    more is NOT safe, because the margin's copy of a part and the machine's
     *    sit a column apart after the flicker mod's reconcile, so the ownership
     *    test reads those columns as unowned and the margin paints a ghost.
     *
     * So the seam is closed where it belongs and nowhere else: in
     * `sdb_ws_margin_pass`, after `sdb_present` has stamped the native block.
     * That is worth 7 of this character's 9 bad frames, with no regression. */
    int lost = x < 4 || x > 255 || (s_ws_reveal && x < 4 + SDB_WS_CLIP);
    /* NOT CULLED MEANS THE MACHINE OWNS THIS SPRITE, AND THAT IS INFORMATION.
     *
     * This used to `return 0`, and the carried part it left behind kept being
     * drawn from a stale row. That is the back-and-forth pixel damage reported
     * from play on 2026-10-17: panning one way the margin's copy lags the
     * machine's and columns go missing, panning the other it leads and columns
     * are drawn twice, and it alternates with every direction change.
     *
     * The reason it could not be fixed downstream is that the ownership map is
     * the wrong question. It compares the machine's rendered frame against its
     * background, so it can only say "the machine painted something HERE"; it
     * cannot say "the machine painted THIS SPRITE over THERE, on a different
     * row". A carried part whose row is one build stale therefore lands on
     * background the machine left alone, ownership calls it free, and the margin
     * draws a second copy of a sprite the machine has already drawn.
     *
     * The game answers it directly. It emits $4D k either side of its own cull,
     * and which side is exactly the question:
     *
     *   - culled (we are here)     -> the margin's to draw;
     *   - not culled (this branch) -> the machine drew it, so stop.
     *
     * And the "not offered at all" case is the f=808 seam, which is the one
     * place a carried part is still owed: at that boundary the game had emitted
     * $4D 1, 3, 5, 7 and had NOT reached 0, 2, 4, 6, which had crossed inside
     * the viewport. Those stay undrawn and keep their place; the moment the
     * game's list catches up and emits them, this branch marks them and they go.
     *
     * So `own` is the game's own answer, per part, and it is what retires a
     * carried part -- not the ownership map and not a threshold. It also clears
     * again when the camera pans back and the same $4D is culled once more,
     * which is the other half of the back-and-forth. */
    if (!lost) {
        if (s_margin_cur_obj >= 0 && s_margin_cur_obj < SDB_MARGIN_OBJECTS
            && addr == SDB_MARGIN_STRIP) {
            SdbMarginObj *o = &s_margin_obj[s_margin_cur_obj];
            int widx = (int)(int8_t)cyc_mod_peek(0x4D);
            int rcx = (int)(cyc_mod_peek((uint16_t)(s_margin_rec + 3))
                          | (cyc_mod_peek((uint16_t)(s_margin_rec + 4)) << 8));
            int ccam = (int)(cyc_mod_peek(SDB_CAM_LO) | (cyc_mod_peek(SDB_CAM_HI) << 8));
            uint16_t at = (uint16_t)(SDB_OAM_BASE + cur);
            for (int i = 0; i < o->n; i++)
                if (o->parts[i].keyed && o->parts[i].id == widx) {
                    SdbMargin *m = &o->parts[i];
                    m->own = 1;
                    m->b = s_margin_build;
                    /* TAKE THE FRESH GEOMETRY, NOT JUST THE CLAIM.
                     *
                     * The game has just told us exactly where it drew this
                     * sprite, and the first version of this branch discarded
                     * that and kept only `own`. So the carried part kept the
                     * column and row from its LAST OFF-EDGE EMISSION -- both
                     * stale -- and every question asked of it afterwards was
                     * asked at the wrong place. Measured: at f=813 the player is
                     * drawn as a 131 px sliver because the part's stale column
                     * AND stale row both miss the machine's copy, and at f=808
                     * the seam's four carried halves are needed at a position
                     * that is one build out of date.
                     *
                     * Refreshing the geometry is what makes the ownership test
                     * answerable: it is then asked where the sprite actually is.
                     * At f=808 the machine has not painted there yet and the
                     * part is owed; at f=813 it has, and the part is retired. */
                    m->dx = (int16_t)(x - (rcx - ccam));
                    /* The anchor, refreshed with the rest of the geometry:
                     * this emission's record X. See SdbMargin. */
                    m->rec_x = (uint16_t)rcx;
                    m->y = (int16_t)(SDB_MARGIN_Y0 - y);
                    m->tile = cyc_mod_peek((uint16_t)(at + 1));
                    m->attr = cyc_mod_peek((uint16_t)(at + 2));
                    break;
                }
        }
        return 0;
    }
    if (y < 4 || y > 255) return 0;

    /* NO LIST-BOUNDARY DETECTION. It is not available, and three attempts are
     * recorded here because each fails in a way that produces a plausible
     * picture.
     *
     * The natural signal is $0617, and it does not work. The game resets the
     * cursor to slot 1 to start a list, but it does that at the top of the walk
     * and then emits the VISIBLE sprites first; by the time a culled sprite is
     * reached the cursor is already part-way up the list. So a site that only
     * ever sees culled sprites never observes the reset. Measured with the cursor
     * tracked on every visit: it rises monotonically for the entire run and never
     * once goes backwards, and the list grows 147 -> 331 -> ... with no reset at
     * all. Tracking it only on captures that pass the filter instead compares a
     * current cursor against a stale one and fires on nothing in particular.
     *
     * Timing is no better. The emission is interleaved with the rest of the
     * frame's work, so captures arrive in bursts of a few sprites separated by
     * one or two empty frames, all belonging to ONE list -- measured 12, 20, 35,
     * then 16, 29, then 24, 40 across a single pan. Treating a gap as a boundary
     * splits each list into fragments (12, 16, 24) and objects drop out of the
     * margins as the camera sweeps, which is the "player appears, vanishes,
     * reappears" symptom.
     *
     * What IS available is identity. $8231 -- one instruction after $821E loads
     * this object's 7-byte display record into $4E -- still has the record pointer
     * in $61/$66, and that is the object index. So the margin list is keyed by
     * OBJECT rather than reassembled from a stream: each capture appends to the
     * current object's slot, and presentation draws every slot. No boundaries to
     * detect, no builds to bridge, and an object cannot vanish because the list it
     * belonged to was cut in the wrong place. See sdb_margin_object(). */

    /* File it under the object $821E is emitting. A character is eight sprites
     * so the slot is exactly sized; anything past that is a runaway and is
     * dropped with a warning rather than overrunning. */
    if (s_margin_cur_obj < 0 || s_margin_cur_obj >= SDB_MARGIN_OBJECTS)
        return 0;                    /* no identity: discard, never guess */
    SdbMarginObj *o = &s_margin_obj[s_margin_cur_obj];
    /* A new sprite LIST: the object's part set is rebuilt from scratch.
     *
     * Not a new VISIT. `$8231` fires once per object per visit, but one visit
     * does not necessarily emit the whole object -- on the pan vector, object 9
     * is visited at frame 624 and emits four parts, again at 626 and emits seven,
     * again at 632 and emits two, and the whole character is eight. Resetting on
     * the visit therefore published a FRAGMENT: frames 631-632 carried the head
     * with no body, which is the "head but no body" at the screen edge, and it is
     * the machine's own body in OAM one camera step away that the fragment then
     * failed to meet. Accumulating across the visits of one list, and starting
     * again when $0617 restarts, is what makes the character whole. */
    /* A LIST BOUNDARY IS NOT THE SAME THING AS A PART HAVING GONE, and this is
     * the fourth attempt at telling them apart (research.md 30). Wiping on the
     * boundary drops parts the machine is itself mid-way through emitting: at
     * trace frame 808 object 3's $4D 0, 2, 4 and 6 had crossed inside the
     * machine's 256 columns and stopped passing the game's cull, so they were
     * not re-offered -- and the machine's own list had not caught up either.
     * The ownership map shows it directly: at f=808 native columns 240-255 are
     * free across all of rows 144-176, and at f=809 they carry the character,
     * every run clipped at column 255.
     *
     * So parts are CARRIED, and retired by sdb_ws_prune_margin against the
     * ownership map instead. But only the parts that HAVE a stable identity can
     * be carried safely: the strip walk identifies each sprite with $4D, so
     * re-emitting one overwrites its own slot no matter how far the object moved
     * or which rows the animation chose this build.
     *
     * The solo emitters cannot be carried and are still dropped here. $4D does
     * not index at $8302 -- it sits at a constant (measured: $4D=8 across a run
     * of them) -- so keying on it would collapse every solo sprite of an object
     * onto one slot. Those keep the (dx, row) key and the boundary wipe. */
    if (o->build != s_margin_build) {
        int keep = 0;
        for (int i = 0; i < o->n; i++)
            if (o->parts[i].keyed) o->parts[keep++] = o->parts[i];
        if (o->n && SDB_CAP_TRACE)
            fprintf(stderr, "sdbb f=%ld boundary: obj %d keeps %d of %d parts "
                            "(build %ld -> %ld)\n",
                    s_frame, s_margin_cur_obj, keep, o->n, o->build, s_margin_build);
        o->n = keep;
        o->build = s_margin_build;
    }
    if (o->n >= SDB_MARGIN_PARTS) {
        s_margin_lost++;
        if (s_margin_lost < 8)
            fprintf(stderr, "sdb: frame %ld: object %d hit the %d-part limit; "
                            "dropped the extra sprites\n", s_frame,
                    s_margin_cur_obj, SDB_MARGIN_PARTS);
        return 0;
    }
    /* The part's column RELATIVE to its object's record anchor. `$821E` computes
     * `$0A/$0B` as (record X - camera) and `$8470` adds it into the running
     * position, so the camera cancels and the world column is just
     * `record_X + part_offset`. Storing the offset rather than the column is
     * what lets presentation re-place the sprite from the record later: the
     * game rewrites every record every frame, but it only emits objects that
     * are on its display list, and off-screen objects mostly are not. */
    int rec_x = (int)(cyc_mod_peek((uint16_t)(s_margin_rec + 3))
                    | (cyc_mod_peek((uint16_t)(s_margin_rec + 4)) << 8));
    int cam   = (int)(cyc_mod_peek(SDB_CAM_LO) | (cyc_mod_peek(SDB_CAM_HI) << 8));
    int16_t dx = (int16_t)(x - (rec_x - cam));
    /* The part's top row, $EC - Y16. See the long note below. */
    int16_t row = (int16_t)(SDB_MARGIN_Y0 - y);
    /* Find-or-append on (offset, row), the part's identity within its object.
     *
     * Appending blindly leaves the previous emission's tiles sitting in the slot
     * until the next list restarts, which is up to six frames of an older
     * animation frame pinned in the overscan band while the machine draws the
     * new one beside it -- the band "catching" a sprite and keeping it. Both
     * halves of a part are a property of the object, not of the frame, so a
     * repeat emission of the same part replaces it rather than doubling it, and
     * a part that has genuinely gone is dropped with the list. */
    /* THE PART'S IDENTITY IS THE GAME'S OWN SPRITE INDEX, `$4D`.
     *
     * `$8483: INC $4D` walks this object's sprite descriptors, and $4D runs 0..7
     * for a character, once per list, in order -- measured at frame 808 on
     * object 3: $4D 0..7 carrying x = 250, 258, 254, 262, 253, 261, 255, 263.
     * The game's cull is X in [4,255], so 1, 3, 5 and 7 are the margin's and
     * **0, 2, 4 and 6 are the machine's**, which is exactly the handover this
     * feature exists to complete.
     *
     * Every previous attempt keyed a part on its POSITION -- (dx, row) -- and
     * that is what made four of them fail the same way. A position is not an
     * identity: the object's part offset drifts because the record anchor and
     * the emission sample different frames, and a running player's animation
     * moves the ROW every build, so a re-emission never matched the part it was
     * replacing and both survived. Measured, the offset drifting from -59 to -67
     * over four frames of one running player, and the object accumulating to
     * `SDB_MARGIN_PARTS` with `backstop` firing.
     *
     * $4D is stable across movement, animation and camera, and it is the only
     * part identity here that comes from the game rather than from arithmetic.
     *
     * NOT AT THE SOLO EMITTERS. $4D does not index at $8302; it holds a constant
     * across them (measured $4D=8), so keying on it there would put every solo
     * sprite of an object in one slot. Those keep (dx, row), which is why the
     * boundary above drops unkeyed parts instead of carrying them. */
    int widx = (int)(int8_t)cyc_mod_peek(0x4D);
    int strip_key = (addr == SDB_MARGIN_STRIP);
    SdbMargin *m = NULL;
    for (int i = 0; i < o->n; i++) {
        if (strip_key) {
            if (o->parts[i].keyed && o->parts[i].id == widx) { m = &o->parts[i]; break; }
        } else if (!o->parts[i].keyed
                   && o->parts[i].dx == dx && o->parts[i].y == row) {
            m = &o->parts[i]; break;
        }
    }
    if (!m) m = &o->parts[o->n++];
    m->keyed = (uint8_t)strip_key;
    m->id = (int16_t)widx;
    m->b = s_margin_build;
    m->own = 0;
    m->dx = dx;
    /* The object's world column at this emission -- the anchor the part is
     * placed from. See SdbMargin. */
    m->rec_x = (uint16_t)rec_x;
    /* Record identity, so a recycled slot is not drawn wearing the previous
     * occupant's tiles. See SdbMarginObj. */
    o->id0 = cyc_mod_peek(s_margin_rec);
    o->id1 = cyc_mod_peek((uint16_t)(s_margin_rec + 1));
    /* The accumulator holds Y16; the game turns it into an OAM Y of
     *
     *     $84A9: LDA $06 / CLC / ADC #$04 / EOR #$FF / SEC / SBC #$10
     *           STA $0200,X
     *
     * which is $EB - Y16, and the PPU's first drawn line is one below that, so
     * the sprite's top line is $EC - Y16. Doing the conversion HERE, with the
     * game's own arithmetic, is why this cannot drift from the picture: the
     * first version passed the raw accumulator through as the row and drew every
     * margin sprite about 36 lines too high, which showed up as players in the
     * upper corners and nowhere near the pitch. Verified against OAM: Y16 = 82
     * gives OAM Y = $99, so the first line is $9A = 154 = 236 - 82. */
    m->y = row;
    m->tile = strip ? cyc_mod_peek((uint16_t)(at + 1)) : cyc_mod_peek(0x02);
    m->attr = strip ? cyc_mod_peek((uint16_t)(at + 2)) : cyc_mod_peek(0x03);

    s_margin_captured = 1;
    s_margin_off_edge++;
    return 0;
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
    /* BEHIND-BACKGROUND PRIORITY, attr bit 5. This was previously a comment
     * claiming Super Dodge Ball does not use it, "measured over a full match".
     * That measurement was wrong: OAM slot 0 carries attr $23 -- bit 5 set --
     * and blitting it unconditionally painted 3 pixels a frame the machine leaves
     * showing background through (126 px over 70 sweep frames, all in rows 46-47 /
     * columns 338-339). Resolved the way cyc_render_sprites resolves it
     * (cyc_render.c:154), against the engine's own per-pixel background coverage.
     * `px - x0` is the native column, because x0 is where native column 0 sits
     * on this canvas. Outside the native 256 there is no captured coverage to
     * test; this function is never called for a margin column (sdb_margin_blit
     * is), so an out-of-range column here can only be the canvas edge. */
    const uint8_t *opq = (attr & 0x20) ? cyc_frame_bg_opaque() : NULL;
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
            if (opq) {
                int nx = px - x0;
                if (nx >= 0 && nx < 256 && opq[(size_t)py * 256 + nx]) continue;
            }
            out[(size_t)py * (size_t)width + (size_t)px] =
                cyc_render_color(16 + (int)(attr & 3) * 4 + v);
        }
    }
}

/* One margin sprite. The same blit as sdb_blit, with the column coming from the
 * game's own 16-bit accumulator instead of an OAM byte (`px0` is that column
 * already mapped onto the canvas), and two differences that are not arbitrary:
 *
 *   - the left-column clip is NOT applied. PPUMASK bit 1 clips the PPU's
 *     leftmost 8 SCREEN columns, which are native columns 0-7; a margin sprite
 *     is by definition not in them, and applying it would need a screen/native
 *     mapping this call does not have. The machine never drew these sprites at
 *     all, so there is nothing to be consistent with.
 *   - behind-background priority (attr bit 5) is honoured against the canvas's
 *     own background pixels. sdb_blit has no coverage buffer because SDB never
 *     sets bit 5, and it still does not; but a margin sprite has no OAM slot,
 *     so if the game ever sets the bit the sprite must not paint over court it
 *     is meant to be behind. cyc_render_background's capture is what the check
 *     needs, and the compositor already has it. */
static void sdb_margin_blit(const SdbMargin *m, int px0, uint32_t *out, int width, int height,
                            const uint32_t *bg, int world_lo, int world_hi,
                            int rx0, const uint8_t *owner)
{
    int rows = cyc_render_line_sprite16(m->y < 240 ? m->y : 239) ? 16 : 8;
    unsigned attr = m->attr;
    uint16_t table = cyc_render_line_sprite_table(m->y < 240 ? m->y : 239);
    for (int r = 0; r < rows; ++r) {
        int py = m->y + r;
        if (py < 0 || py >= height || py >= 240) continue;
        int row = (attr & 0x80) ? rows - 1 - r : r;
        uint16_t pattern = rows == 16
            ? (uint16_t)(((m->tile & 1) ? 0x1000 : 0) + (m->tile & 0xFE) * 16 + (row >= 8 ? 16 : 0))
            : (uint16_t)(table + m->tile * 16);
        uint8_t lo = cyc_render_chr((uint16_t)(pattern + (row & 7)));
        uint8_t hi = cyc_render_chr((uint16_t)(pattern + (row & 7) + 8));
        /* One rule per pixel. The WORLD's bounds are a bound on columns and stay
         * geometric; the native block's are not a bound on PAINTING, so inside it
         * the margin defers only to pixels the machine actually filled. Outside
         * the block there is no owner and nothing to defer to. */
        for (int c = 0; c < 8; ++c) {
            int px = px0 + c;
            if (px < 0 || px >= width) continue;
            if (px < world_lo || px >= world_hi) continue;
            int nx = px - rx0;
            if (nx >= 0 && nx < 256 && owner[(size_t)py * 256 + nx] != 0xFF) continue;
            int bit = (attr & 0x40) ? c : 7 - c;
            int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            if (!v) continue;
            size_t at = (size_t)py * (size_t)width + (size_t)px;
            /* Behind-background priority. Only the native 256 have a captured
             * background pixel to test against; a margin column is one this
             * compositor reconstructed, which is background by construction. */
            if ((attr & 0x20) && bg) {
                /* `rx0`, NOT `(width - 256) / 2`. The centred position is only
                 * where the native block sits when the compositor could not
                 * anchor -- with the camera locked it is at `rx0`, which moves as
                 * the camera does. Reading the background at the centred column
                 * instead tests a pixel of the court that has nothing to do with
                 * this one, so a behind-background sprite stops being occluded:
                 * measured at frame 817, a player standing in front of the crowd
                 * barrier painted straight over it. It is the same class as the
                 * anchor split the compositor's own stamp had to be told about,
                 * one function further down. */
                int nx = px - rx0;
                if (nx >= 0 && nx < 256 && bg[(size_t)py * 256 + nx] != cyc_render_color(0))
                    continue;
            }
            out[at] = cyc_render_color(16 + (int)(attr & 3) * 4 + v);
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
/* The page is CLEAR: every one of the 64 OAM slots is hidden.
 *
 * This is the menu-transition ghost, and it is not a placement question at all.
 * The mod's list is a build old by construction (research 26: one to three
 * frames during play), which is a price worth paying -- but the game is allowed
 * to stop emitting entirely, and then "one build old" is a lie that lasts as
 * long as the silence. Measured on a played level-1 session: the last
 * capture is frame 867 with 60 sprites, `peak` is 0 on every one of frames
 * 868-956, and the mod repaints those same 60 for all 89 of them -- the whole
 * level-1 roster standing around a between-rounds menu for a second and a half.
 * That is the "menu ghost" in AGENTS.md, and the four fixes recorded there all
 * tried either to notice the SILENCE (tightening the list's freshness) or to
 * match the list against OAM by content. Neither can work, for reasons measured
 * on that same capture: the game's cadence leaves 3 quiet frames inside every 4
 * during play (11% of sweep frames), and every player shares tiles, so a content
 * match against OAM is ambiguous thousands of times over.
 *
 * What the machine says needs no threshold and no identity. The game's way of
 * retiring a sprite is to push its row off the screen and change nothing else,
 * and when it clears the screen it clears EVERY slot:
 *
 *     $0200 at frame 873:  F0 FF 23 FA | F0 9E 41 56 | F0 DF 41 4E | ...
 *
 * All sixty-four say y=$F0, with the tile, attribute and X of the very sprites
 * the mod is repainting still in place. A page with nothing visible in it is not
 * "the game is quiet", it is the machine's own answer to "is anything on screen
 * at all" -- and if nothing is, the mod has no dropped sprite to recover and can
 * only invent one. So present nothing.
 *
 * The count is measured rather than assumed, because a rule like this is only
 * worth having if the two states are actually distinguishable. Over that
 * replay's 395 frames: 90 read 64, and they are exactly frames 868-957, the
 * whole silence; every other frame reads 1 to 43. Nothing in between. A partially
 * hidden page is the game's ordinary tail-hide -- the slots this build did not
 * fill -- and is NOT treated as a clear. That is also why this is not the
 * per-entry version of the same idea: dropping each entry whose own slot is
 * hidden was measured firing on slot 63 alone for 107 frames of ordinary play,
 * taking the legs off a player. Over 3000 frames of throw / sweep / idle / pan
 * this test withholds exactly one entry, at frame 40, where the machine's own
 * page is empty.
 *
 * `cyc_render_oam()`, the page the PPU is reading, and not $0200. $0200 is the
 * DMA SOURCE, so it is a frame AHEAD of the picture being presented, and during
 * a level's opening it is emptied to start the next build while the current
 * frame's sprites are still on screen -- measured on the throw vector, frames
 * 45-50: $0200 all-$F0 while live OAM held 21 visible sprites and the mod was
 * holding a 20-entry list. Asking $0200 there withheld all 20, blanking six
 * frames of a title screen the machine was drawing perfectly well. The live page
 * has no such lag: at the menu wipe it goes from 61 visible sprites to 0 between
 * window frames 867 and 868, which is the frame the transition starts. */
static int sdb_page_clear(void)
{
    const uint8_t *oam = cyc_render_oam();
    int hidden = 0;
    if (oam)
        for (int j = 0; j < 64; j++)
            if (oam[j * 4] >= 0xEF) hidden++;
    s_hidden_slots = hidden;
    return oam && hidden == 64;
}

/* Take the machine's X for any list entry that is UNAMBIGUOUSLY the same sprite
 * in OAM; leave the rest alone.
 *
 * The presented layer is a build old by construction (plan.md 15), so an entry
 * the game has since moved is drawn where it WAS. Measured with json_trace.py's
 * positional lag: over the 1200-frame sweep, 42514 of 54479 unambiguous
 * (y, tile, attr) matches agree exactly and 11965 are off -- and every one of
 * those sits on a frame that saw NO build reset, i.e. mid-build, with OAM already
 * carrying the next list. 640 of 1201 frames carry at least one.
 *
 * REJECTING those entries instead is what four previous attempts did, and each
 * cost more than it fixed (research 26): a stale entry and an absent one look the
 * same to a player, and dropping it removes a body. MOVING it is a different
 * decision -- the sprite is still drawn, and it is drawn where the machine has
 * that exact sprite, which is the only position that is not a guess.
 *
 * Unique on BOTH sides or nothing. Every player in a match shares tiles, so a
 * content match with two candidates says nothing about position, and research 26
 * records that tile-and-row matching reports thousands of false hits. An entry
 * that cannot be told apart keeps its captured X, so this can only move a sprite
 * toward the machine, never invent one. */
static const SdbSprite *sdb_reconcile(const SdbSprite *list, int count)
{
    static SdbSprite buf[SDB_MAX_SPRITES];
    const uint8_t *oam = cyc_render_oam();
    if (!oam || count <= 0) return list;

    /* For each list entry: which OAM slots could hold it, if exactly one.
     *
     * The row is matched within SDB_RECONCILE_ROW, not exactly. An entry whose
     * sprite both MOVED and was re-emitted has a new y as well as a new x
     * between builds, and an exact-y match cannot see that at all -- so the
     * entry kept its stale x and the sprite was drawn twice, a couple of pixels
     * apart. The ball straddling the viewport edge is what shows it: the margin
     * completes the PPU-clipped half at the live column and the stale list paints
     * the same half again two columns along, and the ball reads as split.
     *
     * The width is measured, not guessed. Over 23820 presented-list entries on
     * the windowed edge replay, widening the row window from exact to +-2 turns
     * 771 entries from "no candidate at all" into unique matches and adds
     * **zero** ambiguous ones; at +-3 ambiguity starts (9 entries). So +-2 is the
     * last width that is free, and it is a whole-pixel bound rather than a
     * tolerance that "seemed reasonable". */
    static uint8_t slot[SDB_MAX_SPRITES][64];
    static uint8_t nslot[SDB_MAX_SPRITES];
    for (int p = 0; p < count; p++) {
        nslot[p] = 0;
        for (unsigned j = 0; j < 64; j++) {
            if (oam[j * 4] >= 0xEF) continue;        /* the game's hidden slot */
            if (list[p].tile != oam[j * 4 + 1] || list[p].attr != oam[j * 4 + 2]) continue;
            int dy = (int)list[p].y - (int)oam[j * 4];
            if (dy > SDB_RECONCILE_ROW || dy < -SDB_RECONCILE_ROW) continue;
            slot[p][nslot[p]++] = (uint8_t)j;
        }
    }
    /* Unique in the LIST as well: two entries chasing one slot is not a match.
     * Tested against the slot itself rather than against the other entry's y,
     * because with a row window two entries a row apart can chase the same slot. */
    for (int p = 0; p < count; p++) {
        if (nslot[p] != 1) continue;
        for (int q = 0; q < count; q++) {
            if (q == p || nslot[q] != 1) continue;
            if (slot[q][0] == slot[p][0]) { nslot[p] = 2; break; }
        }
    }
    for (int p = 0; p < count; p++) {
        buf[p] = list[p];
        if (nslot[p] == 1) buf[p].x = oam[slot[p][0] * 4 + 3];
    }
    return buf;
}

/* Forward declarations for the post-stamp margin pass in sdb_present, below.
 * `sdb_ws_row_x0` is already declared at the top; the rest are the widescreen
 * state that pass reads. */
static void sdb_ws_build_owner(const uint32_t *native);
/* RETIRE THE CARRIED PARTS THE MACHINE HAS TAKEN OVER, and only those.
 *
 * A carried part -- one that a list boundary did not clear, because it is keyed
 * on the game's own sprite index -- is still worth drawing until the machine
 * gets round to it. That is the f=808 case: $4D 0, 2, 4 and 6 stopped being
 * offered the moment they crossed inside the machine's columns, and the
 * machine's list is one build behind, so for a frame or two nobody draws them.
 *
 * "Until the machine gets round to it" is answered by the ownership map, which
 * is the machine's rendered frame against its background and therefore says
 * where it really painted. A carried part with a FREE column inside the native
 * block is one nobody has drawn: keep it. Everything else has been superseded
 * and goes.
 *
 * It runs HERE, in the compositor, before the margin is drawn and while
 * `cyc_render_line_scroll_x` still means the frame being presented. The
 * post-stamp pass runs later and is too late to retire anything.
 *
 * TWO RULES THAT ARE NOT THRESHOLDS, both measured:
 *
 *  - Columns 0-7 are NOT an answer. `sdb_ws_build_owner` marks them free
 *    unconditionally, because PPUMASK clips them and the machine can never paint
 *    there, so testing them calls every band part owed on every frame and a
 *    carried band part never retires. With the clip included, f=817's
 *    left-margin player went 149 -> 244 ink pixels and f=813 picked up 295 of
 *    pure stale copy.
 *  - Retirement rides on the OBJECT's own captures, never on age. The game's
 *    display list does not contain most off-screen objects (research.md 26), so
 *    an age-based rule empties precisely the sprites this feature draws. An
 *    object the game has dropped from the list is never captured, never gets
 *    here with a fresh build, and keeps its parts -- which is what stops the
 *    margin blinking. */
/* RETIRE A CARRIED PART THE GAME HAS SINCE EMITTED INSIDE ITS OWN CULL.
 *
 * `own` is set in sdb_margin_capture the moment the game offers that $4D inside
 * the cull, which is the machine saying "this sprite is mine". The part is then
 * dropped, and dropped for the only reason worth dropping it for: the game took
 * it over. Not because it is old, not because a column threshold said so, and
 * not because the ownership map could not find a mark -- the ownership map is
 * the wrong instrument here (see the note at the capture site), and using it
 * costs a back-and-forth artefact in play.
 *
 * Retirement rides on the OBJECT's own captures, never on age: the game's
 * display list does not contain most off-screen objects (research.md 26), so an
 * age-based rule empties precisely the sprites this feature draws. An object the
 * game has dropped from the list is never captured, never reaches here with a
 * fresh build, and keeps its parts -- which is what stops the margin blinking.
 *
 * It runs in the compositor, before the margin is drawn, which is where it has
 * to be: the post-stamp pass is later and draws from the same slot list. */
/* RETIRE A CARRIED PART ONLY ONCE THE MACHINE HAS ACTUALLY PAINTED IT.
 *
 * `own` is the game's claim -- sdb_margin_capture sets it the moment the game
 * emits that $4D inside its own cull -- and on its own it is one build too
 * eager. Measured at f=808: the game HAD emitted $4D 0, 2, 4 and 6, at x = 250,
 * 254, 253 and 255, all inside [4,255]. So `own` says the machine owns them
 * while its picture has not caught up, and retiring on the claim alone puts the
 * seam straight back -- object 3's head went 12 -> 40 and 812 came back.
 *
 * The claim and the picture are two questions and both have to be answered:
 *
 *   - `own`   : has the GAME taken this part over?  Its own cull says so.
 *   - painted : has the MACHINE's picture caught up?  The ownership map says so.
 *
 * Neither alone is enough. Retiring on `own` alone loses the f=808 seam;
 * retiring on the ownership map alone is what carried the stale row into f=813,
 * because a part one build out of date is evaluated at its own stale row, lands
 * on background the machine left alone, and gets drawn as a second copy of a
 * sprite already on screen. That is the back-and-forth damage reported from play
 * on 2026-10-17 -- panning one way columns go missing, the other way they are
 * drawn twice.
 *
 * The row window is what makes the two agree. A carried part's row is stale by
 * one build, so its columns are looked for across the rows the machine could
 * have used, not only the one the part happens to name. Measured: at f=813 the
 * machine had the sprite one or two rows away and the narrow test read it as
 * free; f=813's player was drawn as a 131 px sliver. The window is +/-4 rows,
 * which is the row leapfrog research.md 26 measured for exactly this pair of
 * emitters, and it is a window on WHERE TO LOOK rather than a tolerance on what
 * to accept.
 *
 * Retirement rides on the OBJECT's own captures, never on age: the game's
 * display list does not contain most off-screen objects (research.md 26), so an
 * age-based rule empties precisely the sprites this feature draws. */
/* IS THIS MARGIN SLOT STILL THE OBJECT WE CAPTURED?
 *
 * One predicate, called by all three draw sites and by the JSON reporter,
 * because they had three different copies of this rule and they had drifted:
 * the reporter carried two extra terms and read six of seven live objects as
 * dead. See the note at the compositor's own loop for what the rule is.
 *
 * THE CULLING BITS ARE MASKED OUT of byte 0 on purpose. Byte 0 is the object's
 * state byte `$43`, and section 2.1's three gates read exactly
 * `($43 & $70) == $20` and `& $20` -- the flicker mod overrides all three
 * precisely so the object keeps being emitted -- so the game flips those bits
 * while the object is still the same object. */
static int sdb_margin_live(const SdbMarginObj *ob, uint16_t rec)
{
    return ((cyc_mod_peek(rec) & ~SDB_CULL_BITS) == (ob->id0 & ~SDB_CULL_BITS))
        && cyc_mod_peek((uint16_t)(rec + 1)) == ob->id1;
}

static void sdb_ws_prune_margin(void)
{
    for (int o = 0; o < SDB_MARGIN_OBJECTS; o++) {
        SdbMarginObj *ob = &s_margin_obj[o];
        if (!ob->n) continue;
        if (ob->build == ob->pruned) continue;
        uint16_t rec = (uint16_t)(SDB_REC_BASE + SDB_REC_STRIDE * o);
        if (!sdb_margin_live(ob, rec)) continue;        /* a recycled slot */
        ob->pruned = ob->build;
        int row_corr = 0;
        int corr = sdb_object_delta(ob, &row_corr);
        int keep = 0;
        for (int i = 0; i < ob->n; i++) {
            const SdbMargin *m = &ob->parts[i];
            /* The game has not claimed it, so it is ours to draw. */
            if (!m->own) { ob->parts[keep++] = *m; continue; }
            /* Claimed AND the machine's picture has it: drop it. The geometry
             * was refreshed on the claiming emission, so this test is asked
             * where the sprite actually is. */
            int prow = m->y + row_corr;
            prow = prow < 0 ? 0 : (prow >= 240 ? 239 : prow);
            int scroll = cyc_render_line_scroll_x(prow);
            int sx = sdb_margin_col(m, scroll) + corr;
            /* RETIRE ONLY IF THE MACHINE OWNS *ALL* OF THE PART.
             *
             * This was "did the machine paint ANY of this part's columns", and
             * that drops the band. Measured at frame 817 of a played session:
             * object 1 holds a part at sx = 4, which spans columns
             * 4-11. Columns 4-7 are inside the clip band and PPUMASK means the
             * machine can never paint them -- the margin is the only renderer
             * there. Columns 8-11 are in the machine's run and it DID paint
             * them. So "any column painted" retired the part and took columns
             * 4-7 with it, and a left-margin character lost two body rows across
             * two frames:
             *
             *   f=816  o1[c4,r200,t86] o1[c5,r208,t8F]
             *   f=817  (gone)
             *
             * The blit already clips, so drawing the band columns of a part the
             * machine also owns costs nothing: it cannot reach a painted column.
             * So the test is whether ANY column of the part is one the machine
             * cannot draw -- inside the band, or inside the block and free. */
            int owed = 0;
            for (int c = 0; c < 8 && !owed; c++) {
                int nx = sx + c;
                /* The band is free unconditionally, and no other renderer will
                 * touch it: that alone keeps the part. */
                if (nx < SDB_WS_CLIP) { owed = 1; continue; }
                /* PAST THE NATIVE BLOCK IS OURS TOO. A part sitting wholly at
                 * 256+ covers columns the machine cannot reach at all -- its
                 * buffer ends at 255 -- so it is the margin's exclusive
                 * territory and nothing can own it. Skipping these columns
                 * instead of claiming them retired the right-margin sprites:
                 * measured at frames 711->712 and 715->716 of a played session,
                 * object 3 lost the parts at c=257 and c=256,
                 * which are exactly the parts covering 256-265, and a bald
                 * character in the right margin lost its pixels on both.
                 *
                 * This is the same correction the right SEAM needs and the
                 * reason it is not a band special case: the rule is "the machine
                 * owns ALL of it", and a column the machine cannot address is
                 * never owned. */
                if (nx > 255) { owed = 1; continue; }
                if (s_ws_native_owner[(size_t)prow * 256 + nx] == 0xFF) owed = 1;
            }
            if (owed) ob->parts[keep++] = *m;
        }
        if (keep != ob->n && SDB_CAP_TRACE)
            fprintf(stderr, "sdbp f=%ld obj %d retires %d of %d parts\n",
                    s_frame, o, ob->n - keep, ob->n);
        ob->n = keep;
    }
}

static int sdb_ws_margin_pass(uint32_t *canvas, int cw)
{
    /* THE MARGIN HAS TO BE DRAWN *AFTER* THE STAMP, not only inside the
     * compositor. This is the finding that three separate fixes ran into.
     *
     * `sdb_present` replaces the native columns with `s_picture` -- the
     * sprite-free background plus the flicker mod's whole captured list -- AFTER
     * `cyc_render_present` has already run the compositor and painted the margin
     * into the same buffer. Everything the compositor drew inside the native 256
     * is therefore discarded, which is correct for its own contract ("the native
     * 256 columns win") and fatal for a margin part that has to reach them.
     *
     * So the seam could not be fixed in the compositor at all, and three
     * correct-looking attempts there were each measured to be a no-op on the
     * artefact they were written for:
     *   - `right_band` moved from `rx0+256` to `rx0+252` (the machine's columns
     *     end at sx 251, and the two differ by the 4-column OAM bias):
     *     0 differing pixels over 890 frames;
     *   - an ownership map built by walking OAM, consulted in the blit: also 0,
     *     because `cyc_render_sprites` takes a `place` callback and the flicker
     *     mod moves sprites, so OAM says where a sprite IS, not where it was
     *     PAINTED;
     *   - the same map rebuilt from the machine's rendered frame: still 0, for
     *     the reason in this paragraph.
     *
     * The shape is the sibling recomp's, from `docs/WIDESCREEN.md`: "Managed
     * enemies are drawn as complete sprites across both old screen edges; their
     * corresponding native OAM slots are suppressed for presentation. This
     * avoids joining half a host sprite to SMB's column-clipped native sprite."
     * Here the native slot cannot be suppressed -- `s_picture` has to keep the
     * machine's own copy, and gate 2 wants the native block to stay the machine's
     * -- so the host sprite is drawn OVER it instead, and only into the columns
     * the machine left as background.
     *
     * Measured on the margin-locked capture, frame 809: the game holds only the
     * character's LEFT halves in OAM (tile 8D at x=246, 86 at 250, 8F at 249, 30
     * at 251) and culls the right ones, so the margin supplies them. The machine
     * clips its own sprites at the 256-wide buffer edge, so band 152 is machine
     * 411-416 then margin 419-426 -- canvas 417,418 painted by nobody -- and the
     * bands either side of it lose 1 and 3 columns the same way. Two to three
     * columns of eight, walking down the body as the parts hand over, which is
     * the reported shimmer.
     */
    if (!s_margin_on) return 0;
    /* Same cleared page as the compositor's margin loop: nothing to complete when
     * the hardware is painting nothing. See the blit site for the measurement. */
    if (sdb_page_clear()) return 0;
    int drawn = 0;
    for (int o = 0; o < SDB_MARGIN_OBJECTS; o++) {
        SdbMarginObj *ob = &s_margin_obj[o];
        if (!ob->n) continue;
        uint16_t rec = (uint16_t)(SDB_REC_BASE + SDB_REC_STRIDE * o);
        if (!sdb_margin_live(ob, rec)) continue;        /* a recycled slot */
        /* The same "the game stopped emitting it" gate as the compositor's loop,
         * and for the same reason: a right-seam completion of a part the guest
         * retired is the same ghost. */
        if (s_frame - ob->seen > SDB_MARGIN_STALE) {
            s_margin_stale++;
            if (SDB_CAP_TRACE)
                fprintf(stderr, "sdbx f=%ld obj %d (seam) stale %ld frames since its "
                                "last emission at %ld; not completed\n",
                        s_frame, o, s_frame - ob->seen, ob->seen);
            continue;
        }
        int row_corr = 0;
        int corr = sdb_object_delta(ob, &row_corr);
        for (int i = 0; i < ob->n; i++) {
            const SdbMargin *m = &ob->parts[i];
            SdbMargin pm = *m;
            pm.y = (int16_t)(m->y + row_corr);
            m = &pm;
            int prow = m->y < 0 ? 0 : (m->y >= 240 ? 239 : m->y);
            int scroll = cyc_render_line_scroll_x(prow);
            int rx0 = sdb_ws_row_x0(prow);
            /* Where this part goes -- see sdb_margin_col. */
            int sx = sdb_margin_col(m, scroll) + corr;
            int px0 = sx + rx0 + sdb_ws_margin_shift(prow, rx0 - scroll);
            /* ONLY parts that STRADDLE THE RIGHT EDGE of the native block, and
             * only the columns inside it. Three restrictions, each earned:
             *
             *  - outside the block: the compositor already painted those columns,
             *    and repainting them buys nothing;
             *  - inside the block but not straddling: the machine painted those
             *    columns whole and the margin has nothing to add. Measured, this
             *    is where the damage was: letting the pass repaint every part
             *    that merely touched the block cost obj 0 its head at frame 817
             *    (ink 149 -> 67 over rows 103-133) by painting the margin's copy
             *    over the machine's at a column the reconcile had shifted, and
             *    `margin_track` went from 9 anomalies to 10;
             *  - the LEFT seam is left entirely alone. Columns 0-7 are the
             *    revealed band, which the compositor owns by construction
             *    (`completes`), and the left straddle has its own clip. Only the
             *    right edge is where the machine's own sprites are cut short by
             *    the 256-wide buffer and the margin is the only renderer.
             */
            if (px0 + 8 <= rx0 + 256 || px0 >= rx0 + 256) continue;
            int wl = rx0 + (SDB_WS_LO - scroll);
            int wh = rx0 + (SDB_WS_HI - scroll);
            if (px0 + 8 <= wl || px0 >= wh) continue;
            sdb_margin_blit(m, px0, canvas, cw, 240, s_ws_native_bg,
                            wl, wh, rx0, s_ws_native_owner);
            drawn++;
        }
    }
    return drawn;
}

static int sdb_ws_margin_pass(uint32_t *canvas, int cw);

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

    /* A cleared page means there is nothing to recover, so present nothing. See
     * sdb_page_clear. Counted per frame for the trace, because a rule that never
     * fires and a rule that is not there look the same from the outside. */
    s_retired_frame = 0;
    if (count && sdb_page_clear()) {
        s_retired_frame = count;
        s_retired_total += count;
        count = 0;
    }
    if (s_trace)
        fprintf(stderr, "sdbq f=%ld n=%d ret=%d hid=%d\n", s_frame,
                s_retired_frame ? s_retired_frame : count, s_retired_frame, s_hidden_slots);

    /* Put every entry that can be matched to a sprite the machine is holding
     * at that sprite's X. See sdb_reconcile for why moving it beats dropping
     * it, and for the measurement behind it. */
    list = sdb_reconcile(list, count);

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

        /* A wide canvas already carries the machine's own sprites: the
         * compositor's contract is that the native 256 columns win, so its
         * output is the machine's picture, sprites included (see the copy-back in
         * sdb_ws_composite). Blitting the whole captured list on top of that
         * therefore paints every sprite twice, and the two copies disagree --
         * measured: the machine places the character 1-2 px left of where the
         * published list says, because that list is a build or more old. Two
         * 8-wide halves two pixels apart read as a split or doubled sprite.
         *
         * So make this path present what the flicker-only path presents. There,
         * s_picture is a sprite-FREE background and the whole captured list is
         * the sprite layer. Here the same is done to the native columns: replace
         * them with that sprite-free redraw, then draw the whole list over it,
         * leaving the compositor's margin columns untouched.
         *
         * Drawing only the entries the machine dropped was tried first and is
         * wrong: live OAM is NOT a prefix of the captured list, because a
         * restarted list carries over the slots below its cursor verbatim
         * (research 26, prefix carry-over), so OAM holds a mix of old and new
         * sprites and a count comparison decides nothing. It also suppressed the
         * truncation the mod exists to fix, measured as bodies missing parts.
         *
         * The price, stated plainly: the presented sprite layer is one build old,
         * exactly as it already is with the flicker mod alone at cw == 256. That
         * is pre-existing, not introduced here. What must not happen is two
         * disagreeing copies of it. */
        /* Copy AROUND the columns the compositor reconstructed. With overscan
         * on, columns [0, SDB_WS_CLIP) are the revealed ones: the machine paints
         * backdrop there (PPUMASK hides them) and the compositor refilled them
         * from the nametable. Stamping all 256 columns with s_picture puts that
         * backdrop back and the black band returns even with overscan on -- the
         * same `first` the compositor uses for its own seam check at
         * sdb_ws_composite, so the two agree by construction rather than by two
         * copies of the same constant.
         *
         * With overscan off there is nothing reconstructed: all 256 columns are
         * the machine's, including the clip the game asked for, so the band is
         * correct and must be kept. */
        {
            /* ZERO, like the compositor's own stamp, and for the same reason:
             * the machine's native 0..7 are black because PPUMASK hides them, and
             * that is the machine's content for those columns. Skipping them here
             * made this a canvas-column skip, which is the same eight columns only
             * while the block is at canvas 0 -- and with the camera locked it is
             * not. It also would have left the flicker mod's own sprite list
             * un-stamped in eight columns it draws into, which is a third consumer
             * of the same wrong number. */
            if (x0 >= 0 && x0 + 256 <= cw && ch >= 240)
                /* PER ROW, not one x0 for the whole picture: with the camera
                 * locked the compositor puts the court band and the held band at
                 * two different canvas columns, and rows 0-47 -- the skyline, the
                 * names and the health bars -- belong to the held one. Stamping
                 * all 240 rows at the court's column is what made the HUD slide
                 * sideways with the camera while the court sat still; measured at
                 * sweep frame 649, where the two columns are 48 apart, the
                 * flicker-on run's rows 0-48 were the flicker-off run's rows 0-48
                 * shifted exactly +48, with the court band aligning at 0.
                 *
                 * In follow mode both columns are the same number, so this loop
                 * is the same single-x0 copy it always was. */
                for (int y = 0; y < 240; y++) {
                    int rx0 = sdb_ws_row_x0(y);
                    if (rx0 < 0 || rx0 + 256 > cw) continue;
                    int first = sdb_ws_band_native(rx0);
                    if (first >= 256) continue;
                    memcpy(s_canvas + (size_t)y * cw + rx0 + first,
                           s_picture + (size_t)y * 256 + first,
                           (size_t)(256 - first) * sizeof(uint32_t));
                }
        }

        /* Per entry, for the same reason as the stamp above: an entry's own row says
         * which band it belongs to, so a sprite in the held rows is drawn at the
         * held column and one in the court rows at the court's. `sdb_blit` takes
         * one x0 per call and a character's parts share a row band, so this is
         * per entry and not per part -- the split that research 26 records as a
         * character drawn at a sixteenth of his width would be a per-part
         * decision, and this is not one.
         *
         * KNOWN WRONG, AND NOT FIXABLE HERE. `sdb_ws_row_x0` wants a row and this
         * passes the sprite's OAM Y, which is the line ABOVE the first one
         * `sdb_blit` paints. A sprite whose OAM Y sits in the held band while its
         * drawn rows are court rows is therefore placed at the held offset, and
         * one object comes apart by exactly the difference between the two
         * offsets. Measured on a court-cam played session, ball pass at frame
         * 612: the ball's top pair has OAM Y = 46 and paints rows 47-54 -- 46 is
         * held, 47-54 are all court -- so it went to 85 and landed at canvas
         * 140..155 while its bottom half, OAM Y 54, went to 169 and landed at
         * 224..239. One ball, two places, 84 columns apart.
         *
         * `y + 1` is more nearly right and measurably fixes nothing: the sprite
         * is two rows into the held band, not one, and any sprite can be a
         * different distance in.
         *
         * Taking the band from the sprite's LAST drawn row instead does fix
         * those 16 frames -- and breaks 16 others that this version gets right,
         * with NO overlap between the two sets (old 611-618 and 647-654; last-row
         * 619-626 and 639-646). So it trades one set of tears for another rather
         * than removing them, and it was reverted on that measurement: 16 of 95
         * frames tear either way.
         *
         * What the disjoint sets mean is that a rule DOES exist that gets both
         * right, and what it needs is the object's other parts: the 611 case wants
         * the court's offset because the ball is mostly in court rows, and the 619
         * case wants the held offset for the same ball because it is mostly in
         * held rows. Neither is decidable from one sprite alone -- it is a
         * per-OBJECT decision, and this list is flat, one entry per hardware
         * sprite. Grouping it is the same identity work research 31 does with the
         * game's own sprite index ($4D).
         *
         * Until then: `camera = locked` only, and only for objects crossing the
         * game's own scroll split at row 48. Ball cam has one offset for every
         * row, so the question never arises and the picture is correct. */
        for (int i = count - 1; i >= 0; i--)
            sdb_blit(&list[i], s_canvas, cw, *height, sdb_ws_sprite_origin());
        const uint8_t *oam = cyc_render_oam();
        SdbSprite front = { oam[0], oam[1], oam[2], oam[3] };
        sdb_blit(&front, s_canvas, cw, *height, sdb_ws_sprite_origin());
        /* LAST, because everything above overwrites the native columns. The
         * margin's own columns outside the block are already correct from the
         * compositor and are left alone. */
        sdb_ws_margin_pass(s_canvas, cw);
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
 *     sdb <frame> <peak-list> <build-list> <overflow> <done-list> <reset> <lost>
 *
 * `peak-list` is the largest number of sprites assembled for one list that
 * frame. A healthy frame is tens (OAM holds 64, and the cursor pin lets the
 * game emit past that). A value that climbs frame after frame is the runaway
 * capture: the game never returns the cursor to $04, so no new list starts and
 * the list being assembled grows without bound. That is what smears a trail of
 * stale sprites across the picture, so it is worth being able to see in a
 * headless run rather than only in the window.
 *
 * `build-list` is the size of the list that will be PRESENTED, and it is NOT
 * reset per frame: the game's build legitimately spans two frames, so the list
 * has to survive the boundary and nothing clears it until a capture detects a new
 * one. The pair belongs on one line because
 *
 *     peak-list == 0 && build-list > 0
 *
 * is a frame that is about to paint a list it captured on an EARLIER frame --
 * which is the shape of both the ghost-of-the-previous-screen bug and the
 * half-built-list bug (research 26). Most such frames are ordinary build
 * cadence, so the count alone proves nothing; it is what tells the two apart,
 * and it is the first number to look at when the presented sprite layer is
 * wrong. Read-only: it changes no presentation decision. */

/* ------------------------------------------------------------------ *
 * 5b. Structured trace: SDB_TRACE_JSON=<from>-<to>
 * ------------------------------------------------------------------ */

/* ONE JSON object per line, two kinds, discriminated by "k".
 *
 * This exists because the human trace is a bad machine interface, and the
 * project has already been bitten by that twice in a row rather than in the
 * abstract:
 *
 *   - the `sdbw` line opened with no newline, so `margin_track.py`'s regex
 *     matched nothing and the audit reported "no sdbw lines in that log" on
 *     exactly the runs where you turned the trace on to run it;
 *   - and the `part` check it ran then under-fired (wholly off-screen objects
 *     only), so an object straddling the band passed clean while the seventh
 *     margin bug was on screen.
 *
 * Both are ordering and parsing faults, and JSON removes the whole class:
 * `json.loads` per line cannot care whether a closer was printed before an
 * entry, and a field that is absent is absent rather than silently wrong.
 *
 * It is a SEPARATE stream, not a reformatting. The four existing lines print
 * at four different points in the frame -- `sdb`/`sdbc` at frame begin, `sdbw`
 * and `oam-band:` at composite, the run totals only at exit -- and folding
 * them into one object would either move a quantity away from the instant it
 * describes or invent an ordering between two frames that do not share one.
 * So: "f" is emitted at frame begin and says what the flicker mod and the
 * machine hold THAT frame; "m" is emitted at composite and says what this
 * presented frame painted. A reader that wants both joins them on "f".
 *
 * Nothing here changes guest state: these are stderr writes behind an env var,
 * on the same path the existing trace already takes. */
static int sdb_json_want(long f)
{
    return s_json_from >= 0 && f >= s_json_from && f <= s_json_to;
}

/* Every live OAM entry, so "does the game emit fewer sprites or does the mod
 * drop them" is one comparison rather than a manual count. This is the exact
 * question plan item 14 is blocked on, and it needs OAM and the mod's list
 * from the SAME frame in the SAME run -- which is why it cannot be answered
 * from a --frame-log dump (headless, and a different guest state; research
 * section 26). */
static void sdb_json_frame(void)
{
    const uint8_t *oam = cyc_render_oam();
    fprintf(stderr, "{\"k\":\"f\",\"f\":%ld,\"scr\":%d,\"cam\":%d,"
                    "\"cur\":\"$%02X\",\"peak\":%d,\"ovf\":%d,\"pub\":%d,"
                    "\"reset\":%d,\"lost\":%d,\"ret\":%d,\"rtot\":%ld,"
                    "\"mcap\":%d,\"moff\":%ld,\"oam\":[",
            s_frame, cyc_render_line_scroll_x(160),
            (int)(cyc_mod_peek(SDB_CAM_LO) | (cyc_mod_peek(SDB_CAM_HI) << 8)),
            cyc_mod_peek(SDB_CURSOR), s_build_peak, s_overflow, s_done_count,
            s_saw_reset, s_list_lost, s_retired_frame, s_retired_total,
            s_margin_captured, s_margin_off_edge);
    int live = 0;
    for (int j = 0; j < 64; j++) {
        if (oam[j * 4] >= 0xEF) continue;          /* the game's hidden slot */
        fprintf(stderr, "%s[%d,%d,%d,%d,%d]", live++ ? "," : "", j, oam[j * 4],
                oam[j * 4 + 1], oam[j * 4 + 2], oam[j * 4 + 3]);
    }
    /* The PUBLISHED list's entries, so the two sprite sources can be compared by
     * POSITION and not only by count. `pub` above is how many; `pl` is which,
     * as [y, tile, attr, x]. This is the pairing that makes plan.md 15 measurable
     * at all: `delta(oamn - pub)` reads +0 while the machine's copy of a sprite
     * sits one camera step from ours, because both are the same COUNT. Emitted
     * at frame begin, so it pairs the list that was published with the OAM of
     * the frame being entered -- which is the comparison the seam actually makes,
     * one side being a build old. */
    fprintf(stderr, "],\"oamn\":%d,\"pl\":[", live);
    for (int i = 0; i < s_done_count; i++)
        fprintf(stderr, "%s[%d,%d,%d,%d]", i ? "," : "", s_done[i].y,
                s_done[i].tile, s_done[i].attr, s_done[i].x);
    fprintf(stderr, "],\"pr\":[");
    {
        const SdbSprite *rec = sdb_reconcile(s_done, s_done_count);
        for (int i = 0; i < s_done_count; i++)
            fprintf(stderr, "%s[%d,%d,%d,%d]", i ? "," : "", rec[i].y,
                    rec[i].tile, rec[i].attr, rec[i].x);
    }
    fprintf(stderr, "]}\n");
}

/* What this presented frame painted, per object.
 *
 * Each part carries the branch the draw site took for it -- "drawn", "machine",
 * "ghost", "clip", "oob" -- so "ghost" (a part in the revealed band with no OAM
 * entry behind it, the shape of research section 26's seventh bug) is a value
 * in the record rather than something a reader has to infer from the absence of
 * a neighbouring line.
 *
 * `live` is the object's identity still matching its display record. A
 * non-live object is emitted with `"stale": <n>` and an EMPTY parts array,
 * because the draw site skips a recycled slot outright -- before any placement
 * decision exists for its parts. Emitting one would invent a branch the
 * compositor never took.
 *
 * `band` is OAM restricted to the eight columns the PPU hides. */
static void sdb_json_margin(int x0, int width, long diff, int n)
{
    const uint8_t *oam = cyc_render_oam();
    fprintf(stderr, "{\"k\":\"m\",\"f\":%ld,\"x0\":%d,\"cw\":%d,\"diff\":%ld,\"n\":%d,\"obj\":[",
            s_frame, x0, width, diff, n);
    int first = 0;
    for (int o = 0; o < SDB_MARGIN_OBJECTS; o++) {
        SdbMarginObj *ob = &s_margin_obj[o];
        if (!ob->n) continue;
        uint16_t rec = (uint16_t)(SDB_REC_BASE + SDB_REC_STRIDE * o);
        /* THE DRAW SITE'S OWN TEST -- `sdb_margin_live`, the same call the three draw
         * sites make. This used to be a fourth copy with two extra terms (an
         * unmasked byte-0 compare and a leading "byte 0 non-zero"), and the
         * record's byte 0 is zero for 216 of the 252 capture events on the
         * bean-ball capture, so the reporter wrote off six objects out of seven
         * while the draw sites went on painting them: over a 141-frame window it
         * could account for 194 parts of the 719 actually drawn. An instrument
         * that hides six objects in seven cannot be used to ask why the other
         * 84% of the parts the margin holds are not drawn. */
        int live = sdb_margin_live(ob, rec);
        fprintf(stderr, "%s\"i\":%d,\"live\":%d,\"parts\":[", first++ ? ",{" : "{", o, live);
        /* A recycled slot is skipped OUTRIGHT by the draw site -- it `continue`s
         * on the object, before any placement decision -- so there is no branch
         * to report for its parts. Emitting one invents a decision the
         * compositor never took: before this guard, dead slots accounted for
         * 54 of 63 "ghost"s in a 61-frame window, which reads as a guard
         * misfiring when it is really a record the draw site never looked at.
         * Count them and leave the array empty instead. */
        if (!live) {
            fprintf(stderr, "],\"stale\":%d}", ob->n);
            continue;
        }
        int rec_x = (int)(cyc_mod_peek((uint16_t)(rec + 3))
                        | (cyc_mod_peek((uint16_t)(rec + 4)) << 8));
        /* Once per object, as at the draw site: the correction is a property of the
         * object, and recomputing it per part is a parts-squared OAM scan. */
        int row_corr = 0;
        int corr = sdb_object_delta(ob, &row_corr);
        int placed = 0;
        /* Mirrors the draw site's decision EXACTLY, in the same order, and says
         * which branch each part took. Reporting a 0/1 "drawn" here instead
         * would be a lie for every part that is not in the revealed band:
         * `sdb_oam_has` is only consulted there, so using it as a general test
         * answers a question the compositor never asked. And since the band
         * filter was removed (research 26) the compositor does not consult it at
         * all, so neither does this. The outcomes a reader actually wants are:
         * painted by us, left to the machine, clipped into columns we do not own,
         * or off the canvas. */
        int keep = s_ws_reveal ? SDB_WS_CLIP : 0;
        (void)keep;
        for (int i = 0; i < ob->n; i++) {
            const SdbMargin *m = &ob->parts[i];
            int my = m->y + row_corr;
            int prow = my < 0 ? 0 : (my >= 240 ? 239 : my);
            int scroll = cyc_render_line_scroll_x(prow);
            /* Where this part goes -- see sdb_margin_col. The reporter mirrors
             * the draw site's decision EXACTLY, so it has to place the part the
             * same way. */
            int sx = sdb_margin_col(m, scroll) + corr;
            int px0 = sx + x0;
            const char *st;
            /* Mirrors the draw site as it now stands, which asks OWNERSHIP rather
             * than the block's geometry. Reporting the old geometric outcome here
             * would be worse than not reporting: the whole reason this stream
             * exists is that it says which branch each part took, and the two have
             * genuinely diverged. See sdb_ws_build_owner and sdb_ws_margin_pass.
             *
             * The three outcomes the draw site can produce:
             *   oob      -- the part is off the canvas or off the canvas's world
             *   machine  -- a native sprite owns at least one of its columns
             *   drawn    -- ours, including any column inside the native block that
             *               the machine left as background (the seam fix)
             */
            if (px0 + 8 <= 0 || px0 >= width) {
                st = "oob";
            } else {
                int owned = 0;
                for (int c = 0; c < 8 && !owned; ++c) {
                    int nx = sx + c;
                    if (nx < 0 || nx > 255) continue;
                    if (s_ws_native_owner[(size_t)my * 256 + nx] != 0xFF) owned = 1;
                }
                int wl = x0 + (SDB_WS_LO - scroll);
                int wh = x0 + (SDB_WS_HI - scroll);
                if (px0 + 8 <= wl || px0 >= wh)   st = "clip";
                else if (owned)                 st = "machine";
                else                            st = "drawn";
            }
            placed += st[0] == 'd';
            fprintf(stderr, "%s[%d,%d,%d,\"%s\"]", i ? "," : "", sx, my, m->tile, st);
        }
        fprintf(stderr, "],\"drawn\":%d}", placed);
    }
    fprintf(stderr, "],\"band\":[");
    first = 0;
    for (int j = 0; j < 64; j++) {
        if (oam[j * 4] >= 0xEF || oam[j * 4 + 3] >= SDB_WS_CLIP) continue;
        fprintf(stderr, "%s[%d,%d,%d,%d,%d]", first++ ? "," : "", j, oam[j * 4],
                oam[j * 4 + 1], oam[j * 4 + 2], oam[j * 4 + 3]);
    }
    fprintf(stderr, "]}\n");
}

static void sdb_frame_begin(void *ctx)
{
    (void)ctx;
    if (sdb_json_want(s_frame))
        sdb_json_frame();
    if (s_trace) {
        fprintf(stderr, "sdb %ld %d %d %d %d %d %d\n", s_frame, s_build_peak,
                    s_build_count, s_overflow, s_done_count, s_saw_reset,
                    s_list_lost);
        /* The two candidates for "where is this frame's column zero", printed
         * from the frame-begin hook so it does not depend on the margin sprites
         * being on. `cam` is `$C3/$C4`, the camera the game places sprites with;
         * `scr` is `cyc_render_line_scroll_x`, what the PPU latched. They should
         * agree, and the gap between them is the machine's own sprite/bg
         * misalignment for this frame -- which is the visual judder when it is
         * not zero. See the draw site's note on which one the margin uses.
         *
         * `sxs` samples the same call at rows 0, 46 and 47, and `band` is the
         * first and last row scrolling at row 160's value. Mid-match this reads
         * `sxs=0,0,0 band=48-239`: the game holds its skyline/scoreboard band
         * still with a real mid-frame scroll split at row 48, and the engine
         * reports it per row. (An earlier ruling in research 27 said there was
         * no split, sampled at rows 60-239 -- all inside the lower band.) Never
         * hardcode 48; the band is measured so it survives a different course. */
        if (s_ws_aspect != NES_VIDEO_STOCK || s_trace) {
            int court = cyc_render_line_scroll_x(160), lo = -1, hi = -1;
            for (int y = 0; y < 240; y++)
                if (cyc_render_line_scroll_x(y) == court) { if (lo < 0) lo = y; hi = y; }
            fprintf(stderr, "sdbc %ld scr=%d cam=%d sxs=%d,%d,%d band=%d-%d\n",
                    s_frame, court,
                    (int)(cyc_mod_peek(SDB_CAM_LO) | (cyc_mod_peek(SDB_CAM_HI) << 8)),
                    cyc_render_line_scroll_x(0), cyc_render_line_scroll_x(46),
                    cyc_render_line_scroll_x(47), lo, hi);
        }
    }
    s_frame++;
    s_cap_run = s_captured ? s_cap_run + 1 : 0;
    s_captured = 0;
    s_overflow = 0;
    s_build_peak = 0;
    s_saw_reset = 0;
    s_margin_run = s_margin_captured ? s_margin_run + 1 : 0;
    s_margin_captured = 0;
    s_margin_frames++;
}

/* Non-const: menu_items / menu_item_count / menu_callbacks depend on the cheat
 * file, which is not read until the machine is powered on, and a static
 * initialiser cannot call a function. sdb_power_on fills those three, which is
 * why this is a tentative definition with the real one at the bottom of the
 * file, next to the callbacks it names.
 *
 * recomp-ui derives its section list from the `section` string on each item, so
 * a "Cheats" section appears with no engine and no recomp-ui change -- and a
 * game that declares no items gets no section at all, which is why this is safe
 * to be generic about. */
static CycHostExtras SDB_EXTRAS;

static void sdb_power_on(void *ctx)
{
    (void)ctx;
    cyc_render_capture_background(true);
    /* Re-apply the requested CPU-budget enhancement: cyc_power_on() cleared
     * hw_extra_timing after the mod activated. 0 is the stock default, so with
     * the feature off this changes nothing. It is also where a value the launcher
     * could not apply BEFORE the cart existed actually lands -- see
     * sdb_oc_apply -- so this reports instead of assuming. */
    if (s_oc_lines) {
        if (cyc_set_extra_scanlines((unsigned)s_oc_lines))
            fprintf(stderr, "ppu-overclock: %d extra scanlines before NMI (applied)\n",
                    s_oc_lines);
        else
            fprintf(stderr, "ppu-overclock: still refused %d extra scanlines after "
                            "power-on; running at stock timing\n", s_oc_lines);
    }
    s_build_count = 0;
    s_done_count = 0;
    s_overflow = 0;
    s_build_peak = 0;
    s_saw_reset = 0;
    s_prev_cursor = 0;
    s_list_lost = 0;
    s_retired_frame = 0;
    s_retired_total = 0;
    s_cap_total = 0;
    s_done_frame = -100;
    s_frame = 0;
    const char *t = getenv("SDB_TRACE");
    s_trace = t && t[0] == '1';
    sdb_cheats_set_trace(s_trace);
    const char *c = getenv("SDB_TRACE_CAPTURES");
    if (c && c[0]) {
        long a = 0, b = 0;
        if (sscanf(c, "%ld-%ld", &a, &b) == 2) { s_cap_from = a; s_cap_to = b; }
    }
    const char *j = getenv("SDB_TRACE_JSON");
    if (j && j[0]) {
        long a = 0, b = 0;
        if (sscanf(j, "%ld-%ld", &a, &b) == 2) { s_json_from = a; s_json_to = b; }
        else fprintf(stderr, "sdb: SDB_TRACE_JSON wants <from>-<to>, got %s\n", j);
    }
    /* Re-arm the ROM read substitutions: a reset restored the image, and a
     * Game Genie code's compare is only meaningful against what is mapped now.
     * Then hand the rows to the menu, which is the only reason they exist. */
    sdb_cheats_power_on();
    {
        size_t n = 0;
        SDB_EXTRAS.menu_items = sdb_cheats_menu_items(&n);
        SDB_EXTRAS.menu_item_count = n;
        SDB_EXTRAS.menu_callbacks = sdb_cheats_menu_callbacks();
    }
}

/* A frame finished, whether or not it was presented - the host's documented
 * place for per-frame work. This is where the freezes land. */
static void sdb_frame_end(void *ctx)
{
    (void)ctx;
    sdb_cheats_frame_end();
}

/* ---- mod activation ---- */

/* Cheats are no longer a package. The .cht loads at power-on on its own, each
 * entry is its own toggle in the in-game Cheats menu, and --cheat / --cheats /
 * --no-cheats cover the headless path, so there is nothing left for a manifest
 * feature to gate. The package was removed for exactly that reason: a feature
 * switch in front of per-cheat switches carries no information. */

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
    "super-dodgeball.margin.b5-8483",
    "super-dodgeball.margin.b5-8302",
    "super-dodgeball.margin.b5-8231",
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

/* ------------------------------------------------------------------ *
 * 6. Input recording (--record-input): play it, and the run replays
 * ------------------------------------------------------------------ */

/* Every repro vector in tas/vectors/ was written by hand, and every one of
 * them was wrong until it was measured -- the game silently drops input aimed
 * before frame 594, and a margin artifact needs the camera to pan all the way
 * before it shows anything. Neither is expressible as a schedule someone
 * guessed. So this records one instead.
 *
 * The host hands over the final controller bytes through
 * CycHostExtras::input -- "after host mapping and before latching", i.e.
 * exactly what the guest read this frame. Both loops call it in the same
 * place, immediately before cyc_session_frame_begin():
 *
 *     windowed   cyc_sdl.c:1470   cyc_session_input(buttons)
 *                cyc_sdl.c:1471   cyc_session_frame_begin()
 *                cyc_sdl.c:1472   cyc_run_frame()
 *     headless   cyc_host.c:1602  input_tick(frame) -> cyc_session_input(buttons)
 *                cyc_host.c:1612  cyc_session_frame_begin()
 *                cyc_host.c:1613  cyc_run_frame()
 *
 * so `s_frame` here is the index of the frame about to run -- which is exactly
 * what input_tick() matches steps against (`step.frame <= frame`). That is why
 * this is not a new format: it writes precisely the `--input` schedule the host
 * already parses, one "<frame> <BUTTONS>" line per state change, absolute
 * state, "-" for release.
 *
 * The window samples the controller once per guest frame, and skips the sample
 * and cyc_run_frame() together when the menu is open or the window unfocused,
 * so the file cannot drift against the frame counter. What it cannot do is fix
 * the RNG: this game is random, so a replay is the same run only under
 * `--ram-init zeros`. The header says so, because a vector that silently does
 * not reproduce is worse than no vector at all. */
static FILE *s_rec;
static uint8_t s_rec_held[2];
static int s_rec_steps;
static char s_rec_path[512];

/* The host's own button names and bits (cyc_host.c parse_buttons), in the order
 * it lists them, so a recorded line reads like a hand-written one and re-parses
 * to the same mask. */
static const char *const SDB_REC_NAMES[8] = {
    "A", "B", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT"
};

static void sdb_rec_write(uint8_t b, int port)
{
    char names[64];
    size_t k = 0;
    names[0] = '\0';
    for (int i = 0; i < 8; i++) {
        if (!(b & (uint8_t)(0x80u >> i))) continue;
        if (k) names[k++] = '+';
        k += (size_t)snprintf(names + k, sizeof names - k, "%s", SDB_REC_NAMES[i]);
    }
    /* Port 1 needs no prefix -- that is the format the committed vectors use.
     * Port 2 uses the host's own "N:" prefix. */
    fprintf(s_rec, "%ld %s%s\n", s_frame, port ? "2:" : "", k ? names : "-");
    fflush(s_rec);
    s_rec_steps++;
}

static void sdb_input(void *ctx, uint8_t buttons[2])
{
    (void)ctx;
    if (!s_rec) return;
    for (int p = 0; p < 2; p++) {
        if (buttons[p] == s_rec_held[p]) continue;   /* only state changes */
        s_rec_held[p] = buttons[p];
        sdb_rec_write(buttons[p], p);
    }
}

/* Closed from the destructor, which is the only place that runs on a window
 * close as well as on --exit-after. The frame count is only known here, and a
 * replay needs it for --frames: getting it wrong silently replays too little
 * or too much of the run, which is the sort of thing that reads as "the
 * artifact did not reproduce". */
static void sdb_rec_close(void)
{
    if (!s_rec) return;
    fprintf(s_rec, "# %ld frames, %d input steps\n", s_frame, s_rec_steps);
    fclose(s_rec);
    s_rec = NULL;
    fprintf(stderr, "sdb: recorded %ld frames, %d input steps -> %s\n",
            s_frame, s_rec_steps, s_rec_path);
}

static bool sdb_rec_start(const char *path)
{
    snprintf(s_rec_path, sizeof s_rec_path, "%s", path);
    s_rec = fopen(path, "w");
    if (!s_rec) {
        fprintf(stderr, "--record-input: cannot write %s\n", path);
        return false;
    }
    s_rec_held[0] = s_rec_held[1] = 0;
    s_rec_steps = 0;
    fprintf(s_rec,
        "# Super Dodge Ball (USA) - recorded playthrough.\n"
        "#\n"
        "# Written by `nes_game --record-input` from a windowed session. One\n"
        "# \"<frame> <BUTTONS>\" line per state change, absolute state, \"-\" is\n"
        "# release -- the same schedule format `--input` replays, where frame 0\n"
        "# is the first post-power frame. It records exactly the bytes the guest\n"
        "# read each frame, so the replay takes the same path.\n"
        "#\n"
        "# REPLAY IT (the last line of this file has the frame count for\n"
        "# --frames; --ram-init zeros is REQUIRED, not a default):\n"
        "#\n"
        "#     ./build-cycle/nes_game \"roms/Super Dodge Ball (USA).nes\" \\\n"
        "#         --frames <N> --input %s --ram-init zeros\n"
        "#\n"
        "# The game is random, so a replay without --ram-init zeros diverges and\n"
        "# this vector will not reproduce.\n"
        "#\n"
        "# MEASURED 2026-10-13: a headless replay of such a vector is NOT faithful\n"
        "# to the windowed recording it came from. Two windowed runs of one\n"
        "# schedule are byte-identical to each other, but a headless replay of the\n"
        "# recorded vector diverges from both at frame 4 and ends up on different\n"
        "# game state (2666 vs 2121 off-edge captures, 32200 vs 15504 margin\n"
        "# sprites drawn). It is not the input -- the recorded frames are the frames\n"
        "# the schedule fired on, and the divergence starts four frames before the\n"
        "# first press -- so the window and the headless host are simply not running\n"
        "# the same machine yet. That is an open engine question.\n"
        "#\n"
        "# Until it is settled, analyse the recording where it was made: its SDB_TRACE\n"
        "# log is the ground truth for what was on screen.\n", path);
    return true;
}

static const CycHostOption SDB_OPTIONS[] = {
    { "--remove-character-flicker", false, "draw every OAM sprite host-side, so characters are never truncated" },
    { "--ppu-overclock", true, "extra blank scanlines before NMI (0-262); 0 = stock timing" },
    { "--widescreen", true, "present 16:9 / 21:9 / 32:9 / fit instead of stock 4:3" },
    { "--camera", true, "follow = the view pans with the game; locked = the whole court is shown at once" },
    { "--overscan", false, "drop the blank scanlines a CRT hid at the top and bottom" },
    { "--margin-sprites", true, "background-only widescreen margins: do NOT draw the players the game culled off-screen (default is to draw them)" },
    { "--record-input", true, "record the live controller to an --input vector as you play, for replaying headless afterwards" },
    { "--cheat", true, "arm a libretro cheat code now, e.g. --cheat 058B:FF; repeat, or join several with ';'" },
    { "--cheats", true, "load a RetroArch .cht cheat file (supersedes any --cheat already given)" },
    { "--no-cheats", false, "drop every loaded cheat and restore stock RAM" },
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
         * are used by one wipe transition (frame 420), so neither may be cut.
         *
         * SDB_WS_CLIP and s_ws_reveal are declared next to the margin-capture
         * state near the top of this file, because sdb_margin_capture needs to
         * know whether the clip is being revealed: it decides whether the PPU's
         * hidden leftmost 8 columns are columns this feature may paint. */
static int s_ws_fixed;           /* width we asked for by hand, 0 = by mode */
static int s_ws_margin;          /* draw sprites the game culled off-screen */
static int s_ws_margin_only;    /* headless A/B: suppress the above */
static long s_ws_composed, s_ws_pillarboxed, s_ws_last_diff;
static int s_ws_sx_lo = 9999, s_ws_sx_hi = -1;   /* camera scroll seen, SDB_TRACE=3 */

/* The world's own bounds, in world pixels -- and they are the GAME'S, not ours.
 *
 * The margin may show world columns the machine's own 256 do not, but only the
 * ones the game can ever display. That limit is the camera window, and the game
 * states it in code rather than in data:
 *
 *     $E5D8: LDA $61 / CMP #$28 / BCC $E5E8      <- moving left
 *     $E5E8: LDA #$28 / STA $61 / LDA #$00 / STA $62      40  = $28
 *     $E5FF: JMP $E6AB
 *     $E644: LDA $61 / CMP #$E4 / BCC $E652      <- moving right
 *     $E64A: LDA #$D8 / STA $61 / ...                      216 = $D8
 *     $E661: JMP $E6AB
 *     $E6AB: LDA #$00 / STA $C2
 *     $E6AF: LDA #$28 / CMP $61 / BCC $E6B9 / STA $61     lower clamp, 40
 *     $E6B9: LDA #$E0 / CMP $61 / BCS $E6C1 / STA $61     upper clamp, 224
 *     $E6C1: LDA $61 / STA $C3 / LDA $62 / STA $C4         -> the camera
 *
 * `$C3/$C4` is the camera the rest of the game uses -- `$823E/$8246: SEC /
 * SBC $C3 / SBC $C4` is how OAM X is made, and the SDB trace's `cam=` column
 * reads exactly these two bytes. So the window is [$28, $E0] = [40, 224] and
 * the world the machine can display is the camera plus the viewport:
 *
 *     [ 40, 224 + 256 ) = [40, 480)
 *
 * WHICH IS WHY THERE IS NO PER-LEVEL DETECTION. The bounds are two immediate
 * operands; there is no table, no index and no per-course variant, and both
 * ends of the measured evidence agree with the code rather than being fitted to
 * it: on two completely different courses -- the stadium and the London one --
 * the camera stays inside [40, 224] (resting at 40, observed maximum 216,
 * because the target path caps at $D8 before $E6AB's 224 can bite). On the
 * stadium the court's own drawn extent is [32, 480), so the game's window sits
 * 8 columns inside its art at one end and flush at the other, which is what a
 * hand-tuned clamp looks like.
 *
 * An earlier version of this measured the nametable instead: "how many tiles
 * in from the world's edge repeat that edge column", capped at 32 px. It gave
 * the right answer on the stadium by luck and the wrong one everywhere else,
 * because the end wall's art is not a fact about the game's limits. A level
 * whose edge column varied immediately fell to a floor of one tile and got NO
 * bound at all, which is exactly how the London course kept drawing past its
 * own edge. The camera is the signal; the nametable never was.
 *
 * The one wrinkle, measured: `$DBBB` (called once, from $F3A0) initialises the
 * camera to **0**, and the London course sits there for 204 frames at level
 * start -- during which the machine really does display world columns 0..39.
 * The clamp only governs movement after that. So for the first few seconds of
 * that level the margin is bounded 40 columns tighter than the native picture
 * beside it, which is a cosmetic cost for a locked intro camera and is the
 * right way round: everything outside the window is content the game chose not
 * to show, and the alternative is inventing it. */

/* Is this world column one the game can ever display? */
static int sdb_ws_in_bounds(int wx) { return wx >= SDB_WS_LO && wx < SDB_WS_HI; }

/* ---- the camera: follow the game, or anchor the whole court ---- */

/* THE COURT IS 440 COLUMNS AND THE CAMERA PANNS INSIDE 184 OF THEM.
 *
 * The clamp bounds are the whole of what this is built on, and they make the
 * question answerable without looking at the art: the court is
 * [SDB_WS_LO, SDB_WS_HI) = [40, 480) -- 440 columns -- and the camera lives in
 * [40, 224], so the game's own viewport slides 184 columns of a court that is
 * 440 wide.
 *
 * Measured over the sweep vector's match (frames 422-760), the camera actually
 * spans 102..180, inside [72, 192]. So at 21:9 and 32:9 -- 560 and 854 columns
 * -- the ENTIRE COURT IS ALREADY ON SCREEN at every camera position play
 * reaches, today, with no mod at all:
 *
 *     canvas x = native_x0 + (camera + n - native_x0) = camera + n, so the
 *     canvas shows world columns [camera - pad, camera + 256 + pad) with
 *     pad = (width - 256) / 2.
 *
 *     854: pad 299, camera 40..224 -> [.., ..) always covers [40, 480).
 *     560: pad 152, camera 102..180 -> covers [40, 480) for camera >= 72.
 *     426: pad 85 -- cannot cover 440 columns at all, and which END is missing
 *          depends on where the ball is.
 *
 * Which is the whole argument for anchoring, and it is worth stating plainly
 * because it is NOT "show more": the panning buys the player nothing at the two
 * wide aspects. Every column of the court is already visible; the slide is
 * motion with no information in it, and it competes with the one thing being
 * tracked (the ball) for the eye. Anchoring turns the screen into a fixed field
 * with the ball crossing it -- what the genre moved to for exactly this reason.
 *
 * The canvas is therefore a FIXED WINDOW on the court, chosen from the width
 * alone, and the game pans behind it. Only the degenerate case moves it: when
 * the machine's own 256 columns would otherwise hang off the canvas, which is a
 * scene-change wipe (frames 411-420 of the sweep have a scroll all over the
 * place) and nothing else.
 *
 * `s_ws_court` is the MODAL line scroll, not row 160's: the court is the band
 * that most of the screen scrolls with, and during a wipe there is no such band
 * -- the mode becomes one row's value, every row falls through to the held
 * offset below, and the frame renders exactly as it does today. Selecting the
 * band by counting rows is what makes that fallback free. A frame whose camera
 * is outside the game's own window is not a court frame at all -- menus are the
 * obvious case -- and is left centred; see the test below.
 *
 * The anchor is a PRESENTATION offset only. The guest still pans: `$C3/$C4` is
 * untouched, OAM is untouched, and the machine's own 256 columns are still the
 * machine's own picture, byte for byte -- they are simply placed somewhere else
 * on the canvas. Which is the use `cyc_render_set_native_origin` exists for
 * ("a compositor that placed the native picture somewhere other than the
 * native_x0 it was given ... a camera anchored at an area's edge"), so the
 * flicker mod's wide path follows the same number automatically. */
/* (s_ws_lock, s_ws_court, s_ws_x0_court, s_ws_x0_held, s_ws_anchor,
 * s_ws_anchored and s_ws_framed are declared next to s_ws_reveal, near the top
 * of this file, because sdb_present reads them too.) */

static void sdb_ws_anchor_frame(int width, int native_x0)
{
    int votes[512];
    memset(votes, 0, sizeof votes);
    for (int y = 0; y < 240; y++) votes[cyc_render_line_scroll_x(y) & 511]++;
    int best = -1, n = 0;
    for (int v = 0; v < 512; v++)
        if (votes[v] > n) { n = votes[v]; best = v; }
    s_ws_court = best < 0 ? cyc_render_line_scroll_x(160) : best;
    s_ws_here = 0;

    if (!s_ws_lock) {
        /* Follow. Identical to before in every term: one offset for every row,
         * which is what the compositor used unconditionally. */
        s_ws_x0_court = s_ws_x0_held = native_x0;
        s_ws_anchor = s_ws_court - native_x0;
        return;
    }

    /* IS THIS EVEN A COURT FRAME? A title screen or a menu is not world content:
     * it is drawn at screen-space columns and its scroll is 0, because nothing
     * has told the PPU to scroll anything. The court's window does not describe
     * it, and anchoring one put every menu 132 columns left of centre at 21:9
     * (measured: anchor f=12..140 all read `scroll 0, x0 20` where the centred
     * position is 152).
     *
     * The test is the game's OWN camera window -- the clamp at $A6AB, the same
     * [40, 224] the bound is built from -- plus slack, and the slack is not a
     * fudge. Read the clamp:
     *
     *     A6AB: A9 00  LDA #$00    / STA $C2
     *     A6AF: A9 28  LDA #$28    / CMP $61   / BCC $A6B9   ; $61 < 40 -> skip
     *     A6B5:        STA $61                     ; else $61 = 40
     *     A6B9: A9 E0  LDA #$E0    / CMP $61   / BCS $A6C1   ; $61 >= 224 -> skip
     *     A6BF:        STA $61                     ; else $61 = 224
     *     A6C1: A5 61  LDA $61     / STA $C3
     *
     * It clamps the TARGET, and it is one-sided: `BCC` skips the store when
     * `$61` is already below 40, so `$C3` receives 32 verbatim. **The camera can
     * and does go left of 40** -- measured on a court-cam played session, the
     * game asks for `$C3 = 32` and holds it for 32 frames (f621..f652) while a
     * ball is thrown left, which is exactly when the player notices the whole
     * picture jump.
     *
     * So [40, 224] is where the camera RESTS, not where it can reach, and
     * testing `scroll` against it with no slack stood the anchor down for those
     * 32 frames. Standing the anchor down is not a small thing: it re-centres
     * the picture, so declining and re-engaging moves the entire canvas, and
     * that -- not the camera's own 8-column step -- was the 93-column jump.
     * Measured, 16:9, same session: `f623 anchor 40 -> -53` and
     * `f655 anchor -53 -> 40`, 93 columns each way.
     *
     * The slack has to cover the reachable range and nothing more, because the
     * test's real job is still to catch title screens, menus and the $DBBB intro
     * camera -- all of which sit at scroll 0, far outside it. 32 is the furthest
     * the camera has been observed to go left; the right-hand end is the mirror
     * of the clamp's own limit. Menus then still render exactly as they do in
     * follow mode, which is to say centred. */
    if (s_ws_court < SDB_WS_CAM_MIN - SDB_WS_CAM_SLACK ||
        s_ws_court > SDB_WS_CAM_MAX) {
        s_ws_x0_court = s_ws_x0_held = native_x0;
        s_ws_anchor = s_ws_court - native_x0;
        s_ws_framed++;
        return;
    }

    /* THE ANCHOR IS A FUNCTION OF THE WIDTH AND OF NOTHING ELSE. That is the
     * whole contract of this mode: `s` is the world column the canvas's column 0
     * shows, so if it moves, the entire picture moves, and everything this mode
     * exists to do is undone in one frame.
     *
     * It was not. Both clamps below used `s_ws_court`, which is the game's
     * camera, so the anchor inherited every step the camera took whenever the
     * clamp fired -- and the left clamp fires for the entire time the camera is
     * left of centre. Measured on a court-cam played session, 16:9: four
     * distinct anchor values over 681 frames where there must be one, and the
     * view jerking on four of them --
     *
     *     f=591  scroll=45   anchor  47 ->  45    -2 columns
     *     f=595  scroll=40   anchor  45 ->  40    -5
     *     f=623  scroll=32   anchor  40 -> -53   -93   <- the one that reads as
     *     f=655  scroll=40   anchor -53 ->  40   +93      "the camera jumped"
     *
     * The mechanism is one line. With the left clamp engaged, `s == s_ws_court`,
     * and then `s_ws_x0_court = s_ws_court - s` is identically ZERO: the native
     * block is pinned to canvas column 0 whatever the camera does. The camera
     * moving 40 -> 32 moves the block's world content by 8 columns while the
     * block itself stays put, so the world slides under a fixed block. On the
     * way back it slides the other way. The step is the camera's, the 93-column
     * jump is the clamp letting go, and both are the anchor's fault rather than
     * the game's.
     *
     * WHY IT WAS WRITTEN THAT WAY, because the reasoning is not wrong, only the
     * scope is. Both clamps are about keeping the machine's own 256 columns ON
     * THE CANVAS: `s > court` would put native column 0 at a negative canvas
     * column, and the other would run the block off the right edge. That is a
     * real constraint, at the camera's extremes -- but it is a constraint on
     * WHERE THE BLOCK MAY SIT, not on which world column the canvas shows, and
     * satisfying it per frame by moving the anchor destroys the mode. It has to
     * be satisfied by choosing an anchor that is valid across the WHOLE camera
     * range, once.
     *
     * So: pick the anchor from the width, clamp it ONCE against the camera's
     * full range rather than against this frame's camera. Note that the right
     * end of the court's own test is still the nominal `CAM_MAX` and NOT
     * `CAM_HI_SEEN`: the game has never been observed past 216, but a target
     * above `CAM_MAX` is a frame the clamp itself would have rewritten, so
     * standing down there costs nothing and keeps the test honest about what it
     * is -- "the camera is somewhere the clamp could have sent it". The left end
     * is the opposite, because the clamp does NOT rewrite a low target.
     *
     * `want` is the ideal: centre the court's 440 columns on the canvas. It can
     * be negative -- at 16:9 the court is wider than the canvas, so the view
     * starts inside the court's left edge -- and that is fine and intended; a
     * negative anchor means canvas column 0 shows world column `|want|`.
     *
     * The two clamps below then ask the only question left, which is whether a
     * single anchor can keep the block on canvas for EVERY camera:
     *
     *   s_ws_x0_court = s_ws_court - s   must be in [0, width-256]
     *
     * which for `s` over the camera's measured range means
     *
     *   s >= SDB_WS_CAM_HI_SEEN - (width - 256)   (the most negative x0_court)
     *   s <= SDB_WS_CAM_LO_SEEN                   (the most positive one)
     *
     * At 16:9 (width 426) that is s in [46, 32] -- EMPTY, so no single anchor
     * exists and something has to give. The court's 440 columns cannot fit in a
     * 426-wide canvas at every camera position, which is a fact about the level
     * and not about this code. What matters is that the SAME anchor is used at
     * every camera, so the choice is made once and the picture is still, and
     * that whichever end is given up is given up CONSISTENTLY -- which is what
     * the clamp order below decides, and getting it wrong costs the seam check.
     * See the note there.
     *
     * `anchor_gate.py` asserts exactly this -- one value over the frames the
     * anchor engaged on -- and it is why this is a gate failure rather than a
     * preference. It had been passing on the sweep vector, whose camera spends
     * most of its time inside the unclamped middle; only a played session put
     * the camera hard against the left clamp, which is the entire left half of
     * `sdb_ws_sweep`'s pan and was sitting there unnoticed. */
    int s = (SDB_WS_LO + SDB_WS_HI - width) / 2;
    /* Then the two clamps, and they are NOT symmetric and cannot be: the block
     * has to be ON the canvas, all 256 of its columns, because three consumers
     * assume it and none of them can be taught otherwise in this pass --
     *
     *     the seam check        out + y*width + row_x0 + x, x in 0..255, and a
     *                          negative row_x0 indexes before the row
     *     the stamp             memcpy of the whole 256-wide row there
     *     the owner map         sdb_ws_build_owner indexes native columns 0..255
     *
     * With `s` out of range the seam check reads out of bounds and the frame is
     * pillarboxed: measured 1192 px of seam diff and 33 pillarboxed frames over
     * 670, from picking the nearer end of an empty interval. That is worse than
     * anything it fixed, and the mistake was treating "keep the block on canvas"
     * as symmetric when the block can only be off one end at a time.
     *
     * So: s_lo first (block's right edge on canvas), then s_hi (left edge). On an
     * empty interval -- 16:9, where the court is 440 wide and the canvas 426 --
     * the second wins and `s` ends at s_hi, which keeps the block's LEFT edge on
     * the canvas at every camera and lets it run off the right at the far end of
     * the pan. The compositor already has the answer for columns outside the
     * canvas: the margin is painted from the nametable and bounded by
     * `sdb_ws_in_bounds`. What is not acceptable, and is all this fixes, is the
     * anchor moving.
     *
     *   width 426: ideal 47, interval [54,32] empty -> s = 32, x0_court 0..184
     *     over the measured camera range 32..224 (canvas room 0..170, so the last
     *     14 columns of the block leave the right edge at the far end and are
     *     margin-filled)
     *   width 560: ideal -20, interval [-80,32]     -> -20, clean, 52..244
     *   width 854: ideal -167, interval [-374,32]   -> -167, clean, 199..391
     *
     * Measured after both fixes on that session: ONE anchor value per aspect, no
     * anchor changes, seam diff back to 0, and none of the four jerks. */
    int s_lo = SDB_WS_CAM_HI_SEEN - (width - 256);
    int s_hi = SDB_WS_CAM_LO_SEEN;
    if (s < s_lo) s = s_lo;
    if (s > s_hi) s = s_hi;
    s_ws_anchor = s;
    s_ws_x0_court = s_ws_court - s;
    s_ws_x0_held = native_x0;
    s_ws_here = 1;
}

/* Where native column 0 sits, for one screen row.
 *
 * THE HELD BAND IS WHY THIS IS PER ROW. This game splits its scroll mid-frame:
 * rows 0-47 hold scroll 0 and carry the skyline and the scoreboard, rows 48-239
 * scroll with the camera. Measured on the sweep vector mid-pan:
 *
 *     sdbc 601 scr=134 cam=134 sxs=0,0,0 band=48-239
 *
 * (`sxs` samples rows 0, 46 and 47.) So anchoring the whole canvas on the
 * court would slide the HUD 6-7 px every time the camera stepped -- a new
 * artefact, introduced by the thing meant to remove one. The held rows are not
 * world content at all: the game pins them in screen space, which is why
 * `native_x0` has always been the right place for them. Hence two offsets, and
 * the band is decided by MEASUREMENT per row (`cyc_render_line_scroll_x(y) ==
 * s_ws_court`), never by hardcoding 48.
 *
 * No margin sprite has ever been observed in the held band -- 3333 parts over
 * 81 captured frames of a pan, rows 102..217 -- so this only decides background
 * rows and the native block's own placement. It is per row rather than a single
 * offset anyway so that the row's scroll and its offset are always read
 * together, and cannot disagree.
 *
 * BACKGROUND ONLY. This is NOT the right function for a sprite: the sprite layer
 * is one window and this splits it in two, which is the 84-column court-camera
 * step. See `sdb_ws_sprite_origin` below. Nothing above this line is wrong --
 * the two offsets are correct for the background, which really is two windows
 * -- and reading the argument above as if it settled the sprite layer's column
 * as well is what produced that step. */
static int sdb_ws_row_x0(int y)
{
    if (y < 0) y = 0; else if (y > 239) y = 239;
    return cyc_render_line_scroll_x(y) == s_ws_court ? s_ws_x0_court : s_ws_x0_held;
}

/* The first NATIVE column the block must not stamp over, given where the block
 * sits on the canvas.
 *
 * THE BAND IS CANVAS COLUMNS 0..SDB_WS_CLIP-1. `SDB_WS_CLIP` was being applied as
 * a NATIVE offset instead, and those are the same eight columns only while the
 * block sits at canvas 0 -- never true with `camera = locked`, because the block
 * then slides inside the canvas as the camera steps inside the court's window.
 * Measured at 16:9 on a court-cam capture, `row_x0` is 8 at scroll 40 and 0 at
 * scroll 32, so the two readings differ by a whole camera step on a regular basis
 * and the skip moved with it.
 *
 * Returned in native columns because that is what all three consumers index by:
 * the compositor's stamp, `sdb_present`'s own stamp, and the seam check.
 *
 *     the block's native column n is at canvas rx0 + n
 *     the HIDDEN columns are native 0..7, at canvas rx0..rx0+7
 *
 * So the band follows the BLOCK, not the canvas edge, and that is the whole
 * point. It was expressed as a canvas band (canvas 0..7) applied to native
 * columns, which are the same eight only while `rx0 == 0`, and with
 * `camera = locked` they never are: `row_x0` is `scroll - anchor`, measured 8 at
 * scroll 40 and 0 at scroll 32 at 16:9, so the two readings come apart by a whole
 * camera step on a regular basis.
 *
 * THE FLAVOUR THAT MATTERS. A canvas-anchored band asks "are canvas columns 0..7
 * hidden?", and the answer flips with the camera -- which is the reported artefact.
 * A block-anchored band asks "are the block's own hidden columns on the canvas?",
 * and the answer is a property of the block: yes whenever `rx0 <= 7`, no when the
 * block has slid clear of them. That is stable, it does not depend on the camera's
 * parity, and when the answer is no there is genuinely nothing to reveal because
 * canvas 0..7 was never the block's -- it is margin, and out of court bounds, so
 * it is black for an unrelated and correct reason.
 *
 * When `rx0 < 0` the block hangs off the left and all eight hidden columns are on
 * the canvas at canvas `rx0..rx0+7`, so all eight are revealed and the skip is the
 * full `SDB_WS_CLIP`.
 *
 * Returns 0 when the reveal is off, which is the pre-existing behaviour exactly:
 * all 256 columns are the machine's, the clip the game asked for included.
 *
 * MEASURED, replay against replay (two builds, same vector, court cam 16:9), art
 * pixels in canvas columns 0..7:
 *
 *   before:  739 / 0 / 745  flipping every time the camera crossed 8
 *   after:   0 art on every frame in the window, no flip
 *
 * The gate is the band itself, not a whole-image diff. The band's contents depend
 * on where the block sits, so a whole-frame comparison shows the band MOVING
 * rather than showing it fixed -- and a capture-versus-replay diff is meaningless
 * here for a second reason: a windowed capture is 239 rows and a headless replay
 * is 240, so every row is off by one. */
static int sdb_ws_band_native(int rx0)
{
    /* `rx0` IS consulted, and it is the only thing that makes the band stop
     * travelling. See the long note below.
     *
     * The band is the BLOCK'S OWN hidden columns -- native 0..SDB_WS_CLIP-1 --
     * which the machine leaves black because PPUMASK hides them, and which the
     * reveal exists so the compositor's reconstruction shows through instead.
     * `SDB_WS_CLIP` is the native index, so this is the number that was always
     * right.
     *
     * It was worth having one function rather than three copies of
     * `s_ws_reveal ? SDB_WS_CLIP : 0`, because the question "which eight columns
     * are hidden" has two answers the moment the camera is locked: the block's own
     * (native 0..7) or the canvas's leading edge, and those are the same eight
     * only while the block sits at canvas 0. With `camera = locked` the block
     * slides inside the canvas -- `row_x0` is `scroll - anchor`, measured 8 at
     * scroll 40 and 0 at scroll 32 at 16:9 -- so the band's meaning flipped as the
     * camera stepped. Measured on a court-cam capture with the ball thrown left,
     * replayed against itself over frames 600..699: the art count in canvas
     * columns 0..7 alternated between 739 and 0 on every camera crossing, and a
     * sprite standing in those eight columns was swallowed while it read wrong.
     *
     * Neither of the two repairs is right. Making the band follow the canvas is
     * what it already did. Making it follow the block fixes the flicker and stops
     * the reveal doing anything at all -- measured, 167 frames of visible work
     * became 0 -- because the block's hidden columns are only on the canvas about
     * half the time. So the reveal is turned OFF under `camera = locked`, where
     * the two readings cannot be told apart, and left exactly as it was
     * everywhere else. See `sdb_ws_set_camera`.
     *
     * THE BAND FOLLOWS THE BLOCK, AND IT IS ALWAYS `SDB_WS_CLIP` COLUMNS WIDE.
     * `rx0` does not change its width, it only changes how much of it survives
     * when the block hangs off the left edge:
     *
     *     the block's native column n is at canvas rx0 + n
     *     the HIDDEN columns are native 0..SDB_WS_CLIP-1, at canvas rx0..rx0+7
     *
     * `rx0 >= 0` -- all eight are on the canvas, so skip all eight. There is no
     * upper test here and there never should be: `rx0` is 0, 8 or 207 across a
     * locked 16:9/32:9 run, and an earlier version that read `rx0 >=
     * SDB_WS_CLIP` as "slid clear, nothing to reveal" left the machine's backdrop
     * in place at `rx0 == 0`, so the band came back black there. Measured: 1912
     * black of 1912 in those eight columns on every frame the camera sat at 32.
     *
     * `rx0 < 0` -- the block hangs off the left, and the columns still on the
     * canvas are canvas 0..rx0+7, which is `rx0 + SDB_WS_CLIP` of them.
     *
     * So the answer is the full width, clamped against zero for the hanging
     * case.
     *
     * WHAT IT IS NOT, and the earlier version of this function was exactly this
     * and was wrong: a guard of the form `if (rx0 >= SDB_WS_CLIP) return 0`,
     * reading "the hidden columns are off-canvas". Under a locked camera that is
     * true and correct -- there is nothing of the block's to reveal, canvas 0..7 is
     * margin. Under a FOLLOW camera `rx0` is `native_x0` = 85 and the same guard
     * fires too, so it silently turned the reveal off for the entire default
     * camera: measured **650 of 701** follow frames changed against the binary
     * before it. `rx0` alone cannot distinguish "slid clear" from "centred".
     *
     * So there is no early return, and the caller decides: the reveal is a
     * whole-frame decision made in `sdb_ws_set_camera`, not a per-row one. */
    if (!s_ws_reveal) return 0;
    int skip = rx0 >= 0 ? SDB_WS_CLIP : rx0 + SDB_WS_CLIP;
    return skip < 0 ? 0 : skip;
}

/* Where native column 0 sits for the SPRITE LAYER -- one value, every row.
 *
 * `sdb_ws_row_x0` is right for the BACKGROUND and wrong for sprites, and the
 * reason is that they are not the same kind of thing. The background really is
 * two windows: this game splits its scroll mid-frame, rows 0-47 hold scroll 0
 * and carry the roster, rows 48-239 hold the camera, and each window's columns
 * have to go where that window's content is. The sprite layer is ONE window,
 * because the machine has one OAM, one 256-wide buffer and one canvas column per
 * sprite -- the game subtracts the camera from its X and stores a plain screen
 * column, so a sprite belongs to the court's world whatever row it lands on.
 * Placing that one layer at two canvas columns is what makes an object crossing
 * row 48 come apart, or step, by `s_ws_x0_court - s_ws_x0_held` (169 - 85 = 84).
 *
 * MEASURED on a court-cam played session, a thrown ball crossing row 48. The
 * ball moves 2 px a frame and steps 8 every four, like everything else in this
 * game and like MesenCE's copy of it; the only jumps in its canvas column are
 * between frames 524 and 525 (-76) and between 556 and 557 (+92), each one 84
 * plus the 8 columns of real motion. Ball camera never sees it and never could:
 * in follow mode both offsets are `native_x0`, so the question does not arise.
 *
 * THE COST, AND WHY IT IS NOT A NEW ONE. One origin means a sprite above row 48
 * sits 84 columns from the roster behind it -- a stable displacement, measured
 * at exactly 85 over 68 consecutive high-ball frames, because the game's own X
 * is already camera-relative and the camera's contribution cancels.
 *
 * It looks alarming, because 68 of 68 high balls then overlap roster ink. But
 * so does the STOCK picture: measured on the same vector at 256x240 with no
 * mods and follow mode, 68 of 68 as well. The ball genuinely flies over the
 * roster in this game -- it is a high throw over the crowd -- and the game
 * draws it there itself. What one origin changes is which 85 columns of roster
 * it covers, not whether it covers any.
 *
 * The two rejected alternatives are worse, and both were measured on the sweep
 * vector, 16:9, locked, 1000 frames, flicker mod on:
 *
 *   per entry (what this replaces) -- the object is whole but steps 84 on the
 *     one frame the band holding most of its rows changes: 2 such frames per 50
 *     of the court-cam ball window, and 3 more over 1000 sweep frames. Per entry
 *     rather than per object it TORN instead, on 16 of 95, which is why the
 *     per-object rule existed first. Two rules, two defects, one cause: both
 *     asked this function's answer for a layer it does not describe.
 *   one origin for the BACKGROUND too -- no step at all, and the roster slides:
 *     16 distinct left edges over 401 frames in 2-4 px steps, because
 *     `s_ws_x0_court` is `s_ws_court - s` and `s_ws_court` is the camera. Its
 *     left edge also leaves the canvas. Both plainly visible.
 *
 * Unconditional, because on a declined frame and in follow mode
 * `s_ws_x0_court == s_ws_x0_held == native_x0` and this is then exactly what
 * `sdb_ws_row_x0(y)` returned for every row -- so follow mode cannot move. */
static int sdb_ws_sprite_origin(void)
{
    return s_ws_x0_court;
}

/* How far a margin part's placement shifts, so the margin takes one anchor too.
 *
 * A margin part is placed at `world + (row offset - row scroll) - 4`: its
 * object's world column, put through the same anchor as its row's background.
 * `row offset - row scroll` IS that anchor, so it still differs by band once
 * both offsets are equal -- 169 on a held row (scroll 0), -47 on a court row
 * (scroll 216) -- and a margin sprite would take the same step across row 48
 * the collapse just removed from the machine's own sprites.
 *
 * Zero unless the anchor is engaged. In follow mode and on a declined frame this
 * expression is already the identity, so the default camera is untouched by
 * construction rather than by measurement. No margin part has ever been observed
 * in the held band (3333 parts over 81 captured frames of a pan, rows 102-217),
 * so this does not fire in practice -- it is here so that the margin cannot
 * develop the same artefact the moment one does. */
static int sdb_ws_margin_shift(int prow, int origin)
{
    (void)prow;
    if (!s_ws_here) return 0;
    return (s_ws_x0_court - s_ws_court) - origin;
}

/* Put the anchor back to "one offset, the centred one the engine chose" and say
 * it did not engage.
 *
 * Needed on every path where the compositor returns 0. The host's response to a
 * declined frame is to pillarbox the machine's own picture at the CENTRED
 * column, and `sdb_present` reads this state to place the flicker mod's redraw
 * -- so leaving `s_ws_x0_court` pointing somewhere else would have the two
 * paths disagree, and the flicker mod would stamp the picture a camera step away
 * from where the host drew it. The frame-0 and scene-change declines are exactly
 * where a player would see it. */
static void sdb_ws_deanchor(int native_x0)
{
    s_ws_here = 0;
    s_ws_x0_court = s_ws_x0_held = native_x0;
    s_ws_anchor = s_ws_court - native_x0;
}

/* Past the edge of the 512-pixel ADDRESS space there is nothing to read, so the
 * canvas is filled with the backdrop. Inside that space but outside the game's
 * camera window is SDB_WS_LO/SDB_WS_HI's business, above: those columns are
 * perfectly readable, and reading them is what produced the artefact.
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

static int sdb_ws_composite(uint32_t *out, int width, int height, int native_x0,
                            const uint32_t *native, void *user)
{
    (void)user;
    /* Every decline from here on has to leave the anchor neutralised, because
     * the host pillarboxes at the centred column when we return 0 and
     * sdb_present reads this state to place the flicker mod's redraw. */
    if (width <= 256 || height != 240) return 0;
    if (!(cyc_render_line_mask(239) & 0x08)) return 0;   /* background off */

    /* Where this frame's native picture goes on the canvas, decided before a
     * single pixel is drawn: every loop below asks sdb_ws_row_x0() for its own
     * row, so the scroll a row is drawn from and the offset it is drawn at are
     * always read together and cannot disagree. */
    sdb_ws_anchor_frame(width, native_x0);
    if (s_ws_here) s_ws_anchored++;

    const uint8_t *opaque = cyc_frame_bg_opaque();
    for (int y = 0; y < height; y++) {
        int sx = cyc_render_line_scroll_x(y);
        int row_x0 = sdb_ws_row_x0(y);
        /* PPUMASK bit 1 is "show background in the leftmost 8 pixels" -- SET means
         * shown, so the clip exists when the bit is CLEAR. (wideNES's own write-up
         * states the opposite of its code here; the code is right.) This game
         * clears it, and getting the polarity backwards paints real court in
         * the eight columns the PPU painted with the backdrop. The clip is in
         * screen columns, so it lands on the native 256, not on the world. */
        /* THE CLIP IS IN NATIVE COLUMNS, WHICH IS CORRECT, AND IT WAS ALSO BEING
         * OVERRIDDEN BY `s_ws_reveal` IN A WAY THAT PUT THE BANDS IN TWO PLACES.
         *
         * The band the player sees is the BLOCK's own PPUMASK-hidden native 0..7,
         * which lands at canvas `row_x0 .. row_x0+7`. Under a locked camera
         * `row_x0` is `scroll - anchor` and so slides with the camera -- measured
         * 8 at scroll 40 and 0 at scroll 32 at 16:9 -- so the band slides too.
         *
         * With the reveal forced to 0 here (so the reconstruction draws the world
         * everywhere) and `keep` skipping native 0..7 in the stamp (so the machine's
         * PPUMASK backdrop is not put back), the two agree and the band is filled.
         * With the reveal forced OFF, `keep` became 0, the stamp wrote the
         * machine's backdrop into native 0..7, and the band came back as a hard
         * 8-column black bar travelling with the camera. That is the artefact.
         *
         * So the reveal is ON here again and the skip in the stamp is what has to
         * follow the block. See `sdb_ws_band_native`. */
        int clip = (s_ws_reveal || (cyc_render_line_mask(y) & 0x02)) ? 0 : SDB_WS_CLIP;
        /* Native columns the stamp below will NOT put back, so this loop owns
         * them outright -- see `sdb_ws_band_native`. */
        int keep = sdb_ws_band_native(row_x0);
        uint32_t *row = out + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            int off = x - row_x0;
            if (off >= 0 && off < clip) { row[x] = cyc_render_color(0); continue; }
            /* cyc_render_line_scroll_x is 0-511, $2000 bit 0 already folded in,
             * so a canvas column's world column is that line's scroll plus its
             * own offset from native column 0. */
            int wx = sx + off;
            /* Past the stadium's end there is nothing, so the margin stops
             * there -- but ONLY in the margin. The native 256 are the machine's
             * own picture (copied back below) and the seam check below compares
             * this reconstruction against the machine's background over exactly
             * those columns, so bounding them would report a reconstruction
             * failure for a camera position that legitimately shows the end wall
             * -- measured at 225..240 -- and pillarbox a frame that is correct.
             * The margin is ours alone, so bounding it is free of that.
             *
             * AND THE REVEALED BAND IS NOT "THE NATIVE 256" -- THAT IS THE BUG
             * THIS PARAGRAPH HAD. The band is native 0..7, it is ours, and it
             * gets the same bounds test the margin does. Without that it was
             * exempt purely for being inside the block, and the exemption showed:
             * canvas columns 0..7 are world 32..39 under `camera = locked` at
             * EITHER camera position, but the block sits over them only when it
             * is flush left (`row_x0 == 0`). Block flush left, the columns took
             * the margin path and read black; block slid right (`row_x0 == 8`),
             * the same world columns came through the block's exemption and drew
             * 745 nametable pixels that are outside the court. So the band's
             * appearance flipped on every camera step -- one transition instead
             * of the flip-per-step it was, and the same defect. */
            if ((off < 0 || off >= 256 || (off >= 0 && off < keep)) &&
                !sdb_ws_in_bounds(wx)) {
                row[x] = cyc_render_color(0);
                continue;
            }
            row[x] = sdb_ws_pixel(wx, y);
        }
    }

    /* The revealed columns are meant to differ, so they are not compared.
     *
     * AND THE BLOCK MAY HANG OFF AN EDGE, so this is clipped. A still anchor
     * cannot always keep all 256 columns on the canvas: at 16:9 the canvas has
     * room for 170 and the camera's measured range spans 184, so 14 columns of
     * the block sit off the right edge at the far right of a pan whichever
     * anchor is chosen. The old code avoided that by MOVING the anchor, which is
     * the 93-column jump this whole change exists to stop -- so the overhang is
     * accepted and clipped here instead.
     *
     * Clipping is what the canvas is for: the columns that fall off are margin,
     * already reconstructed from the nametable by the loop above and bounded by
     * `sdb_ws_in_bounds`. What must not happen is indexing past the end of a
     * row -- `out + y*width + row_x0 + x` with row_x0 + x >= width reads the
     * NEXT row and compares the wrong thing, which is how the unclipped version
     * reported 1192 px of seam diff and pillarboxed 33 frames of 670. */
    /* In canvas space, like everything else that touches the band. */
    int first = sdb_ws_band_native(sdb_ws_row_x0(0));
    long diff = 0;
    if (cyc_render_background(s_ws_native_bg)) {
        for (int y = 0; y < 240; y++) {
            int rx0 = sdb_ws_row_x0(y);
            int lo = first, hi = 256;
            if (rx0 + lo < 0) lo = -rx0;               /* off the left edge */
            if (rx0 + hi > width) hi = width - rx0;    /* off the right edge */
            if (lo >= hi) continue;
            const uint32_t *row = out + (size_t)y * width + rx0;
            const uint32_t *bg = s_ws_native_bg + (size_t)y * 256;
            for (int x = lo; x < hi; x++) {
                if (opaque && !opaque[(size_t)y * 256 + x]) continue;
                if (row[x] != bg[x]) diff++;
            }
        }
    } else {
        /* No capture available: nothing to verify against, so do not claim a
         * seam we did not check. */
        sdb_ws_deanchor(native_x0);
        return 0;
    }
    /* 2% of the frame's background pixels. Generous enough that a handful of
     * genuinely ambiguous edge pixels do not force a fallback, tight enough
     * that reading the wrong nametable cannot pass. */
    if (diff * 100 > 2 * 256 * 240) {
        s_ws_pillarboxed++;
        s_ws_last_diff = diff;
        sdb_ws_deanchor(native_x0);
        return 0;
    }

    /* The machine's own 256 columns win, so the middle of a wide frame is
     * pixel-for-pixel the stock picture at every aspect ratio -- with one
     * deliberate exception: when the overscan feature is revealing the clipped
     * columns, those are left as reconstructed, or the copy would put the black
     * gutter straight back. */
    /* Whose pixels these are, before anything is copied or drawn over them.
     * `s_ws_native_bg` is valid by here: the seam check above returns 0 when the
     * capture is missing. */
    sdb_ws_build_owner(native);
    sdb_ws_prune_margin();

    /* Clipped for the same reason as the seam check above, and this one is not
     * merely a wrong comparison: an unclipped memcpy of 256 columns at row_x0
     * 180 on a 426-wide canvas writes 10 pixels past the end of every row but
     * the last, into the next row's first columns. The canvas is
     * `width * height` with nothing after it, so the last row's overrun is a
     * heap write off the end of the buffer.
     *
     * The skip is in CANVAS columns, translated per row. `SDB_WS_CLIP` was applied
     * as a NATIVE offset, which is the same eight columns only while the block sits
     * at canvas 0 -- never true with `camera = locked`, where the block slides
     * inside the canvas as the camera steps. Measured at 16:9 on a court-cam
     * capture, `row_x0` is 8 at scroll 40 and 0 at scroll 32, so the skip moved
     * with the camera: it ate eight real columns of court on one side of a step
     * and left eight unwritten on the other, which is the black band appearing
     * and disappearing and a sprite standing in it being swallowed. See
     * `sdb_ws_band_native` for the arithmetic and the measurement. */
    for (int y = 0; y < height; y++) {
        int rx0 = sdb_ws_row_x0(y);
        int lo = sdb_ws_band_native(rx0), hi = 256;
        if (rx0 + lo < 0) lo = -rx0;
        if (rx0 + hi > width) hi = width - rx0;
        if (lo >= hi) continue;
        memcpy(out + (size_t)y * width + rx0 + lo,
               native + (size_t)y * 256 + lo, (hi - lo) * sizeof(uint32_t));
    }

    /* Declare where the native picture actually went. The flicker mod's wide
     * path reads this (cyc_render_native_origin) to place its own sprite list,
     * and the host maps aim through it; with the anchor engaged the block is
     * nowhere near the centred position, so leaving this unsaid would put the
     * flicker mod's sprites 0-400 columns from their own backgrounds. */
    cyc_render_set_native_origin(s_ws_x0_court);

    /* Sprites the game placed outside its own viewport. Drawn AFTER the copy so
     * they cannot touch the native 256, which stay byte-identical to stock by
     * construction rather than by clipping -- a sprite whose column is inside
     * the viewport is one the game did NOT cull, so it is not in this list
     * anyway, and the two sets cannot overlap.
     *
     * Every live object slot is drawn, and "live" is answered from the DISPLAY
     * RECORD rather than from when the object was last emitted. That matters
     * because the two are not the same thing in this game: `$067C` does not
     * contain most off-screen objects, so records 2 and 8 were emitted 4 times
     * and once respectively over 100 frames of the pan vector -- and they are
     * exactly the two standing in the margins. Anything whose lifetime is
     * measured in emissions blinks those two out.
     *
     * So the record decides, on three counts: byte 0 non-zero means the slot is
     * in use at all; bytes 0-1 still matching the capture means the slot has not
     * been recycled onto a different object; and the current column, placed from
     * the record the game rewrites every frame, must be off-screen. Nothing
     * expires, so nothing blinks. The off-screen test is also what keeps a player
     * who walked into the viewport from being drawn twice -- the machine owns
     * those columns and this skips them exactly.
     *
     * Slots are drawn in index order, which is the display list's depth order
     * only loosely -- but these are objects the machine culled outright, so
     * nothing the player can see is covered up by getting their order slightly
     * wrong.
     *
     * SDB_TRACE_CAPTURES=<from>-<to> dumps what was drawn, one line per frame,
     * with the object index, so a position can be watched across builds. */
    if (s_ws_margin) {
        int n = 0;
        /* `x0` here is the COURT band's offset, which is the one the game places
         * objects with and the one every measured part needs (rows 102..217, all
         * of them). The per-part offset is read again inside the loop from the
         * part's own row, so a part that ever did land on the held band would
         * stay glued to the scroll it was computed from rather than be split
         * across two bases -- the collapse research 26 records as a character
         * drawn at a sixteenth of his width. */
        if (SDB_CAP_TRACE)
            fprintf(stderr, "sdbw f=%ld x0=%d:", s_frame, s_ws_x0_court);
        int page_clear = sdb_page_clear();
        for (int o = 0; o < SDB_MARGIN_OBJECTS; o++) {
            SdbMarginObj *ob = &s_margin_obj[o];
            if (!ob->n) continue;
            uint16_t rec = (uint16_t)(SDB_REC_BASE + SDB_REC_STRIDE * o);
            if (SDB_CAP_TRACE)
                fprintf(stderr, " [o%dn=%d id=%02X/%02X rec=%02X/%02X/%02X/%02X]", o, ob->n,
                        ob->id0, ob->id1, cyc_mod_peek(rec),
                        cyc_mod_peek((uint16_t)(rec + 1)),
                        cyc_mod_peek((uint16_t)(rec + 2)),
                        cyc_mod_peek((uint16_t)(rec + 3)));
            /* Unused slot, or a slot the match has handed to a different object:
             * either way these tiles are not this object's any more.
             *
             * Byte 0 is the object's STATE byte `$43`, and the culling bits in it
             * are not part of its identity: section 2.1's gates read exactly
             * `($43 & $70) == $20` and `& $20`, and the flicker mod overrides all
             * three precisely so the object keeps being emitted. So the game flips
             * those bits, byte 0 goes from `$02` to `$22`, and this test read that
             * as "a different object took the slot" and dropped the whole
             * character -- a full-body blink at the margin edge, for exactly as
             * long as the game held the flag. A played session showed it at frames
             * 910-912 and 925-926, reproduced headlessly at 919-921.
             *
             * Masking `$70` is the whole fix and nothing more: the remaining bits
             * and byte 1 still have to match, so a genuinely recycled slot is
             * still caught. Byte 1 is `$41` and does not move with the culling
             * state.
             *
             * THE `!r0` TERM THAT USED TO LEAD THIS TEST IS GONE, and it was
             * wrong on this very question. "byte 0 non-zero means the slot is in
             * use at all" is not a fact about this game: byte 0 is the state byte
             * and the game parks an object at `$00` while still emitting it --
             * that is precisely what it does to the characters walking off the
             * end of the pitch in Bean Ball, which is the case this whole feature
             * exists for. Measured on that capture: 216 of 252 capture events saw
             * byte 0 == `$00`, and 155 of those objects were holding parts at the
             * time. So the margin captured every one of them, and then threw
             * them away here. Over a 141-frame window it held 4570 parts and drew
             * 719. The characters those parts belong to stop being emitted by the
             * game at native column ~250 and vanish, while the canvas still has
             * 167 columns of court to their right.
             *
             * Nothing is lost by dropping it. Each of the three things it stood
             * in for has its own gate, and all three are measured: a slot the
             * game has handed to a DIFFERENT object fails the identity compare
             * here and is discarded at capture by `sdbR` before its old parts can
             * come back (research.md 26b); an object the game has stopped
             * emitting is stood down by `ob->seen` and SDB_MARGIN_STALE below; and
             * a roster the game has hidden wholesale is caught by the page-clear
             * gate. This was the fourth copy of the rule and the only one that
             * answered a question none of them was asked. */
            if (!sdb_margin_live(ob, rec))
                continue;
            /* THE GAME STOPPED EMITTING THIS OBJECT.
             *
             * The record above says the slot is in use and the game keeps
             * rewriting it, so the identity test passes for as long as the slot
             * lives -- which is longer than the object does. A between-rounds
             * screen leaves every record standing and stops emitting the whole
             * roster, and the margin then repaints whatever stood off the right
             * edge of the pitch for the length of the menu. Measured on a played
             * level-1 session: objects 6, 7 and 8 held screen columns 271-330
             * from frame 1250 to 1547 off a capture made at frame 1053, 504
             * frames earlier and never touched since.
             *
             * `ob->seen` is the frame the game last emitted this object, so this
             * is the one liveness question the record cannot answer and the
             * capture can. SDB_MARGIN_STALE carries the measurement; in play no
             * object anywhere in 12000 frames of the four vectors went 140
             * frames without an emission, so nothing that is being played is
             * dropped by this. */
            if (s_frame - ob->seen > SDB_MARGIN_STALE) {
                s_margin_stale++;
                if (SDB_CAP_TRACE)
                    fprintf(stderr, "sdbx f=%ld obj %d stale %ld frames since its last "
                                    "emission at %ld; not drawn\n",
                            s_frame, o, s_frame - ob->seen, ob->seen);
                continue;
            }
            /* The anchor, corrected only on a whole-object basis disagreement. */
            int row_corr = 0;
            int corr = sdb_object_delta(ob, &row_corr);
            /* Only when it acted. Printing the zeroes too would fill the trace with
             * `[corr+0]` per object per frame, which reads as a column of data and
             * is none -- the same "a green line that measured nothing" shape this
             * file keeps having to undo. */
            if (corr && SDB_CAP_TRACE)
                fprintf(stderr, " [corr%+d]", corr);
            if (row_corr && SDB_CAP_TRACE)
                fprintf(stderr, " [corry%+d]", row_corr);
            for (int i = 0; i < ob->n; i++) {
                const SdbMargin *m = &ob->parts[i];
                /* The row, corrected like the anchor. See sdb_object_delta: the
                 * machine's copy of this sprite sits N rows from the captured
                 * one (vertical leapfrog, same staleness class as the columns),
                 * and painting the captured row puts a 1-2px doubled edge on a
                 * sprite the machine is drawing right beside it. */
                SdbMargin pm = *m;
                pm.y = (int16_t)(m->y + row_corr);
                m = &pm;
                /* Current screen column: the record's world column plus this
                 * part's offset, minus the game's own camera.
                 *
                 * The camera is `$C3/$C4`, NOT `cyc_render_line_scroll_x`. That is
                 * not a preference, it is what makes a character whole. The game
                 * places a sprite with `$823E/$8246: SEC / SBC $C3 / SBC $C4`,
                 * so OAM X -- and therefore the column the PPU draws -- is
                 * measured against `$C3/$C4` and nothing else. The scroll the PPU
                 * latched for the frame trails it: measured on the pan vector it
                 * is consistently one camera step (6-7 px) behind, because the
                 * game advances its camera for the frame it is about to render.
                 * So a margin sprite placed with the scroll lands exactly one
                 * step left of the machine's own copy of the same part. Verified
                 * by matching tile and row against OAM on the pan vector: frame
                 * 633, tiles $11/$21/$31/$01 at rows 151/159/167/143, machine
                 * OAM X = 1/1/1/3, ours -5/-5/-5/-3 -- six, every part.
                 *
                 * Six pixels is half a character, and it only shows up where a
                 * character straddles the viewport edge, because that is the only
                 * place the machine's copy and ours are both on screen: you get
                 * two half-characters overlapping with a seam, and the halves do
                 * not meet. Placing with `$C3/$C4` puts ours exactly on top of
                 * the machine's, so the margin completes the character instead of
                 * forking it.
                 *
                 * The cost, stated plainly: the margin BACKGROUND is reconstructed
                 * from the presented frame's scroll, so while the camera is moving
                 * a margin sprite sits up to one camera step ahead of the court
                 * under it. When the camera is still -- most of a match, and all of
                 * the time the player is looking at a stationary scene -- the two
                 * are identical and there is nothing to see. The alternative is not
                 * a tidier trade, it is the tearing above.
                 *
                 * Column 0 of the screen sits at this part's own row offset on
                 * the canvas, so a negative column lands in the left margin and
                 * one past 255 in the right. The offset is measured from the
                 * game's own accumulator, which already carries its OAM bias of
                 * 4, so the same subtraction the guest does puts it back.
                 *
                 * ONE BASIS, and it is the scroll.
                 *
                 * `cyc_render_line_scroll_x` is what the PPU actually latched for
                 * the frame being presented, and it is what the margin background
                 * under this sprite was resolved with. The machine's own sprites
                 * are glued to that background -- measured on the sweep by
                 * world-aligning the stock picture frame by frame: consecutive
                 * frames match to a mean absolute difference of 0.00, and a
                 * tracked sprite's OAM X plus the scroll holds constant across a
                 * 72-pixel pan. So the scroll is right for a part standing alone
                 * in the margin AND for one completing a sprite the PPU clipped,
                 * because the machine's copy of that sprite is on the same basis.
                 *
                 * `$C3/$C4` looks like the alternative -- it is what
                 * `$823E/$8246: SEC / SBC $C3 / SBC $C4` made OAM X from -- and it
                 * measures as one 6-7 px camera step away from the presented
                 * scroll on one frame in three, because at composite time it
                 * already holds the camera for the frame after the one being
                 * presented. Using it puts standalone margin sprites a step ahead
                 * of the court under them.
                 *
                 * Splitting the parts across the two bases -- one for the parts
                 * standing alone, the other for the ones completing a clipped
                 * sprite -- is worse than either. A character is sixteen columns
                 * of two eight-wide sprites; split its two halves across the two
                 * bases and the eight pixels between them collapse to one, so it
                 * is drawn at a sixteenth of its width. Measured on the sweep at
                 * frame 1060, object 10's rows 184, 168 and 176 came out one
                 * pixel apart instead of eight, and were correct again at 1063
                 * when the two bases happened to agree. That is the "shifted a
                 * pixel" and "slightly the wrong size" artifact, and it is the
                 * basis split, not a stale part. */
                int prow = m->y < 0 ? 0 : (m->y >= 240 ? 239 : m->y);
                int scroll = cyc_render_line_scroll_x(prow);
                /* The offset for THIS part's own row: with the anchor engaged the
                 * court band's columns move and the held band's do not, and the
                 * two are read together with the scroll above. */
                int rx0 = sdb_ws_row_x0(prow);
                /* WHERE THIS PART GOES. See sdb_margin_col: the anchor is
                 * the object's world column AT THE EMISSION, not the record's
                 * live value, so that the margin's copy of a sprite and the
                 * machine's copy of it are on the same time base and a
                 * character straddling the machine's 256-column edge stays
                 * whole.
                 *
                 * Measured on a played 16:9 capture, object 5, frames
                 * 549-552: the game emitted the character at frame 548 with
                 * accumulators 259/251/257/249/257/249/256/248, so the machine
                 * drew the LEFT half at OAM X 247/245/245/244 and culled the
                 * RIGHT half (accumulator > 255), which the margin drew. The
                 * record's X moved 355 -> 359 at frame 550, so the margin's half
                 * moved +4 while the machine's stayed at 247: a four-column gap
                 * straight through the character. */
                int sx = sdb_margin_col(m, scroll) + corr
                       + sdb_ws_margin_shift(prow, rx0 - scroll);
                /* Completing a sprite the PPU clipped is the one case the margin
                 * owns rather than the machine. */
                /* THE LEFT MIRROR OF THE RIGHT STRADDLE, and the same test.
                 *
                 * The right side needs a pass because the machine's own 256-wide
                 * buffer clips a sprite at column 255 and the columns it loses
                 * are ones it HAD. The left looks like a mirror and is not:
                 * PPUMASK clips screen columns 0-7, so the machine delivers
                 * nothing there at all, and `completes` is already its renderer.
                 *
                 * But `completes` said `sx >= 0`, and a part at **sx = -1** covers
                 * columns -1..6 -- six columns inside the band that the machine
                 * cannot paint and this test refuses. That is the right side's
                 * case exactly, one sprite further left: a sprite clipped at its
                 * edge with the lost columns unpainted by anybody.
                 *
                 * So the test is the pass's straddle test, not a half-open
                 * interval: does the part CROSS the band's inner edge, in either
                 * direction. `sx < SDB_WS_CLIP && sx + 8 > 0`.
                 *
                 * The margin blit then clips to the band, so a part at sx = -1
                 * paints only what falls inside it and cannot reach a column the
                 * machine owns. */
                int completes = s_ws_reveal && sx < SDB_WS_CLIP && sx + 8 > 0;
                /* NOTE there is deliberately NO `sdb_oam_has` test here.
                 *
                 * One was added to avoid drawing a band sprite the machine also
                 * shows, and it suppressed the pixels the margin exists to paint.
                 * The band is screen columns 0-7, which PPUMASK clips: the PPU
                 * never paints them, so the machine's copy of a sprite standing
                 * there is INVISIBLE, and a part skipped on the grounds that OAM
                 * holds it is a hole nobody else fills. That is the reported
                 * artefact: not a truncated body, but a column or two of pixels
                 * blinking out of a sprite as the viewport edge sweeps across it,
                 * head to toe.
                 *
                 * It flickered because `sdb_oam_has` is a live scan of a list the
                 * game rebuilds over four frames and truncates at 64 sprites, so
                 * whether it answers yes changes frame to frame. Asking it was
                 * asking "did the machine manage to emit this" -- which is the
                 * truncation this whole mod exists to undo, consulted at the one
                 * place where the machine cannot draw anyway.
                 *
                 * The doubling it was guarding against cannot happen: the blit
                 * below clips a band part to `px0 + 8 - left_limit` columns, and
                 * `left_limit` is `native_x0 + SDB_WS_CLIP`, so a part that reaches
                 * here paints ONLY inside the clipped band and cannot touch a
                 * column the machine owns. */
                /* OWNERSHIP, NOT GEOMETRY.
                 *
                 * `sx >= 0 && sx <= 251` is the native block's bounds, not the
                 * machine's painting, and asking it here skips the part outright
                 * before the blit runs. Inside the block the PPU leaves background
                 * wherever the game culled a sprite, and there the margin is the
                 * only renderer -- so this test is what leaves a character with a
                 * slice missing rather than merely stopping a double-draw.
                 *
                 * Measured on the margin-locked capture, frames 809-816: a head
                 * missing canvas 413-414 (native 252-253) for eight rows on every
                 * one of those frames, on top of a worse frame at 808. Those
                 * columns carry the head's outline and skin -- checked against the
                 * whole head at 807, where the same tile has opaque pixels there,
                 * so it is not transparent art. Byte-identical with the flicker
                 * mod on and off, which places the hole in the machine's frame.
                 *
                 * So: skip only where the machine really painted something. */
                int machine_drew = 0;
                if (!completes) {
                    for (int c = 0; c < 8 && !machine_drew; c++) {
                        int nx = sx + c;
                        if (nx < 0 || nx > 255) continue;
                        if (s_ws_native_owner[(size_t)m->y * 256 + nx] != 0xFF)
                            machine_drew = 1;
                    }
                }
                if (machine_drew) continue;
                int px0 = sx + rx0;
                if (SDB_CAP_TRACE)
                    fprintf(stderr, " [o%d:%d,%d,t%02X]", o, sx, m->y, m->tile);
                n++;
                /* Only ever draw the part that lies in the MARGIN. The machine's
                 * picture owns a run of canvas columns and everything else is
                 * ours to paint; the run is NOT always the native 256. With
                 * overscan on, the copy-back above starts at native column 8 and
                 * leaves the eight revealed columns holding the reconstructed
                 * background, so the machine's run is [native_x0+8, native_x0+
                 * 256) and the band belongs to the margin. Painting it is the
                 * whole point of revealing it -- leaving it empty is what put a
                 * hard vertical seam through any player standing in it.
                 *
                 * The blit draws columns [clip_lo, 8-clip_hi), so the two
                 * straddles clip opposite ends: a left-edge straddle keeps
                 * c < left_limit-px0, a right-edge straddle keeps
                 * c >= native_x0+256-px0.
                 * (An earlier version rejected the straddle whole instead of
                 * clipping it, which is why the white-shirt player vanished while
                 * standing at column -4; and the first clip written here had lo/hi
                 * backwards, which drew the native half and dropped the margin
                 * half. Both produced a plausible picture with the player
                 * missing.)
                 *
                 * The same window also carries the world's own bounds, which is a
                 * bound on the MARGIN exactly as the machine's run is: a sprite
                 * standing past the stadium's end is drawn where the game put it,
                 * which is over backdrop. Clipped
                 * rather than dropped, so a character straddling the bound is
                 * still drawn up to it. */
                /* The world's own bound is the only one left that is geometric:
                 * it bounds the WORLD and not a painting, so a part past the
                 * stadium's end is not drawn and one straddling it is cut there,
                 * so a character standing at the bound is still whole up to it. */
                int wl = rx0 + (SDB_WS_LO - scroll);
                int wh = rx0 + (SDB_WS_HI - scroll);
                /* A CLEARED PAGE STANDS THE MARGIN DOWN TOO.
                 *
                 * The margin exists to finish sprites the machine computed and
                 * then CULLED, so it reads liveness from the display record rather
                 * than from OAM. But the game's between-rounds screen retires its
                 * roster by pushing every OAM row to $F0 and leaving the records
                 * standing, so the records still say "live, off-screen" and the
                 * margin repaints a level's player in the margin of a menu with no
                 * sprites on it -- the same ghost as the flicker mod's list, from
                 * the other half of the compositor. Measured on the same capture,
                 * frame 873: object 7, screen column 309, 8 parts, the only ink
                 * the margin contributed to the whole screen.
                 *
                 * The test is the machine's, and it is the gameer's state rather
                 * than ours: when all 64 OAM slots are hidden the hardware paints
                 * nothing, so there is no culled sprite to complete. `s_margin_on`
                 * is deliberately NOT consulted, so this holds with the
                 * character-flicker feature off, where the page is cleared by the
                 * same game code.
                 *
                 * `n` above still counts the parts: the `sdbw` line has to keep
                 * describing the whole list or `margin_track.py` reads the change
                 * as an empty log rather than as what it is. */
                if (page_clear) { s_margin_cleared++; continue; }
                if (px0 + 8 > wl && px0 < wh) {
                    sdb_margin_blit(m, px0, out, width, height, s_ws_native_bg,
                                    wl, wh, rx0, s_ws_native_owner);
                    s_margin_drawn++;
                }
            }
        }
        /* What the game put in the hidden band, straight from OAM.
         *
         * This is the one question a --frame-log cannot answer here: --frame-log
         * implies headless (cyc_host.c:1100), and headless does not run the same
         * guest state as the window (measured: frames 0-3 match, frame 4
         * diverges), so an OAM dump from a headless run describes a different
         * game. The window is the only place this state exists, so the window has
         * to report it.
         *
         * It decides whether a margin sprite standing in columns 0-7 is
         * completing something the machine drew from column 8, or standing alone
         * with columns 8-12 that nobody will ever paint. PPUMASK bit 1 is clear
         * for this game so the PPU never paints those eight columns at all.
         *
         * NOTE this block ends the `sdbw` line with its own newline, which is
         * why the `n=%d` closer is printed BEFORE it rather than after. Both
         * fixes have been made here in sequence and each undid part of the
         * other: printing the closer first stopped `sdbm` being glued onto the
         * last `[oN:...]` entry, but with `oam-band:` sitting between them the
         * closer then landed on a line of its own -- so `margin_track.py`'s
         * regex, which wants `... n=NN` at the END of the `sdbw` line, matched
         * nothing and the audit reported "no sdbw lines in that log" on a
         * perfectly healthy run. `n` is final after the draw loop, so the
         * closer goes here, and this block prints after it. */
        s_margin_count = n;
        if (SDB_CAP_TRACE)
            fprintf(stderr, " n=%d\n", n);
        if (n > s_margin_peak) s_margin_peak = n;
        if (SDB_CAP_TRACE) {
            const uint8_t *oam = cyc_render_oam();
            fprintf(stderr, "oam-band:");
            for (int j = 0; j < 64; j++) {
                if (oam[j * 4] >= 0xEF || oam[j * 4 + 3] >= SDB_WS_CLIP) continue;
                fprintf(stderr, " [%d:y%d,$%02X,$%02X,x%d]", j, oam[j * 4], oam[j * 4 + 1],
                        oam[j * 4 + 2], oam[j * 4 + 3]);
            }
            fprintf(stderr, "\n");
        }
        /* The per-frame margin line lives HERE, not in the frame-begin hook:
         * `n` is what this presented frame actually painted, and frame-begin for
         * the next frame is the only other place that could print it -- one
         * frame late, against a `s_frame` counter that has already moved on. */
        if (s_trace)
            fprintf(stderr, "sdbm %ld %d %ld\n", s_frame, n, s_margin_off_edge);
    }

    if (sdb_json_want(s_frame))
        sdb_json_margin(s_ws_x0_court, width, diff, s_margin_count);

    /* The anchor, in the structured stream rather than on the human `sdbw` line,
     * which margin_track.py parses with a regex wanting `n=` at the end and
     * which is not going to be broken to carry this. `x0` is the COURT band's
     * offset and `xh` the held band's, so a gate cropping the machine's own 256
     * columns out of a presented frame has everything it needs; `scroll` is the
     * court's own camera and `anchor` the world column at canvas column 0, so
     * "did the view move" is `anchor` not changing. */
    if (sdb_json_want(s_frame))
        fprintf(stderr, "{\"k\":\"anchor\",\"f\":%ld,\"x0\":%d,\"xh\":%d,"
                        "\"scroll\":%d,\"anchor\":%d,\"cw\":%d,\"lock\":%d,\"here\":%d}\n",
                s_frame, s_ws_x0_court, s_ws_x0_held, s_ws_court, s_ws_anchor,
                width, s_ws_lock, s_ws_here);

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

/* What the reveal should be, in one place and in one precedence: an explicit
 * `--overscan` first, then `SDB_OVERSCAN` (the headless A/B AGENTS.md documents
 * for gate 5), then widescreen's own default of revealing.
 *
 * The environment read lives HERE rather than in an activation plugin because
 * the two ways of turning widescreen on have to answer identically: with it in
 * the plugin, `SDB_OVERSCAN=0 --widescreen 21:9` ignored the switch (the plugin
 * never runs) while the same environment with `--mods-root` honoured it, which
 * is the same class of bug as the one the default fixes. Measured: 280 of 701
 * frames differing between those two. */
static int sdb_ws_reveal_ask(void)
{
    if (s_ws_reveal_explicit >= 0) return s_ws_reveal_explicit;
    const char *e = getenv("SDB_OVERSCAN");
    if (e && e[0]) return atoi(e) != 0 ? SDB_WS_CLIP : 0;
    return SDB_WS_CLIP;
}

static void sdb_ws_apply(void)
{
    int wide = s_ws_aspect != NES_VIDEO_STOCK;

    /* THE REVEAL FOLLOWS WIDESCREEN unless something explicitly turned it off.
     *
     * It used not to, and the two ways of turning widescreen on disagreed:
     * `sdb_ws_enable` (the mod activation path -- the launcher, or any
     * `--mods-root`) called `sdb_ws_set_overscan(1)`, while the `--widescreen`
     * option handler set the aspect and nothing else. So a bare
     * `--widescreen 21:9` left s_ws_reveal at 0 and presented the eight columns
     * PPUMASK hides as the black band the PPU itself painted -- the exact
     * artefact widescreen exists to remove, and the reason `overscan` was
     * folded into this feature in the first place. AGENTS.md's documented
     * widescreen gate command uses `--widescreen`, so its "no black gutter"
     * expectation was being measured with the reveal off.
     *
     * Resolving it HERE rather than in a handler is the part that matters.
     * Stamping it from `--widescreen` would make the two flags order-dependent:
     * `--overscan 0 --widescreen 21:9` would end up revealing and
     * `--widescreen 21:9 --overscan 0` would not, which is a worse bug than the
     * one being fixed. `--overscan` is a bare flag, so it cannot carry a
     * default of its own; asking here can.
     *
     * Stock width resolves to 0: there is no compositor to draw the columns
     * and no margin to own them. */
    s_ws_reveal = wide ? sdb_ws_reveal_ask() : 0;

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
    /* Records the ASK; sdb_ws_apply resolves it against the current aspect, so
     * the two flags commute. */
    s_ws_reveal_explicit = on ? SDB_WS_CLIP : 0;
    sdb_ws_apply();
}

/* The camera policy. `follow` is what the compositor has always done -- the
 * canvas is centred on whatever the game's camera is this frame -- and `locked`
 * makes it a fixed window on the court instead. See sdb_ws_anchor_frame. */
static void sdb_ws_set_camera(int locked)
{
    s_ws_lock = locked ? 1 : 0;
    /* Nothing about the reveal is decided here, and that took a measurement to
     * establish. It was: `if (s_ws_lock) s_ws_reveal = 0`, on the reading that
     * canvas columns 0..7 have one meaning under a locked camera and another under
     * a follow one. They do -- but the band is the BLOCK's own PPUMASK-hidden
     * native 0..7, which travels with the block, so it can be anchored to the
     * block and both cameras keep it. Turning the reveal off instead left the
     * machine's backdrop in those eight columns and produced a hard 8-column
     * black bar sweeping left and right with the camera, which is the same
     * artefact by the other route.
     *
     * The fix is `sdb_ws_band_native`, which skips as many native columns as the
     * block's hidden columns actually occupy on the canvas. See there for the
     * `rx0`-guard that looked equivalent and was not: it cannot tell "slid clear"
     * from "centred" and it changed 650 of 701 follow frames. */
}

/* Reset callbacks run before every activation pass, so a disabled feature
 * reliably restores the stock 256-column picture. */
static void sdb_ws_disable(void)
{
    s_ws_aspect = NES_VIDEO_STOCK;
    s_ws_reveal = 0;
    s_ws_reveal_explicit = -1;
    s_ws_fixed = 0;
    s_ws_lock = 0;
    s_ws_margin = 0;
    s_margin_run = 0;
    s_margin_peak = 0;
    s_margin_lost = 0;
    memset(s_margin_obj, 0, sizeof s_margin_obj);
    s_margin_cur_obj = -1;
    s_margin_rec = 0;
    nes_mod_set_function_hook_enabled("super-dodgeball.margin.b5-8483", 0);
    nes_mod_set_function_hook_enabled("super-dodgeball.margin.b5-8302", 0);
    nes_mod_set_function_hook_enabled("super-dodgeball.margin.b5-8231", 0);
    cyc_render_set_compositor(NULL, NULL);
    cyc_video_set_mode(NES_VIDEO_STOCK);
}

/* SDB_TRACE=1: one line at exit, so a headless run says whether the margins
 * were actually drawn or quietly pillarboxed away every frame. */
static void sdb_ws_report(void)
{
    if (s_trace && s_ws_composed + s_ws_pillarboxed)
        fprintf(stderr, "widescreen: %ld composed, %ld pillarboxed, seam diff %ld px, "
                        "world bounds [%d,%d)%s\n",
                s_ws_composed, s_ws_pillarboxed, s_ws_last_diff,
                SDB_WS_LO, SDB_WS_HI,
                s_ws_lock ? ", camera locked" : "");
    /* The anchor, so a headless run says whether the lock engaged rather than
     * leaving a still picture to be interpreted. `s=` is the world column the
     * canvas's column 0 held on the last composed frame. */
    if (s_trace && s_ws_lock && s_ws_composed)
        fprintf(stderr, "camera: %ld of %ld frames anchored (%d left centred: menus "
                        "and anything outside the camera window), court scroll %d, "
                        "canvas world [%d,%d), native block at column %d\n",
                s_ws_anchored, s_ws_composed + s_ws_pillarboxed, s_ws_framed,
                s_ws_court, s_ws_anchor,
                s_ws_anchor + (s_ws_fixed ? s_ws_fixed : 0), s_ws_x0_court);
    if (s_trace && s_margin_on)
        fprintf(stderr, "margin sprites: %ld captured (%ld off-edge), %ld drawn, "
                        "peak %d, backstop %ld, %ld withheld (page cleared), "
                        "%ld stood down (game stopped emitting)\n",
                s_margin_frames, s_margin_off_edge, s_margin_drawn,
                s_margin_peak, s_margin_lost, s_margin_cleared, s_margin_stale);
    /* Printed whenever the run did anything with sprites, not only under
     * SDB_TRACE: it is the number that says whether the retirement filter ever
     * fired, and a filter that never fires is indistinguishable from one that is
     * not there. Zero over a whole match is the expected reading; a non-zero
     * number on a menu transition is the ghost fix reporting its work. */
    if (s_on && (s_retired_total || s_trace))
        fprintf(stderr, "flicker: %ld of %ld presented list entries withheld (the "
                        "game's OAM page was cleared)\n",
                s_retired_total, s_retired_total + s_cap_total);
    /* The same totals as a record, so a run's verdict is readable without
     * scraping prose. Printed whenever the JSON stream is on, not only under
     * SDB_TRACE: it is one line and it is the line that says whether the
     * margins were drawn at all. */
    if (s_json_from >= 0)
        fprintf(stderr, "{\"k\":\"r\",\"frames\":%ld,\"composed\":%ld,\"pillarboxed\":%ld,"
                        "\"diff\":%ld,\"mframes\":%ld,\"mcap\":%ld,\"moff\":%ld,"
                        "\"mdrawn\":%ld,\"mpeak\":%d,\"mlost\":%ld,\"mcleared\":%ld,"
                        "\"mstale\":%ld,\"retired\":%ld}\n",
                s_frame, s_ws_composed, s_ws_pillarboxed, s_ws_last_diff,
                s_margin_frames, s_margin_frames, s_margin_off_edge,
                s_margin_drawn, s_margin_peak, s_margin_lost, s_margin_cleared,
                s_margin_stale, s_retired_total);
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

static void sdb_ms_enable(void);
static void sdb_ms_disable(void);

/* Widescreen implies margin sprites: it is what the margins are FOR, and a
 * background-only margin is not a wider game, it is the same frame with black
 * columns glued on. Measured, 40% of the objects on the pitch are outside the
 * viewport at any moment, so "background only" leaves most of the field of play
 * invisible while still paying for the columns.
 *
 * Not a separate feature on purpose. It was one, and it shipped broken in the
 * only way that matters: `margin-sprites` defaulted off, the window ran it
 * off, and the picture looked exactly like the feature did not exist -- with
 * every other gate green, because the gates measured what the flag asked for
 * rather than what the player could see. There is nothing here worth choosing
 * against: the guest is byte-identical with the capture on or off (RAM, CIRAM,
 * OAM, palette, picture and cycle count), so it trades no fidelity away.
 *
 * `s_ws_margin_only` exists solely for the headless A/B (`--margin-sprites 0`),
 * which is how the difference is measured at all. */
static void sdb_ws_enable(void)
{
    sdb_ws_set_aspect(sdb_ws_aspect_from(getenv("SDB_WIDE"), "aspect"));
    /* Widescreen implies revealing the 8 clipped left-hand columns, same as it
     * implies margin sprites: they are only drawn by the compositor, so with
     * overscan left off the widest part of the new picture is a black gutter the
     * game asked for. Not a separate feature on purpose -- it was one, and the
     * two are not separable in practice: an overscan feature that is off while
     * widescreen is on produces the same frame with a black band at the margin
     * boundary, which is the thing widescreen exists to stop doing. The default
     * itself lives in sdb_ws_apply, so `--widescreen` and this path agree.
     *
     * SDB_OVERSCAN still forces the clip back OFF, for the headless A/B in
     * research 26 gate 5 (the "no black gutter" measurement needs the band to be
     * measurable in the first place). The launcher cannot turn it off. It is
     * read by sdb_ws_reveal_ask, so it works identically with and without
     * `--mods-root`. */
    /* Camera policy. SDB_CAMERA first for the headless A/B, then the launcher's
     * option, then follow -- the default is what every existing baseline was
     * measured against. Same order and same asymmetry as the aspect above: with
     * the feature enabled the launcher wins, and --camera only reaches the
     * compositor when widescreen is enabled from the command line. */
    {
        const char *cam = getenv("SDB_CAMERA");
        char buf[16] = "";
        int locked = 0;
        if (cam && cam[0]) locked = !strcmp(cam, "locked") || !strcmp(cam, "1");
        else if (nes_mod_option_value("super-dodgeball.widescreen", "widescreen",
                                      "camera", buf, sizeof buf))
            locked = !strcmp(buf, "locked") || !strcmp(buf, "1");
        sdb_ws_set_camera(locked);
    }
    if (!s_ws_margin_only) sdb_ms_enable();
}

/* Margin sprites: enable the two capture sites. Called by widescreen, because
 * the margins and the sprites in them are one decision. Presentation only --
 * each callback returns 0, the site's own instruction and the game's own cull
 * both run, and $0617 is not touched, so OAM and the guest's own picture are
 * bit-identical with this on or off. */
static void sdb_ms_enable(void)
{
    nes_mod_set_function_hook_enabled("super-dodgeball.margin.b5-8483", 1);
    nes_mod_set_function_hook_enabled("super-dodgeball.margin.b5-8302", 1);
    nes_mod_set_function_hook_enabled("super-dodgeball.margin.b5-8231", 1);
    s_ws_margin = 1;
    s_margin_on = 1;
}

static void sdb_ms_disable(void)
{
    nes_mod_set_function_hook_enabled("super-dodgeball.margin.b5-8483", 0);
    nes_mod_set_function_hook_enabled("super-dodgeball.margin.b5-8302", 0);
    nes_mod_set_function_hook_enabled("super-dodgeball.margin.b5-8231", 0);
    s_ws_margin = 0;
    s_margin_on = 0;
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
 * scopes it to MMC3 and MMC1, whose IRQ is write-triggered at $8000/$8001
 * and never clocked by the PPU. Measured against
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
        /* Not an error, and saying so was the bug. cyc_extra_scanlines_supported()
         * reads the cart's mapper, and the launcher enables features BEFORE the
         * ROM is attached, so at this point the mapper is still 0 and the gate
         * says no. power_on() re-applies s_oc_lines once the cart exists and it
         * succeeds; the old message claimed the backend had refused a setting
         * that was in fact applied, which reads as a broken feature and sends you
         * looking for a fault in the wrong place. Say what is actually true. */
        fprintf(stderr, "ppu-overclock: %d extra scanlines queued; the cart is not "
                        "attached yet, applying at power-on\n", lines);
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
        /* Same as the feature: widescreen brings the margin sprites with it. */
        if (!s_ws_margin_only) sdb_ms_enable();
        return true;
    }
    if (!strcmp(name, "--overscan")) {
        /* Declared as a bare flag, so the host passes no value at all (a NULL
         * here means "flag given", not "flag not given"). */
        sdb_ws_set_overscan(value == NULL || value[0] == '\0' || atoi(value) != 0);
        return true;
    }
    if (!strcmp(name, "--margin-sprites")) {
        /* NOT a feature: widescreen already draws the margin sprites. This is
         * the headless A/B switch, and it defaults to drawing them, so
         * `--margin-sprites 0` is the only way to get background-only margins
         * without editing a manifest. */
        const char *e = getenv("SDB_MARGIN_SPRITES");
        int on = e && e[0] ? atoi(e) != 0 : (value == NULL || value[0] == '\0' || atoi(value) != 0);
        s_ws_margin_only = !on;
        if (on) sdb_ms_enable();
        else s_ws_margin = s_margin_on = 0;
        return true;
    }
    if (!strcmp(name, "--camera")) {
        /* The camera policy, same asymmetry as the aspect: the launcher's
         * `camera` option wins whenever the widescreen feature is enabled. */
        if (value && !strcmp(value, "follow")) { sdb_ws_set_camera(0); return true; }
        if (value && !strcmp(value, "locked")) { sdb_ws_set_camera(1); return true; }
        fprintf(stderr, "--camera: needs a value, follow or locked\n");
        return false;
    }
    if (!strcmp(name, "--record-input")) {
        /* Opens before the first frame: options run at cyc_session_start(), and
         * power_on() after them, so nothing is lost and the header is on disk
         * even if the window is closed immediately. */
        if (!value || !value[0]) {
            fprintf(stderr, "--record-input: needs a file to write\n");
            return false;
        }
        if (s_rec) sdb_rec_close();          /* a second one replaces the first */
        return sdb_rec_start(value);
    }
    {
        /* Cheats are a separate translation unit; -1 means "not ours". Order
         * matters: an option given on the command line has to beat the launcher's
         * saved selection, and this runs after the mods activate. */
        int rc = sdb_cheats_option(name, value);
        if (rc >= 0) return rc != 0;
    }
    return false;
}

/* The real definition, here rather than at the forward declaration so
 * sizeof SDB_OPTIONS sees a complete type. */
static CycHostExtras SDB_EXTRAS = {
    .present = sdb_present,
    .power_on = sdb_power_on,
    .frame_begin = sdb_frame_begin,
    .frame_end = sdb_frame_end,
    .input = sdb_input,
    .options = SDB_OPTIONS,
    .option_count = sizeof SDB_OPTIONS / sizeof SDB_OPTIONS[0],
    .option = sdb_option,
    .tcp_setup = sdb_cheats_tcp_setup,
};

const CycHostExtras *cyc_host_extras(void) { return &SDB_EXTRAS; }

/* One report per run, on the way out. SDB_TRACE=1 turns it on. */
__attribute__((destructor)) static void sdb_report_at_exit(void)
{
    sdb_ws_report();
    sdb_cheats_report();
    sdb_rec_close();
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
        { 0x8483, sdb_margin_capture },
        { 0x8302, sdb_margin_capture },
        { 0x8231, sdb_margin_object },
    };
    if (!nes_mod_register_activation_plugin("super-dodgeball.character-flicker", sdb_enable) ||
        !nes_mod_register_activation_plugin("super-dodgeball.ppu-overclock", sdb_oc_enable) ||
        !nes_mod_register_activation_plugin("super-dodgeball.widescreen", sdb_ws_enable) ||
        !nes_mod_register_reset_callback(sdb_disable) ||
        !nes_mod_register_reset_callback(sdb_oc_disable) ||
        !nes_mod_register_reset_callback(sdb_ws_disable))
        fprintf(stderr, "sdb: mod registration failed\n");
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0]; i++)
        if (!nes_mod_register_function_entry_plugin(SDB_HOOK_IDS[i], sites[i].addr, sites[i].fn))
            fprintf(stderr, "sdb: hook registration failed for $%04X\n", sites[i].addr);
}