# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
"""Slices the 2D snap geometry out of RenderPage2D.cpp for snap_2d_geometry_test.cpp.

VERBATIM extraction is the point. A hand-copied version of this code would drift from what the
application compiles, and a geometry test that passes against a stale copy is worse than no test.
The span runs from the section's first constant to the anchor helper that follows it; both markers
are stable comment/-declaration text rather than line numbers.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SOURCE = REPO_ROOT / "code-core" / "RenderPage2D.cpp"
BEGIN = "constexpr double kSnapPi ="
END = "/* The anchor of whichever tool is running"


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")
    if BEGIN not in text or END not in text:
        print(f"error: markers not found in {SOURCE}; the snap section moved or was renamed",
              file=sys.stderr)
        return 1

    span = text[text.index(BEGIN):text.index(END)]
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("snap2d_extracted.inc")
    out.write_text(span, encoding="utf-8", newline="\n")
    print(f"Extracted {span.count(chr(10))} lines to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
