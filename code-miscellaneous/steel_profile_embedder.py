# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
"""Generate embedded C++ data for the steel section catalog (Catalog/profiles_hot_*.csv).

The emitted header contains only the row data; the record struct and the family
enum live in the checked-in code-core/SteelProfileCatalog.h, which #includes the
generated file. Design doc: website/content/civil/SteelTable.md.
"""

import csv
import sys
from pathlib import Path

CATALOG_ID_MIN = 1 << 32  # Catalog band is [2^32, 2^40) — see website/content/software/id.md.
CATALOG_ID_MAX = 1 << 40

# profiles_hot_<FAMILY>.csv -> C++ SteelProfileFamily enumerator.
FAMILY_BY_FILE_SUFFIX = {
    "ANGLE": "Angle",
    "BAR": "Bar",
    "BULB": "Bulb",
    "CHANNEL": "Channel",
    "CHS": "CHS",
    "I": "I",
    "RAIL": "Rail",
    "RHS": "RHS",
    "TEE": "Tee",
}

# CSV geometry column -> SteelProfileRecord float field, per family.
GEOMETRY_COLUMNS = {
    "ANGLE": {"a": "a", "b": "b", "t": "t", "t2": "t2", "r1": "r1", "r2": "r2"},
    "BAR": {"a": "a", "b": "b"},
    "BULB": {"h": "h", "t": "t", "bulb_h": "bulbH", "r1": "r1"},
    "CHANNEL": {"h": "h", "b": "b", "tw": "tw", "tf": "tf", "r1": "r1", "r2": "r2", "flange_slope": "flangeSlope"},
    "CHS": {"d": "d", "t": "t"},
    "I": {"h": "h", "b": "b", "tw": "tw", "tf": "tf", "r1": "r1", "r2": "r2", "flange_slope": "flangeSlope"},
    "RAIL": {"h": "h", "b_head": "bHead", "b_foot": "bFoot", "tw": "tw"},
    "RHS": {"h": "h", "b": "b", "t": "t", "r_out": "rOut", "r_in": "rIn"},
    "TEE": {"h": "h", "b": "b", "tw": "tw", "tf": "tf", "r1": "r1", "r2": "r2", "flange_slope": "flangeSlope"},
}

# Geometry columns that must be present and strictly positive.
REQUIRED_COLUMNS = {
    "ANGLE": ("a", "b", "t"),
    "BAR": ("a",),  # b additionally required for FLAT bars, checked separately.
    "BULB": ("h", "t", "bulb_h"),
    "CHANNEL": ("h", "b", "tw", "tf"),
    "CHS": ("d", "t"),
    "I": ("h", "b", "tw", "tf"),
    "RAIL": ("h", "b_head", "b_foot", "tw"),
    "RHS": ("h", "b", "t"),
    "TEE": ("h", "b", "tw", "tf"),
}

IDENTITY_COLUMNS = ("id", "key", "status", "superseded_by", "country", "code",
                    "series", "designation", "alt_designation", "mass", "availability")

# All float fields of SteelProfileRecord in struct declaration order.
FLOAT_FIELDS = ("h", "b", "tw", "tf", "r1", "r2", "flangeSlope",
                "a", "t", "t2", "d", "rOut", "rIn", "bulbH", "bHead", "bFoot", "mass")


def parse_catalog_id(text: str, context: str) -> int:
    try:
        value = int(text, 16)
    except ValueError:
        raise RuntimeError(f"{context}: id '{text}' is not a hex integer")
    if not CATALOG_ID_MIN <= value < CATALOG_ID_MAX:
        raise RuntimeError(f"{context}: id {text} outside the catalog band [2^32, 2^40)")
    return value


def parse_dimension(row: dict, column: str, required: bool, context: str) -> float:
    text = (row.get(column) or "").strip()
    if not text:
        if required:
            raise RuntimeError(f"{context}: required column '{column}' is empty")
        return 0.0
    try:
        value = float(text)
    except ValueError:
        raise RuntimeError(f"{context}: column '{column}' value '{text}' is not numeric")
    if value < 0.0 or (required and value <= 0.0):
        raise RuntimeError(f"{context}: column '{column}' value {text} out of range")
    return value


def parse_csv(path: Path, family_suffix: str) -> list[dict]:
    geometry = GEOMETRY_COLUMNS[family_suffix]
    required = set(REQUIRED_COLUMNS[family_suffix])
    rows: list[dict] = []
    with path.open(newline="", encoding="utf-8-sig") as csv_file:
        reader = csv.DictReader(csv_file)
        header = set(reader.fieldnames or ())
        missing = [c for c in (*IDENTITY_COLUMNS, *geometry) if c not in header]
        if missing:
            raise RuntimeError(f"{path.name}: missing columns {missing}")

        for line_number, row in enumerate(reader, start=2):
            context = f"{path.name}:{line_number}"
            record = {
                "id": parse_catalog_id((row.get("id") or "").strip(), context),
                "family": FAMILY_BY_FILE_SUFFIX[family_suffix],
                "key": (row.get("key") or "").strip(),
                "country": (row.get("country") or "").strip(),
                "code": (row.get("code") or "").strip(),
                "series": (row.get("series") or "").strip(),
                "designation": (row.get("designation") or "").strip(),
                "altDesignation": (row.get("alt_designation") or "").strip(),
                **{field: 0.0 for field in FLOAT_FIELDS},
            }
            if not record["designation"]:
                raise RuntimeError(f"{context}: designation is empty")

            status = (row.get("status") or "").strip()
            if status not in ("active", "superseded"):
                raise RuntimeError(f"{context}: status '{status}' must be active|superseded")
            record["statusActive"] = 1 if status == "active" else 0

            availability = (row.get("availability") or "").strip()
            if availability not in ("current", "legacy"):
                raise RuntimeError(f"{context}: availability '{availability}' must be current|legacy")
            record["availabilityCurrent"] = 1 if availability == "current" else 0

            superseded_by = (row.get("superseded_by") or "").strip()
            record["supersededBy"] = parse_catalog_id(superseded_by, context) if superseded_by else 0

            flat_bar = family_suffix == "BAR" and record["series"].upper() == "FLAT"
            for column, field in geometry.items():
                column_required = column in required or (flat_bar and column == "b")
                record[field] = parse_dimension(row, column, column_required, context)

            record["mass"] = parse_dimension(row, "mass", True, context)
            rows.append(record)
    return rows


def cpp_string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cpp_float(value: float) -> str:
    text = f"{value:.9g}"
    if "." not in text and "e" not in text and "E" not in text:
        text += ".0"  # "160f" is not a valid C++ literal; "160.0f" is.
    return text + "f"


def generate_header(rows: list[dict]) -> str:
    lines = [
        "// This file is generated by code-miscellaneous/steel_profile_embedder.py.",
        "// Edit Catalog/profiles_hot_*.csv (through Catalog/catalog_editor_v2.py), then rebuild.",
        "// SteelProfileRecord and SteelProfileFamily are defined by the including header,",
        "// code-core/SteelProfileCatalog.h — do not include this file directly.",
        "#pragma once",
        "",
        "inline constexpr SteelProfileRecord kSteelProfiles[] = {",
    ]
    for row in rows:
        fields = [
            f"0x{row['id']:010X}ull",
            f"0x{row['supersededBy']:010X}ull" if row["supersededBy"] else "0ull",
            f"SteelProfileFamily::{row['family']}",
            str(row["statusActive"]),
            str(row["availabilityCurrent"]),
            cpp_string(row["key"]),
            cpp_string(row["country"]),
            cpp_string(row["code"]),
            cpp_string(row["series"]),
            cpp_string(row["designation"]),
            cpp_string(row["altDesignation"]),
            *(cpp_float(row[field]) for field in FLOAT_FIELDS),
        ]
        lines.append("    { " + ", ".join(fields) + " },")
    lines += [
        "};",
        "",
        "inline constexpr uint32_t kSteelProfileCount = "
        "sizeof(kSteelProfiles) / sizeof(kSteelProfiles[0]);",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: steel_profile_embedder.py <CatalogDir> <OutputHeader>", file=sys.stderr)
        return 2

    catalog_dir = Path(sys.argv[1])
    output_path = Path(sys.argv[2])

    try:
        rows: list[dict] = []
        for suffix in sorted(FAMILY_BY_FILE_SUFFIX):
            csv_path = catalog_dir / f"profiles_hot_{suffix}.csv"
            if not csv_path.is_file():
                raise RuntimeError(f"Missing catalog file: {csv_path}")
            rows.extend(parse_csv(csv_path, suffix))

        rows.sort(key=lambda row: row["id"])  # FindSteelProfileById binary-searches on id.
        for previous, current in zip(rows, rows[1:]):
            if previous["id"] == current["id"]:
                raise RuntimeError(
                    f"Duplicate catalog id 0x{current['id']:010X} "
                    f"({previous['key']} and {current['key']})")

        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(generate_header(rows), encoding="utf-8", newline="\n")
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print(f"[Pre-Build] Embedded {len(rows)} steel profiles into {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
