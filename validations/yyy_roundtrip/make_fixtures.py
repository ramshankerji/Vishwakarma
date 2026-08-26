"""Generate the .yyy fixtures the save/load round-trip test loads.

    python validations/yyy_roundtrip/make_fixtures.py

Writes into SampleFiles/ (gitignored). Re-run any time; every file is rewritten from scratch and
the content is deterministic, so a fixture regenerated on another machine is byte-comparable.

WHY THESE FIXTURES EXIST. id.md §8 step 3 moves the nine Cad2D*RecordCPU types onto META_DATA
and, later, into the arena. §9 names the risk plainly: that migration runs through the save/load
path, "where a defect corrupts user files silently rather than crashing". A drawing that still
LOOKS right after a reload is not evidence. These files plus check_roundtrip.py are the oracle
that says so. The identity half has since landed with these staying green throughout; the
residency half has not.

WHAT IS DELIBERATELY IN THEM. Each fixture targets a field step 3 touches:

  01_every_type        one of all nine 2D record types, so no type is migrated untested.
  02_asset_instance    the container/generator SPLIT. An asset member's parent is its
                       Asset2DInsert, not the Page2D it is drawn on, and the file stores only
                       ONE parent id -- the loader rebuilds the split from the stored parent's
                       TYPE. That reconstruction is the single most breakable thing in step 3,
                       because containerMemoryId and parentObjectId were exactly the two fields
                       that became memoryIDContainer and memoryIDGenerator.
  03_two_pages         two Page2D containers in one file, so a migration that loses track of
                       which page an object belongs to shows up as objects on the wrong sheet
                       rather than as a crash.
  04_edge_values       negative and very large coordinates, a 2-point and a many-point
                       polyline, a soft-deleted row, zero-valued fields that proto3 ELIDES
                       entirely, and non-ASCII text. Zero-elision matters: a field the encoder
                       omits and a field written as zero must decode identically.
  05_mixed_2d_3d       2D geometry beside a Scene3D container, so the 3D half is proven
                       untouched by a 2D-only migration.
  90_known_defect_*    NOT part of the suite -- a quarantined reproducer. See its own comment
                       and the README; RunRoundTrip.ps1 skips 90_ unless asked for it.

Fixtures are small on purpose. This harness answers "is it correct", not "is it fast" -- the
performance question already has its own numbers in id.md §11.7 and §11.8.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yyy   # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SAMPLE_DIR = os.path.join(REPO_ROOT, "SampleFiles")

BLACK = 0xFF000000
RED = 0xFF0000FF
PAPER_MM = 2          # Cad2DLineWeightMode::PaperMM
SCREEN_PIXEL = 1      # Cad2DLineWeightMode::ScreenPixel


def page(name, width=841.0, height=594.0):
    return {"name": name, "width_mm": width, "height_mm": height}


def line(x1, y1, x2, y2, weight=0.25, mode=PAPER_MM, color=BLACK):
    return {"x1": x1, "y1": y1, "x2": x2, "y2": y2,
            "line_weight": weight, "line_weight_mode": mode, "color_abgr": color}


def polyline(points, weight=0.25, color=BLACK):
    return {"points": [{"x": x, "y": y} for x, y in points],
            "line_weight": weight, "line_weight_mode": PAPER_MM, "color_abgr": color}


def polygon(cx, cy, radius, sides=6, rotation=45.0, color=BLACK):
    return {"line_segment_count": sides, "center_x": cx, "center_y": cy, "radius": radius,
            "rotation_degrees": rotation, "line_weight": 0.25,
            "line_weight_mode": PAPER_MM, "color_abgr": color}


def circle(cx, cy, radius, color=BLACK):
    return {"center_x": cx, "center_y": cy, "radius": radius, "line_weight": 0.25,
            "line_weight_mode": PAPER_MM, "color_abgr": color}


def ellipse(cx, cy, rx, ry, rotation=0.0, color=BLACK):
    return {"center_x": cx, "center_y": cy, "radius_x": rx, "radius_y": ry,
            "line_weight": 0.25, "line_weight_mode": PAPER_MM, "color_abgr": color,
            "rotation_radians": rotation}


def arc(cx, cy, rx, ry, sx, sy, ex, ey, rotation=0.0, color=BLACK):
    return {"center_x": cx, "center_y": cy, "radius_x": rx, "radius_y": ry,
            "start_x": sx, "start_y": sy, "end_x": ex, "end_y": ey, "line_weight": 0.25,
            "line_weight_mode": PAPER_MM, "color_abgr": color, "rotation_radians": rotation}


def text(x, y, body, height=5.0, rotation=0.0, justification=0, color=BLACK):
    return {"x": x, "y": y, "text_height_cu": height, "rotation_radians": rotation,
            "color_abgr": color, "font": 0, "justification": justification,
            "x_offset_cu": 0.0, "y_offset_cu": 0.0, "text": body}


# -------------------------------------------------------------------------------------------
# 01 -- one of every 2D record type, on a single page.
# -------------------------------------------------------------------------------------------

def fixture_every_type():
    return [
        (1, None, "Page2D", page("Every Type")),
        (2, 1, "Line2D", line(0.0, 0.0, 100.0, 50.0)),
        (3, 1, "Polyline2D", polyline([(0.0, 0.0), (50.0, 25.0), (100.0, 0.0), (150.0, 40.0)])),
        (4, 1, "Polygon2D", polygon(200.0, 100.0, 40.0, sides=6)),
        (5, 1, "Circle2D", circle(300.0, 100.0, 25.0)),
        (6, 1, "Ellipse2D", ellipse(400.0, 100.0, 50.0, 25.0, rotation=0.7853981633974483)),
        (7, 1, "Arc2D", arc(500.0, 100.0, 30.0, 30.0, 530.0, 100.0, 500.0, 130.0)),
        (8, 1, "Text2D", text(0.0, 200.0, "Every Type Fixture")),
    ]


# -------------------------------------------------------------------------------------------
# 02 -- the container/generator split, which is what step 3 is most likely to break.
#
# Row 2 is the hidden MASTER definition; its members (4, 5) hang off the DEFINITION.
# Rows 6, 7 are a placed instance's members and hang off the INSERT (3), NOT off the page.
# Row 8 is an ordinary page object, so both linkages exist side by side in one file.
# -------------------------------------------------------------------------------------------

def fixture_asset_instance():
    return [
        (1, None, "Page2D", page("Asset Instance")),
        # Definitions are TAB-level and carry no parent: BuildRowsFromTab writes
        # `row.parentId = 0; // Definitions are tab-level; no page owns them.`, and the master
        # members below hang off the definition rather than off any page. Giving this row the
        # page as its parent is what an earlier version of this fixture did, and the round trip
        # correctly reported the parent coming back as None.
        (2, None, "Asset2DDefinition", {"asset_number": 1, "base_x": 10.0, "base_y": 10.0}),
        (3, 1, "Asset2DInsert", {"definition_id": 2, "x": 300.0, "y": 200.0,
                                 "scale_x": 2.0, "scale_y": -1.0, "rotation_degrees": 30.0}),
        (4, 2, "Line2D", line(0.0, 0.0, 20.0, 0.0)),        # master member
        (5, 2, "Circle2D", circle(10.0, 10.0, 5.0)),        # master member
        (6, 3, "Line2D", line(300.0, 200.0, 340.0, 200.0)),  # placed member
        (7, 3, "Circle2D", circle(320.0, 190.0, 10.0)),      # placed member
        (8, 1, "Line2D", line(0.0, 400.0, 100.0, 400.0, color=RED)),  # plain page object
    ]


# -------------------------------------------------------------------------------------------
# 03 -- two containers, so page membership is provable rather than assumed.
# -------------------------------------------------------------------------------------------

def fixture_two_pages():
    return [
        (1, None, "Page2D", page("Sheet One", 841.0, 594.0)),
        (2, None, "Page2D", page("Sheet Two", 420.0, 297.0)),
        (3, 1, "Line2D", line(0.0, 0.0, 10.0, 10.0)),
        (4, 1, "Circle2D", circle(50.0, 50.0, 5.0)),
        (5, 2, "Line2D", line(0.0, 0.0, 20.0, 20.0, color=RED)),
        (6, 2, "Text2D", text(10.0, 10.0, "Sheet Two")),
        (7, 2, "Polyline2D", polyline([(0.0, 0.0), (5.0, 5.0), (10.0, 0.0)])),
    ]


# -------------------------------------------------------------------------------------------
# 04 -- values chosen to break a careless migration rather than to look like a drawing.
# -------------------------------------------------------------------------------------------

def fixture_edge_values():
    return [
        (1, None, "Page2D", page("Edge Values")),
        # All-zero line: proto3 elides EVERY field, so the payload blob is empty. A decoder that
        # treats "no fields" as "no record" loses this row silently.
        (2, 1, "Line2D", {"x1": 0.0, "y1": 0.0, "x2": 0.0, "y2": 0.0, "line_weight": 0.0,
                          "line_weight_mode": 0, "color_abgr": 0}),
        # Large-magnitude and negative coordinates: CPU side is double, GPU side is float32,
        # so this is where a migration that narrows a field shows up.
        (3, 1, "Line2D", line(-1234567.891, -9876543.21, 1234567.891, 9876543.21)),
        (4, 1, "Circle2D", circle(-500.5, -250.25, 0.001)),
        # Minimum-length and long polylines.
        (5, 1, "Polyline2D", polyline([(0.0, 0.0), (1.0, 1.0)])),
        (6, 1, "Polyline2D", polyline([(float(i), float(i * i % 97)) for i in range(64)])),
        # Non-ASCII, in the one type with a heap member. (The EMPTY string is fixture 90 --
        # the application's save path skips empty text, so it is not a round-trip case.)
        (8, 1, "Text2D", text(10.0, 10.0, "अभियान विश्वकर्मा — 2D")),
        # A soft-deleted row. The loader filters lifecycle_state != 0, so this must NOT come
        # back as live geometry, and a re-save must not resurrect it either.
        (9, 1, "Line2D", line(1.0, 1.0, 2.0, 2.0), 1, 1),
        # Non-default enum and a fully saturated colour.
        (10, 1, "Line2D", line(5.0, 5.0, 6.0, 6.0, weight=3.5, mode=SCREEN_PIXEL,
                               color=0xFFFFFFFF)),
        # Polygon at the clamp boundaries the renderer applies (3 and 16 segments).
        (11, 1, "Polygon2D", polygon(100.0, 100.0, 10.0, sides=3, rotation=0.0)),
        (12, 1, "Polygon2D", polygon(200.0, 100.0, 10.0, sides=16, rotation=359.9)),
    ]


# -------------------------------------------------------------------------------------------
# 05 -- 2D beside a Scene3D container, so a 2D-only migration is provably 2D-only.
# -------------------------------------------------------------------------------------------

def fixture_mixed_2d_3d():
    return [
        (1, None, "Scene3D", {"name": "Model"}),
        (2, None, "Page2D", page("Drawing")),
        (3, None, "Folder", {"name": "Documents", "short_code": "DOC"}),
        (4, 2, "Line2D", line(0.0, 0.0, 100.0, 0.0)),
        (5, 2, "Text2D", text(0.0, 10.0, "Plan")),
        (6, 2, "Arc2D", arc(50.0, 50.0, 20.0, 20.0, 70.0, 50.0, 50.0, 70.0)),
    ]


# -------------------------------------------------------------------------------------------
# 90 -- KNOWN DEFECT REPRODUCER, not part of the round-trip suite. See the README.
#
# DecodeText2D ends `return !text.text.empty();`, so ONE Text2D row carrying an empty string
# makes LoadYyyIntoTab fail outright and the whole project file becomes unopenable. The save
# path already takes the opposite view -- BuildRowsFromTab skips empty text rather than failing
# -- so the two halves disagree about the same degenerate record.
#
# Not reachable from this application's own save today, which is why it is quarantined here
# rather than left failing in fixture 04: nothing the app writes can contain such a row. It is
# still worth keeping, because the response is disproportionate (one bad row costs the entire
# file) and because a third-party writer or a future import path could produce one.
# -------------------------------------------------------------------------------------------

def fixture_empty_text():
    return [
        (1, None, "Page2D", page("Empty Text")),
        (2, 1, "Line2D", line(0.0, 0.0, 10.0, 10.0)),
        (3, 1, "Text2D", text(0.0, 0.0, "")),
    ]


FIXTURES = [
    ("01_every_type.yyy", fixture_every_type),
    ("02_asset_instance.yyy", fixture_asset_instance),
    ("03_two_pages.yyy", fixture_two_pages),
    ("04_edge_values.yyy", fixture_edge_values),
    ("05_mixed_2d_3d.yyy", fixture_mixed_2d_3d),
    ("90_known_defect_empty_text.yyy", fixture_empty_text),
]


def main():
    if not os.path.isdir(SAMPLE_DIR):
        os.makedirs(SAMPLE_DIR)
    for name, build in FIXTURES:
        path = os.path.join(SAMPLE_DIR, name)
        rows = build()
        yyy.write(path, rows)
        readback = yyy.read(path)
        assert len(readback) == len(rows), "%s: wrote %d rows, read %d" % (
            name, len(rows), len(readback))
        live = sum(1 for row in readback.values() if row.lifecycle_state == 0)
        print("%-24s %2d rows (%d live)  %6d bytes" % (
            name, len(rows), live, os.path.getsize(path)))
    print("\nFixtures in %s" % SAMPLE_DIR)


if __name__ == "__main__":
    main()
