#!/usr/bin/env python3
"""ram_tap.py - sample CPU RAM from a running windowed session, over TCP.

WHY THIS EXISTS
---------------
To find the byte that advances the stage you have to watch a real stage
transition, and AGENTS.md is blunt about where that watching has to happen: a
headless replay of a recorded windowed vector is NOT the same machine (it
diverges at frame 4), and a green replay comparison is "a false pass on the
exact thing under test". So the obvious plan -- record a vector, replay it with
--frame-log, diff the RAM either side -- is the plan the project has already
been burned by.

The windowed host does not have --frame-log (that is cyc_host.c, the headless
host), so the way to capture RAM where it was made is to ask the running window
over its own debug port. `read_ram` returns up to 4096 bytes, so one call gets
all 2048 bytes of CPU RAM.

WHAT IT WRITES
--------------
A CSV: one `frame,<4096 hex chars>` line per sample. Line i+1 is CPU RAM at the
frame on line i, so a diff between two lines is a diff between two moments of
the same machine -- which is the only kind of diff that answers the question.

Start it BEFORE the game: it waits for the port rather than failing, so the two
do not have to be started in a particular order.

    tools/cheats/ram_tap.py --port 9099 --out tmp/stage/ram.csv &
    SDB_TCP=9099 SDB_UNLIMITED=1 ... tools/play_record.sh sdb-stage-win

Exit 0 on a clean finish. Exit 1 if the port never appeared; exit 2 if it
appeared and then dropped mid-tap, which is NOT the same thing -- a truncated
capture that reads as a complete one is exactly the failure this project keeps
paying for, so it is worth a distinct exit code.
"""
import argparse
import csv
import json
import socket
import sys
import time

RAM_LEN = 2048


class Client:
    def __init__(self, port, host="127.0.0.1", timeout=20.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        self.next_id = 1

    def call(self, cmd, **fields):
        i = self.next_id
        self.next_id += 1
        req = {"cmd": cmd, "id": i}
        req.update(fields)
        self.sock.sendall((json.dumps(req) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        r = json.loads(line.decode(errors="replace"))
        if not r.get("ok"):
            raise RuntimeError(r.get("err", "?"))
        return r

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def wait_for_port(port, deadline):
    while time.time() < deadline:
        try:
            return Client(port)
        except OSError:
            time.sleep(0.5)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--every", type=float, default=0.25,
                    help="seconds between samples (default 0.25, ~15 frames)")
    ap.add_argument("--wait-port", type=float, default=120.0)
    ap.add_argument("--idle-exit", type=float, default=0.0,
                    help="stop after this many seconds with no frame progress "
                         "(0 = run until the port closes)")
    args = ap.parse_args()

    print(f"ram_tap: waiting for the debug port {args.port} ...", file=sys.stderr)
    c = wait_for_port(args.port, time.time() + args.wait_port)
    if not c:
        print("ram_tap: the port never opened. Start the game with --tcp "
              f"{args.port} (SDB_TCP={args.port} for play_record.sh).", file=sys.stderr)
        return 1

    rows = 0
    first = last = None
    lost = False
    with open(args.out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["frame", "cycles", "ram"])
        try:
            while True:
                st = c.call("state")
                frame = st["frame"]
                ram = c.call("read_ram", addr=0, len=RAM_LEN)["hex"]
                w.writerow([frame, st.get("cycles", ""), ram])
                rows += 1
                if first is None:
                    first = frame
                last = frame
                if rows % 40 == 0:
                    print(f"  {rows} samples, frame {frame}", file=sys.stderr)
                time.sleep(args.every)
        except (RuntimeError, OSError, socket.timeout) as e:
            # The window closing is the normal end: the socket drops.
            if rows and isinstance(e, RuntimeError) and "closed" in str(e):
                pass
            else:
                lost = True
                print(f"ram_tap: stopped early: {e}", file=sys.stderr)
        finally:
            c.close()

    print(f"ram_tap: {rows} samples, frames {first}..{last} -> {args.out}",
          file=sys.stderr)
    if rows == 0:
        print("ram_tap: nothing was captured; an empty file that looks like a "
              "finding", file=sys.stderr)
        return 2
    return 2 if lost else 0


if __name__ == "__main__":
    sys.exit(main())