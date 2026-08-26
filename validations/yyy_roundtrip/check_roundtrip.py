"""Compare two .yyy files field by field, or dump one in a normalised form.

    python validations/yyy_roundtrip/check_roundtrip.py dump    SampleFiles/01_every_type.yyy
    python validations/yyy_roundtrip/check_roundtrip.py compare BEFORE.yyy AFTER.yyy

`compare` is the round-trip oracle: BEFORE is a fixture, AFTER is what the application wrote when
it loaded that fixture and saved it again. Exit code 0 means every surviving object came back with
identical type, parent linkage, schema version and field values. Non-zero means it did not, and
the report says which object and which field.

Comparison is by DECODED FIELD, never by blob bytes -- two encoders may order or elide fields
differently and still mean the same thing. Numbers are compared exactly, with no epsilon: doubles
and floats both cross protobuf as raw IEEE-754, so any drift at all is a defect, not rounding.
`-0.0` and `0.0` are reported as different for the same reason.

THREE TRANSFORMATIONS ARE EXPECTED, and are applied as rules rather than per-file exceptions:

  1. Tombstones are purged. The loader selects `lifecycle_state = 0` only, and a save rewrites the
     table from what is in memory, so a soft-deleted row present in BEFORE is legitimately absent
     from AFTER. Reported as an informational line, not a failure.
  2. Non-positive presentation values are replaced by the loader's defaults -- see SANITISED
     below. Every `Decode*` in DataStorage.cpp reads these as `value > 0 ? value : <default>`,
     so a stored zero cannot survive a load by construction. Reported, not failed, and only
     when the value moves from non-positive to exactly the documented default: a zero that comes
     back as anything ELSE is still a failure.
  3. Nothing else. Object ids survive, because a save reuses `persistedId` and only allocates a
     fresh one when it is 0.

If a future change makes a fourth transformation legitimate, add it here as a stated rule with
its reason -- not as a silenced diff.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yyy   # noqa: E402


# (type, field) -> the value DataStorage.cpp's decoder substitutes for a non-positive one.
# Kept as data rather than prose so the rule and the report cannot drift apart.
SANITISED = {
    ("Page2D", "width_mm"): 841.0,
    ("Page2D", "height_mm"): 594.0,
    ("Line2D", "line_weight"): 0.25,
    ("Polyline2D", "line_weight"): 0.25,
    ("Polygon2D", "line_weight"): 0.25,
    ("Circle2D", "line_weight"): 0.25,
    ("Ellipse2D", "line_weight"): 0.25,
    ("Arc2D", "line_weight"): 0.25,
    ("Text2D", "text_height_cu"): 3.5,
}


def is_sanitised_default(type_name, field, before_value, after_value):
    """True for the one rewrite the loader is documented to perform: a non-positive stored value
    coming back as exactly that field's default."""
    expected = SANITISED.get((type_name, field))
    if expected is None:
        return False
    return (isinstance(before_value, float) and isinstance(after_value, float)
            and before_value <= 0.0 and after_value == expected)


def show(value):
    """Exact, stable rendering. repr() on floats keeps -0.0 distinct from 0.0 and is round-trip
    exact for doubles, which is what a no-drift comparison needs."""
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, list):
        return "[" + ", ".join(show(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{" + ", ".join("%s=%s" % (k, show(v)) for k, v in sorted(value.items())) + "}"
    return repr(value)


def dump_rows(rows, stream=sys.stdout):
    for object_id in sorted(rows):
        row = rows[object_id]
        stream.write("#%d %s parent=%s schema=%d lifecycle=%d\n" % (
            row.object_id, row.type_name,
            "-" if row.parent_id is None else row.parent_id,
            row.schema_version, row.lifecycle_state))
        for name in sorted(row.fields):
            stream.write("    %-22s %s\n" % (name, show(row.fields[name])))


def compare(before_path, after_path, stream=sys.stdout):
    before = yyy.read(before_path)
    after = yyy.read(after_path)

    problems = []
    notes = []

    purged = [i for i, row in before.items()
              if row.lifecycle_state != 0 and i not in after]
    for object_id in sorted(purged):
        notes.append("#%d %s was soft-deleted and is absent after the round trip (expected: a "
                     "save rewrites the table from live objects only)"
                     % (object_id, before[object_id].type_name))

    expected = {i: row for i, row in before.items() if row.lifecycle_state == 0}

    for object_id in sorted(set(expected) - set(after)):
        row = expected[object_id]
        problems.append("MISSING  #%d %s (parent=%s) did not survive the round trip"
                        % (object_id, row.type_name, row.parent_id))

    for object_id in sorted(set(after) - set(before)):
        row = after[object_id]
        problems.append("ADDED    #%d %s (parent=%s) appeared from nowhere"
                        % (object_id, row.type_name, row.parent_id))

    for object_id in sorted(set(expected) & set(after)):
        old, new = expected[object_id], after[object_id]
        where = "#%d %s" % (object_id, old.type_name)
        if old.type_name != new.type_name:
            problems.append("TYPE     %s became %s" % (where, new.type_name))
            continue
        if old.parent_id != new.parent_id:
            problems.append("PARENT   %s parent %s -> %s  (container/generator linkage)"
                            % (where, old.parent_id, new.parent_id))
        if old.schema_version != new.schema_version:
            problems.append("SCHEMA   %s schema_version %d -> %d"
                            % (where, old.schema_version, new.schema_version))
        if old.lifecycle_state != new.lifecycle_state:
            problems.append("STATE    %s lifecycle %d -> %d"
                            % (where, old.lifecycle_state, new.lifecycle_state))
        for name in sorted(set(old.fields) | set(new.fields)):
            before_value = old.fields.get(name, "<absent>")
            after_value = new.fields.get(name, "<absent>")
            was, now = show(before_value), show(after_value)
            if was == now:
                continue
            if is_sanitised_default(old.type_name, name, before_value, after_value):
                notes.append("%s .%s %s -> %s (expected: the loader replaces a non-positive "
                             "value with this field's default)" % (where, name, was, now))
                continue
            problems.append("FIELD    %s .%s\n             before %s\n             after  %s"
                            % (where, name, was, now))

    stream.write("BEFORE %s  (%d rows, %d live)\n"
                 % (os.path.basename(before_path), len(before), len(expected)))
    stream.write("AFTER  %s  (%d rows)\n" % (os.path.basename(after_path), len(after)))
    for note in notes:
        stream.write("  note: %s\n" % note)
    if problems:
        stream.write("\n%d PROBLEM(S):\n" % len(problems))
        for problem in problems:
            stream.write("  %s\n" % problem)
        return 1
    stream.write("  OK - %d objects round-tripped with identical fields.\n" % len(expected))
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "dump":
        dump_rows(yyy.read(argv[2]))
        return 0
    if len(argv) >= 4 and argv[1] == "compare":
        return compare(argv[2], argv[3])
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
