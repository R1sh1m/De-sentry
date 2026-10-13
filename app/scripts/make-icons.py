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

import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover - a developer-tools script
    sys.exit("Pillow is required: pip install Pillow")

OUT = Path(__file__).resolve().parent.parent / "src-tauri" / "icons"

SHIELD_TOP = (0, 113, 227, 240)
SHIELD_BOTTOM = (0, 64, 133, 245)
SHIELD_STROKE = (41, 151, 255, 220)
SHIELD_FACET = (41, 151, 255, 80)

WHITE = (255, 255, 255, 255)
WHITE_SOFT = (255, 255, 255, 190)
CYAN_GLOW = (100, 210, 255, 255)


def cubic(
    p0: tuple[float, float],
    p1: tuple[float, float],
    p2: tuple[float, float],
    p3: tuple[float, float],
    steps: int = 24,
) -> list[tuple[float, float]]:
    """Samples a cubic Bézier segment for Pillow's raster drawing APIs."""
    points = []
    for step in range(steps + 1):
        t = step / steps
        inverse = 1 - t
        points.append((
            inverse**3 * p0[0]
            + 3 * inverse**2 * t * p1[0]
            + 3 * inverse * t**2 * p2[0]
            + t**3 * p3[0],
            inverse**3 * p0[1]
            + 3 * inverse**2 * t * p1[1]
            + 3 * inverse * t**2 * p2[1]
            + t**3 * p3[1],
        ))
    return points


def shield_path(s: float) -> list[tuple[float, float]]:
    """Samples the curved shield path used by the homepage SVG."""
    top = (s * 0.50, s * 0.08)
    right_top = (s * 0.85, s * 0.225)
    right_bottom = (s * 0.85, s * 0.55)
    bottom = (s * 0.50, s * 0.92)
    left_bottom = (s * 0.15, s * 0.55)
    left_top = (s * 0.15, s * 0.225)
    return (
        [top, right_top]
        + cubic(right_top, (s * 0.85, s * 0.55), (s * 0.70, s * 0.78), bottom)[1:]
        + cubic(bottom, (s * 0.30, s * 0.78), (s * 0.15, s * 0.55), left_top)[1:]
    )


def render(size: int) -> Image.Image:
    """Draws at 4x and downsamples with Lanczos antialiasing."""
    scale = 4
    s = size * scale
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    # The app icon uses the same transparent Aegis mark as the homepage.
    # Platform launchers provide their own background treatment.
    # 1. Sentry Shield Body
    shield_pts = shield_path(s)
    cx, cy = s * 0.50, s * 0.50

    # Shield fill
    shield_img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    sdraw = ImageDraw.Draw(shield_img)
    sdraw.polygon(shield_pts, fill=SHIELD_BOTTOM)

    # Left facet highlight (light reflection)
    left_facet = (
        [(cx, s * 0.08), (s * 0.15, s * 0.225)]
        + cubic(
            (s * 0.15, s * 0.225),
            (s * 0.15, s * 0.55),
            (s * 0.30, s * 0.78),
            (cx, s * 0.92),
        )[1:]
        + [(cx, cy)]
    )
    sdraw.polygon(left_facet, fill=SHIELD_FACET)

    # Shield outline stroke
    stroke_w = max(2, int(s * 0.022))
    sdraw.line(shield_pts + [shield_pts[0]], fill=SHIELD_STROKE, width=stroke_w, joint="curve")

    image = Image.alpha_composite(image, shield_img)
    draw = ImageDraw.Draw(image)

    # 2. Constellation Nodes & Mesh Lines
    # Equilateral-like constellation inside shield: Top, Bottom-Right, Bottom-Left
    node_top = (cx, s * 0.28)
    node_br = (s * 0.70, s * 0.60)
    node_bl = (s * 0.30, s * 0.60)
    center_core = (cx, s * 0.49)

    mesh_stroke = max(1, int(s * 0.018))
    # Triangle interconnects
    draw.line([node_top, node_br], fill=WHITE_SOFT, width=mesh_stroke)
    draw.line([node_br, node_bl], fill=WHITE_SOFT, width=mesh_stroke)
    draw.line([node_bl, node_top], fill=WHITE_SOFT, width=mesh_stroke)
    # Spokes to central sentinel core
    draw.line([node_top, center_core], fill=WHITE_SOFT, width=mesh_stroke)
    draw.line([node_br, center_core], fill=WHITE_SOFT, width=mesh_stroke)
    draw.line([node_bl, center_core], fill=WHITE_SOFT, width=mesh_stroke)

    # 3. Central Sentinel Core (Aura glow + Diamond spark)
    aura_r = s * 0.12
    aura_box = [center_core[0] - aura_r, center_core[1] - aura_r, center_core[0] + aura_r, center_core[1] + aura_r]
    aura_img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    adraw = ImageDraw.Draw(aura_img)
    adraw.ellipse(aura_box, fill=(100, 210, 255, 80))
    image = Image.alpha_composite(image, aura_img)
    draw = ImageDraw.Draw(image)

    spark_r = s * 0.07
    spark = [
        (center_core[0], center_core[1] - spark_r),
        (center_core[0] + spark_r * 0.85, center_core[1]),
        (center_core[0], center_core[1] + spark_r),
        (center_core[0] - spark_r * 0.85, center_core[1]),
    ]
    draw.polygon(spark, fill=WHITE)

    # 4. Satellite Nodes
    sat_r = s * 0.045
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
    shield_pts = shield_path(s)
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
