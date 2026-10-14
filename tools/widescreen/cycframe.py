#!/usr/bin/env python3
"""cycframe.py - reader for the `--frame-log` format, shared by these tools.

FORMAT (authoritative: `runner/cyc/cyc_host.c`, `write_frame_log`)

    "CYCFRAME"                      8 bytes
    u32 version                     2
    per frame:
      u32 frame
      u32 cycles low, u32 cycles high
      u32 lengths[6]                CPU RAM, CIRAM, cartridge RAM, CHR RAM,
                                    picture bytes, FDS sound-unit state
      u8  palette[32]
      u8  oam[256]
      then the six blobs, in that order

Version 1 had neither the FDS length nor the FDS blob (`lengths[5]`, no
trailer). The picture is 256x240 little-endian u16 colour indices, written as
`index | emphasis << 6`.

`cycles` is in the header and no tool here ever read it, because all four threw
it away as an unnamed unpack field. It is the authoritative per-frame cycle
count, which is what the `mode=... cycles=` line on stdout reports in total.

WHY THIS EXISTS
---------------
Four scripts in this directory each re-implemented the same twenty lines of
header parsing. That is not a style complaint: a change to the format would
break three of them *silently*, and a tool that reads nothing reports nothing
wrong. That is the same failure this project has already paid for twice with its
own trace, where a missing newline left an audit reporting "no lines in that log"
on a perfectly healthy run (research.md 26). One reader, one place to be wrong.
"""
import collections
import os
import struct

MAGIC = b"CYCFRAME"
VERSION = 2
PICTURE_W, PICTURE_H = 256, 240
PICTURE_BYTES = PICTURE_W * PICTURE_H * 2
PALETTE_BYTES = 32
OAM_BYTES = 256

#: Blob order after the palette and OAM.
SECTIONS = ("ram", "ciram", "cart", "chr", "picture", "audio")
ALL = ("frame", "cycles", "palette", "oam") + SECTIONS
DEFAULT_WANT = ("frame", "cycles", "palette", "oam", "ram", "ciram", "picture")


class Record:
    """One frame of a log. Any field not asked for is None.

    Attribute access rather than a namedtuple so `want` can be honoured per
    run: seeking past a blob is far cheaper than reading it, and cartridge RAM
    plus CHR RAM is 256 KiB a frame that most tools have no use for.
    """

    __slots__ = ALL

    def __init__(self, **fields):
        for name in ALL:
            setattr(self, name, fields.get(name))

    def __repr__(self):
        return f"<frame {self.frame}>"


def records(path, want=None):
    """Yield a Record per frame in a CYCFRAME log.

    want: iterable of field names to materialise; everything else is skipped
    with a seek. Unreadable or truncated logs stop at the last whole record
    rather than raising, which is what a run killed mid-log leaves behind.
    """
    want = frozenset(DEFAULT_WANT if want is None else want)
    unknown = want - set(ALL)
    if unknown:
        raise ValueError(f"unknown field(s): {', '.join(sorted(unknown))}; "
                         f"known: {', '.join(ALL)}")

    with open(path, "rb") as f:
        if f.read(len(MAGIC)) != MAGIC:
            raise SystemExit(f"{path}: not a CYCFRAME log")
        (version,) = struct.unpack("<I", f.read(4))
        if version not in (1, 2):
            raise SystemExit(f"{path}: CYCFRAME version {version}; this reader "
                             f"knows 1 and 2")
        nsections = len(SECTIONS) if version >= 2 else len(SECTIONS) - 1
        head_size = (3 + nsections) * 4
        unpack = struct.Struct(f"<{3 + nsections}I").unpack

        # Seeking past EOF SUCCEEDS in Python and the next read simply returns
        # b'', so a log truncated mid-record would otherwise still yield that
        # record -- silently, on the exact runs where the data is short. That is
        # the failure mode this module exists to remove, so every skip is bounds
        # checked against the real file size.
        size = os.fstat(f.fileno()).st_size

        while True:
            head = f.read(head_size)
            if len(head) < head_size:
                return                      # end of log, or a truncated tail
            frame, clo, chi, *lengths = unpack(head)
            rec = {"frame": frame if "frame" in want else None,
                   "cycles": (clo | (chi << 32)) if "cycles" in want else None}

            for name, nbytes in (("palette", PALETTE_BYTES), ("oam", OAM_BYTES)):
                if name in want:
                    data = f.read(nbytes)
                    if len(data) < nbytes:
                        return
                    rec[name] = data
                else:
                    f.seek(nbytes, os.SEEK_CUR)

            for name, nbytes in zip(SECTIONS[:nsections], lengths):
                if name in want:
                    data = f.read(nbytes)
                    if len(data) < nbytes:
                        return
                    rec[name] = data
                else:
                    f.seek(nbytes, os.SEEK_CUR)
            # A version-1 log has no FDS blob to consume; there is nothing to
            # do for it, the loop simply reads the next header.

            # A skipped blob may have run past the end even though nothing was
            # read, so the position is checked once more before yielding.
            if f.tell() > size:
                return

            yield Record(**rec)


def picture_indices(picture):
    """256x240 palette indices from a picture blob, as a flat list.

    The low 6 bits are the palette index and bit 6 is the colour-emphasis bit.
    Most callers compare against a backdrop index and only care about the low 6,
    so masking happens here once rather than in each caller.
    """
    if len(picture) != PICTURE_BYTES:
        raise SystemExit(f"picture blob is {len(picture)} bytes, expected "
                         f"{PICTURE_BYTES}")
    return [w & 0x3F for w in struct.unpack(f"<{PICTURE_W * PICTURE_H}H", picture)]


def backdrop(picture):
    """The most common palette index in a picture: this game's backdrop.

    There is no "index 0 means background" rule on the NES -- the universal
    backdrop is whatever the game picks -- so this is measured per frame rather
    than assumed.
    """
    return collections.Counter(picture_indices(picture)).most_common(1)[0][0]