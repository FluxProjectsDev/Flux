#!/usr/bin/env python3
#
# Copyright (C) 2026 FebriCahyaa
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# DEVELOPMENT TOOL — composes the Flux installer ASCII logo.
#
# This is NOT run during installation and is NOT packaged. It exists so the art is authored from
# an explicit set of waypoints that can be adjusted and re-reviewed, rather than by counting
# spaces by hand across 30 lines. Its output is pasted into module/installer/ui.sh as a fixed,
# quoted heredoc, and .github/scripts/verify-installer.sh then holds that heredoc to a
# byte-for-byte golden fixture.
#
# Shape source of truth: logo_ascii_bertema_terminal_flux.png (the approved reference), which is
# a reference image only and is never packaged.
#
# A note on what that reference is, because it changed the approach: it is an IMAGE DEPICTING
# ASCII art, not a terminal capture. Its dot spacing is irregular (measured gaps of 9.5, 25,
# 21.5, 12.5 px in one texture row) where a real character grid would be near-constant, so there
# is no exact character grid in it to recover. The silhouette was therefore traced from the image
# and the characters composed to match it.
#
# Usage:
#   python3 scripts/compose-ascii-logo.py            print the composed art
#   python3 scripts/compose-ascii-logo.py --shell    print it as a shell heredoc body
#   python3 scripts/compose-ascii-logo.py --png OUT  render it to an image for visual comparison

import argparse
import sys

COLS, ROWS = 40, 24


class Canvas:
    def __init__(self, cols, rows):
        self.cols = cols
        self.rows = rows
        self.g = [[" "] * cols for _ in range(rows)]

    def put(self, r, c, ch):
        if 0 <= r < self.rows and 0 <= c < self.cols:
            self.g[r][c] = ch

    def hline(self, r, c0, c1, ch="_"):
        for c in range(min(c0, c1), max(c0, c1) + 1):
            self.put(r, c, ch)

    def vline(self, c, r0, r1, ch="|"):
        for r in range(min(r0, r1), max(r0, r1) + 1):
            self.put(r, c, ch)

    def diag(self, r0, c0, r1, c1, ch):
        """Row-stepped line: one glyph per row, column interpolated. Terminal art reads better
        with exactly one mark per row than with a Bresenham run that doubles up unevenly."""
        steps = abs(r1 - r0)
        if steps == 0:
            return
        dr = 1 if r1 > r0 else -1
        for i in range(steps + 1):
            r = r0 + i * dr
            c = int(round(c0 + (c1 - c0) * i / steps))
            self.put(r, c, ch)

    def dots(self, r, c0, c1, step=2):
        """The horizontal dotted texture that fills both ribbons in the reference."""
        for c in range(c0, c1 + 1, step):
            if self.g[r][c] == " ":
                self.put(r, c, ".")

    def render(self):
        return [("".join(row)).rstrip() for row in self.g]


def compose():
    """Waypoints traced from the reference silhouette.

    The governing shapes, in the order they read: one long top ribbon sweeping right; a shorter
    second ribbon below it; a continuous left edge that curves in from the top-left, runs down as
    the spine, and carries on to a point at the bottom-left; and the diagonal inner cut that
    separates spine from ribbons. Both ribbons carry the horizontal dotted texture.

    The left edge is deliberately ONE continuous run from the shoulder to the tip. An earlier
    draft drew the spine as a separate vertical bar and it read as a floating pipe next to the
    mark rather than as its edge.
    """
    cv = Canvas(COLS, ROWS)

    # ── Continuous left edge: shoulder curve -> spine -> tail ────────────────
    cv.put(1, 6, "-")
    cv.put(1, 7, "'")
    cv.diag(2, 5, 5, 2, "/")
    cv.vline(1, 6, 20, "|")
    cv.vline(0, 21, 23, "|")

    # ── Upper ribbon ─────────────────────────────────────────────────────────
    cv.hline(0, 8, 39, "_")
    cv.diag(1, 38, 6, 32, "\\")
    cv.hline(6, 29, 31, "-")
    cv.hline(7, 10, 28, "-")
    cv.put(7, 29, "'")
    for r, (a, b) in ((1, (10, 36)), (2, (8, 36)), (3, (7, 35)),
                      (4, (6, 33)), (5, (5, 31)), (6, (4, 27))):
        cv.dots(r, a, b)

    # ── Inner flick: the diagonal cut between spine and ribbons ──────────────
    cv.diag(8, 9, 13, 3, "/")

    # ── Lower ribbon ─────────────────────────────────────────────────────────
    cv.hline(10, 13, 33, "_")
    cv.diag(11, 32, 15, 26, "\\")
    cv.hline(15, 17, 25, "-")
    cv.put(15, 26, "'")
    cv.hline(16, 13, 16, "-")
    cv.diag(11, 11, 16, 5, "/")
    for r, (a, b) in ((11, (14, 30)), (12, (12, 29)), (13, (11, 27)), (14, (9, 25))):
        cv.dots(r, a, b)

    # ── Tail: converges to a point at the bottom left ────────────────────────
    cv.diag(17, 11, 23, 2, "/")
    # Filled to the spine on the left and the diagonal on the right, so the tail reads as a
    # tapering wedge. Left sparse in an earlier draft it read as loose specks beside a pipe.
    for r, (a, b) in ((17, (3, 9)), (18, (3, 8)), (19, (3, 7)),
                      (20, (2, 6)), (21, (2, 4)), (22, (1, 3))):
        cv.dots(r, a, b)

    return cv.render()


# ── Narrow fallback ──────────────────────────────────────────────────────────
# The same mark at roughly half scale, for a console that actually reports a width the detailed
# art will not fit in. It is a fallback, not the default: module managers and recovery almost
# never set COLUMNS, and an unset width takes the detailed banner.
COMPACT_COLS, COMPACT_ROWS = 22, 14


def compose_compact():
    cv = Canvas(COMPACT_COLS, COMPACT_ROWS)

    cv.put(1, 3, "-")
    cv.put(1, 4, "'")
    cv.diag(2, 2, 3, 1, "/")
    cv.vline(1, 4, 11, "|")
    cv.vline(0, 12, 13, "|")

    cv.hline(0, 5, 21, "_")
    cv.diag(1, 20, 4, 17, "\\")
    cv.hline(4, 15, 16, "-")
    cv.hline(5, 6, 14, "-")
    cv.put(5, 15, "'")
    for r, (a, b) in ((1, (6, 19)), (2, (5, 18)), (3, (4, 17)), (4, (3, 13))):
        cv.dots(r, a, b)

    cv.diag(6, 5, 8, 2, "/")

    cv.hline(7, 8, 18, "_")
    cv.diag(8, 17, 10, 15, "\\")
    cv.hline(10, 10, 14, "-")
    cv.put(10, 15, "'")
    cv.hline(11, 7, 9, "-")
    cv.diag(8, 6, 11, 4, "/")
    for r, (a, b) in ((8, (9, 14)), (9, (8, 13))):
        cv.dots(r, a, b)

    cv.diag(12, 5, 13, 2, "/")
    cv.dots(12, 2, 3)

    return cv.render()


# ── Wordmark ─────────────────────────────────────────────────────────────────
# Mixed-case "Flux" in the same dashed outline idiom as the emblem, drawn by hand rather than
# taken from a FIGlet font: the reference's letterforms are outline-style with broken strokes,
# which no stock FIGlet font produces.
WORDMARK = [
    "  _____  _                       ",
    " |  ___|| |  _   _  __  __       ",
    " | |__  | | | | | | \\ \\/ /       ",
    " |  __| | | | | | |  \\  /        ",
    " | |    | | | |_| |  /  \\        ",
    " |_|    |_|  \\__,_| /_/\\_\\       ",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shell", action="store_true")
    ap.add_argument("--compact", action="store_true")
    ap.add_argument("--png")
    ap.add_argument("--wordmark", action="store_true", default=True)
    args = ap.parse_args()

    if args.compact:
        lines = compose_compact() + ["", "Flux"]
    else:
        lines = compose() + [""] + [w.rstrip() for w in WORDMARK]

    if args.png:
        render_png(lines, args.png)
        return 0

    if args.shell:
        for ln in lines:
            print(ln)
        return 0

    width = max(len(x) for x in lines)
    for i, ln in enumerate(lines):
        print("%2d|%s|" % (i, ln))
    print("\nwidth=%d height=%d" % (width, len(lines)))
    return 0


def render_png(lines, out):
    """Render the art the way a terminal would, for visual comparison with the reference."""
    from PIL import Image, ImageDraw, ImageFont

    cw, ch = 14, 30
    W = max(len(x) for x in lines) * cw + 60
    H = len(lines) * ch + 60
    img = Image.new("RGB", (W, H), (10, 12, 22))
    d = ImageDraw.Draw(img)
    font = None
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            font = ImageFont.truetype(p, 26)
            break
        except OSError:
            continue
    for i, ln in enumerate(lines):
        d.text((30, 30 + i * ch), ln, font=font, fill=(245, 240, 210))
    img.save(out)
    print("wrote %s (%dx%d)" % (out, W, H))


if __name__ == "__main__":
    sys.exit(main())
