# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
"""Generate server/api/commands.py (command id -> name) from code-core/ListOfCommands.h.

The telemetry client reports ribbon usage as {commandId: count} with numeric ids only;
the /api/stats dashboard maps those ids back to names via COMMAND_NAMES. ListOfCommands.h
is the source of truth, so re-run this after adding or renaming ribbon commands:

    python code-miscellaneous/command_names_generator.py

Paths are resolved relative to the repository, so it can be run from any directory.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE = REPO_ROOT / "code-core" / "ListOfCommands.h"
OUTPUT = REPO_ROOT / "server" / "api" / "commands.py"

# Matches enum lines of the form:  NAME = 1234567890,  (ids are 10-digit, see the header).
COMMAND_RE = re.compile(r"^\s*([A-Z0-9_]+)\s*=\s*(\d{10})\s*,", re.M)


def main() -> int:
    if not SOURCE.exists():
        print(f"error: source not found: {SOURCE}", file=sys.stderr)
        return 1

    pairs = COMMAND_RE.findall(SOURCE.read_text(encoding="utf-8"))
    if not pairs:
        print(f"error: no command definitions matched in {SOURCE}", file=sys.stderr)
        return 1

    lines = ["# Generated from code-core/ListOfCommands.h - command id to name mapping.",
             "# Regenerate when new commands are added (see server/README.md).",
             "", "COMMAND_NAMES = {"]
    lines += [f'    {num}: "{name}",' for name, num in pairs]
    lines.append("}")
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    print(f"Wrote {len(pairs)} commands to {OUTPUT.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
