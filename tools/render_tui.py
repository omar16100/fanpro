#!/usr/bin/env python3
"""Render a terminal frame to a PNG for the README.

Takes the output of `tmux capture-pane -e` (text plus ANSI colour) and draws
it with a monospace font inside window chrome.

This renders the terminal BUFFER rather than grabbing the screen. That is
deliberate: a screen grab captures whatever else happens to be on the display,
and the first attempt at this screenshot caught a browser window instead of
the terminal.

Usage:
    tmux new-session -d -s cap -x 88 -y 16 'fanpro top'
    sleep 15
    tmux capture-pane -t cap -p -e > /tmp/frame.ansi
    tmux kill-session -t cap
    python3 tools/render_tui.py /tmp/frame.ansi docs/images/fanpro-top.png "fanpro top"
"""
import re
import sys

from PIL import Image, ImageDraw, ImageFont

SCALE = 2  # render at 2x so the image stays crisp when scaled down

# A restrained dark terminal palette. Muted rather than saturated, matching
# the instrument-panel intent of the TUI itself.
BG_WINDOW = (18, 20, 24)
BG_TITLE = (32, 35, 41)
SHADOW = (8, 9, 11)
PAGE = (13, 14, 17)
FG = (206, 211, 219)
DIM = (108, 116, 128)

# xterm-256 indices the TUI uses, mapped to the same muted family.
XTERM = {
    80: (94, 196, 208), 81: (104, 198, 224), 108: (138, 178, 132),
    167: (198, 106, 100), 173: (206, 146, 106), 179: (206, 176, 116),
    238: (66, 70, 78), 245: (140, 148, 158), 252: (212, 216, 222),
}
BASIC = {
    30: (70, 74, 82), 31: (198, 106, 100), 32: (138, 178, 132),
    33: (206, 176, 116), 34: (110, 150, 200), 35: (176, 130, 196),
    36: (94, 196, 208), 37: FG, 90: DIM, 97: (255, 255, 255),
}

SGR = re.compile(r"\x1b\[([0-9;]*)m")


def parse(lines):
    """Turn ANSI-coloured lines into a grid of (char, fg, bold, reverse)."""
    grid = []
    for line in lines:
        fg, bold, rev = FG, False, False
        cells, i = [], 0
        while i < len(line):
            m = SGR.match(line, i)
            if m:
                params = [int(p or 0) for p in (m.group(1) or "0").split(";")]
                k = 0
                while k < len(params):
                    p = params[k]
                    if p == 0:
                        fg, bold, rev = FG, False, False
                    elif p == 1:
                        bold = True
                    elif p == 7:
                        rev = True
                    elif p == 27:
                        rev = False
                    elif p == 39:
                        fg = FG
                    elif p in BASIC:
                        fg = BASIC[p]
                    elif p == 38 and k + 2 < len(params) and params[k + 1] == 5:
                        fg = XTERM.get(params[k + 2], FG)
                        k += 2
                    k += 1
                i = m.end()
                continue
            if line[i] == "\x1b":  # any other escape: skip to its final letter
                j = i + 1
                while j < len(line) and not line[j].isalpha():
                    j += 1
                i = j + 1
                continue
            cells.append((line[i], fg, bold, rev))
            i += 1
        grid.append(cells)

    while grid and not "".join(c[0] for c in grid[-1]).strip():
        grid.pop()
    return grid


def render(grid, out_path, title):
    cell_w, cell_h = 11 * SCALE, 22 * SCALE
    pad = 20 * SCALE
    title_h = 34 * SCALE
    radius = 10 * SCALE

    font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 17 * SCALE, index=0)
    font_b = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 17 * SCALE, index=1)
    font_t = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 13 * SCALE, index=0)

    cols = max((len(r) for r in grid), default=80)
    win_w = cols * cell_w + pad * 2
    win_h = title_h + len(grid) * cell_h + int(pad * 1.6)
    margin = 22 * SCALE

    img = Image.new("RGB", (win_w + margin * 2, win_h + margin * 2), PAGE)
    d = ImageDraw.Draw(img)

    # A soft drop shadow, drawn as a few offset rounded rects rather than a
    # blur so the file stays small.
    for k in range(6, 0, -1):
        d.rounded_rectangle(
            [margin - k // 2, margin + k, margin + win_w + k // 2, margin + win_h + k],
            radius=radius, fill=SHADOW,
        )

    d.rounded_rectangle([margin, margin, margin + win_w, margin + win_h],
                        radius=radius, fill=BG_WINDOW)
    # Title bar: the same rounded rect clipped to its top half.
    d.rounded_rectangle([margin, margin, margin + win_w, margin + title_h + radius],
                        radius=radius, fill=BG_TITLE)
    d.rectangle([margin, margin + title_h - 1, margin + win_w, margin + title_h],
                fill=(44, 48, 55))

    # Traffic lights.
    for i, colour in enumerate([(237, 106, 94), (232, 182, 84), (99, 194, 92)]):
        cx = margin + 20 * SCALE + i * 20 * SCALE
        cy = margin + title_h // 2
        r = 6 * SCALE
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=colour)

    tw = d.textlength(title, font=font_t)
    d.text((margin + win_w / 2 - tw / 2, margin + title_h / 2 - 8 * SCALE),
           title, font=font_t, fill=DIM)

    # Full padding under the title bar: half a pad clipped the ascenders of
    # the first text row against the chrome.
    ox, oy = margin + pad, margin + title_h + pad
    for y, row in enumerate(grid):
        for x, (ch, fg, bold, rev) in enumerate(row):
            if ch == " " and not rev:
                continue
            px, py = ox + x * cell_w, oy + y * cell_h
            colour = fg
            if rev:
                d.rectangle([px, py, px + cell_w, py + cell_h], fill=fg)
                colour = BG_WINDOW
            if ch != " ":
                d.text((px, py), ch, font=font_b if bold else font, fill=colour)

    img.save(out_path)
    print(f"{out_path}: {img.width}x{img.height} ({len(grid)} rows x {cols} cols)")


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    caption = sys.argv[3] if len(sys.argv) > 3 else "fanpro top"
    with open(src, encoding="utf-8", errors="replace") as fh:
        render(parse(fh.read().rstrip("\n").split("\n")), dst, caption)
