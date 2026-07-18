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
# Flux module banner generator.
#
# Emits BOTH outputs from one geometry description:
#
#   module/assets/branding/banner.svg   editable vector source
#   module/assets/branding/banner.webp  the raster that ships in the package
#
# One generator rather than a hand-drawn SVG plus a separately-exported raster, because those
# two drift: someone edits the SVG, forgets to re-export, and the package ships a banner that no
# longer matches its own source. Here the SVG and the WebP are two serialisations of the same
# coordinates, so they cannot disagree.
#
# The wordmark is stroke geometry, not type. Rendering "FLUX" with a system font would make the
# output depend on whichever font the generating machine happened to have installed, which is
# both non-deterministic and a licensing question about an asset we would then be shipping. The
# letterforms below are drawn from explicit coordinates and owned by this repository.
#
# Colours are the WebUI's own Material 3 dark tokens (webui/src/assets/base.css), so the banner a
# module manager shows and the UI the user opens are visibly the same product.
#
# Determinism: no randomness, no time, no font metrics in the geometry, fixed supersample factor.
# The GEOMETRY is fully reproducible; the WebP *encoding* is not byte-stable across libwebp
# versions, so --check compares the raster by decoded pixel content and the SVG byte for byte.
# See _images_match for why that is the property worth asserting.
#
# Usage: python3 scripts/render-banner.py [--check]
#   --check  regenerate into a temp dir and diff against the committed files; do not write.

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

# ── Canvas ───────────────────────────────────────────────────────────────────
# 2:1. Module managers crop banners to their own aspect ratios, and they do it from the edges,
# so every element that carries meaning stays inside SAFE_X/SAFE_Y. The motif is allowed to run
# to the bleed precisely because losing part of it costs nothing.
W, H = 1280, 640
SUPERSAMPLE = 3
SAFE_X, SAFE_Y = 128, 96

# ── Material 3 dark tokens, from webui/src/assets/base.css ───────────────────
SURFACE = (28, 16, 20)             # --surface           #1c1014
SURFACE_CONTAINER = (40, 28, 32)   # --surfaceContainer  #281c20
SURFACE_HIGH = (51, 39, 42)        # --surfaceContainerHigh #33272a
ON_SURFACE = (242, 221, 226)       # --onSurface         #f2dde2
ON_SURFACE_VARIANT = (213, 194, 198)  # --onSurfaceVariant #d5c2c6
PRIMARY = (255, 176, 204)          # --primary           #ffb0cc
SECONDARY = (226, 189, 200)        # --secondary         #e2bdc8
TERTIARY = (240, 188, 149)         # --tertiary          #f0bc95


def hexcolor(rgb):
    return "#%02x%02x%02x" % rgb


# ── Wordmark geometry ────────────────────────────────────────────────────────
# Each glyph is a list of strokes in a local coordinate space where the cap height is 1.0 and the
# baseline is y=1.0. A stroke is either:
#   ("line", x0, y0, x1, y1)
#   ("arc",  cx, cy, r, start_deg, end_deg)   angles measured clockwise from 3 o'clock, screen-y
# Advance is the glyph's horizontal cell width, also in cap-height units.
GLYPHS = {
    "F": {
        "advance": 0.66,
        "strokes": [
            ("line", 0.0, 0.0, 0.0, 1.0),
            ("line", 0.0, 0.0, 0.60, 0.0),
            ("line", 0.0, 0.46, 0.48, 0.46),
        ],
    },
    "L": {
        "advance": 0.62,
        "strokes": [
            ("line", 0.0, 0.0, 0.0, 1.0),
            ("line", 0.0, 1.0, 0.56, 1.0),
        ],
    },
    "U": {
        "advance": 0.78,
        "strokes": [
            ("line", 0.0, 0.0, 0.0, 0.64),
            ("line", 0.72, 0.0, 0.72, 0.64),
            ("arc", 0.36, 0.64, 0.36, 0.0, 180.0),
        ],
    },
    "X": {
        "advance": 0.76,
        "strokes": [
            ("line", 0.0, 0.0, 0.70, 1.0),
            ("line", 0.70, 0.0, 0.0, 1.0),
        ],
    },
}

WORDMARK = "FLUX"
TRACKING = 0.20  # extra advance between glyphs, in cap-height units


def layout_wordmark(cap, origin_x, origin_y):
    """Resolve the wordmark into absolute-pixel strokes at the given cap height."""
    out = []
    pen = origin_x
    for ch in WORDMARK:
        g = GLYPHS[ch]
        for s in g["strokes"]:
            if s[0] == "line":
                _, x0, y0, x1, y1 = s
                out.append(
                    ("line",
                     pen + x0 * cap, origin_y + y0 * cap,
                     pen + x1 * cap, origin_y + y1 * cap)
                )
            else:
                _, cx, cy, r, a0, a1 = s
                out.append(
                    ("arc", pen + cx * cap, origin_y + cy * cap, r * cap, a0, a1)
                )
        pen += (g["advance"] + TRACKING) * cap
    width = pen - TRACKING * cap - origin_x
    return out, width


# ── Signal-flow motif ────────────────────────────────────────────────────────
# The "energy line" idea, kept deliberately quiet: a few phase-shifted curves whose amplitude
# decays to the left, so they read as flow converging on the wordmark rather than as decoration
# competing with it. Sampled to polylines here so the SVG and the raster trace identical points.
FLOW_LINES = [
    # (y_center, amplitude, phase, colour, alpha, width_px)
    (0.22, 54.0, 0.00, PRIMARY, 96, 3.5),
    (0.38, 68.0, 1.10, SECONDARY, 74, 3.0),
    (0.54, 60.0, 2.20, TERTIARY, 66, 3.0),
    (0.70, 72.0, 3.05, PRIMARY, 58, 2.5),
    (0.84, 46.0, 4.20, SECONDARY, 40, 2.5),
]
FLOW_X0, FLOW_X1 = 0.44, 1.03  # fractions of W; runs past the right edge on purpose
FLOW_SAMPLES = 220


FADE_END = 0.34  # fraction of the curve over which opacity ramps from 0 to full


def fade(t):
    """Opacity ramp along a flow curve: 0 at its left end, full past FADE_END, eased."""
    if t >= FADE_END:
        return 1.0
    u = t / FADE_END
    return u * u * (3.0 - 2.0 * u)


def flow_polyline(y_frac, amp, phase):
    """Sample one flow curve into absolute pixel points."""
    x0, x1 = FLOW_X0 * W, FLOW_X1 * W
    pts = []
    for i in range(FLOW_SAMPLES + 1):
        t = i / FLOW_SAMPLES
        x = x0 + (x1 - x0) * t
        # Amplitude ramps in from zero at the left so the curves emerge rather than being cut off.
        ramp = t * t * (3.0 - 2.0 * t)
        y = y_frac * H + math.sin(t * 5.2 + phase) * amp * ramp
        pts.append((x, y))
    return pts


# Nodes on the flow: three small filled dots, placed at sampled positions so they sit exactly on
# their curve in both outputs.
FLOW_NODES = [(0, 0.62), (1, 0.80), (2, 0.44), (3, 0.68)]
NODE_R = 7.0


# ── Raster output ────────────────────────────────────────────────────────────
def draw_round_line(d, p0, p1, width, fill):
    """A line with round caps. PIL's line() joints are square, so caps are drawn explicitly."""
    d.line([p0, p1], fill=fill, width=int(round(width)))
    r = width / 2.0
    for (x, y) in (p0, p1):
        d.ellipse([x - r, y - r, x + r, y + r], fill=fill)


def draw_arc_stroke(d, cx, cy, r, a0, a1, width, fill):
    """Stroke an arc by sampling it; PIL's arc() width behaves inconsistently across versions."""
    steps = max(8, int(abs(a1 - a0) / 3.0))
    pts = []
    for i in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    for i in range(len(pts) - 1):
        draw_round_line(d, pts[i], pts[i + 1], width, fill)


def render_raster(path):
    s = SUPERSAMPLE
    img = Image.new("RGB", (W * s, H * s), SURFACE)

    # Background: a soft diagonal lift toward the upper right, built by row blending. Cheap, and
    # it keeps the surface from reading as flat black on an OLED panel.
    grad = Image.new("RGB", (W * s, H * s))
    gd = ImageDraw.Draw(grad)
    for y in range(H * s):
        t = y / float(H * s - 1)
        # surface -> surface_container, eased
        e = t * t * (3.0 - 2.0 * t)
        col = tuple(
            int(round(SURFACE[i] + (SURFACE_CONTAINER[i] - SURFACE[i]) * e)) for i in range(3)
        )
        gd.line([(0, y), (W * s, y)], fill=col)
    img = grad

    overlay = Image.new("RGBA", (W * s, H * s), (0, 0, 0, 0))
    od = ImageDraw.Draw(overlay)

    # Flow motif, under the wordmark.
    sampled = []
    for (y_frac, amp, phase, col, alpha, lw) in FLOW_LINES:
        pts = [(x * s, y * s) for (x, y) in flow_polyline(y_frac, amp, phase)]
        sampled.append(pts)
        # Per-segment alpha so the curve emerges instead of switching on at its first point. The
        # SVG expresses the same fade as a stroke gradient; see render_svg.
        for i in range(len(pts) - 1):
            t = i / float(len(pts) - 1)
            draw_round_line(od, pts[i], pts[i + 1], lw * s, col + (int(alpha * fade(t)),))

    for (line_idx, t) in FLOW_NODES:
        pts = sampled[line_idx]
        x, y = pts[int(t * (len(pts) - 1))]
        col = FLOW_LINES[line_idx][3]
        r = NODE_R * s
        od.ellipse([x - r, y - r, x + r, y + r], fill=col + (150,))

    img = Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB")
    d = ImageDraw.Draw(img)

    # Wordmark.
    cap = 200.0
    stroke = 26.0
    strokes, mark_w = layout_wordmark(cap, SAFE_X, 0)
    # Vertically centre the block formed by the wordmark plus the subtitle beneath it.
    block_h = cap + 34.0 + 26.0
    top = (H - block_h) / 2.0
    strokes, mark_w = layout_wordmark(cap, SAFE_X, top)
    for st in strokes:
        if st[0] == "line":
            _, x0, y0, x1, y1 = st
            draw_round_line(d, (x0 * s, y0 * s), (x1 * s, y1 * s), stroke * s, ON_SURFACE)
        else:
            _, cx, cy, r, a0, a1 = st
            draw_arc_stroke(d, cx * s, cy * s, r * s, a0, a1, stroke * s, ON_SURFACE)

    # Accent rule under the wordmark, primary token.
    rule_y = top + cap + 34.0
    draw_round_line(
        d, (SAFE_X * s, rule_y * s), ((SAFE_X + 92.0) * s, rule_y * s), 8.0 * s, PRIMARY
    )

    # Subtitle. This is the one element that uses a font; it is small supporting text, and the
    # font is used only at generation time — nothing about it ships.
    sub_y = rule_y + 26.0
    draw_tracked_text(d, SUBTITLE, SAFE_X * s, sub_y * s, 34.0 * s, 7.0 * s, ON_SURFACE_VARIANT)

    img = img.resize((W, H), Image.LANCZOS)
    # method=6 is the slowest/best encoder setting and, with a fixed input, a deterministic one.
    img.save(path, "WEBP", quality=92, method=6)
    return img


SUBTITLE = "ADAPTIVE RUNTIME ENGINE"

_FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "DejaVuSans-Bold.ttf",
    "DejaVuSans.ttf",
]


def load_font(size):
    from PIL import ImageFont
    for cand in _FONT_CANDIDATES:
        try:
            return ImageFont.truetype(cand, int(round(size)))
        except OSError:
            continue
    raise SystemExit(
        "error: no DejaVu font found. Install fonts-dejavu-core, or the subtitle cannot be "
        "rendered deterministically."
    )


def draw_tracked_text(d, text, x, y, size, tracking, fill):
    """Letter-spaced small caps run. Drawn glyph by glyph so tracking is explicit."""
    font = load_font(size)
    pen = x
    for ch in text:
        d.text((pen, y), ch, font=font, fill=fill)
        adv = d.textlength(ch, font=font)
        pen += adv + tracking


# ── Vector output ────────────────────────────────────────────────────────────
def render_svg(path):
    cap = 200.0
    stroke = 26.0
    block_h = cap + 34.0 + 26.0
    top = (H - block_h) / 2.0
    strokes, _ = layout_wordmark(cap, SAFE_X, top)

    parts = []
    parts.append(
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d" role="img" aria-label="Flux — Adaptive Runtime Engine">'
        % (W, H, W, H)
    )
    parts.append(
        "<title>Flux — Adaptive Runtime Engine</title>"
        "<desc>Generated by scripts/render-banner.py. Edit that script, not this file.</desc>"
    )
    # One gradient for the surface, plus one per flow curve carrying that curve's opacity ramp.
    # The raster applies the same ramp per segment (see `fade`); a stroke gradient is simply how
    # a vector renderer expresses it without emitting a path per segment.
    defs = [
        '<linearGradient id="surface" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/>'
        "</linearGradient>" % (hexcolor(SURFACE), hexcolor(SURFACE_CONTAINER))
    ]
    for idx, (_, _, _, col, alpha, _) in enumerate(FLOW_LINES):
        x0 = FLOW_X0 * W
        x1 = FLOW_X1 * W
        defs.append(
            '<linearGradient id="flow%d" gradientUnits="userSpaceOnUse" '
            'x1="%.2f" y1="0" x2="%.2f" y2="0">'
            '<stop offset="0" stop-color="%s" stop-opacity="0"/>'
            '<stop offset="%.3f" stop-color="%s" stop-opacity="%.3f"/>'
            '<stop offset="1" stop-color="%s" stop-opacity="%.3f"/>'
            "</linearGradient>"
            % (idx, x0, x1, hexcolor(col), FADE_END, hexcolor(col), alpha / 255.0,
               hexcolor(col), alpha / 255.0)
        )
    parts.append("<defs>" + "".join(defs) + "</defs>")
    parts.append('<rect width="%d" height="%d" fill="url(#surface)"/>' % (W, H))

    # Flow motif.
    parts.append('<g fill="none" stroke-linecap="round">')
    sampled = []
    for idx, (y_frac, amp, phase, col, alpha, lw) in enumerate(FLOW_LINES):
        pts = flow_polyline(y_frac, amp, phase)
        sampled.append(pts)
        dstr = "M " + " L ".join("%.2f %.2f" % (x, y) for (x, y) in pts)
        parts.append(
            '<path d="%s" stroke="url(#flow%d)" stroke-width="%.1f"/>' % (dstr, idx, lw)
        )
    parts.append("</g>")

    for (line_idx, t) in FLOW_NODES:
        pts = sampled[line_idx]
        x, y = pts[int(t * (len(pts) - 1))]
        col = FLOW_LINES[line_idx][3]
        parts.append(
            '<circle cx="%.2f" cy="%.2f" r="%.1f" fill="%s" fill-opacity="%.3f"/>'
            % (x, y, NODE_R, hexcolor(col), 150 / 255.0)
        )

    # Wordmark.
    parts.append(
        '<g fill="none" stroke="%s" stroke-width="%.1f" stroke-linecap="round">'
        % (hexcolor(ON_SURFACE), stroke)
    )
    for st in strokes:
        if st[0] == "line":
            _, x0, y0, x1, y1 = st
            parts.append('<path d="M %.2f %.2f L %.2f %.2f"/>' % (x0, y0, x1, y1))
        else:
            _, cx, cy, r, a0, a1 = st
            sx = cx + r * math.cos(math.radians(a0))
            sy = cy + r * math.sin(math.radians(a0))
            ex = cx + r * math.cos(math.radians(a1))
            ey = cy + r * math.sin(math.radians(a1))
            large = 1 if abs(a1 - a0) > 180 else 0
            sweep = 1 if a1 > a0 else 0
            parts.append(
                '<path d="M %.2f %.2f A %.2f %.2f 0 %d %d %.2f %.2f"/>'
                % (sx, sy, r, r, large, sweep, ex, ey)
            )
    parts.append("</g>")

    rule_y = top + cap + 34.0
    parts.append(
        '<path d="M %.2f %.2f L %.2f %.2f" stroke="%s" stroke-width="8" '
        'stroke-linecap="round"/>' % (SAFE_X, rule_y, SAFE_X + 92.0, rule_y, hexcolor(PRIMARY))
    )

    # The raster draws the subtitle with DejaVu; the vector asks for a generic sans-serif so the
    # source stays free of any font dependency. Editors will see a near-, not pixel-, match.
    parts.append(
        '<text x="%.2f" y="%.2f" fill="%s" font-family="sans-serif" font-size="34" '
        'font-weight="700" letter-spacing="7">%s</text>'
        % (SAFE_X, rule_y + 26.0 + 34.0 * 0.80, hexcolor(ON_SURFACE_VARIANT), SUBTITLE)
    )

    parts.append("</svg>")
    svg = "\n".join(parts) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(svg)
    return svg


# ── Entry point ──────────────────────────────────────────────────────────────



# The comparison lives in scripts/asset_check.py, shared with the other generator. See that file
# for why --check compares decoded pixels rather than encoded bytes.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asset_check import images_match as _images_match, text_match as _text_match  # noqa: E402


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(REPO_ROOT, "module", "assets", "branding")


def main():
    ap = argparse.ArgumentParser(description="Generate the Flux module banner.")
    ap.add_argument(
        "--check",
        action="store_true",
        help="regenerate into a temp dir and compare with the committed assets",
    )
    args = ap.parse_args()

    if args.check:
        tmp = tempfile.mkdtemp(prefix="flux-banner-")
        svg_tmp = os.path.join(tmp, "banner.svg")
        webp_tmp = os.path.join(tmp, "banner.webp")
        render_svg(svg_tmp)
        render_raster(webp_tmp)
        ok = True
        for name, generated, compare in (
            ("banner.svg", svg_tmp, _text_match),
            ("banner.webp", webp_tmp, _images_match),
        ):
            committed = os.path.join(OUT_DIR, name)
            if not os.path.exists(committed):
                sys.stderr.write("FAIL: %s is not committed\n" % name)
                ok = False
                continue
            same, why = compare(committed, generated)
            if same:
                print("OK: %s matches its generator" % name)
            else:
                sys.stderr.write(
                    "FAIL: %s differs from what render-banner.py generates (%s). "
                    "Re-run: python3 scripts/render-banner.py\n" % (name, why)
                )
                ok = False
        return 0 if ok else 1

    os.makedirs(OUT_DIR, exist_ok=True)
    svg_path = os.path.join(OUT_DIR, "banner.svg")
    webp_path = os.path.join(OUT_DIR, "banner.webp")
    render_svg(svg_path)
    render_raster(webp_path)
    print("wrote %s (%d bytes)" % (svg_path, os.path.getsize(svg_path)))
    print("wrote %s (%d bytes, %dx%d)" % (webp_path, os.path.getsize(webp_path), W, H))
    return 0


if __name__ == "__main__":
    sys.exit(main())
