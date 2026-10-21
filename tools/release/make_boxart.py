#!/usr/bin/env python3
"""Compose the launcher's hero art (the panel on the left of the game card).

The launcher draws assets/img/boxart.tga in a portrait slot, and draws a
generic cartridge placeholder when there is none. This builds ours from the
project's own material: a screenshot of the recompilation and the title set
in the launcher's own font.

Why we draw one instead of using the retail box front: the retail cover
carries the Nintendo Seal of Quality and the line "Licensed by Nintendo for
Play on The Nintendo Entertainment System". This program is not licensed by
Nintendo and says so in its README, so that image in this launcher would
assert something false. The artwork is also CSG Imagesoft's. The reference
recompilation (Super Mario Bros.) draws its own box front too.

    python3 tools/release/make_boxart.py
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
SCREENSHOT = ROOT / "docs/assets/title.png"
FONT_BOLD = ROOT / "external/recomp-ui/assets/common/fonts/LatoLatin-Bold.ttf"
FONT_REGULAR = ROOT / "external/recomp-ui/assets/common/fonts/LatoLatin-Regular.ttf"
OUT = ROOT / "docs/assets/boxart.tga"

W, H = 300, 434          # the reference box front's proportions
MARGIN = 14


def main() -> int:
    if not SCREENSHOT.is_file():
        print(f"missing {SCREENSHOT}", file=sys.stderr)
        return 2
    if not FONT_BOLD.is_file():
        print(f"missing {FONT_BOLD}", file=sys.stderr)
        return 2

    art = Image.new("RGB", (W, H), (14, 14, 20))
    draw = ImageDraw.Draw(art)

    title_font = ImageFont.truetype(str(FONT_BOLD), 27)
    sub_font = ImageFont.truetype(str(FONT_REGULAR), 12)
    tag_font = ImageFont.truetype(str(FONT_BOLD), 11)

    # --- title ------------------------------------------------------------
    # The wordmark is set in the launcher's own font; the accent colour is
    # the launcher's red, so the panel reads as part of the same UI.
    title = "SUPER"
    subtitle = "DODGE BALL"
    tag = "RECOMPILATION"

    def centred(text: str, font, y: int, fill) -> None:
        box = draw.textbbox((0, 0), text, font=font)
        draw.text(((W - (box[2] - box[0])) // 2, y), text, font=font, fill=fill)

    centred(title, title_font, 16, (238, 238, 244))
    centred(subtitle, title_font, 44, (226, 62, 62))
    centred(tag, tag_font, 78, (150, 150, 165))

    # --- the screenshot ---------------------------------------------------
    shot = Image.open(SCREENSHOT).convert("RGB")
    frame_h = H - 78 - 46 - MARGIN * 2
    ratio = frame_h / shot.size[1]
    shot = shot.resize((int(shot.size[0] * ratio), frame_h), Image.LANCZOS)
    if shot.size[0] > W - MARGIN * 2:
        # The screenshot is wider than the frame: crop it to the panel's
        # proportions rather than letterboxing it.
        crop = (shot.size[0] - (W - MARGIN * 2)) // 2
        shot = shot.crop((crop, 0, crop + W - MARGIN * 2, frame_h))
    x = (W - shot.size[0]) // 2
    y = 78 + MARGIN
    art.paste(shot, (x, y))
    draw.rectangle([x - 1, y - 1, x + shot.size[0], y + shot.size[1]],
                    outline=(64, 64, 76), width=1)

    # --- footer -----------------------------------------------------------
    centred("NES  ·  CYCLE-ACCURATE", tag_font, H - 32, (120, 120, 135))
    centred("2026", tag_font, H - 18, (90, 90, 104))

    art.save(OUT, format="TGA", compression="tga_rle")
    print(f"wrote {OUT} ({art.size[0]}x{art.size[1]})")
    art.save("/tmp/opencode/boxart_preview.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
