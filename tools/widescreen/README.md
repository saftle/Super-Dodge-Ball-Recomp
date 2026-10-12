# tools/widescreen — analysis behind research.md §25

Two scripts, both read a `--frame-log` binary and both back a specific claim in
`docs/research.md` §25. Nothing here runs the emulator; make the log first.

## Make the logs

```bash
mkdir -p tmp/ws
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 1100 \
  --input tas/vectors/sdb_throw.input --ram-init zeros \
  --frame-log tmp/ws/throw.bin --frame-log-frames 0:1099
./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" --frames 1100 \
  --input tas/vectors/sdb_idle.input --ram-init zeros \
  --frame-log tmp/ws/idle.bin --frame-log-frames 0:1099
```

The logs are ~127 MiB each; `tmp/` is scratch, so regenerate rather than commit
them. Use a **copy** of the mods dir if you also pass `--mods-root`, because
`state.toml` is shared mutable state.

## `nametable.py` — why the margins can be read, not remembered

```bash
python3 tools/widescreen/nametable.py tmp/ws/throw.bin --rows 14,16,18,24,28
python3 tools/widescreen/nametable.py tmp/ws/throw.bin --stability 640 760 6,7,8,16,17,27
```

Claims it backs:

- **The court is a 512-pixel-wide looping stadium.** The 64-tile span across both
  nametables reads as `C0 C0 C0 C0 | 4E 4D … | 04 03 … | 2C 2B … | C0 C0 C0 C0` —
  backdrop, left sideline, crowd, right sideline, backdrop. That is why the
  widescreen compositor resolves margin pixels from the nametable instead of
  remembering them the way wideNES does, and why it needs no world cache, no
  isolated guest call and no hook sites.
- **The world is static for the whole match.** `--stability` reports the number
  of differing tile bytes frame to frame; it is 0 over a match, so the nametable
  a margin column needs is already correct at present time.
- **Attributes live at page offset `0x3C0`, not `$2800`.** The last line of every
  dump is the attribute region. This cart is horizontally mirroring, so `$2800`
  aliases `$2000` and the textbook attribute address reads tile bytes.

## `blank_extent.py` — how much overscan may be cropped

```bash
python3 tools/widescreen/blank_extent.py tmp/ws/throw.bin tmp/ws/idle.bin
```

Reports, per row and per column, how many frames carry a non-backdrop pixel. A
band that is 0 in **every** frame can be cropped with certainty; anything else
cannot. Measured result for Super Dodge Ball over 2112 frames with ink:

| band | frames with ink | verdict |
|---|---|---|
| columns 0–7 | **0 / 2112** | removable |
| columns 248–255 | 1372+ | content — keep |
| rows 0–15 | 2 / 2112 | frame 420 only |
| rows 232–239 | 2 / 2112 | frame 420 only |

The two are the **same frame in both vectors**: frame 420, a full-screen wipe
whose top and bottom are deliberately filled. So the vertical bands are not
lossless and the mod does not offer a vertical crop; it *reveals* the 8 clipped
left columns instead, because cutting them would splice the world (see
research.md §25).

## Attribution

Written for this project from the `--frame-log` format documented in
`runner/cyc/cyc_host.c` (`write_frame_log`). No third-party code.