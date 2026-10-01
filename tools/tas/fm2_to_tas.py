#!/usr/bin/env python3
"""fm2 -> canonical TAS JSON (FCEUX movies).

Usage:
  python3 tools/tas/fm2_to_tas.py in.fm2 out.tas.json [--frames N]

Grammar (fceux.com/web/help/fm2.html + src/movie.cpp):
  Header: `key value` lines (first key must be `version`=3) until the first
  line starting with `|`. value = rest of line after first whitespace.
  Text record: `|commands|P1|P2|...|` (P1 columns RLDUTSBA; pressed = any
  char other than ' ' or '.'). commands = int bitmask (1=soft reset,
  2=power, 4/8=FDS, 16/32=VS coin, 64=service). Fourscore lines carry
  P1..P4 (we take P1). `length` caps record count. `binary 1` selects the
  binary record layout (1 cmd byte + LSB-first pad bytes bit0=A..bit7=Right).
  `savestate` present => savestate-anchored (NOT power-on: unsuitable for
  our harness runs, parser still converts the input stream and flags it).

Canonical frames use recomp mask bits:
  A=0x80 B=0x40 SELECT=0x20 START=0x10 UP=0x08 DOWN=0x04 LEFT=0x02 RIGHT=0x01.
"""
import base64
import binascii
import json
import sys

RECOMP_MASK = {
    "A": 0x80, "B": 0x40, "SELECT": 0x20, "START": 0x10,
    "UP": 0x08, "DOWN": 0x04, "LEFT": 0x02, "RIGHT": 0x01,
}
# text column index -> button (R,L,D,U,sTart,Select,B,A)
TEXT_COLS = ["RIGHT", "LEFT", "DOWN", "UP", "START", "SELECT", "B", "A"]
# binary LSB-first bit -> button
BIN_BITS = ["A", "B", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT"]


def decode_blob(s):
    """romChecksum blob -> hex MD5 string. Stored as base64 of the hexified
    MD5 (optionally `base64:`-prefixed) or plain `0x` hex."""
    s = s.strip()
    if s.startswith("base64:"):
        s = s[len("base64:"):]
    try:
        raw = base64.b64decode(s)
        try:
            hx = raw.decode("ascii")
            if len(hx) == 32:
                return hx.lower()  # base64 of hexified MD5 (spec form)
        except UnicodeDecodeError:
            pass
        if len(raw) == 16:
            return raw.hex()  # base64 of raw digest (seen in the wild)
    except Exception:
        pass
    try:
        h = s[2:] if s.startswith("0x") else s
        binascii.unhexlify(h)
        return h.lower()
    except Exception:
        return ""


def parse_text_field(field):
    m = 0
    for i, btn in enumerate(TEXT_COLS):
        if i < len(field) and field[i] not in (" ", "."):
            m |= RECOMP_MASK[btn]
    return m


def parse(path):
    with open(path, "rb") as f:
        raw = f.read()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = raw.decode("latin-1")
    lines = text.splitlines()
    header = {}
    header_order = []
    records = []
    in_log = False
    for ln in lines:
        if not in_log and ln.startswith("|"):
            in_log = True
        if not in_log:
            if not ln.strip():
                continue
            parts = ln.split(None, 1)
            k = parts[0]
            v = parts[1] if len(parts) > 1 else ""
            header[k] = v
            header_order.append(k)
        else:
            if ln.startswith("|"):
                records.append(ln)
    if not header_order or header_order[0] != "version":
        raise ValueError("not an fm2 file (must begin with 'version 3')")
    if header.get("version", "").strip() != "3":
        raise ValueError(f"unsupported fm2 version {header.get('version')!r}")
    binary = header.get("binary", "0").strip() == "1"
    fourscore = header.get("fourscore", "0").strip() == "1"
    length = int(header.get("length", "-1").strip() or "-1")
    if length >= 0:
        records = records[:length]
    masks = []
    resets = []
    if not binary:
        for i, ln in enumerate(records):
            parts = ln.split("|")
            # ['', commands, P1, P2, ..., '']
            try:
                cmd = int(parts[1]) if len(parts) > 1 and parts[1].strip() else 0
            except ValueError:
                cmd = 0
            if cmd & 0x43:  # reset/power/service bits set
                resets.append((i, cmd))
            fields = [p for p in parts[2:] if p != ""]
            p1 = fields[0] if fields else ""
            masks.append(parse_text_field(p1))
    else:
        # binary layout after opening '|': cmd byte + pad bytes
        blob = "".join(records)
        data = blob.encode("latin-1")
        pos = 0
        nfields = 4 if fourscore else 2
        i = 0
        while pos < len(data):
            if data[pos:pos + 1] != b"|":
                pos += 1
                continue
            pos += 1
            need = 1 + nfields
            if pos + need > len(data):
                break
            cmd = data[pos]
            pos += 1
            if cmd & 0x43:
                resets.append((i, cmd))
            m = 0
            for _ in range(nfields if not fourscore else 1):
                b = data[pos]
                pos += 1
                for bit, btn in enumerate(BIN_BITS):
                    if b & (1 << bit):
                        m |= RECOMP_MASK[btn]
            if fourscore:  # skip P2..P4
                pos += 3
            masks.append(m)
            i += 1
            if length >= 0 and i >= length:
                break
    md5hex = decode_blob(header.get("romChecksum", ""))
    meta = {
        "format": "fm2",
        "rom_filename": header.get("romFilename", ""),
        "rom_md5": md5hex,
        "pal": header.get("palFlag", "0").strip() == "1",
        "new_ppu": header.get("NewPPU", "0").strip() == "1",
        "fourscore": fourscore,
        "binary_log": binary,
        "savestate_anchored": "savestate" in header,
        "guid": header.get("guid", ""),
        "rerecord_count": header.get("rerecordCount", ""),
        "resets": resets,
    }
    return masks, meta


def main():
    src = sys.argv[1]
    dst = sys.argv[2]
    maxf = None
    if "--frames" in sys.argv:
        maxf = int(sys.argv[sys.argv.index("--frames") + 1])
    masks, meta = parse(src)
    if maxf:
        masks = masks[:maxf]
    if meta["savestate_anchored"]:
        print("WARNING: savestate-anchored movie — input stream converts, "
              "but harness runs need power-on TAS (frame bases differ)")
    if meta["resets"]:
        print(f"NOTE: {len(meta['resets'])} records with reset/power bits: "
              f"{meta['resets'][:5]}")
    out = {"source": src, "rom_md5_headerless": meta["rom_md5"],
           "mask_map": "A=0x80 B=0x40 SELECT=0x20 START=0x10 UP=0x08 DOWN=0x04 LEFT=0x02 RIGHT=0x01",
           "fm2_meta": {k: v for k, v in meta.items() if k != "resets"},
           "frames": masks}
    with open(dst, "w") as f:
        json.dump(out, f)
    n_in = sum(1 for m in masks if m)
    first = next((i for i, m in enumerate(masks) if m), None)
    print(f"wrote {dst}: {len(masks)} frames, {n_in} with input, "
          f"first input at {first}, md5={meta['rom_md5']}, pal={meta['pal']}")


if __name__ == "__main__":
    main()
