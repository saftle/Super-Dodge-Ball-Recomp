# tools/state/

Save-state anchors for `tools/play_record.sh`: a played capture records the
inputs, and these pin the *machine*.

## What this is for

`play_record.sh` writes a vector, so a played session can be replayed. A vector
carries the INPUTS to a run, not the run. Super Dodge Ball is random, and
AGENTS.md's standing finding is that a headless replay of a recorded windowed
vector is a **different machine from frame 4** — so by the time you reach the
artifact you are measuring something else. AGENTS.md puts a number on what that
costs: a replay comparison "reported 0 differing pixels over 1263 frames" on a
build that visibly doubled a running player. A green replay is not weak evidence
here; it is evidence of nothing.

A save state pins CPU, RAM, PPU, mapper, APU, the host's frame counter and the
mods' records at one frame. The game's RNG is ordinary guest RAM, so it is pinned
with everything else. Loading the state puts a headless replay **exactly** on the
played run's machine from that frame on.

## The measurement, which is the reason to read this before using it

Measured 2026-10-17, both captures anchored mid-match, guest chunks meaning
CPU / RAM / PPU / picture / mod records:

| capture | anchor | at +3 | at +6 | at +9 |
|---|---|---|---|---|
| `sdb_throw.input`, scripted | 605 | 0 | 55 | **85** |
| a played session | 604 | 0 | — | **0** |

Both anchors are exact at the frame they were taken, and both are clean at +3. At
+9 the scripted run has drifted by 85 bytes of guest state and **the played session
has not drifted at all** — every guest byte identical.

**So the drift onset is content-dependent, and no number should be written down as
a frame count.** It depends on what the guest is doing when the two machines part
company. The cause is not the index, not the state machinery and not the audio
path:

- **Not an index error.** Both states record the same frame in their own header
  (offset 28, `Header.frame`). Sliding the replay against a windowed state over
  ±10 frames, the minimum sits exactly at the frame the window reported — 608↔608
  and 614↔614 — so `probe_mem_frame = probe_frame - 1` is confirmed, not assumed.
- **Not the APU.** The `APU  ` chunk differs by ~15–24 bytes on every frame after
  the anchor *including the first*, while every other chunk is identical there. It
  is a standing offset, not a drift, and `--wav-out` on the replay does not remove
  it (1112 → 1108 bytes at a 120-frame probe). It also does not track the guest
  divergence: the guest is identical at +3 with the APU differing, and one capture
  drifted by +9 with the APU differing no more than at +3.
- **Not input timing.** Both hosts apply input before `cyc_session_frame_begin()`
  (`cyc_sdl.c:1452`, `cyc_host.c:1602`), keyed on the same counter.

What remains is the windowed/headless divergence AGENTS.md already records as
open and unattributed. **Do not guess at it from here** — this is a second
measurement of it, at a known-good starting point, not an explanation.

**And the practical consequence: two 450 508-byte states cannot be taken closer
than about 6 frames apart** on a 16.6 ms host loop (the first save lands 3 frames
late, the second 6). The clean window is +3. **A probe cannot reach it.** That is
why `SDB_ANCHOR_PROBE` defaults to 0: a check that reports drift every single time
is not a gate, it is noise, and a capture that prints a red verdict by default
teaches you to ignore it. Turn the probe on when the drift is the question — and
expect the answer to depend on what the game is doing, because that is what it
now does.

## `state_anchor.py`

Takes the states out of a **running window** over its TCP debug server.

The windowed host is `cyc_sdl.c` and has no `--save-state F:FILE` — that flag is
`cyc_host.c`, the headless host. What the window has is `save_state path=...`,
which writes to any path and **replies with the frame it saved at**. That reply
is the whole reason this works: a `ping` frame and a save are separated by a host
loop, so the frame you asked for is not the frame you got (measured: 3 frames at
frame 600), and only one of them is right.

Exit 0 anchor taken (and probe taken if one was asked for), 1 the port never
opened, 2 nothing was saved, 3 the anchor is fine but the probe frame was never
reached. 2 and 3 are distinct on purpose: a state that exists and was not
verified is not a state that does not work.

One client at a time — the host's server holds a single connection
(`cyc_tcp.c`: `s_client`), so this cannot share a port with `ram_tap.py`.

## `state_check.py`

Compares the replay's state against the window's probe state.

```
python3 tools/state/state_check.py tmp/<name>
```

Exit 0 the machine matches, 1 it does not, **2 examined nothing**.

**It walks the chunks.** A state is a tag/length sequence (`cyc_state.c`), and
saying "1112 of 449 508 bytes differ" without saying *where* made the first
version useless: the difference was scattered across RAM, CIRAM and the PPU, and
so is a different machine, while 20 APU bytes are not. RAM is deliberately not
read out of the `HW  ` chunk — it sits at the offset of `ram` in `HwMachine`, so
extracting it means depending on a C struct's layout, and a tool that reported a
RAM diff after the layout moved would be reporting a lie about the machine.

**The APU is reported and is not fatal.** That is an argument about scope, not a
claim of equivalence: this gate asks "is the replay the same machine", and the
APU is the one chunk no measurement in this project reads. The APU figure is
printed on every run so it is never invisible, and a change in it stays visible.

## Both traps, and they are the same one

**A label you cannot trust is worse than no label.** The first version took the
probe with `read_ram` and labelled it with the `ping` that preceded it. `ping`
then a 2048-byte `read_ram` is two round trips inside a 16.6 ms frame, so the
RAM belongs 2–3 frames past the label — measured, consistently 3. The comparison
built on it reported 160–460 differing RAM bytes at *every* frame, which reads as
"the replay is a different machine" and is actually a mislabelled sample. A save
state is used for the probe instead: it cannot be mislabelled, because it says
what frame it is.

**Check the gate can go green before believing a red.** The red above is real and
is the drift; but the same red would have appeared had the offset been wrong, had
the mods differed, or had the state been rejected. They are told apart
structurally — same recorded frame, same file length, per-chunk breakdown — not
by judgement.