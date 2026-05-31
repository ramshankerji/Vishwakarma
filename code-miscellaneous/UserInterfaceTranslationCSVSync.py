# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
"""
Synchronize per-language UI translation CSV files from the canonical English CSV.

The English file, UserInterfaceTranslation.csv, owns the canonical WorldID list.
This script creates or updates one CSV per non-English UILanguage value:

    UserInterfaceTranslation_01_Hindi.csv
    ...
    UserInterfaceTranslation_45_Hungarian.csv

Existing translator text is preserved by WordID. Missing WordID rows are added
with English text as the initial Human translation so the application has a
complete fallback set from day one.
"""

import csv
import sys
from pathlib import Path
from typing import Dict, Iterable, List

from UserInterfaceTranslationCompiler import LANGUAGES

TARGET_FIELDS = [ "WordID", "ShortWordName", 
    "English", "Human", "Comments", "Grok", "ChatGPT", "Gemini", "Claude",
]
ID_FIELDS = ("WordID", "WorldID")

def read_rows(path: Path) -> List[dict]:
    with path.open(newline="", encoding="utf-8") as csv_file:
        reader = csv.DictReader(csv_file)
        if not reader.fieldnames:
            return []
        return list(reader)


def write_rows(path: Path, rows: Iterable[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=TARGET_FIELDS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def parse_world_id(row: dict, path: Path) -> int:
    world_id_text = ""
    for field_name in ID_FIELDS:
        world_id_text = (row.get(field_name) or "").strip()
        if world_id_text:
            break

    if not world_id_text:
        raise ValueError(f"Missing WordID in {path.name}")
    return int(world_id_text, 0)

def english_text(row: dict) -> str:
    return (row.get("English") or "").strip()

def target_file_name(language_index: int, language_name: str) -> str:
    return f"UserInterfaceTranslation_{language_index:02d}_{language_name}.csv"

def existing_rows_by_world_id(path: Path) -> Dict[int, dict]:
    if not path.exists(): return {}

    rows: Dict[int, dict] = {}
    for row in read_rows(path):
        try: rows[parse_world_id(row, path)] = row
        except ValueError as exc:
            print(f"WARNING: {exc}; row skipped.", file=sys.stderr)
    return rows

def canonical_rows(path: Path) -> List[dict]:
    rows = read_rows(path)
    if not rows:
        print(f"ERROR: No rows found in canonical file {path}", file=sys.stderr)
        sys.exit(1)

    seen = set()
    canonical = []
    for row in rows:
        try:
            world_id = parse_world_id(row, path)
        except ValueError as exc:
            print(f"WARNING: {exc}; row skipped.", file=sys.stderr)
            continue

        if world_id in seen:
            print(f"WARNING: Duplicate WorldID {world_id} in {path.name}; later row skipped.", file=sys.stderr)
            continue

        seen.add(world_id)
        canonical.append(row)

    return canonical

def synchronized_row(canonical_row: dict, existing_row: dict) -> dict:
    fallback_text = english_text(canonical_row)

    return {
        "WordID": (canonical_row.get("WordID") or canonical_row.get("WorldID") or "").strip(),
        "ShortWordName": (canonical_row.get("ShortWordName") or "").strip(),
        "English": fallback_text,

        # Preserve existing translations. New rows start with English text.
        "Human": (existing_row.get("Human") or fallback_text).strip(),

        "Comments": (
            canonical_row.get("Comments")
            or existing_row.get("Comments")
            or ""
        ).strip(),

        "Grok": (existing_row.get("Grok") or "").strip(),
        "ChatGPT": (existing_row.get("ChatGPT") or "").strip(),
        "Gemini": (existing_row.get("Gemini") or "").strip(),
        "Claude": (existing_row.get("Claude") or "").strip(),
    }

def synchronized_row_old(canonical_row: dict, existing_row: dict) -> dict:
    fallback_text = english_text(canonical_row)
    return {
        "WordID": (canonical_row.get("WordID") or canonical_row.get("WorldID") or "").strip(),
        "ShortWordName": (canonical_row.get("ShortWordName") or "").strip(),
        "English": fallback_text,
        "Human": (existing_row.get("Human") or fallback_text).strip(),
        "Comments": (canonical_row.get("Comments") or existing_row.get("Comments") or "").strip(),
        "Grok": (existing_row.get("Grok") or "").strip(),
        "ChatGPT": (existing_row.get("ChatGPT") or "").strip(),
        "Gemini": (existing_row.get("Gemini") or "").strip(),
        "Claude": (existing_row.get("Claude") or "").strip(),
    }

def main() -> None:
    script_dir = Path(__file__).resolve().parent
    source_path = script_dir / "UserInterfaceTranslation.csv"
    canonical = canonical_rows(source_path)

    for language_index, language_name in enumerate(LANGUAGES[1:], start=1):
        target_path = script_dir / target_file_name(language_index, language_name)
        existing = existing_rows_by_world_id(target_path)

        output_rows = []
        for row in canonical:
            world_id = parse_world_id(row, source_path)
            output_rows.append(synchronized_row(row, existing.get(world_id, {})))

        write_rows(target_path, output_rows)
        print(f"Synced {target_path.name}: {len(output_rows)} rows")

    print(f"Synced {len(LANGUAGES) - 1} language files from {source_path.name}.")


if __name__ == "__main__":
    main()
