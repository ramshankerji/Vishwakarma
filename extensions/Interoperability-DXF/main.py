# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
""".dxf importer — the second Vishwakarma extension.

The host streams the raw .dxf file bytes over IPC; this worker parses them
with the pure-Python DXF reader (imported as a plain sibling module) and
streams Page2D elements back as CreatePage2DBatch messages.

Conversion policy (MVP, host has no layer concept):
  LINE                 -> Page2D line
  LWPOLYLINE/POLYLINE  -> series of Page2D lines (bulge arcs tessellated)
  CIRCLE               -> Page2D polygon (regular, high segment count)
  TEXT / MTEXT         -> Page2D text (plain content only; the host renders
                          everything with its embedded MSDF font)
  DIMENSION            -> Page2D lines + text (host has no dimension element)
Everything else the reader supports (arcs, ellipses, splines, ...) is counted
and skipped for now. Coordinates pass through unscaled: one DXF drawing unit
becomes one Page2D ComputerUnit.
"""

from __future__ import annotations

import math
import re
import sys
import traceback
from collections import Counter

import vishwakarma_api as vk
import InteroperabilityWithDXFFile as dxf

LINES_PER_BATCH = 5000
TEXTS_PER_BATCH = 1000
POLYGONS_PER_BATCH = 1000
CIRCLE_SEGMENT_COUNT = 64
ARC_TESSELLATION_STEP = math.pi / 12.0  # 15 degrees per segment on bulge arcs
MIN_SEGMENT_LENGTH = 1e-9
FALLBACK_TEXT_HEIGHT = 2.5

# Host-side Cad2DTextJustification values (3x3 grid, row-major from top-left).
JUSTIFY_GRID = ((0, 1, 2),   # top:    TopLeft, TopMiddle, TopRight
                (3, 4, 5),   # middle: MiddleLeft, Center, MiddleRight
                (6, 7, 8))   # bottom: BottomLeft, BottomCenter, BottomRight


# ---------------------------------------------------------------------------
# Text content cleanup
# ---------------------------------------------------------------------------

_MTEXT_UNICODE_RE = re.compile(r'\\U\+([0-9A-Fa-f]{4})')
_MTEXT_CONTROL_RE = re.compile(r'\\[ACFHTQWacfhtqw][^;\\{}]*;')
_MTEXT_STACK_RE = re.compile(r'\\S([^;]*);')
_MTEXT_SINGLE_RE = re.compile(r'\\[LlOoKkNPX~]')
_TEXT_SPECIAL_RE = re.compile(r'%%(.)')

_TEXT_SPECIAL = {'d': '°', 'D': '°',   # degree
                 'p': '±', 'P': '±',   # plus/minus
                 'c': 'Ø', 'C': 'Ø',   # diameter
                 '%': '%'}


def decode_text_specials(value: str) -> str:
    """Decode TEXT %% control codes; %%u / %%o (underline/overline) vanish."""
    return _TEXT_SPECIAL_RE.sub(lambda m: _TEXT_SPECIAL.get(m.group(1), ''), value)


def mtext_plain_text(value: str) -> str:
    """Strip MTEXT inline formatting down to plain displayable text."""
    # Protect escaped literals before removing control codes.
    value = value.replace('\\\\', '\x01').replace('\\{', '\x02').replace('\\}', '\x03')
    value = _MTEXT_UNICODE_RE.sub(lambda m: chr(int(m.group(1), 16)), value)
    value = _MTEXT_CONTROL_RE.sub('', value)
    value = _MTEXT_STACK_RE.sub(lambda m: m.group(1).replace('^', '/').replace('#', '/'), value)
    value = _MTEXT_SINGLE_RE.sub(lambda m: ' ' if m.group(0) in ('\\P', '\\~') else '', value)
    value = value.replace('{', '').replace('}', '')
    value = value.replace('\x01', '\\').replace('\x02', '{').replace('\x03', '}')
    return decode_text_specials(value).strip()


# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------

def tessellate_bulge(x1, y1, x2, y2, bulge):
    """Yield straight segments approximating one bulged polyline edge."""
    chord = math.hypot(x2 - x1, y2 - y1)
    sweep = 4.0 * math.atan(bulge)
    if chord < MIN_SEGMENT_LENGTH or abs(sweep) < 1e-6:
        yield (x1, y1, x2, y2)
        return

    radius = chord / (2.0 * math.sin(abs(sweep) / 2.0))  # |sweep|/2 is in (0, pi)
    # Center sits on the chord's perpendicular bisector; signed distance keeps
    # the CCW-for-positive-bulge convention (including sweeps beyond pi).
    center_offset = (chord / 2.0) / math.tan(sweep / 2.0)
    mid_x, mid_y = (x1 + x2) / 2.0, (y1 + y2) / 2.0
    normal_x, normal_y = -(y2 - y1) / chord, (x2 - x1) / chord
    center_x = mid_x + normal_x * center_offset
    center_y = mid_y + normal_y * center_offset

    start_angle = math.atan2(y1 - center_y, x1 - center_x)
    steps = max(2, math.ceil(abs(sweep) / ARC_TESSELLATION_STEP))
    prev_x, prev_y = x1, y1
    for step in range(1, steps + 1):
        if step == steps:
            next_x, next_y = x2, y2  # land exactly on the true endpoint
        else:
            angle = start_angle + sweep * step / steps
            next_x = center_x + radius * math.cos(angle)
            next_y = center_y + radius * math.sin(angle)
        yield (prev_x, prev_y, next_x, next_y)
        prev_x, prev_y = next_x, next_y


def polyline_to_lines(vertices, closed):
    """vertices: iterable of (x, y, bulge). Yields (x1, y1, x2, y2) tuples."""
    points = list(vertices)
    if len(points) < 2:
        return
    edges = list(zip(points, points[1:]))
    if closed:
        edges.append((points[-1], points[0]))
    for (x1, y1, bulge), (x2, y2, _) in edges:
        for segment in tessellate_bulge(x1, y1, x2, y2, bulge):
            sx1, sy1, sx2, sy2 = segment
            if math.hypot(sx2 - sx1, sy2 - sy1) >= MIN_SEGMENT_LENGTH:
                yield segment


def text_justification(halign: int, valign: int) -> int:
    row = 0 if valign == 3 else (1 if valign == 2 else 2)  # top/middle/bottom
    col = 2 if halign == 2 else (1 if halign in (1, 4) else 0)
    return JUSTIFY_GRID[row][col]


def format_measurement(value: float) -> str:
    text = f'{value:.2f}'.rstrip('0').rstrip('.')
    return text if text else '0'


def _style_float(dim_style, code, default):
    if dim_style is None:
        return default
    try:
        value = float(dim_style.overrides.get(code, ''))
    except ValueError:
        return default
    return value if math.isfinite(value) and value > 0.0 else default


class Converter:
    """Accumulates Page2D elements from DXF entities and tracks statistics."""

    def __init__(self, doc):
        self.doc = doc
        self.lines = []
        self.texts = []
        self.polygons = []
        self.imported = Counter()
        self.skipped = Counter()

    # -- per entity type ---------------------------------------------------

    def add_line(self, e):
        x1, y1 = e.start[0], e.start[1]
        x2, y2 = e.end[0], e.end[1]
        if math.hypot(x2 - x1, y2 - y1) < MIN_SEGMENT_LENGTH:
            self.skipped['LINE (zero length)'] += 1
            return
        self.lines.append((x1, y1, x2, y2))
        self.imported['LINE'] += 1

    def add_lwpolyline(self, e):
        segments = list(polyline_to_lines(e.vertices, e.closed))
        if not segments:
            self.skipped['LWPOLYLINE (degenerate)'] += 1
            return
        self.lines.extend(segments)
        self.imported['LWPOLYLINE'] += 1

    def add_polyline(self, e):
        vertices = [(x, y, bulge) for x, y, _z, bulge in e.vertices]
        segments = list(polyline_to_lines(vertices, e.closed))
        if not segments:
            self.skipped['POLYLINE (degenerate)'] += 1
            return
        self.lines.extend(segments)
        self.imported['POLYLINE'] += 1

    def add_circle(self, e):
        if not (e.radius > 0.0 and math.isfinite(e.radius)):
            self.skipped['CIRCLE (bad radius)'] += 1
            return
        self.polygons.append((e.center[0], e.center[1], e.radius,
                              CIRCLE_SEGMENT_COUNT, 0.0))
        self.imported['CIRCLE'] += 1

    def add_text(self, e):
        content = decode_text_specials(e.text).strip()
        if not content:
            self.skipped['TEXT (empty)'] += 1
            return
        use_align = (e.halign != 0 or e.valign != 0) and e.align_point[:2] != (0.0, 0.0)
        x, y = (e.align_point if use_align else e.insert)[:2]
        height = e.height if e.height > 0.0 else FALLBACK_TEXT_HEIGHT
        self.texts.append((x, y, height, math.radians(e.rotation),
                           text_justification(e.halign, e.valign), content))
        self.imported['TEXT'] += 1

    def add_mtext(self, e):
        content = mtext_plain_text(e.text)
        if not content:
            self.skipped['MTEXT (empty)'] += 1
            return
        justification = e.attachment - 1 if 1 <= e.attachment <= 9 else 0
        height = e.height if e.height > 0.0 else FALLBACK_TEXT_HEIGHT
        self.texts.append((e.insert[0], e.insert[1], height,
                           math.radians(e.rotation), justification, content))
        self.imported['MTEXT'] += 1

    def add_dimension(self, e):
        produced = False
        text = self._dimension_text(e)
        if text:
            style = self.doc.dim_styles.get(e.dim_style)
            height = (_style_float(style, 140, FALLBACK_TEXT_HEIGHT)   # DIMTXT
                      * _style_float(style, 40, 1.0))                  # DIMSCALE
            self.texts.append((e.text_midpoint[0], e.text_midpoint[1], height,
                               0.0, 4, text))  # 4 = Center justification
            produced = True

        if (e.dim_type & 15) in (0, 1):  # rotated/horizontal/vertical, aligned
            produced |= self._linear_dimension_lines(e)

        if produced:
            self.imported['DIMENSION'] += 1
        else:
            self.skipped['DIMENSION (no drawable parts)'] += 1

    def _dimension_text(self, e):
        if e.text == ' ':  # single space means: text suppressed
            return ''
        measured = format_measurement(e.measurement)
        if not e.text.strip():
            return measured
        return mtext_plain_text(e.text).replace('<>', measured).strip()

    def _linear_dimension_lines(self, e):
        d10 = (e.defpoint[0], e.defpoint[1])
        p13 = (e.defpoint2[0], e.defpoint2[1])
        p14 = (e.defpoint3[0], e.defpoint3[1])
        if (e.dim_type & 15) == 1:  # aligned: dim line parallel to 13->14
            ux, uy = p14[0] - p13[0], p14[1] - p13[1]
            length = math.hypot(ux, uy)
            if length < MIN_SEGMENT_LENGTH:
                return False
            ux, uy = ux / length, uy / length
        else:                       # rotated: direction from the angle field
            angle = math.radians(e.angle)
            ux, uy = math.cos(angle), math.sin(angle)

        def project(p):
            t = (p[0] - d10[0]) * ux + (p[1] - d10[1]) * uy
            return (d10[0] + t * ux, d10[1] + t * uy)

        e1 = project(p13)
        e2 = project(p14)
        added = False
        for (a, b) in ((p13, e1), (p14, e2), (e1, e2)):
            if math.hypot(b[0] - a[0], b[1] - a[1]) >= MIN_SEGMENT_LENGTH:
                self.lines.append((a[0], a[1], b[0], b[1]))
                added = True
        return added

    # -- dispatch ----------------------------------------------------------

    _HANDLERS = {
        'Line': add_line,
        'LWPolyline': add_lwpolyline,
        'Polyline': add_polyline,
        'Circle': add_circle,           # exact type only: Arc is skipped below
        'Text': add_text,
        'MText': add_mtext,
        'Dimension': add_dimension,
    }

    def convert(self):
        for entity in self.doc.entities:
            handler = self._HANDLERS.get(type(entity).__name__)
            if handler is not None:
                handler(self, entity)
            else:
                self.skipped[type(entity).__name__.upper()] += 1


def run() -> None:
    channel = vk.HostChannel()
    request = channel.recv_import_request()

    try:
        doc = dxf.read_dxf_bytes(request.file_bytes, request.file_name)
    except Exception as exc:  # Parser failure must reach the host, not crash silently.
        channel.send_result(False, f"Failed to parse '{request.file_name}': {exc}")
        return

    converter = Converter(doc)
    converter.convert()

    total = len(converter.lines) + len(converter.texts) + len(converter.polygons)
    imported = ', '.join(f'{name} {count}' for name, count in converter.imported.most_common())
    skipped = ', '.join(f'{name} {count}' for name, count in sorted(
        (converter.skipped + doc.discarded_counts).items()))
    channel.send_log(
        f"Parsed '{request.file_name}': {total} Page2D elements "
        f"({len(converter.lines)} lines, {len(converter.texts)} texts, "
        f"{len(converter.polygons)} polygons) from [{imported or 'nothing'}]"
        + (f"; skipped [{skipped}]" if skipped else ""))

    for start in range(0, len(converter.lines), LINES_PER_BATCH):
        channel.send_page2d_batch(lines=converter.lines[start:start + LINES_PER_BATCH])
    for start in range(0, len(converter.texts), TEXTS_PER_BATCH):
        channel.send_page2d_batch(texts=converter.texts[start:start + TEXTS_PER_BATCH])
    for start in range(0, len(converter.polygons), POLYGONS_PER_BATCH):
        channel.send_page2d_batch(polygons=converter.polygons[start:start + POLYGONS_PER_BATCH])

    channel.send_result(True, "", total_elements=total)


if __name__ == "__main__":
    try:
        run()
    except Exception:
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)
