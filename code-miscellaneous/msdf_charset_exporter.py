import sys
from pathlib import Path
from fontTools.ttLib import TTFont

def codepoint_ranges(values):
    values = sorted(set(values))
    if not values:
        return
    start = prev = values[0]
    for value in values[1:]:
        if value == prev + 1:
            prev = value
            continue
        yield start, prev
        start = prev = value
    yield start, prev

def export_charset(font_path, out_path):
    font = TTFont(font_path)
    codepoints = set()
    for table in font["cmap"].tables:
        if table.isUnicode():
            codepoints.update(table.cmap.keys())

    # Keep text-control codepoints out of the atlas. UI text uses drawable glyphs and space.
    codepoints = {cp for cp in codepoints if cp == 0x20 or cp >= 0x21}

    lines = []
    for start, end in codepoint_ranges(codepoints):
        if start == end:
            lines.append(f"0x{start:X}")
        else:
            lines.append(f"[0x{start:X}, 0x{end:X}]")

    Path(out_path).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {len(codepoints)} Unicode codepoints to {out_path}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: msdf_charset_exporter.py <font> <out_charset>")
        sys.exit(1)
    export_charset(sys.argv[1], sys.argv[2])
