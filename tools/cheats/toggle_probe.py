"""Toggle a RAM freeze on and off over TCP and watch the byte.

The point is the DISARM. `sdb_cheat_frame_end` writes the cheat's value every
frame, so stopping the write is not the same as undoing it: whatever the cheat
last wrote stays in RAM until the game writes that byte itself, and for $06AE
(captain face) the game loads it once and never touches it again. So a toggle
that reads Off while the face is still modified is the bug, and the only way to
catch a regression is to read the byte after turning it off.
"""
import json
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9099
ADDR = 0x06AE
WANT = 0x0F


class C:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=8)
        self.buf = b""
        self.n = 0

    def call(self, *args, **kw):
        # The verb travels in "cmd", and the cheat handler takes its own
        # selection in "op" -- cyc_tcp has already consumed "cmd" by the time a
        # handler sees the line, which is the bug this signature's shape avoids
        # re-introducing.
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


def ram(c, addr):
    r = c.call(cmd="read_ram", addr=addr, len=1)
    return int(r["hex"], 16)


c = C(PORT)
print("cheat list ->", json.dumps(c.call(cmd="cheat", op="list").get("0")))
idx = 0

before = ram(c, ADDR)
print(f"  $%04X before          : {before}" % ADDR)

on = c.call(cmd="cheat", op="on", index=idx)
time.sleep(0.5)
armed = ram(c, ADDR)
print(f"  armed   -> {json.dumps({k: on[k] for k in ('on','addr','ram','saved')})}")
saved = int(on["saved"].lstrip("$"), 16)
print(f"  $%04X armed           : {armed}  (want ${WANT:02X})" % ADDR)
assert armed == WANT, f"arm failed: {armed} != {WANT}"

off = c.call(cmd="cheat", op="off", index=idx)
time.sleep(0.5)
restored = ram(c, ADDR)
print(f"  disarmed-> {json.dumps({k: off[k] for k in ('on','addr','ram','saved','restored')})}")
want = int(off["saved"].lstrip("$"), 16)
print(f"  $%04X after off       : {restored}")
assert off["restored"] == 1, f"no restore was reported: {off}"
assert restored == want, f"byte is {restored}, expected the saved {want}"
print(f"  restored to the value the game had: ${want:02X}; at start it was ${before:02X}")
print("PASS: disarm put the byte back")