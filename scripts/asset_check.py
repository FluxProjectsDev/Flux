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
# Shared comparison for the asset generators' --check mode.
#
# Used by scripts/render-banner.py and scripts/render-icons.py. It lives in one module rather
# than being copied into both because it IS the anti-drift check — letting two copies of it
# diverge would be a particularly silly way to fail.
#
# The question these generators' --check answers is "does the committed artwork still match what
# its generator produces?". The obvious implementation, comparing file bytes, answers a different
# and wrong question:
#
#   WebP byte output is not stable across libwebp/Pillow versions. The first CI run of this check
#   failed on exactly that — action.webp was byte-identical locally under Pillow 12.3 and differed
#   under the runner's older build, with pixel-identical content. A check that goes red when a
#   dependency is upgraded, while nothing about the artwork changed, is a check people learn to
#   silence.
#
# So rasters are compared by decoded pixel content, with a tolerance, and SVG (which these
# scripts emit as text, verbatim) is compared byte for byte.
#
# On the tolerance: the banner is encoded lossy. Re-encoding the same pixels with a different
# libwebp build yields a file that decodes to *almost* the same pixels, differing by a few units
# in a few channels. A real change to the artwork — a moved glyph, a different colour, a resized
# canvas — is nothing like that: it moves thousands of pixels by tens or hundreds of units. The
# thresholds sit in the wide gap between those two regimes, so the check still fails loudly for
# every change worth catching.

MAX_CHANNEL_DELTA = 12  # no single channel may differ by more than this
MAX_MEAN_DELTA = 1.5  # and the average difference must stay far below it


def images_match(path_a, path_b):
    """Compare two rasters by decoded pixel content. Returns (ok, reason)."""
    from PIL import Image

    with Image.open(path_a) as a, Image.open(path_b) as b:
        if a.size != b.size:
            return False, "size %dx%d != %dx%d" % (a.size + b.size)
        pa = a.convert("RGBA").tobytes()
        pb = b.convert("RGBA").tobytes()

    if pa == pb:
        return True, ""

    if len(pa) != len(pb):
        return False, "decoded byte length differs"

    worst = 0
    total = 0
    for x, y in zip(pa, pb):
        d = x - y if x > y else y - x
        total += d
        if d > worst:
            worst = d
    mean = total / float(len(pa))

    if worst > MAX_CHANNEL_DELTA or mean > MAX_MEAN_DELTA:
        return False, "pixel content differs (max channel delta %d, mean %.3f)" % (worst, mean)
    return True, ""


def text_match(path_a, path_b):
    """Byte comparison, for output these scripts write as text. Returns (ok, reason)."""
    with open(path_a, "rb") as a, open(path_b, "rb") as b:
        return (a.read() == b.read()), "content differs"
