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
# Flux branding validation.
#
# The banner is NOT generated. It is a conversion of an artwork the maintainer approved, and the
# approved source is not in this repository — it is supplied out of band and stays out of the
# tree (see .gitignore). So this cannot be a "regenerate and diff" check the way the icon
# generator's --check is; there is nothing here to regenerate from.
#
# What it does instead is pin the artwork's decoded identity. A banner is easy to replace by
# accident: a generator gets re-run, a placeholder gets committed, someone recolours it to match
# a theme. Each of those changes the pixels, and this notices.
#
# Why decoded pixels rather than the file's own bytes: a WebP bitstream is not stable across
# libwebp versions, so hashing the file makes the check go red on a dependency upgrade while the
# artwork is untouched — a check that cries wolf is a check someone disables. Decoding, by
# contrast, is deterministic for a given bitstream, so the decoded buffer is a stable identity
# for the artwork itself.
#
# The named assertions below are not redundant with that hash. The hash says "this changed"; the
# assertions say WHICH approved property was lost, which is the difference between a failure
# someone can act on and one they have to bisect.
#
# Usage: python3 scripts/verify-branding.py

import hashlib
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("error: Pillow is required (pip install Pillow)\n")
    sys.exit(1)

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BANNER = os.path.join(REPO_ROOT, "module", "assets", "branding", "banner.webp")

# The approved banner, converted from the maintainer-supplied source at 1280x720 (the source is
# 1672x941; 0.5625 against 0.56280, so the approved aspect ratio survives the resize).
BANNER_SIZE = (1280, 720)

# SHA-256 of the decoded RGB buffer, not of the file. Regenerate deliberately, and only when the
# approved artwork itself changes:
#   python3 -c "from PIL import Image; import hashlib, numpy; \
#     print(hashlib.sha256(numpy.asarray(Image.open( \
#     'module/assets/branding/banner.webp').convert('RGB')).tobytes()).hexdigest())"
BANNER_PIXELS_SHA256 = "7ad51533cd0febff2e887f26e5303d38dee459412537f81398531c5c230a768d"

failures = []


def fail(msg):
    failures.append(msg)
    sys.stderr.write("FAIL: %s\n" % msg)


def ok(msg):
    print("OK: %s" % msg)


def check_real_webp(path):
    """A .webp extension proves nothing; module managers decode the bitstream, so check that."""
    with open(path, "rb") as fh:
        head = fh.read(12)
    if head[:4] != b"RIFF" or head[8:12] != b"WEBP":
        fail("%s is not a real WebP file (RIFF/WEBP header absent)" % os.path.relpath(path, REPO_ROOT))
        return False
    return True


def check_banner():
    rel = os.path.relpath(BANNER, REPO_ROOT)
    if not os.path.exists(BANNER):
        fail("%s is missing" % rel)
        return
    if not check_real_webp(BANNER):
        return

    im = Image.open(BANNER).convert("RGB")
    if im.size != BANNER_SIZE:
        fail("%s is %dx%d, expected %dx%d" % ((rel,) + im.size + BANNER_SIZE))

    px = im.tobytes()
    got = hashlib.sha256(px).hexdigest()
    if got != BANNER_PIXELS_SHA256:
        fail(
            "%s no longer decodes to the approved artwork.\n"
            "      expected pixel sha256 %s\n"
            "      got                   %s\n"
            "      The banner is a conversion of a maintainer-approved source, not generated art.\n"
            "      Do not regenerate or redraw it; re-convert from the approved source."
            % (rel, BANNER_PIXELS_SHA256, got)
        )
    else:
        ok("%s matches the approved artwork (decoded pixels)" % rel)

    # The named identity properties. These survive a re-encode at a different quality, so they
    # still hold if the hash above is deliberately re-pinned after a recompression.
    w, h = im.size
    n = w * h

    def at(x, y):
        i = (y * w + x) * 3
        return px[i], px[i + 1], px[i + 2]

    def lum(p):
        return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]

    # Near-black premium background. Sampled at the corners, which the composition keeps clear of
    # the wordmark and the motif.
    corner = []
    for (cx, cy) in ((0, 0), (w - 40, 0), (0, h - 40), (w - 40, h - 40)):
        for y in range(cy, cy + 40):
            for x in range(cx, cx + 40):
                corner.append(lum(at(x, y)))
    corner_mean = sum(corner) / len(corner)
    if corner_mean > 45.0:
        fail("the banner background is not near-black (corner luminance %.1f > 45)" % corner_mean)
    else:
        ok("near-black premium background (corner luminance %.1f)" % corner_mean)

    # The gold signal-flow motif, and the absence of the rejected pink/mauve palette. The
    # superseded generated banner was built from Material 3 pink tokens; this is what tells that
    # artwork apart from the approved one even if it were resized to match.
    gold = pink = 0
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r > g > b and r > 110 and (r - b) > 35:
            gold += 1
        if r > 150 and b > g and (r - g) > 40:
            pink += 1

    if gold * 1000 < n:  # < 0.1%
        fail("the gold signal-flow motif is absent (%.3f%% warm pixels)" % (100.0 * gold / n))
    else:
        ok("gold signal-flow motif present (%.3f%% warm pixels)" % (100.0 * gold / n))

    if pink * 1000 > n:  # > 0.1%
        fail(
            "the banner carries a pink/mauve palette (%.3f%% of pixels). The approved artwork is "
            "near-black and gold; this looks like the superseded generated banner."
            % (100.0 * pink / n)
        )
    else:
        ok("no pink/mauve palette (%.3f%% of pixels)" % (100.0 * pink / n))


def check_prop_references():
    """Every branding path module.prop names must resolve to a file compile_zip.sh will place.

    module.prop names flat, module-root paths (banner.webp), because manager support for nested
    asset paths is not uniform. Those files do not exist in the source tree — compile_zip.sh
    copies them there from module/assets/. So this maps each key back to its source.
    """
    prop = os.path.join(REPO_ROOT, "module", "module.prop")
    # actionIcon and webuiIcon are deliberately absent from module.prop: no official Flux emblem
    # exists, so each manager draws its own default rather than a stand-in this project would be
    # asserting as its identity. They are not listed here either — a key that reappears without an
    # approved asset behind it should fail as an unknown path, not resolve to something generated.
    sources = {
        "banner.webp": os.path.join("module", "assets", "branding", "banner.webp"),
        "donate.webp": os.path.join("module", "assets", "icons", "donate.webp"),
    }
    with open(prop, "r", encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    seen = 0
    for line in lines:
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        if key not in ("banner", "webuiIcon", "actionIcon", "donateIcon") or not value:
            continue
        seen += 1
        src = sources.get(value)
        if src is None:
            fail("module.prop %s=%s is not a known branding asset path" % (key, value))
        elif not os.path.exists(os.path.join(REPO_ROOT, src)):
            fail("module.prop %s=%s resolves to %s, which does not exist" % (key, value, src))
        else:
            ok("module.prop %s=%s resolves to %s" % (key, value, src))
    if seen == 0:
        fail("module.prop declares no branding assets at all")


def main():
    check_banner()
    check_prop_references()
    if failures:
        sys.stderr.write("\n%d branding check(s) failed\n" % len(failures))
        return 1
    print("\nbranding OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
