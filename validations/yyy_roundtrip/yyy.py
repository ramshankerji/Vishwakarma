"""Read, write and normalise Vishwakarma `.yyy` project files.

A `.yyy` is a SQLite database whose `object_store` table holds one row per engineering object,
with the object's fields in a per-type protobuf blob. This module knows both halves, so a file can
be compared to another file FIELD BY FIELD rather than byte by byte -- which is what a save/load
round-trip test needs, since two encoders may lay the same values out differently.

Why a hand-rolled protobuf codec instead of the generated Python bindings: the .proto files are
compiled for LITE_RUNTIME C++ only, and generating Python bindings would put a protoc dependency
and a build step between this harness and the thing it is testing. The wire format used here is
just varints, fixed32, fixed64 and length-delimited bytes; the whole codec is under 80 lines.

Read `validations/yyy_roundtrip/README.md` before changing anything here.
"""

import sqlite3
import struct

# ---------------------------------------------------------------------------------------------
# Object types -- code-core/CommonNamedNumbers.h, enum class ObjectType.
# ---------------------------------------------------------------------------------------------

OBJECT_TYPES = {
    0: "Unknown", 1: "Pyramid", 2: "Cuboid", 3: "Cone", 4: "Cylinder", 5: "Parallelepiped",
    6: "Sphere", 7: "FrustumOfPyramid", 8: "FrustumOfCone", 9: "Pipe", 10: "Folder",
    11: "Page2D", 12: "Scene3D", 13: "Line2D", 14: "Polyline2D", 15: "Polygon2D",
    16: "Text2D", 17: "Circle2D", 18: "Ellipse2D", 19: "Arc2D", 20: "Torus", 21: "Ellipsoid",
    22: "Asset2DDefinition", 23: "Asset2DInsert", 24: "Elbow", 25: "Tee", 26: "Flange",
    27: "LineMember",
}

TYPE_NUMBERS = {name: number for number, name in OBJECT_TYPES.items()}

GEOMETRY_2D_TYPES = ("Line2D", "Polyline2D", "Polygon2D", "Text2D", "Circle2D", "Ellipse2D",
                     "Arc2D")
ASSET_2D_TYPES = ("Asset2DDefinition", "Asset2DInsert")

# Default schema_version per type -- CommonNamedNumbers.h, kGeometry2D*SchemaVersion.
DEFAULT_SCHEMA_VERSION = {
    "Folder": 1, "Page2D": 1, "Scene3D": 1,
    "Line2D": 1, "Polyline2D": 1, "Polygon2D": 1, "Text2D": 1, "Circle2D": 1,
    "Ellipse2D": 2, "Arc2D": 2,          # v2 added rotation_radians
    "Asset2DDefinition": 1, "Asset2DInsert": 2,   # v2 added scale/rotation
}

# ---------------------------------------------------------------------------------------------
# Message schemas -- code-core/DataStorage_*.proto.
# field number -> (name, kind). Kinds: f64, f32, u32, u64, str, msg:<Name>, rep:<Name>
# Only the messages this harness needs to READ or WRITE are listed. Anything absent still
# round-trips correctly: decode() falls back to a generic field dump that is exact but unnamed.
# ---------------------------------------------------------------------------------------------

SCHEMAS = {
    "Folder": {1: ("name", "str"), 2: ("short_code", "str"),
               3: ("previous_sequence_no", "u64"), 4: ("next_sequence_no", "u64")},
    "Page2D": {1: ("name", "str"), 2: ("width_mm", "f32"), 3: ("height_mm", "f32"),
               4: ("previous_sequence_no", "u64"), 5: ("next_sequence_no", "u64")},
    "Scene3D": {1: ("name", "str"), 2: ("previous_sequence_no", "u64"),
                3: ("next_sequence_no", "u64")},
    "Line2D": {1: ("x1", "f64"), 2: ("y1", "f64"), 3: ("x2", "f64"), 4: ("y2", "f64"),
               5: ("line_weight", "f32"), 6: ("line_weight_mode", "u32"),
               7: ("color_abgr", "u32")},
    "Polyline2DPoint": {1: ("x", "f64"), 2: ("y", "f64")},
    "Polyline2D": {1: ("points", "rep:Polyline2DPoint"), 2: ("line_weight", "f32"),
                   3: ("line_weight_mode", "u32"), 4: ("color_abgr", "u32")},
    "Polygon2D": {1: ("line_segment_count", "u32"), 2: ("center_x", "f64"),
                  3: ("center_y", "f64"), 4: ("radius", "f64"),
                  5: ("rotation_degrees", "f64"), 6: ("line_weight", "f32"),
                  7: ("line_weight_mode", "u32"), 8: ("color_abgr", "u32")},
    "Circle2D": {1: ("center_x", "f64"), 2: ("center_y", "f64"), 3: ("radius", "f64"),
                 4: ("line_weight", "f32"), 5: ("line_weight_mode", "u32"),
                 6: ("color_abgr", "u32")},
    "Ellipse2D": {1: ("center_x", "f64"), 2: ("center_y", "f64"), 3: ("radius_x", "f64"),
                  4: ("radius_y", "f64"), 5: ("line_weight", "f32"),
                  6: ("line_weight_mode", "u32"), 7: ("color_abgr", "u32"),
                  8: ("rotation_radians", "f64")},
    "Arc2D": {1: ("center_x", "f64"), 2: ("center_y", "f64"), 3: ("radius_x", "f64"),
              4: ("radius_y", "f64"), 5: ("start_x", "f64"), 6: ("start_y", "f64"),
              7: ("end_x", "f64"), 8: ("end_y", "f64"), 9: ("line_weight", "f32"),
              10: ("line_weight_mode", "u32"), 11: ("color_abgr", "u32"),
              12: ("rotation_radians", "f64")},
    "Text2D": {1: ("x", "f64"), 2: ("y", "f64"), 3: ("text_height_cu", "f32"),
               4: ("rotation_radians", "f32"), 5: ("color_abgr", "u32"), 6: ("font", "u64"),
               7: ("justification", "u32"), 8: ("x_offset_cu", "f32"),
               9: ("y_offset_cu", "f32"), 10: ("text", "str")},
    "Asset2DDefinition": {1: ("asset_number", "u32"), 2: ("base_x", "f64"),
                          3: ("base_y", "f64")},
    "Asset2DInsert": {1: ("definition_id", "u64"), 2: ("x", "f64"), 3: ("y", "f64"),
                      4: ("scale_x", "f64"), 5: ("scale_y", "f64"),
                      6: ("rotation_degrees", "f64")},
}

# Kind -> the zero proto3 elides. Decoding fills these in so a written-out zero and an omitted
# field compare equal, which is what the C++ encoder's proto3 semantics mean.
ZEROS = {"f64": 0.0, "f32": 0.0, "u32": 0, "u64": 0, "str": ""}


# ---------------------------------------------------------------------------------------------
# Protobuf wire format
# ---------------------------------------------------------------------------------------------

def _read_varint(buf, pos):
    result = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise ValueError("truncated varint")
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, pos
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")


def _write_varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def _raw_fields(blob):
    """Parse to {field_number: [(wire_type, raw_value)]} without needing a schema.

    The key is read as a VARINT, not a single byte: field numbers above 15 need two key bytes,
    and a decoder that assumes one byte silently misreads every field after them.
    """
    fields = {}
    pos = 0
    while pos < len(blob):
        key, pos = _read_varint(blob, pos)
        field_number = key >> 3
        wire_type = key & 0x07
        if wire_type == 0:
            value, pos = _read_varint(blob, pos)
        elif wire_type == 1:
            value = blob[pos:pos + 8]
            pos += 8
        elif wire_type == 2:
            length, pos = _read_varint(blob, pos)
            value = blob[pos:pos + length]
            pos += length
        elif wire_type == 5:
            value = blob[pos:pos + 4]
            pos += 4
        else:
            raise ValueError("unsupported wire type %d at byte %d" % (wire_type, pos))
        fields.setdefault(field_number, []).append((wire_type, value))
    return fields


def decode(blob, message_name):
    """Decode a payload into an ordered dict of named fields.

    Unknown message types come back as {"_raw_field_<n>": <value>} -- exact, so they still
    compare correctly, just without names. That is deliberate: the 3D types are not part of this
    harness's job, but a fixture containing them must still round-trip provably.
    """
    fields = _raw_fields(blob)
    schema = SCHEMAS.get(message_name)
    if schema is None:
        return {"_raw_field_%d" % number: [v for _, v in values]
                for number, values in sorted(fields.items())}

    out = {}
    for number in sorted(schema):
        name, kind = schema[number]
        present = fields.get(number, [])
        if kind.startswith("rep:"):
            sub = kind[4:]
            out[name] = [decode(value, sub) for _, value in present]
            continue
        if not present:
            out[name] = ZEROS[kind]
            continue
        _, value = present[-1]   # proto3: last one wins
        if kind == "f64":
            out[name] = struct.unpack("<d", value)[0]
        elif kind == "f32":
            out[name] = struct.unpack("<f", value)[0]
        elif kind == "str":
            out[name] = value.decode("utf-8")
        else:
            out[name] = value

    for number in sorted(fields):
        if number not in schema:
            out["_unknown_field_%d" % number] = [v for _, v in fields[number]]
    return out


def encode(values, message_name):
    """Encode a dict of named fields. Zero/empty values are elided, as proto3 does."""
    schema = SCHEMAS[message_name]
    by_name = {name: (number, kind) for number, (name, kind) in schema.items()}
    out = bytearray()
    for name in sorted(values, key=lambda n: by_name[n][0]):
        number, kind = by_name[name]
        value = values[name]
        if kind.startswith("rep:"):
            for item in value:
                payload = encode(item, kind[4:])
                out += _write_varint(number << 3 | 2) + _write_varint(len(payload)) + payload
            continue
        if value == ZEROS[kind]:
            continue                                  # proto3 elides defaults
        if kind == "f64":
            out += _write_varint(number << 3 | 1) + struct.pack("<d", value)
        elif kind == "f32":
            out += _write_varint(number << 3 | 5) + struct.pack("<f", value)
        elif kind == "str":
            raw = value.encode("utf-8")
            out += _write_varint(number << 3 | 2) + _write_varint(len(raw)) + raw
        else:
            out += _write_varint(number << 3 | 0) + _write_varint(value)
    return bytes(out)


# ---------------------------------------------------------------------------------------------
# The file itself
# ---------------------------------------------------------------------------------------------

SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS file_info (
  key TEXT PRIMARY KEY,
  value BLOB NOT NULL
);
CREATE TABLE IF NOT EXISTS object_store (
  object_id INTEGER PRIMARY KEY CHECK (object_id > 0 AND object_id < 1099511627776),
  parent_id INTEGER,
  object_type INTEGER NOT NULL,
  schema_version INTEGER NOT NULL DEFAULT 1,
  lifecycle_state INTEGER NOT NULL DEFAULT 0 CHECK (lifecycle_state BETWEEN 0 AND 3),
  data BLOB NOT NULL,
  FOREIGN KEY(parent_id) REFERENCES object_store(object_id)
);
CREATE INDEX IF NOT EXISTS idx_object_parent ON object_store(parent_id);
CREATE INDEX IF NOT EXISTS idx_object_parent_live
  ON object_store(parent_id, object_id) WHERE lifecycle_state = 0;
CREATE INDEX IF NOT EXISTS idx_object_type_live
  ON object_store(object_type, object_id) WHERE lifecycle_state = 0;
"""


def _read_only_uri(path):
    """Open read-only, so reading a fixture never creates -wal/-shm beside it. Built through
    pathlib because a Windows path needs a drive letter and forward slashes to be a legal URI."""
    import pathlib
    return pathlib.Path(path).absolute().as_uri() + "?mode=ro"


class Row(object):
    """One object_store row, with its payload already decoded."""

    def __init__(self, object_id, parent_id, type_name, schema_version, lifecycle_state, fields):
        self.object_id = object_id
        self.parent_id = parent_id
        self.type_name = type_name
        self.schema_version = schema_version
        self.lifecycle_state = lifecycle_state
        self.fields = fields

    def __repr__(self):
        return "Row(%d, %s, %s)" % (self.object_id, self.type_name, self.fields)


def read(path):
    """Read a .yyy into {object_id: Row}, payloads decoded. Reads every lifecycle state.

    The application's loader only selects `lifecycle_state = 0`, but a round-trip check wants to
    see soft-deleted rows too: silently dropping or resurrecting a tombstone is exactly the kind
    of defect this harness exists to catch.
    """
    connection = sqlite3.connect(_read_only_uri(path), uri=True)
    try:
        cursor = connection.execute(
            "SELECT object_id, parent_id, object_type, schema_version, lifecycle_state, data "
            "FROM object_store ORDER BY object_id;")
        rows = {}
        for object_id, parent_id, type_number, schema_version, lifecycle, blob in cursor:
            type_name = OBJECT_TYPES.get(type_number, "Unknown(%d)" % type_number)
            rows[object_id] = Row(object_id, parent_id, type_name, schema_version, lifecycle,
                                  decode(bytes(blob), type_name))
        return rows
    finally:
        connection.close()


def read_file_info(path):
    connection = sqlite3.connect(_read_only_uri(path), uri=True)
    try:
        info = {}
        for key, value in connection.execute("SELECT key, value FROM file_info;"):
            info[key] = value.decode("utf-8", "replace") if isinstance(value, bytes) else value
        return info
    finally:
        connection.close()


def write(path, rows, object_counter_next=None):
    """Write a .yyy from an ordered list of (object_id, parent_id, type_name, fields) tuples.

    Rows are inserted in the order given, and the order MATTERS: `PRAGMA foreign_keys = ON` is set
    by the application, so a child inserted before its parent is rejected per statement. Emit
    containers first. Any row may also be a 6-tuple carrying (schema_version, lifecycle_state).
    """
    import os
    if os.path.exists(path):
        os.remove(path)
    for suffix in ("-wal", "-shm"):
        if os.path.exists(path + suffix):
            os.remove(path + suffix)

    connection = sqlite3.connect(path)
    try:
        connection.executescript(SCHEMA_SQL)
        connection.execute("PRAGMA foreign_keys = ON;")
        highest = 0
        for row in rows:
            object_id, parent_id, type_name, fields = row[0], row[1], row[2], row[3]
            schema_version = row[4] if len(row) > 4 else DEFAULT_SCHEMA_VERSION[type_name]
            lifecycle = row[5] if len(row) > 5 else 0
            connection.execute(
                "INSERT INTO object_store"
                "(object_id, parent_id, object_type, schema_version, lifecycle_state, data) "
                "VALUES(?, ?, ?, ?, ?, ?);",
                (object_id, parent_id, TYPE_NUMBERS[type_name], schema_version, lifecycle,
                 encode(fields, type_name)))
            highest = max(highest, object_id)

        for key, value in (
                ("file_format_version", "yyy-mvp-3"),
                ("file_kind", "yyy"),
                ("last_saved_by_application", "Vishwakarma"),
                ("schema_catalog_version", "8"),
                ("object_counter_next", str(object_counter_next or highest + 1))):
            connection.execute("INSERT OR REPLACE INTO file_info(key, value) VALUES(?, ?);",
                               (key, value.encode("utf-8")))
        connection.commit()
    finally:
        connection.close()
