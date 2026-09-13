#!/usr/bin/env python3
"""Generates the application icon set from the De-Sentry Aegis brand mark.

The mark is the Sentry Aegis: a faceted cryptographic shield enclosing a
tri-vector peer mesh constellation linked to an illuminated central sentinel
aperture. This matches the vector mark in the header, boot loader, and About
sheet.

Everything is drawn here rather than checked in as a binary blob so the icon
can be regenerated at any size without hunting for a source file, and so the
repository holds no opaque artwork nobody can edit.

    python app/scripts/make-icons.py
    # or
    npm run icons

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

# Palette: Action Blue and Deep Sapphire from DESIGN.md
BG_TOP = (10, 30, 60, 255)       # Midnight navy top
BG_MID = (0, 85, 175, 255)       # Action Blue mid
BG_BOTTOM = (0, 45, 115, 255)    # Deep Sapphire bottom

SHIELD_TOP = (0, 125, 245, 240)
SHIELD_BOTTOM = (0, 55, 130, 245)
SHIELD_STROKE = (65, 185, 255, 220)
SHIELD_FACET = (41, 151, 255, 80)

WHITE = (255, 255, 255, 255)
WHITE_SOFT = (255, 255, 255, 190)
CYAN_GLOW = (100, 210, 255, 255)


def shield_polygon(s: float) -> list[tuple[float, float]]:
    """Geometric coordinates of the Sentry Aegis shield."""
    cx = s * 0.50
    return [
        (cx, s * 0.13),          # Top point
        (s * 0.84, s * 0.25),    # Top-right corner
        (s * 0.84, s * 0.56),    # Mid-right flank
        (cx, s * 0.88),          # Bottom tip
        (s * 0.16, s * 0.56),    # Mid-left flank
        (s * 0.16, s * 0.25),    # Top-left corner
    ]


def render(size: int) -> Image.Image:
    """Draws at 4x and downsamples with Lanczos antialiasing."""
    scale = 4
    s = size * scale
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    # 1. Rounded-square ground with multi-stop vertical gradient
    radius = int(s * 0.225)
    gradient = Image.new("RGBA", (1, s))
    for y in range(s):
        t = y / max(1, s - 1)
        if t < 0.5:
            t2 = t * 2
            col = tuple(round(BG_TOP[i] + (BG_MID[i] - BG_TOP[i]) * t2) for i in range(4))
        else:
            t2 = (t - 0.5) * 2
            col = tuple(round(BG_MID[i] + (BG_BOTTOM[i] - BG_MID[i]) * t2) for i in range(4))
        gradient.putpixel((0, y), col)

    ground = gradient.resize((s, s))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=radius, fill=255)
    image.paste(ground, (0, 0), mask)

    # Subtle inner rim hairline
    rim_mask = Image.new("L", (s, s), 0)
    rim_draw = ImageDraw.Draw(rim_mask)
    rim_draw.rounded_rectangle([1, 1, s - 2, s - 2], radius=radius, outline=255, width=max(1, int(s * 0.012)))
    rim_color = Image.new("RGBA", (s, s), (150, 210, 255, 45))
    image.paste(rim_color, (0, 0), rim_mask)

    # 2. Sentry Shield Body
    shield_pts = shield_polygon(s)
    cx, cy = s * 0.50, s * 0.50

    # Shield fill
    shield_img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    sdraw = ImageDraw.Draw(shield_img)
    sdraw.polygon(shield_pts, fill=SHIELD_BOTTOM)

    # Left facet highlight (light reflection)
    left_facet = [
        (cx, s * 0.13),
        (s * 0.16, s * 0.25),
        (s * 0.16, s * 0.56),
        (cx, s * 0.88),
        (cx, cy),
    ]
    sdraw.polygon(left_facet, fill=SHIELD_FACET)

    # Shield outline stroke
    stroke_w = max(2, int(s * 0.022))
    sdraw.line(shield_pts + [shield_pts[0]], fill=SHIELD_STROKE, width=stroke_w, joint="curve")

    image = Image.alpha_composite(image, shield_img)
    draw = ImageDraw.Draw(image)

    # 3. Constellation Nodes & Mesh Lines
    # Equilateral-like constellation inside shield: Top, Bottom-Right, Bottom-Left
    node_top = (cx, s * 0.31)
    node_br = (s * 0.69, s * 0.61)
    node_bl = (s * 0.31, s * 0.61)
    center_core = (cx, s * 0.50)

    mesh_stroke = max(1, int(s * 0.018))
    # Triangle interconnects
    draw.line([node_top, node_br], fill=WHITE_SOFT, width=mesh_stroke)
    draw.line([node_br, node_bl], fill=WHITE_SOFT, width=mesh_stroke)
    draw.line([node_bl, node_top], fill=WHITE_SOFT, width=mesh_stroke)
    # Spokes to central sentinel core
    draw.line([node_top, center_core], fill=WHITE_SOFT, width=mesh_stroke)
    draw.line([node_br, center_core], fill=WHITE_SOFT, width=mesh_stroke)
    draw.line([node_bl, center_core], fill=WHITE_SOFT, width=mesh_stroke)

    # 4. Central Sentinel Core (Aura glow + Diamond spark)
    aura_r = s * 0.10
    aura_box = [center_core[0] - aura_r, center_core[1] - aura_r, center_core[0] + aura_r, center_core[1] + aura_r]
    aura_img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    adraw = ImageDraw.Draw(aura_img)
    adraw.ellipse(aura_box, fill=(100, 210, 255, 80))
    image = Image.alpha_composite(image, aura_img)
    draw = ImageDraw.Draw(image)

    spark_r = s * 0.05
    spark = [
        (center_core[0], center_core[1] - spark_r),
        (center_core[0] + spark_r * 0.85, center_core[1]),
        (center_core[0], center_core[1] + spark_r),
        (center_core[0] - spark_r * 0.85, center_core[1]),
    ]
    draw.polygon(spark, fill=WHITE)

    # 5. Satellite Nodes
    sat_r = s * 0.046
    for pt in (node_top, node_br, node_bl):
        box = [pt[0] - sat_r, pt[1] - sat_r, pt[0] + sat_r, pt[1] + sat_r]
        draw.ellipse(box, fill=WHITE)

    return image.resize((size, size), Image.LANCZOS)


def render_tray(size: int = 64) -> Image.Image:
    """Renders a sharp monochrome template icon for the system tray."""
    scale = 4
    s = size * scale
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    # Shield outline
    shield_pts = shield_polygon(s)
    stroke_w = max(2, int(s * 0.045))
    draw.line(shield_pts + [shield_pts[0]], fill=(255, 255, 255, 240), width=stroke_w, joint="curve")

    # Constellation
    cx = s * 0.50
    node_top = (cx, s * 0.32)
    node_br = (s * 0.67, s * 0.60)
    node_bl = (s * 0.33, s * 0.60)
    center_core = (cx, s * 0.50)

    mesh_stroke = max(1, int(s * 0.030))
    draw.line([node_top, node_br], fill=(255, 255, 255, 200), width=mesh_stroke)
    draw.line([node_br, node_bl], fill=(255, 255, 255, 200), width=mesh_stroke)
    draw.line([node_bl, node_top], fill=(255, 255, 255, 200), width=mesh_stroke)
    draw.line([node_top, center_core], fill=(255, 255, 255, 200), width=mesh_stroke)
    draw.line([node_br, center_core], fill=(255, 255, 255, 200), width=mesh_stroke)
    draw.line([node_bl, center_core], fill=(255, 255, 255, 200), width=mesh_stroke)

    # Spark
    spark_r = s * 0.055
    spark = [
        (center_core[0], center_core[1] - spark_r),
        (center_core[0] + spark_r, center_core[1]),
        (center_core[0], center_core[1] + spark_r),
        (center_core[0] - spark_r, center_core[1]),
    ]
    draw.polygon(spark, fill=(255, 255, 255, 255))

    sat_r = s * 0.055
    for pt in (node_top, node_br, node_bl):
        box = [pt[0] - sat_r, pt[1] - sat_r, pt[0] + sat_r, pt[1] + sat_r]
        draw.ellipse(box, fill=(255, 255, 255, 255))

    return image.resize((size, size), Image.LANCZOS)


def write_icns(path: Path, images: dict[str, Image.Image]) -> None:
    """Writes an .icns file."""
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
        # Windows Store / MSIX logos.
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

    # Tray icon
    tray = render_tray(64)
    tray.save(OUT / "tray.png", format="PNG")

    print(f"wrote {len(sizes) + 3} icon files to {OUT}")


if __name__ == "__main__":
    main()
