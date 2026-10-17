#!/usr/bin/env python3
"""gg_selftest.py - check the Game Genie decoder against the published scheme.

WHY THIS EXISTS
---------------
`src/sdb_cheats.c` implements the Code Masters / Galoob Game Genie's CODE
ENCODING, from the technical notes at
<https://tuxnes.sourceforge.net/gamegenie.html>, rather than from any emulator's
source -- deliberately, because this project is PolyForm Noncommercial and the
obvious reference implementations are GPL. That choice is only worth anything if
the implementation is demonstrably correct against the SPECIFICATION and not
merely self-consistent, so this drives the real binary and asks it what it
decoded.

THE THREE CASES
---------------
  GOSSIP    the notes' own 6-character example: address $D1DD, data $14
  ZEXPYGLA  the notes' 8-character example:  address $94A7, data $02, compare $03
  one we derived ourselves and cross-checked against the raw `AAAA:VV` spelling:
            NYGEAE for $8048 = $FF

HOW IT DRIVES THE DECODER
-------------------------
Windowed run + the game's `cheat` TCP command (`cheat list` reports each entry's
decoded address and value). That reads the real C decoder's output rather than a
reimplementation of it, which is the point: a Python copy of the algorithm would
pass even if the C had drifted.

Exit 0 if every case matches, 1 otherwise. Needs a running game with --tcp; the
caller starts one.
"""
import json
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9099

# code -> (expected address, expected value, expected compare-or-None)
CASES = {
    "GOSSIP":   (0xD1DD, 0x14, None),   # the notes' 6-character example
    "ZEXPYGLA": (0x94A7, 0x02, 0x03),   # the notes' 8-character example
    "NYGEAE":   (0x8048, 0xFF, None),   # ours, cross-checked against 8048:FF
}


class C:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""
        self.n = 0

    def call(self, **kw):
        self.n += 1
        req = {"id": self.n}
        req.update(kw)
        self.s.sendall((json.dumps(req) + "\n").encode())
        while b"\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise RuntimeError("closed")
            self.buf += d
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode())

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def main():
    c = C(PORT)
    # Arm each case by index, reading back what the decoder produced.
    listing = c.call(cmd="cheat", op="list")
    entries = {k: v for k, v in listing.items() if isinstance(v, dict)}
    if not entries:
        print("gg_selftest: the cheat list came back empty - start the game with "
              "--cheats <file> containing one code per case", file=sys.stderr)
        return 1

    by_addr = {int(v["addr"].lstrip("$"), 16): (k, v) for k, v in entries.items()}
    failures = 0
    for code, (addr, value, compare) in CASES.items():
        if addr not in by_addr:
            print(f"  MISS {code:<9} no entry decoded to ${addr:04X}")
            failures += 1
            continue
        key, got = by_addr[addr]
        gv = int(got["value"].lstrip("$"), 16)
        gcv = int(got["compare"].lstrip("$"), 16)
        # $00 is the "no compare" sentinel for a code that carries none
        gcv = None if gcv == 0 else gcv
        ok = gv == value and gcv == compare
        print(f"  {'ok  ' if ok else 'FAIL'} {code:<9} -> ${addr:04X}=${gv:02X} "
              f"compare={'-' if gcv is None else '$%02X' % gcv}   "
              f"(expected ${value:02X}, compare={'-' if compare is None else '$%02X' % compare})")
        failures += 0 if ok else 1
    if failures:
        print(f"gg_selftest: {failures} case(s) disagree with the technical notes")
        return 1
    print("gg_selftest: the decoder matches the published scheme's worked examples")
    return 0


if __name__ == "__main__":
    sys.exit(main())