# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
# This file is no longer in use since, SVG files are now directly integrated into the codebase.
# The SVG library converts embedded SVGs into RGB textures directly.
"""
Compiles monochrome SVG icons into a single TTF font. Project structure expected:
Project/
├── Media/
│   ├── icon_<32 bit number>_<some_name>.svg
│   ├── icon_000983040_home.svg
│   ├── icon_983041_settings.svg
│   └── ...
├── Fonts/
└── code-miscellaneous/
    └── build_icon_font.py

Requirements: pip install fonttools svgpathtools
Optional but recommended: pip install cu2qu
External dependency: fontforge CLI must be installed and available in PATH.
Linux: sudo apt install fontforge
Windows: Install FontForge and add it to PATH.
Usage: python build_icon_font.py
"""

import os
import re
import sys
import math
import shutil
import tempfile
import subprocess
import xml.etree.ElementTree as ET

from pathlib import Path

# CONFIG
FONT_NAME = "SVG_Icons_Generated_Font"
EM_SIZE = 1000
ASCENT = 850
DESCENT = 150

ICON_VIEWBOX_SIZE = 48.0

# Unicode private use ranges
PRIVATE_USE_RANGES = [
    (0xE000, 0xF8FF),       # BMP PUA
    (0xF0000, 0xFFFFD),     # Plane 15
    (0x100000, 0x10FFFD),   # Plane 16
]

# Supported SVG shape tags
SUPPORTED_TAGS = { "path", "rect", "circle", "ellipse", "polygon", "polyline", "line",}
# PATHS
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
ICONS_DIR = PROJECT_ROOT / "Media"
FONTS_DIR = PROJECT_ROOT / "Fonts"
OUTPUT_TTF = FONTS_DIR / f"{FONT_NAME}.ttf"

# HELPERS

def is_private_use(codepoint: int) -> bool:
    for start, end in PRIVATE_USE_RANGES:
        if start <= codepoint <= end:
            return True
    return False

def warn(msg: str): print(f"[WARNING] {msg}")
def info(msg: str): print(f"[INFO] {msg}")
def error(msg: str): print(f"[ERROR] {msg}")
def strip_namespace(tag: str) -> str: return tag.split("}")[-1]
def create_hexagon_svg(output_path: Path):# Create fallback hexagon SVG.
    cx = 24
    cy = 24
    r = 16
    pts = []
    for i in range(6):
        angle = math.radians(60 * i - 30)
        x = cx + r * math.cos(angle)
        y = cy + r * math.sin(angle)
        pts.append(f"{x:.2f},{y:.2f}")
    polygon = " ".join(pts)
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg"
width="48" height="48" viewBox="0 0 48 48">
<polygon points="{polygon}" fill="black"/>
</svg>'''

    output_path.write_text(svg, encoding="utf-8")

def validate_svg(svg_path: Path):
    # Returns: (is_valid, has_supported_shapes, warnings)
    warnings = []
    has_supported = False

    try:
        tree = ET.parse(svg_path)
        root = tree.getroot()

    except Exception as e:
        warnings.append(f"Failed parsing SVG: {e}")
        return False, False, warnings

    for elem in root.iter():
        tag = strip_namespace(elem.tag)
        if tag in SUPPORTED_TAGS: has_supported = True
        elif tag in { "svg", "g", "defs", "title", "desc", "metadata", "clipPath", "mask", "style", }:
            continue
        else: warnings.append(f"Unsupported SVG object '{tag}' in {svg_path.name}")

    return True, has_supported, warnings

def generate_fontforge_script(entries, output_ttf: Path, temp_dir: Path):
    #Generate temporary FontForge Python script.

    ff_script = temp_dir / "ff_build.py"
    lines = [
        "import fontforge",
        "",
        f'font = fontforge.font()',
        f'font.fontname = "{FONT_NAME}"',
        f'font.familyname = "{FONT_NAME}"',
        f'font.fullname = "{FONT_NAME}"',
        f'font.em = {EM_SIZE}',
        f'font.ascent = {ASCENT}',
        f'font.descent = {DESCENT}',
        "",
    ]

    for codepoint, svg_path in entries:

        escaped = str(svg_path).replace("\\", "\\\\")

        lines.extend([
            f"glyph = font.createChar({codepoint})",
            f'glyph.importOutlines(r"{escaped}")',
            "glyph.removeOverlap()",
            "glyph.correctDirection()",
            "",
        ])

    out_path = str(output_ttf).replace("\\", "\\\\")
    lines.extend([
        f'font.generate(r"{out_path}")',
        'print("Font generated successfully.")',
    ])
    ff_script.write_text("\n".join(lines), encoding="utf-8")
    return ff_script

def main():# MAIN
    if not ICONS_DIR.exists():
        error(f"Icons directory not found: {ICONS_DIR}")
        sys.exit(1)

    FONTS_DIR.mkdir(parents=True, exist_ok=True)

    pattern = re.compile(
        r"^icon_([0-9]+)_([A-Za-z0-9_\-]+)\.svg$",
        re.IGNORECASE
    )

    svg_files = sorted(ICONS_DIR.glob("*.svg"))

    if not svg_files:
        error("No SVG files found.")
        sys.exit(1)

    info(f"Found {len(svg_files)} SVG files")

    font_entries = []

    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        for svg_file in svg_files:
            match = pattern.match(svg_file.name)
            if not match:
                warn(f"Skipping invalid filename format: {svg_file.name}"  )
                continue

            raw_num = match.group(1)
            icon_name = match.group(2)

            try: codepoint = int(raw_num, 10)
            except Exception:
                warn(f"Invalid decimal codepoint: {raw_num}")
                continue

            if not is_private_use(codepoint):
                warn(
                    f"Codepoint U+{codepoint:06X} "
                    f"({codepoint}) outside private use range. "
                    f"Skipping {svg_file.name}"
                )
                continue

            is_valid, has_supported, warnings = validate_svg(svg_file)
            for w in warnings: warn(w)
            final_svg = svg_file
            if not is_valid or not has_supported:
                warn(
                    f"No supported vector shapes found in "
                    f"{svg_file.name}. Using fallback hexagon."
                )
                fallback_svg = (
                    tmp_dir /
                    f"fallback_{codepoint}.svg"
                )
                create_hexagon_svg(fallback_svg)
                final_svg = fallback_svg
            font_entries.append((codepoint, final_svg))
            info(
                f"Mapped {svg_file.name} "
                f"-> U+{codepoint:06X}"
            )

        if not font_entries:
            error("No valid icons available for font generation.")
            sys.exit(1)

        ff_script = generate_fontforge_script(
            font_entries,
            OUTPUT_TTF,
            tmp_dir
        )

        info("Launching FontForge...")
        try:
            result = subprocess.run(
                ["fontforge", "-script", str(ff_script)],
                check=True,
                capture_output=True,
                text=True,
            )
            print(result.stdout)
            if result.stderr.strip(): print(result.stderr)

        except FileNotFoundError:
            error(
                "FontForge executable not found in PATH.\n"
                "Install FontForge and ensure 'fontforge' command works."
            )
            #input("\nPress Enter to exit...") #Disabled for GitHub runners.
            sys.exit(1)

        except subprocess.CalledProcessError as e:
            error("FontForge build failed.")
            print(e.stdout)
            print(e.stderr)
            #input("\nPress Enter to exit...") #Disabled for GitHub runners.
            sys.exit(1)

    info(f"TTF output written to:")
    info(str(OUTPUT_TTF))
    #input("\nPress Enter to exit...") #Disabled for GitHub runners.

if __name__ == "__main__":
    main()
