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
# Flux module manager icons.
#
# Emits, from one geometry description each:
#
#   module/assets/icons/action.webp + .svg   the module manager's Action button
#   module/assets/icons/donate.webp + .svg   the Support/Donate destination
#
# There is deliberately no webui icon here. `webuiIcon` points at webroot/icon.webp, which the
# WebUI build already produces from webui/public/icon.webp — the product icon has one source, and
# copying it into module/assets/ would create a second one to forget to update.
#
# Same rules as scripts/render-banner.py, for the same reasons: geometry this repository owns
# rather than a downloaded icon set, reproducible output, no font and no network. The glyphs are
# drawn from explicit coordinates so the SVG and the WebP cannot drift. --check compares rasters
# by decoded pixel content, because WebP bytes are not stable across libwebp versions.
#
# Colours are the WebUI's Material 3 dark tokens (webui/src/assets/base.css). That file is the
# source of truth; the values are restated here because a .css cannot be imported from Python.
#
# Usage: python3 scripts/render-icons.py [--check]

import argparse
import math
import os
import sys
import tempfile

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.stderr.write("error: Pillow is required (pip install Pillow)\n")
    sys.exit(1)

SIZE = 192
SUPERSAMPLE = 4
CORNER_R = 44

SURFACE_CONTAINER_HIGH = (51, 39, 42)  # --surfaceContainerHigh #33272a
PRIMARY = (255, 176, 204)              # --primary             #ffb0cc
TERTIARY = (240, 188, 149)             # --tertiary            #f0bc95


def hexcolor(rgb):
    return "#%02x%02x%02x" % rgb


# ── Glyph geometry, in a 0..1 box that is then inset into the tile ───────────
# Action: a bolt. Flux's own energy motif, and the same idea the WebUI's BoltCharge icon carries,
# redrawn here as a closed polygon we own rather than lifted from an icon font.
BOLT = [
    (0.52, 0.00), (0.14, 0.56), (0.40, 0.56), (0.34, 1.00),
    (0.76, 0.42), (0.49, 0.42), (0.62, 0.00),
]


def heart_polygon(samples=180):
    """The standard parametric heart, sampled. Normalised into the same 0..1 box as BOLT."""
    pts = []
    for i in range(samples):
        t = 2.0 * math.pi * i / samples
        x = 16.0 * math.sin(t) ** 3
        y = -(13.0 * math.cos(t) - 5.0 * math.cos(2 * t)
              - 2.0 * math.cos(3 * t) - math.cos(4 * t))
        pts.append((x, y))
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    span = max(x1 - x0, y1 - y0)
    # Centre the shorter axis so the glyph sits square in its box.
    ox = (span - (x1 - x0)) / 2.0
    oy = (span - (y1 - y0)) / 2.0
    return [((x - x0 + ox) / span, (y - y0 + oy) / span) for (x, y) in pts]


ICONS = {
    # `inset` is tuned per glyph rather than shared: the bolt is a tall narrow shape and the
    # heart a full square one, so an identical inset makes the bolt read as the smaller icon.
    # These values equalise their apparent weight, not their bounding boxes.
    "action": {"points": BOLT, "color": PRIMARY, "inset": 0.20, "aspect": 0.68},
    "donate": {"points": heart_polygon(), "color": TERTIARY, "inset": 0.27, "aspect": 1.0},
}


def place(points, inset, aspect):
    """Map 0..1 glyph coordinates into absolute tile pixels, centred, preserving the glyph box."""
    box = SIZE * (1.0 - 2.0 * inset)
    w = box * aspect
    h = box
    ox = (SIZE - w) / 2.0
    oy = (SIZE - h) / 2.0
    return [(ox + x * w, oy + y * h) for (x, y) in points]


# ── Raster ───────────────────────────────────────────────────────────────────
def render_raster(name, spec, path):
    s = SUPERSAMPLE
    img = Image.new("RGBA", (SIZE * s, SIZE * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(
        [0, 0, SIZE * s - 1, SIZE * s - 1],
        radius=CORNER_R * s,
        fill=SURFACE_CONTAINER_HIGH + (255,),
    )
    pts = [(x * s, y * s) for (x, y) in place(spec["points"], spec["inset"], spec["aspect"])]
    d.polygon(pts, fill=spec["color"] + (255,))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save(path, "WEBP", quality=95, method=6, lossless=True)


# ── Vector ───────────────────────────────────────────────────────────────────
def render_svg(name, spec, path):
    pts = place(spec["points"], spec["inset"], spec["aspect"])
    dstr = "M " + " L ".join("%.2f %.2f" % (x, y) for (x, y) in pts) + " Z"
    svg = (
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
        'role="img" aria-label="Flux %s icon">\n'
        "<desc>Generated by scripts/render-icons.py. Edit that script, not this file.</desc>\n"
        '<rect width="%d" height="%d" rx="%d" fill="%s"/>\n'
        '<path d="%s" fill="%s"/>\n'
        "</svg>\n"
        % (SIZE, SIZE, SIZE, SIZE, name, SIZE, SIZE, CORNER_R,
           hexcolor(SURFACE_CONTAINER_HIGH), dstr, hexcolor(spec["color"]))
    )
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(svg)





# The comparison lives in scripts/asset_check.py, shared with the other generator. See that file
# for why --check compares decoded pixels rather than encoded bytes.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_check import images_match as _images_match, text_match as _text_match  # noqa: E402


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(REPO_ROOT, "module", "assets", "icons")


def generate(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    written = []
    for name, spec in sorted(ICONS.items()):
        svg_path = os.path.join(out_dir, name + ".svg")
        webp_path = os.path.join(out_dir, name + ".webp")
        render_svg(name, spec, svg_path)
        render_raster(name, spec, webp_path)
        written += [svg_path, webp_path]
    return written


def main():
    ap = argparse.ArgumentParser(description="Generate the Flux module manager icons.")
    ap.add_argument("--check", action="store_true",
                    help="regenerate into a temp dir and compare with the committed assets")
    args = ap.parse_args()

    if args.check:
        tmp = tempfile.mkdtemp(prefix="flux-icons-")
        generate(tmp)
        ok = True
        for name in sorted(ICONS):
            for ext in ("svg", "webp"):
                fn = "%s.%s" % (name, ext)
                committed = os.path.join(OUT_DIR, fn)
                generated = os.path.join(tmp, fn)
                if not os.path.exists(committed):
                    sys.stderr.write("FAIL: %s is not committed\n" % fn)
                    ok = False
                    continue
                compare = _text_match if ext == "svg" else _images_match
                same, why = compare(committed, generated)
                if same:
                    print("OK: %s matches its generator" % fn)
                else:
                    sys.stderr.write(
                        "FAIL: %s differs from what render-icons.py generates (%s). "
                        "Re-run: python3 scripts/render-icons.py\n" % (fn, why)
                    )
                    ok = False
        return 0 if ok else 1

    for p in generate(OUT_DIR):
        print("wrote %s (%d bytes)" % (p, os.path.getsize(p)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
