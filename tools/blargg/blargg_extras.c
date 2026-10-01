/* blargg_extras.c — generic NESRecomp harness for blargg test ROMs.
 *
 * Unlike a game port's extras.c (which carries game-specific hooks), this
 * file is deliberately neutral: it runs the recompiled RESET/NMI paths with
 * stock runner semantics and polls the blargg $6000 completion protocol.
 *
 * Protocol (see instr_test-v5/readme.txt, "Output at $6000"):
 *   $6001-$6003 = $DE $B0 $61  (magic: "a blargg test is running")
 *   $6000 = $80                 test running
 *   $6000 = $81                 reset requested (noted, not serviced — v1)
 *   $6000 = $00-$7F             test completed with that result code
 *   $6004+ = NUL-terminated ASCII result text
 *
 * Verdict is printed as a single [BLARGG] JSON line on stdout and the
 * process exits immediately (PASS=0, FAIL=1, TIMEOUT=2). The runner's
 * run_blargg.py parses that line; any exit without it is TIMEOUT/HANG/ERROR.
 *
 * Patch-neutrality notes (why this file exists separately from src/extras.c):
 *   - No NMI-tunnel arming (g_tunnel_site stays 0 = disabled), so patch 004
 *     cannot fire here. A PASS means the tunnel machinery is inert by default.
 *   - No PPUMASK/PPUCTRL forcing and no g_ppumask_translate opt-in: PPU mask
 *     behavior stays faithful, so CPU/PPU tests measure the runner, not hacks.
 *   - No dispatch overrides and no replaced functions: every JMP/JSR target
 *     must resolve through normal discovery + the interp fallback, which is
 *     exactly the code patches 002/003 guard. Dispatch misses and BRKs are
 *     reported in the JSON (misses/brks fields) for regression diffing.
 *
 * Env knobs (set by run_blargg.py, all optional):
 *   BLARGG_TEST_NAME   label echoed in the JSON (default "unknown")
 *   BLARGG_MAX_FRAMES  frames before TIMEOUT verdict (default 3600 = ~60s)
 */
#include "game_extras.h"
#include "nes_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

extern void func_RESET(void);
extern void func_NMI(void);

static const char *s_test_name = "unknown";
static uint64_t s_max_frames = 3600;
static int s_reset_seen = 0;
static int s_verdict_done = 0;
static int s_magic_seen = 0;
static pthread_mutex_t s_verdict_lock = PTHREAD_MUTEX_INITIALIZER;

/* Copy up to cap bytes of NUL-terminated guest text at $6004 (g_sram[4])
 * into out, replacing non-printables with '?'. 0x00 and 0xFF both
 * terminate: fresh SRAM is 0xFF, so trailing unwritten bytes are not text.
 * Always NUL-terminates. */
static void copy_detail(char *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 4; i < 0x2000 && n + 1 < cap; i++) {
        uint8_t c = g_sram[i];
        if (c == 0 || c == 0xFF)
            break;
        out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        if (n >= 512)
            break;
    }
    out[n] = '\0';
}

static void emit_verdict(const char *status, int exit_code, uint8_t code) {
    char detail[513];
    pthread_mutex_lock(&s_verdict_lock);
    if (s_verdict_done) {
        pthread_mutex_unlock(&s_verdict_lock);
        return;
    }
    s_verdict_done = 1;
    pthread_mutex_unlock(&s_verdict_lock);
    copy_detail(detail, sizeof(detail));
    /* Escape backslash/quote for JSON (detail is already printable ASCII). */
    char esc[1025];
    size_t m = 0;
    for (size_t i = 0; detail[i] && m + 2 < sizeof(esc); i++) {
        if (detail[i] == '"' || detail[i] == '\\')
            esc[m++] = '\\';
        esc[m++] = detail[i];
    }
    esc[m] = '\0';
    printf("[BLARGG] {\"name\": \"%s\", \"status\": \"%s\", \"code\": %u, "
           "\"frames\": %llu, \"misses\": %u, \"brks\": %llu, "
           "\"reset_seen\": %d, \"detail\": \"%s\"}\n",
           s_test_name, status, (unsigned)code,
           (unsigned long long)g_frame_count, g_miss_count_any,
           (unsigned long long)g_brk_count, s_reset_seen, esc);
    fflush(stdout);
    nesrecomp_expect_process_exit();
    exit(exit_code);
}

/* Return 1 and fill *code if a final verdict is observable in SRAM. */
static int poll_sram(uint8_t *code) {
    if (g_sram[1] != 0xDE || g_sram[2] != 0xB0 || g_sram[3] != 0x61)
        return 0;
    if (!s_magic_seen) {
        s_magic_seen = 1;
        fprintf(stderr, "[BLARGG] magic seen (frame %llu)\n",
                (unsigned long long)g_frame_count);
    }
    uint8_t st = g_sram[0];
    if (st == 0x80)
        return 0;
    if (st == 0x81) {
        s_reset_seen = 1; /* v1: note it, keep waiting (no soft-reset API) */
        return 0;
    }
    *code = st;
    return 1;
}

/* Wall-clock poller: blargg CPU tests run to completion on the main thread
 * and may never advance a frame (no NMI needed, `forever` parks with NMI
 * off), so verdict observation cannot wait for frame boundaries. */
static void *poller_thread(void *arg) {
    (void)arg;
    for (;;) {
        usleep(50000);
        uint8_t code = 0;
        if (poll_sram(&code)) {
            if (code == 0x00)
                emit_verdict("PASS", 0, code);
            else
                emit_verdict("FAIL", 1, code);
        }
    }
    return NULL;
}

const char *game_get_name(void) { return "blargg-harness"; }
const char *g_rom_path_for_extras = "";

void game_on_init(void) {
    /* Stock semantics: no tunnel arming, no mask translation, no overrides.
     * Whatever the linked runner does by default is what gets measured. */
    const char *name = getenv("BLARGG_TEST_NAME");
    if (name && name[0])
        s_test_name = name;
    const char *mf = getenv("BLARGG_MAX_FRAMES");
    if (mf && mf[0]) {
        long v = atol(mf);
        if (v > 0)
            s_max_frames = (uint64_t)v;
    }
    printf("[BLARGG] {\"name\": \"%s\", \"status\": \"START\", \"code\": 255, "
           "\"frames\": 0, \"misses\": 0, \"brks\": 0, "
           "\"reset_seen\": 0, \"detail\": \"\"}\n",
           s_test_name);
    fflush(stdout);
    {
        pthread_t th;
        pthread_create(&th, NULL, poller_thread, NULL);
        pthread_detach(th);
    }
}

void game_on_frame(uint64_t fc) {
    (void)fc;
}

void game_post_nmi(uint64_t fc) {
    (void)fc;
    if (s_verdict_done)
        return;
    /* Blargg magic first: ignore $6000 until a test claims it. Fresh SRAM
     * is 0xFF, so unclaimed reads ($FF...) can never match. */
    uint8_t code = 0;
    if (poll_sram(&code)) {
        if (code == 0x00)
            emit_verdict("PASS", 0, code);
        else
            emit_verdict("FAIL", 1, code);
    }
    if (g_frame_count >= s_max_frames)
        emit_verdict("TIMEOUT", 2, 255);
}

int game_handle_arg(const char *k, const char *v) {
    (void)k;
    (void)v;
    return 0;
}

const char *game_arg_usage(void) { return ""; }

uint32_t game_get_expected_crc32(void) { return 0; }

int game_dispatch_override(uint16_t a) {
    (void)a;
    return 0;
}

uint8_t game_ram_read_hook(uint16_t pc, uint16_t a, uint8_t v) {
    (void)pc;
    (void)a;
    return v;
}

void game_run_nmi(void) {
    /* Hardware gate: without the NMI frame the RTI tail misfires. */
    if (!(g_ppuctrl & 0x80))
        return;
    {
        uint16_t wb = g_code_window_base;
        func_NMI();
        g_code_window_base = wb;
    }
}

void game_run_main(void) {
    func_RESET();
}

void game_post_render(uint32_t *fb) {
    (void)fb;
}

void game_fill_frame_record(void *record) {
    (void)record;
}

int game_handle_debug_cmd(const char *cmd, int id, const char *json) {
    (void)cmd;
    (void)id;
    (void)json;
    return 0;
}
