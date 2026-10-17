#!/usr/bin/env python3
"""ui_shot.py - drive the in-game menu over TCP and screenshot what it shows.

WHY THIS EXISTS
---------------
The runtime menu is recomp-ui's own renderer, reached through
CycHostExtras.menu_items. Reading that code says a "Cheats" section with a BOOL
row per entry will appear there; it does not say what the player will see, and
the difference matters when the question is whether the feature is visual at
all. So: open the menu, drive it with synthetic navigation, and read back the
rendered frame with the host's own `screenshot ... layer=ui` path, which is a
read-back of what the window presents rather than the machine's native picture.

That last part is the point. `--screenshot` captures the machine's 256x240 and
never sees the menu (AGENTS.md: mod/UI work needs --present-out or the window).
`layer=ui` is the other path.

The menu does not pause the game, so every frame here is a live frame behind the
menu. That is not incidental: it is the thing a mid-game cheat toggle has to
work on.

USAGE
    ui_shot.py --port 9099 --out tmp/ui/menu.png --do 'open' --do 'nav:down:3' --do 'shot'

    --do open            open the menu (idempotent: toggles only if it differs)
    --do nav:down:3      send DOWN three times
    --do nav:accept      send Enter / A
    --do shot            read back the current window contents
    --do wait:30         sleep 30 frames' worth (settle the renderer)
    --key <k>            one of toggle/back/up/down/left/right/accept, no nav:

Exit 0 if every step was accepted, 1 on the first refusal, 2 if the window never
came up. Refusals print the server's own message: "menu: this window has no
runtime menu" is a build problem (recomp-ui off), and it is worth seeing rather
than inferring.
"""
import argparse
import json
import socket
import sys
import time

NAV = ("toggle", "back", "up", "down", "left", "right", "accept")


class Client:
    def __init__(self, port, host="127.0.0.1", timeout=5.0):
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
                raise RuntimeError("connection closed while awaiting %s" % cmd)
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode(errors="replace"))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def connect(port, deadline):
    last = None
    while time.time() < deadline:
        try:
            return Client(port)
        except OSError as e:
            last = e
            time.sleep(0.25)
    raise SystemExit(f"could not reach the debug port {port}: {last}")


def run_step(cl, step, out, args):
    if step == "open":
        r = cl.call("menu", open=True)
    elif step.startswith("nav:"):
        parts = step.split(":")
        direction = parts[1]
        if direction not in NAV:
            raise SystemExit(f"nav wants one of {NAV}, got {direction!r}")
        # "nav:down" sends one; "nav:down:3" sends three. The count is optional
        # because sending one is what you want almost every time.
        count = int(parts[2]) if len(parts) > 2 else 1
        r = None
        for _ in range(count):
            r = cl.call("menu", nav=direction)
            # The menu processes input on its own frame. Firing several navs
            # back to back loses them -- measured: five DOWNs in one go moved
            # the section list three rows, which silently lands you on a
            # different section than you asked for and, in the System list, one
            # Enter from closing the game.
            time.sleep(0.09)
    elif step in NAV:
        r = cl.call("menu", nav=step)
    elif step.startswith("wait:"):
        time.sleep(int(step.split(":", 1)[1]) / 60.0)
        return True
    elif step == "shot":
        r = cl.call("screenshot", path=out, layer="ui")
    else:
        raise SystemExit(f"unknown --do step {step!r}")
    ok = r.get("ok")
    print(f"  {step:<16} {'ok' if ok else 'REFUSED: ' + str(r)}", file=sys.stderr)
    return bool(ok)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--do", action="append", default=[])
    ap.add_argument("--wait-port", type=float, default=20.0)
    args = ap.parse_args()

    cl = connect(args.port, time.time() + args.wait_port)
    try:
        for step in (args.do or ["open", "shot"]):
            if not run_step(cl, step, args.out, args):
                print(f"ui_shot: step {step!r} was refused; nothing trustworthy here",
                      file=sys.stderr)
                return 1
    finally:
        cl.close()
    print(f"  wrote {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())