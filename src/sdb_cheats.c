/*
 * sdb_cheats.c - libretro / RetroArch cheats for Super Dodge Ball.
 *
 * The .cht FORMAT is RetroArch's, and it is the interchange format worth
 * supporting: a cheat file that works in a libretro NES core works here, so
 * libretro-database is a source of codes and not just ours.
 *
 * PROVENANCE, and it matters because this project is PolyForm Noncommercial
 * while the obvious references for this format are GPL:
 *   - The Game Genie decoder implements the CODE-MASTERS/GALOOB DEVICE's code
 *     encoding, from the published technical notes linked at sdb_decode_gg --
 *     a scheme description, not an implementation.
 *   - The Pro Action Replay / GameShark decoder implements that code layout's
 *     own convention.
 *   - The .cht parser implements the documented file layout.
 * No GPL source was transcribed and nothing here is derived from one. The
 * formats are not copyrightable and the decoders are ordinary implementations
 * of published specifications. The one third-party input is cheats/Super Dodge
 * Ball (USA).cht, whose thirteen original entries come from libretro-database;
 * each entry in it says where it came from, and NOTICE.md records the upstream
 * project's licence as a fact about that project rather than a claim over our
 * file, which is ours in part.
 *
 * THIS CHANGES THE GUEST. A cheat writes the machine's RAM (or substitutes a
 * byte the CPU reads out of PRG ROM), so unlike the widescreen and flicker
 * presentation paths there is no "presentation only" claim to make: with a
 * freeze armed the game computes different things. Hence the separate, default-
 * off super-dodge-ball.cheats package, and hence every gate here is "cheats OFF
 * is byte-identical to stock", not "cheats ON looks right".
 *
 * Two mechanisms, because the cycle backend has two ways to read a byte:
 *
 *   RAM write ($0000-$7FFF)      cyc_mod_poke, once per frame at frame_end.
 *                                A freeze: the game may change the byte during
 *                                the frame, and we put it back. This is exactly
 *                                what FCEU's periodic cheats do.
 *
 *   PRG ROM ($8000-$FFFF)        cyc_mod_set_rom_read_override, a substitution
 *                                on the value the bus delivers. FCEUmm cannot
 *                                do this one - it drops raw cheats aimed above
 *                                $7FFF - and a Game Genie code is defined as a
 *                                substitution on a ROM read, so this is the only
 *                                way to honour one.
 *
 * What a Game Genie code CANNOT do on a recompiled machine, and why: the
 * recompiler turns an opcode into a goto and an address operand into a
 * compile-time constant (recompiler/src/cyc_codegen.c). Those bytes are not on
 * the bus any more, so no read hook can reach them. Immediates and data
 * operands ARE read at run time and do respond - for this ROM, 3327 folded
 * operand reads and 1405 run-time reads both funnel through the two hook sites.
 * So a substitution reliably changes the constants the game compares against,
 * and cannot retarget an instruction. Patches/008 documents the measurement.
 *
 * Reported, never silently ignored: a code we cannot decode, a RAM address the
 * engine refuses (an I/O register, PRG ROM in a write cheat), and a .cht that
 * is missing. A cheat that loads and does nothing is worse than one that fails.
 */

#include "cyc_host_extras.h"
#include "cyc_mod.h"
#include "cyc_tcp.h"
#include "mod_runtime.h"
#include "recomp_runtime_ui.h"

#include "sdb_cheats.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDB_CHEAT_MAX      64
#define SDB_CHEAT_DESC     96
#define SDB_CHEAT_PATH     1024
#define SDB_CHEAT_CODE     256

/* The Game Genie's alphabet, from the technical notes at sdb_decode_gg: the
 * character at index i is the hex nibble i, which is why it is not hex order. */
static const char SDB_GG_ALPHA[17] = "APZLGITYEOXUKSVN";

/* One resolved cheat. */
typedef enum {
    SDB_CHEAT_WRITE = 0,  /* freeze: cyc_mod_poke every frame */
    SDB_CHEAT_COND,       /* freeze, but only while the byte still reads compare */
    SDB_CHEAT_SUBST       /* PRG ROM read substitution */
} SdbCheatKind;

typedef struct {
    SdbCheatKind kind;
    uint16_t addr;
    uint8_t  value;
    uint8_t  compare;    /* SDB_CHEAT_WRITE: unused */
    int      enabled;
    /* What the machine held at the instant this cheat was armed, so switching
     * it off can put it back. See sdb_cheat_arm. */
    uint8_t  saved;
    int      have_saved;
    int      armed;      /* have we applied this cheat yet? the transition is the
                          * thing that matters: save on the way in, restore on
                          * the way out */
    int      oneshot;    /* write once, then disarm WITHOUT restoring */
    int      fired;      /* a one-shot that has already fired */
    char     desc[SDB_CHEAT_DESC];
} SdbCheat;

static SdbCheat s_cheats[SDB_CHEAT_MAX];
static int s_cheat_count;
static int s_cheat_pokes;        /* per-frame freeze writes actually issued */
static int s_cheat_skipped;      /* COND cheats whose compare did not match */
static int s_cheat_declined;     /* addresses the engine refused */
static int s_cheat_rejected;     /* code strings we could not decode */
static int s_cheat_applied;      /* ROM substitutions armed */
static int s_cheat_restored;     /* RAM bytes put back on disarming */
static int s_cheat_fired;        /* one-shot cheats that have fired */
static int s_trace;
static char s_cheat_file[SDB_CHEAT_PATH];
/* --no-cheats: suppress the default file, so an explicit "I want stock" sticks
 * across power_on. Without it there is no way to say "no cheats" at all. */
static int s_suppressed;

static void sdb_cheat_build_menu(void);

/* ------------------------------------------------------------------------- *
 * helpers
 * ------------------------------------------------------------------------- */

static int sdb_hex(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        int c = (unsigned char)s[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

static int sdb_all_hex(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* ------------------------------------------------------------------------- *
 * code decoders
 * ------------------------------------------------------------------------- */

/* Game Genie, 6 or 8 characters.
 *
 * PROVENANCE. This implements the Code Masters / Galoob Game Genie's CODE
 * ENCODING -- a documented description of what the physical device accepts --
 * not the source of any emulator. See "NES Game Genie Technical Notes"
 * <https://tuxnes.sourceforge.net/gamegenie.html>, which gives the alphabet
 * and the three formulas below. Reimplemented from that description on
 * purpose: an implementation of a published scheme is not a derivative work of
 * whichever project happens to implement it too.
 *
 * The scheme: each character maps to a hex nibble via APZLGITYEOXUKSVN; the
 * low 3 bits of one nibble and the top bit of another are combined into a
 * nibble of the result, which is why the arithmetic looks shuffled. The address
 * is 15 bits because bit 15 is always set -- codes only ever address the top
 * half of the address space, $8000-$FFFF.
 */
static int sdb_decode_gg(const char *s, uint16_t *addr, uint8_t *value, uint8_t *compare)
{
    uint8_t n[8];
    size_t len = strlen(s);
    unsigned a, v, c;

    if (len != 6 && len != 8) return 0;
    for (size_t i = 0; i < len; i++) {
        const char *p = strchr(SDB_GG_ALPHA, toupper((unsigned char)s[i]));
        if (!p || !*p) return 0;
        n[i] = (uint8_t)(p - SDB_GG_ALPHA);
    }

    /* address = 0x8000 + (n3&7)<<12 | (n5&7)<<8 | (n4&8)<<8
     *                   | (n2&7)<<4 | (n1&8)<<4 | (n4&7)  | (n3&8) */
    a = 0x8000u
      | ((unsigned)(n[3] & 7) << 12)
      | ((unsigned)(n[5] & 7) <<  8)
      | ((unsigned)(n[4] & 8) <<  8)
      | ((unsigned)(n[2] & 7) <<  4)
      | ((unsigned)(n[1] & 8) <<  4)
      |  (unsigned)(n[4] & 7)
      |  (unsigned)(n[3] & 8);

    /* data: identical except bit 3 comes from n7 on the 8-character form and
     * from n5 on the 6-character one. */
    v =   ((unsigned)(n[1] & 7) << 4)
      |  ((unsigned)(n[0] & 8) << 4)
      |   (unsigned)(n[0] & 7)
      |   (unsigned)(len == 8 ? (n[7] & 8) : (n[5] & 8));

    c = 0;
    if (len == 8)
        /* compare = (n7&7)<<4 | (n6&8)<<4 | (n6&7) | (n5&8) */
        c = ((unsigned)(n[7] & 7) << 4)
          | ((unsigned)(n[6] & 8) << 4)
          |  (unsigned)(n[6] & 7)
          |  (unsigned)(n[5] & 8);

    *addr = (uint16_t)a;
    *value = (uint8_t)v;
    *compare = (uint8_t)c;
    return 1;
}

/* Pro Action Replay / GameShark, 8 hex digits, read as four bytes: the address
 * is bytes 1-2 and the value is byte 3, so the LEADING byte is discarded. That
 * is the layout's own convention -- the first pair is not an address -- and not
 * a quirk of this implementation. A code aimed above $8000 becomes a read
 * substitution rather than a write, since a write there would be refused. */
static int sdb_decode_par(const char *s, uint16_t *addr, uint8_t *value)
{
    if (strlen(s) != 8 || !sdb_all_hex(s, 8)) return 0;
    *addr  = (uint16_t)((sdb_hex(s + 2, 2) << 8) | sdb_hex(s + 4, 2));
    *value = (uint8_t)sdb_hex(s + 6, 2);
    return 1;
}

/* The two raw spellings the .cht format uses for RAM:
 *   AAAA:VV     length 7,  ':'  at index 4        -> hold the byte at this value
 *   AAAA?CC:VV  length 10, '?'  at index 4, ':' at 7 -> hold it, but only while the
 *                                                   byte still reads the compare
 * The kind is decided by the caller, not here: an address in PRG ROM cannot be
 * written, so the same spelling means a read substitution up there. */
static int sdb_decode_raw(const char *s, SdbCheatKind *kind,
                          uint16_t *addr, uint8_t *value, uint8_t *compare)
{
    size_t n = strlen(s);
    if (n == 7 && s[4] == ':') {
        if (!sdb_all_hex(s, 4) || !sdb_all_hex(s + 5, 2)) return 0;
        *addr = (uint16_t)sdb_hex(s, 4);
        *value = (uint8_t)sdb_hex(s + 5, 2);
        *compare = 0;
        *kind = SDB_CHEAT_WRITE;
        return 1;
    }
    if (n == 10 && s[4] == '?' && s[7] == ':') {
        if (!sdb_all_hex(s, 4) || !sdb_all_hex(s + 5, 2) || !sdb_all_hex(s + 8, 2)) return 0;
        *addr = (uint16_t)sdb_hex(s, 4);
        *compare = (uint8_t)sdb_hex(s + 5, 2);
        *value = (uint8_t)sdb_hex(s + 8, 2);
        *kind = SDB_CHEAT_COND;
        return 1;
    }
    return 0;
}

/* One token, in FCEUmm's order of attempts. */
static int sdb_decode_one(const char *tok, SdbCheatKind *kind,
                          uint16_t *addr, uint8_t *value, uint8_t *compare)
{
    uint16_t a;
    uint8_t v, c = 0;

    if (sdb_decode_raw(tok, kind, addr, value, compare)) {
        /* Above $8000 the engine refuses a write, and a Game Genie code is a
         * read substitution anyway, so anything in PRG ROM substitutes. */
        if (*addr >= 0x8000) *kind = SDB_CHEAT_SUBST;
        return 1;
    }
    if (sdb_decode_gg(tok, &a, &v, &c)) {
        /* GG always addresses $8000-$BFFF; it is a read substitution by
         * definition. An 8-character code's compare is evaluated once, when the
         * cheat is armed - re-testing it on every read would be self-fulfilling,
         * since the substitution is what would make it no longer match. */
        *kind = SDB_CHEAT_SUBST;
        *addr = a;
        *value = v;
        *compare = c;
        return 1;
    }
    if (sdb_decode_par(tok, &a, &v)) {
        *kind = a >= 0x8000 ? SDB_CHEAT_SUBST : SDB_CHEAT_WRITE;
        *addr = a;
        *value = v;
        *compare = 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- *
 * the cheat list
 * ------------------------------------------------------------------------- */

static int sdb_cheat_room(void)
{
    if (s_cheat_count < SDB_CHEAT_MAX) return 1;
    fprintf(stderr, "cheats: table full at %d entries; ignoring the rest\n", SDB_CHEAT_MAX);
    return 0;
}

/* One code string may hold several codes joined by any of + , ; . _ or a
 * space, so "--cheat 058B:FF;0553:FF" and a .cht line doing the same thing are
 * the same call. Splitting on those is what lets a single .cht entry arm a
 * whole team at once. */
static void sdb_add_code_flags(const char *code, const char *desc, int enabled,
                               int oneshot);

static void sdb_add_code(const char *code, const char *desc, int enabled)
{
    sdb_add_code_flags(code, desc, enabled, 0);
}

static void sdb_add_code_flags(const char *code, const char *desc, int enabled,
                               int oneshot)
{
    char buf[SDB_CHEAT_CODE];
    char *tok, *save = NULL;

    if (!code || !*code) return;
    snprintf(buf, sizeof buf, "%s", code);

    for (tok = strtok_r(buf, "+,;._ ", &save); tok; tok = strtok_r(NULL, "+,;._ ", &save)) {
        SdbCheatKind kind;
        uint16_t addr;
        uint8_t value, compare;

        if (!sdb_decode_one(tok, &kind, &addr, &value, &compare)) {
            s_cheat_rejected++;
            fprintf(stderr, "cheats: cannot decode \"%s\" (in \"%s\")\n", tok,
                    desc && *desc ? desc : code);
            continue;
        }
        if (!sdb_cheat_room()) break;

        SdbCheat *c = &s_cheats[s_cheat_count++];
        c->kind = kind;
        c->addr = addr;
        c->value = value;
        c->compare = compare;
        c->enabled = enabled;
        c->saved = 0;
        c->have_saved = 0;
        c->armed = 0;
        c->oneshot = oneshot;
        c->fired = 0;
        snprintf(c->desc, sizeof c->desc, "%s", desc && *desc ? desc : tok);
    }
}

/* Arm the ROM substitutions. Called when a cheat set is built and again at
 * power-on, because a compare is only meaningful against the image as mapped
 * at that moment. */
static void sdb_cheat_arm(void)
{
    cyc_mod_clear_rom_read_overrides();
    s_cheat_applied = 0;
    s_cheat_declined = 0;
    s_cheat_restored = 0;

    for (int i = 0; i < s_cheat_count; i++) {
        SdbCheat *c = &s_cheats[i];

        /* A RAM freeze writes its byte EVERY FRAME, so switching it off has to
         * put something back. Merely stopping the writes is not enough, and the
         * reason is specific to this game: $06AE (the captain-face code) is
         * loaded when a character is drawn and then never written again, so the
         * frozen value simply stayed and the face stayed modified with the row
         * reading Off. A toggle that says Off while the effect is still on
         * screen is worse than no toggle at all.
         *
         * Restore what the machine held at the instant the cheat was armed --
         * NOT the value the cheat wanted. That is an approximation and the best
         * one available: the game may legitimately have changed the byte in
         * between, and there is no way to know what it would have been. It is
         * still strictly better than leaving the cheat's value in place.
         *
         * A ROM substitution needs none of this. The image was never written,
         * so dropping the override restores the real byte for free -- which is
         * why this half of the fix is only about RAM.
         */
        if (c->kind != SDB_CHEAT_SUBST) {
            int want = c->enabled && c->addr < 0x8000;
            if (!want && c->armed && c->have_saved && !(c->oneshot && c->fired)) {
                if (cyc_mod_poke(c->addr, c->saved)) {
                    s_cheat_restored++;
                    if (s_trace)
                        fprintf(stderr, "cheats: turned \"%s\" off, restored $%04X "
                                        "to $%02X\n", c->desc, c->addr, c->saved);
                } else {
                    fprintf(stderr, "cheats: turned \"%s\" off but the engine "
                                    "refused to restore $%04X; it keeps $%02X\n",
                            c->desc, c->addr, c->value);
                }
            }
            /* The restore point is NOT captured here. It is captured in
             * frame_end, immediately before the first write that clobbers it,
             * because "when the cheat was armed" is often much too early to be
             * useful: a .cht entry that ships enable = true arms at power_on,
             * while the byte still holds whatever power-on left there, and that
             * is not what the game had by the time anyone noticed the effect.
             * Capturing one instruction before the overwrite is the only moment
             * that knows. */
            c->armed = want;
            continue;
        }

        if (!c->enabled) continue;
        if (c->compare && cyc_mod_peek(c->addr) != c->compare) continue;  /* one-shot */
        if (!cyc_mod_set_rom_read_override(c->addr, c->value)) {
            fprintf(stderr, "cheats: no room for the substitution at $%04X\n", c->addr);
            s_cheat_declined++;
            continue;
        }
        s_cheat_applied++;
    }
}

static void sdb_cheat_reset(void)
{
    /* Restore first: the counters this reports are wiped on the next lines, and
     * a restore that increments a counter it then zeroes reads as no restore. */
    for (int i = 0; i < s_cheat_count; i++) {
        SdbCheat *c = &s_cheats[i];
        if (c->armed && c->have_saved && cyc_mod_poke(c->addr, c->saved)) {
            s_cheat_restored++;
            if (s_trace)
                fprintf(stderr, "cheats: dropped \"%s\", restored $%04X to $%02X\n",
                        c->desc, c->addr, c->saved);
        }
    }
    s_cheat_count = 0;
    s_cheat_pokes = 0;
    s_cheat_skipped = 0;
    s_cheat_declined = 0;
    s_cheat_rejected = 0;
    s_cheat_applied = 0;
    s_cheat_restored = 0;
    s_cheat_file[0] = '\0';
    cyc_mod_clear_rom_read_overrides();
}

/* ------------------------------------------------------------------------- *
 * the .cht file
 * ------------------------------------------------------------------------- */

static char *sdb_cht_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    if (end - s >= 2 && ((*s == '"' && end[-1] == '"') || (*s == '\'' && end[-1] == '\''))) {
        *--end = '\0';
        s++;
    }
    return s;
}

/* The .cht file layout: `cheats = N`, then per entry cheatN_desc, cheatN_code
 * and cheatN_enable, in any order, plus an optional structured form
 * (cheatN_address + cheatN_value) for when a code has already been resolved
 * into an address. Both are read, the structured form winning when present.
 * Values may be quoted; comments start at #. Unknown keys are ignored, which
 * is what lets cheatN_once below be additive rather than a competing spelling. */
static int sdb_load_cht(const char *path)
{
    static char desc[SDB_CHEAT_MAX][SDB_CHEAT_DESC];
    static char code[SDB_CHEAT_MAX][SDB_CHEAT_CODE];
    static long  addr[SDB_CHEAT_MAX];
    static long  value[SDB_CHEAT_MAX];
    static int   enable[SDB_CHEAT_MAX];
    static int   once[SDB_CHEAT_MAX];
    static int   have_addr[SDB_CHEAT_MAX];
    static int   declared = -1;
    FILE *f;
    char line[SDB_CHEAT_CODE + SDB_CHEAT_DESC + 64];
    int rc = 0;

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cheats: cannot open %s\n", path);
        return 0;
    }

    for (int i = 0; i < SDB_CHEAT_MAX; i++) {
        desc[i][0] = code[i][0] = '\0';
        addr[i] = value[i] = 0;
        enable[i] = once[i] = have_addr[i] = 0;
    }

    while (fgets(line, sizeof line, f)) {
        char *eq, *key, *val, *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = sdb_cht_trim(line);
        val = sdb_cht_trim(eq + 1);

        if (!strcmp(key, "cheats")) {
            declared = (int)strtol(val, NULL, 0);
            continue;
        }
        if (strncmp(key, "cheat", 5) || !isdigit((unsigned char)key[5])) continue;
        {
            char *end;
            long idx = strtol(key + 5, &end, 10);
            const char *field = end;
            if (*field != '_' || idx < 0 || idx >= SDB_CHEAT_MAX) continue;
            field++;
            if (!strcmp(field, "desc")) snprintf(desc[idx], SDB_CHEAT_DESC, "%s", val);
            else if (!strcmp(field, "code")) snprintf(code[idx], SDB_CHEAT_CODE, "%s", val);
            else if (!strcmp(field, "enable"))
                enable[idx] = (!strcmp(val, "true") || !strcmp(val, "1"));
            else if (!strcmp(field, "once"))
                /* An addition to the format, not a new spelling: RetroArch's
                 * loader skips keys it does not know, so a .cht using it still
                 * loads there (the entry just freezes, as it always would). */
                once[idx] = (!strcmp(val, "true") || !strcmp(val, "1"));
            else if (!strcmp(field, "address")) { addr[idx] = strtol(val, NULL, 0); have_addr[idx] = 1; }
            else if (!strcmp(field, "value")) value[idx] = strtol(val, NULL, 0);
        }
    }
    fclose(f);

    for (int i = 0; i < SDB_CHEAT_MAX; i++) {
        if (have_addr[i]) {
            /* The structured form: an address the frontend resolved for us. The
             * engine takes it verbatim; $8000-and-up is a read substitution
             * because a write there is refused. */
            SdbCheat *c;
            if (!sdb_cheat_room()) break;
            c = &s_cheats[s_cheat_count++];
            c->kind = addr[i] >= 0x8000 ? SDB_CHEAT_SUBST : SDB_CHEAT_WRITE;
            c->addr = (uint16_t)addr[i];
            c->value = (uint8_t)value[i];
            c->compare = 0;
            c->enabled = enable[i];
            c->saved = 0;
            c->have_saved = 0;
            c->armed = 0;
            c->oneshot = once[i];
            c->fired = 0;
            snprintf(c->desc, sizeof c->desc, "%s", desc[i][0] ? desc[i] : "structured");
            continue;
        }
        if (!code[i][0]) continue;
        sdb_add_code_flags(code[i], desc[i], enable[i], once[i]);
    }

    if (declared >= 0 && declared != s_cheat_count)
        fprintf(stderr, "cheats: %s declares %d cheats, %d loaded\n", path, declared,
                s_cheat_count);

    snprintf(s_cheat_file, sizeof s_cheat_file, "%s", path);
    sdb_cheat_arm();
    sdb_cheat_build_menu();
    return 1;
}

/* ------------------------------------------------------------------------- *
 * application
 * ------------------------------------------------------------------------- */

void sdb_cheats_power_on(void)
{
    /* Load the default cheat file when nothing else supplied one, so the
     * in-game Cheats section is simply there. This used to be gated behind the
     * super-dodge-ball.cheats feature, which meant the player had to switch
     * "cheats" on and then pick individual codes, and doing only the first did
     * nothing. The gate bought nothing: no row armed means no write, so "every
     * row off" is the same default-off guarantee stated per cheat.
     *
     * --cheats names its own file, so it lands here already loaded. */
    if (!s_cheat_count && !s_suppressed) sdb_cheats_load_default();

    /* A reset invalidated the restore point: the byte's value from before the
     * reset is stale, and the game's own post-reset value is the better thing
     * to put back. Re-arm from scratch so the next disarm restores that. */
    for (int i = 0; i < s_cheat_count; i++) {
        s_cheats[i].armed = 0;
        s_cheats[i].have_saved = 0;
    }

    /* A save state or a reset puts RAM back without the substitutions being
     * re-evaluated, and the host may build its menu either side of a reset. */
    sdb_cheat_arm();
    sdb_cheat_build_menu();
}

void sdb_cheats_frame_end(void)
{
    s_cheat_pokes = 0;
    s_cheat_skipped = 0;
    if (!s_cheat_count) return;

    for (int i = 0; i < s_cheat_count; i++) {
        SdbCheat *c = &s_cheats[i];
        if (!c->enabled || c->kind == SDB_CHEAT_SUBST) continue;
        if (c->kind == SDB_CHEAT_COND && cyc_mod_peek(c->addr) != c->compare) {
            s_cheat_skipped++;
            continue;
        }
        /* Take the restore point on the first write that actually clobbers,
         * so switching this off later can put the game's own value back rather
         * than whatever the byte happened to hold when it was armed. */
        if (c->armed && !c->have_saved && cyc_mod_peek_ok(c->addr, &c->saved)) {
            c->have_saved = 1;
            if (s_trace)
                fprintf(stderr, "cheats: \"%s\" armed, keeping $%02X from $%04X "
                                "to restore on disarm\n", c->desc, c->saved, c->addr);
        }
        if (cyc_mod_poke(c->addr, c->value)) s_cheat_pokes++;
        else if (!s_cheat_declined++)    /* count once; the address never changes */
            fprintf(stderr, "cheats: the engine refuses a write to $%04X (%s)\n",
                    c->addr, c->desc);

        /* A one-shot writes for exactly one frame and then switches itself off,
         * and it must NOT restore: the write is the event the game reacts to,
         * and putting the byte back on the next frame would erase it. This is
         * the difference that decides whether a "defeat" cheat works at all --
         * the game reacts to the TRANSITION to the new value, so a freeze at
         * that value never triggers anything, which is exactly why freezing all
         * six opponents' energy at zero did not end the match. */
        if (c->oneshot) {
            c->fired = 1;
            c->enabled = 0;
            c->armed = 0;
            s_cheat_fired++;
            if (s_trace)
                fprintf(stderr, "cheats: \"%s\" fired once ($%04X = $%02X) and "
                                "switched itself off\n", c->desc, c->addr, c->value);
        }
    }
}

void sdb_cheats_report(void)
{
    if (!s_trace) return;
    if (!s_cheat_count && !s_cheat_rejected) {
        fprintf(stderr, "cheats: none loaded\n");
        return;
    }
    fprintf(stderr, "cheats: %d loaded%s%s, %d ROM substitutions armed, %d freeze "
                    "writes, %d conditionals skipped, %d restored on disarm, "
                    "%d one-shots fired, %d refused, %d undecodable\n",
            s_cheat_count,
            s_cheat_file[0] ? " from " : " from --cheat only",
            s_cheat_file[0] ? s_cheat_file : "",
            s_cheat_applied, s_cheat_pokes, s_cheat_skipped, s_cheat_restored,
            s_cheat_fired, s_cheat_declined, s_cheat_rejected);
}

/* ------------------------------------------------------------------------- *
 * host wiring
 * ------------------------------------------------------------------------- */

/* RetroArch looks in a `cheats` directory beside the content, so we do too:
 * the launcher runs nes_game with build-cycle/ as its working directory. This
 * is the launcher's default, not a CLI form - `--cheats` takes a value, because
 * a developer running a headless A/B wants to say WHICH file and not have one
 * picked for them by a directory listing. */
int sdb_cheats_load_default(void)
{
    static const char *names[] = {
        "cheats/Super Dodge Ball (USA).cht",
        "cheats/super-dodge-ball.cht",
        "../cheats/Super Dodge Ball (USA).cht",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        FILE *f = fopen(names[i], "r");
        if (f) { fclose(f); return sdb_load_cht(names[i]); }
    }
    fprintf(stderr, "cheats: no cheat file found (looked for cheats/Super Dodge "
                    "Ball (USA).cht and two variants)\n");
    return 0;
}

/* Returns false for a value we refuse, which stops the program (exit 2). */
int sdb_cheats_option(const char *name, const char *value)
{
    if (!strcmp(name, "--cheat")) {
        if (!value || !*value) {
            fprintf(stderr, "--cheat: needs at least one code, e.g. --cheat 058B:FF\n");
            return 0;
        }
        s_suppressed = 0;
        sdb_add_code(value, value, 1);
        sdb_cheat_arm();
        sdb_cheat_build_menu();
        return 1;
    }
    if (!strcmp(name, "--cheats")) {
        sdb_cheat_reset();
        s_suppressed = 0;
        return sdb_load_cht(value);
    }
    if (!strcmp(name, "--no-cheats")) {
        sdb_cheat_reset();
        s_suppressed = 1;
        return 1;
    }
    return -1;   /* not ours */
}

void sdb_cheats_set_trace(int on) { s_trace = on; }

int sdb_cheats_enabled(void) { return s_cheat_count; }

/* ---- the `cheat` TCP command ----
 *
 * The menu is a mouse-driven widget, so there is no way to exercise a toggle
 * from a script, and "turning a cheat off" is exactly the path that shipped
 * broken. This makes it drivable:
 *
 *   cheat list      what is loaded, and what each is
 *   cheat on N      arm entry N
 *   cheat off N     disarm entry N, restoring a RAM freeze's byte
 *   cheat offall    disarm every entry
 */
static void sdb_cheat_tcp_list(int id)
{
    char fields[4096];
    size_t used = 0;
    fields[0] = '\0';

    for (int i = 0; i < s_cheat_count && used < sizeof fields - 256; i++) {
        SdbCheat *c = &s_cheats[i];
        char one[256], desc[96];
        const char *k = c->kind == SDB_CHEAT_SUBST ? "subst" :
                        c->kind == SDB_CHEAT_COND   ? "cond"  : "freeze";
        /* The description is the player's text and goes in a JSON string. */
        cyc_tcp_quote(c->desc, desc, sizeof desc);
        snprintf(one, sizeof one, "%s\"%d\":{\"desc\":%s,\"kind\":\"%s\","
                 "\"addr\":\"$%04X\",\"value\":\"$%02X\",\"compare\":\"$%02X\","
                 "\"on\":%s}",
                 used ? "," : "", i, desc, k, c->addr, c->value, c->compare,
                 c->enabled ? "true" : "false");
        used += (size_t)snprintf(fields + used, sizeof fields - used, "%s", one);
    }
    cyc_tcp_ok(id, fields);
}

static void sdb_cheat_tcp(int id, const char *line)
{
    /* NOTE: the verb is NOT "cmd". cyc_tcp has already consumed that member to
     * dispatch to this handler, so reading "cmd" back out of the line finds
     * nothing and every request falls through to the usage message. */
    char op[32];
    if (!cyc_tcp_str(line, "op", op, sizeof op)) op[0] = '\0';

    if (!strcmp(op, "list")) { sdb_cheat_tcp_list(id); return; }

    if (!strcmp(op, "offall")) {
        for (int i = 0; i < s_cheat_count; i++) s_cheats[i].enabled = 0;
        sdb_cheat_arm();
        char f[64];
        snprintf(f, sizeof f, "\"restored\":%d", s_cheat_restored);
        cyc_tcp_ok(id, f);
        return;
    }

    int on;
    if (!strcmp(op, "on")) on = 1;
    else if (!strcmp(op, "off")) on = 0;
    else { cyc_tcp_err(id, "cheat: op list|on|off|offall, plus index for on/off"); return; }

    long idx;
    if (!cyc_tcp_long(line, "index", &idx)) {
        cyc_tcp_err(id, "cheat: on/off needs an index");
        return;
    }
    if (idx < 0 || idx >= s_cheat_count) {
        char f[96];
        snprintf(f, sizeof f, "cheat: no entry %ld (have %d)", idx, s_cheat_count);
        cyc_tcp_err(id, f);
        return;
    }

    s_cheats[idx].enabled = on;
    s_cheats[idx].fired = 0;      /* re-arming a one-shot re-arms the cheat */
    sdb_cheat_arm();

    SdbCheat *c = &s_cheats[idx];
    char f[192];
    uint8_t now = c->kind == SDB_CHEAT_SUBST ? 0 : cyc_mod_peek(c->addr);
    snprintf(f, sizeof f,
             "\"index\":%ld,\"on\":%s,\"addr\":\"$%04X\",\"ram\":\"$%02X\","
             "\"saved\":\"$%02X\",\"restored\":%d",
             idx, on ? "true" : "false", c->addr, now, c->saved, s_cheat_restored);
    cyc_tcp_ok(id, f);
}

void sdb_cheats_tcp_setup(void)
{
    cyc_tcp_register("cheat",
                     "cheats: op list|on|off|offall, index for on/off",
                     sdb_cheat_tcp);
}

/* ------------------------------------------------------------------------- *
 * the in-game menu: one BOOL row per cheat
 * ------------------------------------------------------------------------- */

/* cyc_ui_menu_create() copies the item array ONCE, so the label and description
 * strings it points at have to live until the menu is destroyed. Static pools,
 * not stack buffers. */
static char s_label_pool[SDB_CHEAT_MAX][SDB_CHEAT_DESC];
static char s_desc_pool[SDB_CHEAT_MAX][160];
static char s_key_pool[SDB_CHEAT_MAX][24];
static RecompRuntimeUiItem s_menu_items[SDB_CHEAT_MAX];
static int s_menu_built;

/* What the row says it does, in the machine's own terms. A row that only said
 * "Infinite Health" would be repeating the file's claim; this says what the code
 * actually is, which is the thing a reader can check. */
static void sdb_cheat_describe(const SdbCheat *c, char *out, size_t n)
{
    switch (c->kind) {
        case SDB_CHEAT_WRITE:
            if (c->oneshot)
                snprintf(out, n, "Write $%02X to $%04X once, then switch itself "
                                 "off. For an event, not a level: the game has to "
                                 "SEE the value change.", c->value, c->addr);
            else
                snprintf(out, n, "Freeze $%04X to $%02X every frame. Changes guest RAM.",
                         c->addr, c->value);
            break;
        case SDB_CHEAT_COND:
            snprintf(out, n, "While $%04X still reads $%02X, write $%02X. "
                             "Changes guest RAM.", c->addr, c->compare, c->value);
            break;
        case SDB_CHEAT_SUBST:
            if (c->compare)
                snprintf(out, n, "If the byte at $%04X reads $%02X, the CPU reads "
                                 "$%02X instead. Does not touch RAM.", c->addr,
                         c->compare, c->value);
            else
                snprintf(out, n, "The CPU reads $%02X at $%04X instead. "
                                 "Does not touch RAM.", c->value, c->addr);
            break;
    }
}

/* Rebuild the rows. Cheap and idempotent: called when a cheat set is loaded and
 * from power_on, because the host may build its menu either side of a reset. */
static void sdb_cheat_build_menu(void)
{
    s_menu_built = s_cheat_count;
    for (int i = 0; i < s_cheat_count; i++) {
        SdbCheat *c = &s_cheats[i];

        snprintf(s_label_pool[i], SDB_CHEAT_DESC, "%s", c->desc);
        sdb_cheat_describe(c, s_desc_pool[i], sizeof s_desc_pool[i]);
        snprintf(s_key_pool[i], sizeof s_key_pool[i], "sdb.cheat.%d", i);

        RecompRuntimeUiItem *it = &s_menu_items[i];
        memset(it, 0, sizeof *it);
        it->key = s_key_pool[i];            /* the host copies the pointer */
        it->section = "Cheats";             /* must not start with "cyc." */
        it->label = s_label_pool[i];
        it->description = s_desc_pool[i];
        it->type = RECOMP_RUNTIME_UI_BOOL;
        it->minimum = 0;
        it->maximum = 1;
        it->step = 1;
    }
    if (s_trace)
        fprintf(stderr, "cheats: %d rows built for the in-game Cheats section "
                        "(first: \"%s\" -- %s)\n", s_menu_built,
                s_menu_built > 0 ? s_label_pool[0] : "", 
                s_menu_built > 0 ? s_desc_pool[0] : "");
}

/* The key carries the row's index, so get/set need no side table. */
static int sdb_cheat_index(const RecompRuntimeUiItem *item)
{
    const char *k = item ? item->key : NULL;
    if (!k || strncmp(k, "sdb.cheat.", 10)) return -1;
    int i = atoi(k + 10);
    return (i >= 0 && i < s_cheat_count) ? i : -1;
}

static int sdb_cheat_get(void *ctx, const RecompRuntimeUiItem *item, int *value_out)
{
    (void)ctx;
    int i = sdb_cheat_index(item);
    if (i < 0) return 0;
    *value_out = s_cheats[i].enabled ? 1 : 0;
    return 1;
}

static int sdb_cheat_set(void *ctx, const RecompRuntimeUiItem *item, int value)
{
    (void)ctx;
    int i = sdb_cheat_index(item);
    if (i < 0) return 0;
    s_cheats[i].enabled = value != 0;
    s_cheats[i].fired = 0;         /* re-arming a one-shot re-arms the cheat */
    /* Re-arm the ROM substitutions so turning one off takes it out of the
     * table, and the compare an armed Game Genie code carries is tested against
     * the image as mapped right now. */
    sdb_cheat_arm();
    return 1;
}

static int sdb_cheat_row_enabled(void *ctx, const RecompRuntimeUiItem *item)
{
    (void)ctx;
    return sdb_cheat_index(item) >= 0;
}

static const RecompRuntimeUiCallbacks SDB_CHEAT_MENU_CB = {
    .get_value = sdb_cheat_get,
    .set_value = sdb_cheat_set,
    .is_enabled = sdb_cheat_row_enabled,
};

const struct RecompRuntimeUiItem *sdb_cheats_menu_items(size_t *count)
{
    if (count) *count = (size_t)(s_menu_built > 0 ? s_menu_built : 0);
    return s_menu_built > 0 ? s_menu_items : NULL;
}

const struct RecompRuntimeUiCallbacks *sdb_cheats_menu_callbacks(void)
{
    return s_menu_built > 0 ? &SDB_CHEAT_MENU_CB : NULL;
}