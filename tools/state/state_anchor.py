#!/usr/bin/env python3
"""state_anchor.py - write save states out of a running windowed session.

WHY THIS EXISTS
---------------
`tools/play_record.sh` records what the guest read, so a played run can be
replayed. The vector alone is not enough, because Super Dodge Ball is a random
game: the machine's divergence between a windowed run and a headless replay of
the same vector starts at frame 4 (AGENTS.md, "Windowed and headless run
different guest state"), so by the time you reach the artifact the two are
different runs that happen to share an input log. Replaying such a run measures
the wrong thing, and it reports success while doing so.

A save state pins the whole machine -- CPU, RAM, PPU, mapper, APU, the host's
frame counter and the mods' records -- so the game's RNG, which is ordinary
guest RAM, is pinned with everything else. Loading it makes the headless replay
*be* the windowed run from that frame on, which is what "reproducible" has to
mean here.

WHY IT TALKS TCP
----------------
The windowed host is `cyc_sdl.c` and has no `--save-state F:FILE`: that flag is
`cyc_host.c`, the headless host. What the window does have is a debug server
(`--tcp`) whose `save_state` command writes a state to any path, and a `ping`
command that reports the frame counter. So this polls for the frame it wants and
asks once -- and takes the frame the server REPORTS BACK as the truth, because
the request is dispatched on a later host loop than the ping that preceded it.
The frame you asked for and the frame you got are not the same number and only
one of them is right.

WHAT IT WRITES
--------------
    state.bin   the machine at the anchor frame -- this is the artifact
    probe.bin   the machine at <anchor + --probe> frames -- the target
    anchor.json the frame numbers, so nothing downstream recomputes them
    (anchor.json also goes to stdout, one line, for the caller's log)

TWO STATES AND NOT "A STATE PLUS A RAM DUMP"
--------------------------------------------
The first version of this took the probe with `read_ram` and labelled it with
the `ping` frame that preceded it. That is wrong by however many frames the
window ran during the round trip -- measured at two to three, since a ping plus
a 2048-byte dump is two round trips inside a 16.6 ms frame -- and it is wrong
in a way that cannot be seen in the output, because the label looks like a
frame number. A comparison built on it reported 160-460 differing RAM bytes at
every frame, which reads as "the replay is a different machine" and is actually
just a mislabelled sample.

So the probe is a second save state. `save_state` REPLIES with the frame it
saved at, which makes both frame numbers authoritative on both sides of the
comparison, and `state_check.py` then asks the only question that can be asked
without knowing the layout of a C struct inside the state file: are these two
files byte for byte the same?

THE OFF-BY-ONE, WHICH IS NOT OPTIONAL
-------------------------------------
`--save-state M:FILE` in the headless host saves the machine after frame M RAN;
the window's TCP counter is `cyc_ring_frame`, which counts frames COMPLETED, so
a `save_state` there lands at a machine where F frames have run. Window frame F
is therefore headless `--save-state F-1`. Recorded as `probe_mem_frame` so the
conversion happens once, here, and nowhere else. Getting it backwards compares
two different frames and reports it as a difference in the machine.
"""
import argparse
import json
import os
import socket
import sys
import time

# Poll often enough to land within a frame or two of the request, rarely enough
# to leave the single TCP client slot free. The window's frame counter only
# moves on its host loop, so a faster poll buys nothing past that.
IDLE_SLEEP = 0.004
NEAR_FRAMES = 40


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
            time.sleep(0.05)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--frame", type=int, required=True,
                    help="window-side frame counter to anchor at")
    ap.add_argument("--out", required=True, help="directory to write into")
    ap.add_argument("--probe", type=int, default=0,
                    help="frames after the anchor to take a second state at, 0 "
                         "for none (default 0: the state is the artifact, and a "
                         "450 KB states cannot be taken closer than ~6 frames "
                         "apart, so a probe never reaches the window where the two "
                         "machines still agree -- see tools/state/README.md)")
    ap.add_argument("--wait-port", type=float, default=180.0)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    state_path = os.path.join(args.out, "state.bin")
    probe_path = os.path.join(args.out, "probe.bin")

    print(f"state_anchor: waiting for the debug port {args.port} ...", file=sys.stderr)
    c = wait_for_port(args.port, time.time() + args.wait_port)
    if not c:
        print("state_anchor: the port never opened. Start the game with --tcp "
              f"{args.port} (SDB_TCP={args.port} or SDB_ANCHOR_PORT={args.port} "
              "for play_record.sh).", file=sys.stderr)
        return 1

    anchor_frame = None
    probe_frame = None
    lost = None
    # How many frames late a save lands: the ping that asked for it versus the
    # frame the server reported. Writing half a megabyte costs the window real
    # frames (measured: 3 at frame 600), so this is subtracted from the probe's
    # trigger to bring the ASK nearer the intent. It is not a correction to be
    # trusted -- the second save lands later than the first (measured: 6 rather
    # than 3, the window still catching up from the anchor) -- so the ACHIEVED
    # gap goes into anchor.json and everything downstream uses the frame the
    # server reported, never the one that was asked for.
    lag = 0
    try:
        while True:
            frame = c.call("ping")["frame"]
            if anchor_frame is None:
                if frame >= args.frame:
                    r = c.call("save_state", path=state_path)
                    # The server's own count, not the ping that asked for it.
                    anchor_frame = r["frame"]
                    lag = max(0, anchor_frame - frame)
                    print(f"state_anchor: saved {state_path} at window frame "
                          f"{anchor_frame} (asked for {args.frame}, {lag} frames "
                          f"late)", file=sys.stderr)
                    if args.probe <= 0:
                        break
            elif frame >= anchor_frame + args.probe - lag:
                probe_frame = c.call("save_state", path=probe_path)["frame"]
                print(f"state_anchor: saved {probe_path} at window frame "
                      f"{probe_frame} ({probe_frame - anchor_frame} past the "
                      f"anchor, asked for {args.probe})", file=sys.stderr)
                break
            if args.frame - frame > NEAR_FRAMES:
                time.sleep(IDLE_SLEEP)
    except (RuntimeError, OSError, socket.timeout) as e:
        # The window closing is the normal end of a finite run.
        lost = str(e)
    finally:
        c.close()

    def wrote(path):
        return os.path.getsize(path) if os.path.exists(path) else 0

    if anchor_frame is None or not wrote(state_path):
        if lost:
            print(f"state_anchor: stopped before the anchor: {lost}", file=sys.stderr)
        print("state_anchor: no state written. A capture with a vector and "
              "screenshots is still valid; there is just nothing to replay it "
              "from. Raise the frame toward the end of the run, or lower "
              "SDB_ANCHOR.", file=sys.stderr)
        return 2

    rec = {
        "port": args.port,
        "want_frame": args.frame,
        "anchor_frame": anchor_frame,
        "state": os.path.basename(state_path),
        "state_bytes": wrote(state_path),
        "probed": False,
    }

    if probe_frame is None or not wrote(probe_path):
        # probe=0 is "no probe was asked for", which is a finished job and not a
        # failure. Reporting it as the run-ended-early case prints "the run ended
        # before the +0-frame probe", which is nonsense and reads as a broken
        # capture.
        if args.probe <= 0:
            with open(os.path.join(args.out, "anchor.json"), "w") as fh:
                json.dump(rec, fh, indent=2)
            print(json.dumps(rec))
            return 0
        if lost:
            print(f"state_anchor: {lost} (state is fine, the probe was not "
                  "reached)", file=sys.stderr)
        else:
            print("state_anchor: the probe frame was never reached", file=sys.stderr)
        print(json.dumps(rec))
        return 3

    rec.update({
        "probed": True,
        "probe": os.path.basename(probe_path),
        "probe_frame": probe_frame,
        # What the gap actually is, as opposed to what --probe asked for.
        "probe_gap": probe_frame - anchor_frame,
        # Window frame F is headless --save-state F-1. See the module docstring.
        "probe_mem_frame": probe_frame - 1,
    })
    with open(os.path.join(args.out, "anchor.json"), "w") as fh:
        json.dump(rec, fh, indent=2)
    print(json.dumps(rec))
    return 0


if __name__ == "__main__":
    sys.exit(main())