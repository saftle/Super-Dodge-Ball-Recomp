/*
 * sdb_cheats.h - what sdb_host_extras.c calls in sdb_cheats.c.
 *
 * The cheat engine is its own translation unit because it is a different kind of
 * thing from the three presentation mods: those never touch the guest, this one
 * writes its RAM. It shares nothing with them but the host extras struct, and
 * keeping it separate makes that boundary obvious rather than a matter of which
 * half of a 3700-line file you are reading.
 *
 * See src/sdb_cheats.c for the format, the two mechanisms, and what a Game
 * Genie code can and cannot do on a recompiled machine.
 */
#pragma once

#include <stddef.h>

/* Host options. Returns 1 when the option was ours and was accepted, 0 when it
 * was ours and the value was refused (the host exits 2), -1 when it was not
 * ours at all. */
int sdb_cheats_option(const char *name, const char *value);

/* The launcher's default: find a cheat file where RetroArch keeps its own
 * (cheats/ beside the executable) and load it. Distinct from --cheats, which
 * takes the path to use. Returns 0 when there is nothing to load, which is not
 * an error - it just means the feature had nothing to turn on. */
int sdb_cheats_load_default(void);

/* The machine was (re)powered on. Re-evaluates the compare an armed Game Genie
 * code carries, because a compare only means anything against the image as it
 * is mapped at that moment. */
void sdb_cheats_power_on(void);

/* A frame finished and nothing was presented - the host's documented place for
 * per-frame work. Applies every armed freeze. */
void sdb_cheats_frame_end(void);

/* SDB_TRACE=1: one summary line on the way out, like the other mod reports. */
void sdb_cheats_set_trace(int on);
void sdb_cheats_report(void);

/* How many cheats are loaded. */
int sdb_cheats_enabled(void);

/* Register the `cheat` TCP command (list / on N / off N). This is a debugging
 * affordance with teeth, not a convenience: the menu is the only other way to
 * toggle a row, and it is not drivable headlessly, so without this the disarm
 * path -- which is the half that was wrong the first time -- has no automated
 * gate at all. Registered through CycHostExtras.tcp_setup, whose callback
 * takes the host's context pointer: the signature must match it exactly, or
 * a C23 compiler (GCC 14 and later) rejects the assignment as an error
 * rather than a warning. */
void sdb_cheats_tcp_setup(void *ctx);

/* ---- the in-game menu ----
 *
 * One BOOL row per loaded cheat in a "Cheats" section, labelled with the entry's
 * own description and footed with what it actually does to the machine. Toggling
 * a row arms it immediately; nothing is written until a row is on, so "no rows
 * on" IS the default-off guarantee -- per cheat, which is a stronger statement
 * than one feature switch.
 *
 * The rows are a static array rebuilt in place rather than a list the host
 * keeps, because cyc_ui_menu_create() copies menu_items into a 128-entry array
 * ONCE: labels have to outlive this function and the array has to exist before
 * the menu is built. Cheats load at session start, which is before it.
 *
 * section and key must not start with "cyc." -- that prefix is the host's. */
const struct RecompRuntimeUiItem *sdb_cheats_menu_items(size_t *count);
const struct RecompRuntimeUiCallbacks *sdb_cheats_menu_callbacks(void);