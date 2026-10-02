# patches/ — RETIRED with the cycle switch (2026-10-02)

Nothing here applies to anything. The cycle backend never consumed these
(they patch legacy-runner files), and `setup.sh` no longer applies them.
Kept as a record + the 002 upstream proposal below. Backup copies also
exist in `/tmp/legacy_backup/` and git history.

| Patch | What | Fate |
|---|---|---|
| `002_interp_ram_dummy` | RAM-trampoline interp RTS pops live-caller bytes; pre-push dummy pair | **Propose upstream** (real legacy bug, research §19.1, draft in `docs/plan.md`) |
| `003_interp_rom_clamp` | ROM-miss pop guard (002's companion) | Lapses with 002's decision |
| `004_nmi_tunnel_unwind` | Start-tunnel longjmp unwind | Game-specific; lapses |
| `005_hang_watchdog_default` | 90 s idle auto-kill default | Taste call; lapses |
| `001_nmi_tail_unwind` (was already archived) | NMI-tail unwind for the `$FCB3` spin | Dormant, never fired; see `archived-patches.md` |

Rule: no new patches. Ever. Runner work goes upstream or it doesn't happen.
