#!/usr/bin/env bash
# render-icons.sh — regenerate the asset-catalog PNGs from pistomp-mark.svg.
#
# The mark is the only artwork we keep; every PNG under Assets.xcassets is
# derived. Needs rsvg-convert (brew install librsvg).
#
#   ./app/artwork/render-icons.sh

set -euo pipefail

ART="$(cd "$(dirname "$0")" && pwd)"
CAT="$ART/../PiStompCompanion/Assets.xcassets"
SVG="$ART/pistomp-mark.svg"

command -v rsvg-convert >/dev/null || {
    echo "rsvg-convert not found — brew install librsvg" >&2; exit 1; }

# Menu bar: template image, 18pt. The mark's own viewBox already carries a
# 2% margin, so render it edge to edge.
rsvg-convert -w 64  -h 64  "$SVG" -o "$CAT/MenuBarIcon.imageset/menubaricon.png"
rsvg-convert -w 128 -h 128 "$SVG" -o "$CAT/MenuBarIcon.imageset/menubaricon@2x.png"

# App icon: the same mark on a light rounded plate. Bare black line art on
# transparent all but vanishes against a dark-mode Finder background, so the
# app icon carries its own ground; the menu-bar image above stays a template.
# Plate geometry follows Apple's macOS grid — 824pt of a 1024pt canvas, 185pt
# corner radius — with a soft contact shadow under it.
rsvg-convert -w 560 -h 560 "$SVG" -o "$ART/.appicon-inner.png"
python3 - "$ART/.appicon-inner.png" "$CAT/AppIcon.appiconset/appicon-1024.png" <<'PY'
import sys
from PIL import Image, ImageDraw, ImageFilter

S, PLATE, RADIUS = 1024, 824, 185
PAPER, INK = (246, 241, 231, 255), (26, 26, 26)
box = ((S - PLATE) // 2, (S - PLATE) // 2, (S + PLATE) // 2, (S + PLATE) // 2)

out = Image.new('RGBA', (S, S), (0, 0, 0, 0))

shadow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
ImageDraw.Draw(shadow).rounded_rectangle(
    (box[0], box[1] + 16, box[2], box[3] + 16), RADIUS, fill=(0, 0, 0, 70))
out.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(22)))

plate = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(plate)
d.rounded_rectangle(box, RADIUS, fill=PAPER)
d.rounded_rectangle(box, RADIUS, outline=(0, 0, 0, 30), width=3)
out.alpha_composite(plate)

# Recolour the mark from pure black to ink, keeping its antialiased alpha.
mark = Image.open(sys.argv[1]).convert('RGBA')
tinted = Image.new('RGBA', mark.size, INK + (0,))
tinted.putalpha(mark.getchannel('A'))
out.alpha_composite(tinted, ((S - mark.width) // 2, (S - mark.height) // 2))

out.save(sys.argv[2])
PY
rm -f "$ART/.appicon-inner.png"

echo "wrote:"
echo "  $CAT/MenuBarIcon.imageset/menubaricon.png (64)"
echo "  $CAT/MenuBarIcon.imageset/menubaricon@2x.png (128)"
echo "  $CAT/AppIcon.appiconset/appicon-1024.png (1024)"
