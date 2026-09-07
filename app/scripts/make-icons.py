#!/usr/bin/env python3
"""Generates the application icon set from one vector description.

The mark is a hexagon (a node) with three satellites linked to it (its
replicas) -- the same shape as the `node` and `network` glyphs in the UI, so
the dock icon and the sidebar read as the same product.

Everything is drawn here rather than checked in as a binary blob so the icon
can be regenerated at any size without hunting for a source file, and so the
repository holds no opaque artwork nobody can edit.

    python app/scripts/make-icons.py

Requires Pillow. Writes into app/src-tauri/icons/.
"""

from __future__ import annotations

import math
import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover - a developer-tools script
    sys.exit("Pillow is required: pip install Pillow")

OUT = Path(__file__).resolve().parent.parent / "src-tauri" / "icons"

# Action Blue from DESIGN.md, on a rounded square in the same blue at full
# saturation. A transparent icon disappears against a dark dock; a filled one
# does not.
BLUE = (0, 102, 204, 255)
BLUE_DEEP = (0, 74, 153, 255)
WHITE = (255, 255, 255, 255)
WHITE_SOFT = (255, 255, 255, 205)


def hexagon(cx: float, cy: float, r: float) -> list[tuple[float, float]]:
    # Flat-top hexagon, matching the `node` glyph's orientation.
    return [
        (cx + r * math.cos(math.radians(angle)), cy + r * math.sin(math.radians(angle)))
        for angle in range(-90, 270, 60)
    ]


def render(size: int) -> Image.Image:
    """Draws at 4x and downsamples, which is cheaper than antialiasing by hand."""
    scale = 4
    s = size * scale
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    # Rounded-square ground with a vertical gradient, so the icon has depth at
    # 32px. Drawn row by row and clipped by a rounded-rectangle mask: a hard
    # two-tone split reads as a printing defect at small sizes.
    radius = int(s * 0.225)
    gradient = Image.new("RGBA", (1, s))
    for y in range(s):
        t = y / max(1, s - 1)
        gradient.putpixel(
            (0, y),
            tuple(round(BLUE[i] + (BLUE_DEEP[i] - BLUE[i]) * t) for i in range(4)),
        )
    ground = gradient.resize((s, s))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=radius, fill=255)
    image.paste(ground, (0, 0), mask)

    cx = cy = s / 2
    core_r = s * 0.150
    orbit_r = s * 0.295
    satellite_r = s * 0.052
    stroke = max(1, int(s * 0.020))

    # Links first, so the nodes sit on top of them.
    for angle in (-90, 30, 150):
        rad = math.radians(angle)
        x = cx + orbit_r * math.cos(rad)
        y = cy + orbit_r * math.sin(rad)
        draw.line([(cx, cy), (x, y)], fill=WHITE_SOFT, width=stroke)

    draw.polygon(hexagon(cx, cy, core_r), fill=WHITE)

    for angle in (-90, 30, 150):
        rad = math.radians(angle)
        x = cx + orbit_r * math.cos(rad)
        y = cy + orbit_r * math.sin(rad)
        draw.ellipse(
            [x - satellite_r, y - satellite_r, x + satellite_r, y + satellite_r],
            fill=WHITE,
        )

    return image.resize((size, size), Image.LANCZOS)


def write_icns(path: Path, images: dict[str, Image.Image]) -> None:
    """Writes an .icns by hand.

    Pillow only saves ICNS on macOS, and this has to run wherever the icons are
    regenerated. The container format is trivial: a magic, a total length, then
    length-prefixed typed chunks -- and modern macOS accepts PNG payloads for
    every type used here.
    """
    chunks = b""
    for ostype, image in images.items():
        payload = to_png_bytes(image)
        chunks += ostype.encode("ascii") + struct.pack(">I", len(payload) + 8) + payload
    path.write_bytes(b"icns" + struct.pack(">I", len(chunks) + 8) + chunks)


def to_png_bytes(image: Image.Image) -> bytes:
    from io import BytesIO

    buffer = BytesIO()
    image.save(buffer, format="PNG")
    return buffer.getvalue()


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)

    sizes = {
        "32x32.png": 32,
        "128x128.png": 128,
        "128x128@2x.png": 256,
        "icon.png": 512,
        # Windows Store / MSIX logos. Harmless elsewhere, required if the
        # bundle target is ever extended to msix.
        "Square30x30Logo.png": 30,
        "Square44x44Logo.png": 44,
        "Square71x71Logo.png": 71,
        "Square89x89Logo.png": 89,
        "Square107x107Logo.png": 107,
        "Square142x142Logo.png": 142,
        "Square150x150Logo.png": 150,
        "Square284x284Logo.png": 284,
        "Square310x310Logo.png": 310,
        "StoreLogo.png": 50,
    }
    for name, size in sizes.items():
        render(size).save(OUT / name, format="PNG")

    # .ico carries every size Windows picks between; leaving out 16 and 24
    # gives a blurry taskbar at 100% scaling.
    render(256).save(
        OUT / "icon.ico",
        format="ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
    )

    write_icns(
        OUT / "icon.icns",
        {
            "ic07": render(128),
            "ic08": render(256),
            "ic09": render(512),
            "ic11": render(32),
            "ic12": render(64),
            "ic13": render(256),
            "ic14": render(512),
        },
    )

    # The tray icon is monochrome-friendly: a template image on macOS is tinted
    # by the system, so a coloured tray icon looks wrong next to every other
    # one in the menu bar.
    tray = render(64).convert("LA").convert("RGBA")
    tray.save(OUT / "tray.png", format="PNG")

    print(f"wrote {len(sizes) + 3} icon files to {OUT}")


if __name__ == "__main__":
    main()
