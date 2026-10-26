#!/usr/bin/env python3
"""Generates platform icon files from the canonical 1024px PNG artwork.

The source is app/src-tauri/icons/icon-source.png. The generated ICNS, ICO,
and platform PNG sizes all come from this image so every bundle uses the same
icon artwork and background.

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
SOURCE_ICON = OUT / "icon-source.png"


def load_master_icon() -> Image.Image:
    """Loads the supplied 1024px square iOS icon export."""
    try:
        with Image.open(SOURCE_ICON) as source:
            if source.size != (1024, 1024):
                sys.exit(f"Canonical app icon must be 1024x1024: {SOURCE_ICON}")
            return source.convert("RGBA")
    except OSError as exc:
        sys.exit(f"Could not read Icon Composer source {SOURCE_ICON}: {exc}")


MASTER_ICON = load_master_icon()


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
    """Resizes the canonical artwork for the platform's legacy icon formats."""
    if size == MASTER_ICON.width:
        return MASTER_ICON.copy()
    return MASTER_ICON.resize((size, size), Image.LANCZOS)


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
            "ic10": render(1024),
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
