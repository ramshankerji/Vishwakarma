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
  BLOCK + INSERT       -> Asset2D definition + placed instances carrying the
                          INSERT's scale / rotation / mirror; the host bakes
                          the transform into each instance's member geometry.
                          Rectangular arrays expand into one instance per
                          cell. Nested INSERTs inside a block are flattened
                          (with their local transforms) into the definition's
                          master geometry.
Everything else the reader supports (arcs, ellipses, splines, ...) is counted
and skipped for now. Coordinates pass through unscaled: one DXF drawing unit
becomes one Page2D ComputerUnit. The converted drawing is translated as a
whole so its bounding box centers on the Page2D origin: real DXF files often
sit 10^5..10^6 units away from their origin, which would land far outside the
host's default view and beyond float32 GPU coordinate precision.
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
ASSET_INSERTS_PER_BATCH = 5000
CIRCLE_SEGMENT_COUNT = 64
ARC_TESSELLATION_STEP = math.pi / 12.0  # 15 degrees per segment on bulge arcs
MIN_SEGMENT_LENGTH = 1e-9
FALLBACK_TEXT_HEIGHT = 2.5

# Block/INSERT (Asset2D) policy. Instances carry the INSERT's scale / rotation
# and the host bakes the transform into the placed member geometry. An INSERT
# whose scale is ~zero would collapse its members to a point (and the host
# rejects it), so those are skipped.
DEGENERATE_SCALE_TOL = 1e-9
MAX_BLOCK_RECURSION_DEPTH = 16     # Guards pathological / cyclic nested blocks.
MAX_ARRAY_CELLS = 100_000          # Cap on one INSERT's col*row expansion.

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

    def recenter(self, extra_points=()):
        """Translate this converter's elements so the bounding box of them plus
        `extra_points` (anchors that are translated by the caller, e.g. asset
        insert points) centers on the origin. Returns the applied (dx, dy).
        Scale is untouched."""
        xs, ys = [], []
        for x1, y1, x2, y2 in self.lines:
            xs += (x1, x2)
            ys += (y1, y2)
        for t in self.texts:
            xs.append(t[0])
            ys.append(t[1])
        for cx, cy, radius, _n, _rot in self.polygons:
            xs += (cx - radius, cx + radius)
            ys += (cy - radius, cy + radius)
        for px, py in extra_points:
            xs.append(px)
            ys.append(py)
        if not xs:
            return 0.0, 0.0
        dx = -(min(xs) + max(xs)) / 2.0
        dy = -(min(ys) + max(ys)) / 2.0
        if dx == 0.0 and dy == 0.0:
            return 0.0, 0.0
        self.lines = [(x1 + dx, y1 + dy, x2 + dx, y2 + dy)
                      for x1, y1, x2, y2 in self.lines]
        self.texts = [(x + dx, y + dy, height, rotation, justification, content)
                      for x, y, height, rotation, justification, content in self.texts]
        self.polygons = [(cx + dx, cy + dy, radius, count, rot)
                         for cx, cy, radius, count, rot in self.polygons]
        return dx, dy

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

    def convert(self, entities=None):
        for entity in (self.doc.entities if entities is None else entities):
            handler = self._HANDLERS.get(type(entity).__name__)
            if handler is not None:
                handler(self, entity)
            else:
                self.skipped[type(entity).__name__.upper()] += 1


# ---------------------------------------------------------------------------
# Block (Asset2D) resolution: flatten a BLOCK definition's geometry into
# (lines, texts, polygons) in the block's own frame, recursively expanding
# nested INSERTs. Cached per block name; a visiting set breaks cycles.
# ---------------------------------------------------------------------------

def transform_geometry(geom, tx, ty, base_x, base_y, sx, sy, rotation_rad):
    """Map (lines, texts, polygons) from a block frame to a placement:
    world = (tx, ty) + R(rotation) * S(sx, sy) * (P - base). Text height and
    polygon radius scale by the axis magnitude (an approximation for the
    non-uniform / rotated case, which cannot be a regular polygon)."""
    lines, texts, polygons = geom
    cos_r, sin_r = math.cos(rotation_rad), math.sin(rotation_rad)

    def xf(px, py):
        qx, qy = (px - base_x) * sx, (py - base_y) * sy
        return (tx + qx * cos_r - qy * sin_r, ty + qx * sin_r + qy * cos_r)

    out_lines = []
    for x1, y1, x2, y2 in lines:
        ax, ay = xf(x1, y1)
        bx, by = xf(x2, y2)
        out_lines.append((ax, ay, bx, by))
    out_texts = []
    height_scale = abs(sy) if sy else 1.0
    for x, y, height, rotation, justification, content in texts:
        nx, ny = xf(x, y)
        out_texts.append((nx, ny, height * height_scale,
                          rotation + rotation_rad, justification, content))
    out_polygons = []
    radius_scale = max(abs(sx), abs(sy)) or 1.0
    rotation_deg = math.degrees(rotation_rad)
    for cx, cy, radius, count, rot in polygons:
        ncx, ncy = xf(cx, cy)
        out_polygons.append((ncx, ncy, radius * radius_scale, count, rot + rotation_deg))
    return out_lines, out_texts, out_polygons


def resolve_block_geometry(doc, name, cache, visiting=None, depth=0):
    """Return (lines, texts, polygons) for a block in its own frame, flattening
    nested INSERTs. Returns empty geometry for unknown/cyclic/too-deep blocks."""
    if name in cache:
        return cache[name]
    if visiting is None:
        visiting = set()
    block = doc.blocks.get(name)
    if block is None or name in visiting or depth > MAX_BLOCK_RECURSION_DEPTH:
        return ([], [], [])

    visiting.add(name)
    conv = Converter(doc)
    plain = [e for e in block.entities if not isinstance(e, dxf.Insert)]
    conv.convert(plain)
    lines, texts, polygons = list(conv.lines), list(conv.texts), list(conv.polygons)

    for insert in (e for e in block.entities if isinstance(e, dxf.Insert)):
        child = resolve_block_geometry(doc, insert.name, cache, visiting, depth + 1)
        child_block = doc.blocks.get(insert.name)
        base = child_block.base if child_block is not None else (0.0, 0.0, 0.0)
        tl, tt, tp = transform_geometry(child, insert.insert[0], insert.insert[1],
                                        base[0], base[1], insert.x_scale, insert.y_scale,
                                        math.radians(insert.rotation))
        lines += tl
        texts += tt
        polygons += tp

    visiting.discard(name)
    result = (lines, texts, polygons)
    cache[name] = result
    return result


def geometry_bbox(geometry):
    """Bounding box (min_x, min_y, max_x, max_y) of resolved block geometry
    (lines, texts, polygons) in the block frame; None when empty."""
    lines, texts, polygons = geometry
    xs, ys = [], []
    for x1, y1, x2, y2 in lines:
        xs += (x1, x2)
        ys += (y1, y2)
    for t in texts:
        xs.append(t[0])
        ys.append(t[1])
    for cx, cy, radius, _n, _rot in polygons:
        xs += (cx - radius, cx + radius)
        ys += (cy - radius, cy + radius)
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def array_cells(insert):
    """Yield the insertion points of an INSERT's rectangular array (usually one
    cell). Spacings live in the block frame, so each cell offset gets the
    INSERT's scale and rotation. Capped so a hostile row/col count cannot
    explode memory."""
    cols, rows = insert.col_count, insert.row_count
    if cols * rows > MAX_ARRAY_CELLS:
        cols, rows = 1, 1
    rotation = math.radians(insert.rotation)
    cos_r, sin_r = math.cos(rotation), math.sin(rotation)
    for row in range(rows):
        for col in range(cols):
            ox = col * insert.col_spacing * insert.x_scale
            oy = row * insert.row_spacing * insert.y_scale
            yield (insert.insert[0] + ox * cos_r - oy * sin_r,
                   insert.insert[1] + ox * sin_r + oy * cos_r)


def run() -> None:
    channel = vk.HostChannel()
    request = channel.recv_import_request()

    try:
        doc = dxf.read_dxf_bytes(request.file_bytes, request.file_name)
    except Exception as exc:  # Parser failure must reach the host, not crash silently.
        channel.send_result(False, f"Failed to parse '{request.file_name}': {exc}")
        return

    # Model-space geometry (everything except block references) converts as before.
    model_entities = [e for e in doc.entities if not isinstance(e, dxf.Insert)]
    model_inserts = [e for e in doc.entities if isinstance(e, dxf.Insert)]
    converter = Converter(doc)
    converter.convert(model_entities)

    # Block references. Every INSERT of a defined, non-empty block becomes an
    # Asset2D instance carrying the INSERT's scale / rotation / mirror; the
    # host bakes that transform into the instance's member geometry.
    block_cache = {}
    definitions = {}        # block name -> definition dict (built lazily)
    bbox_by_key = {}        # definition key -> block-frame geometry bbox
    asset_inserts = []      # (key, x, y, sx, sy, rotation_deg), recentered below
    next_key = 1
    for insert in model_inserts:
        name = insert.name
        # Skip anonymous / system / external-reference blocks (dimensions, layouts).
        if not name or name.startswith('*'):
            converter.skipped['INSERT (system block)'] += 1
            continue
        if name not in doc.blocks:
            converter.skipped['INSERT (undefined block)'] += 1
            continue
        if (abs(insert.x_scale) < DEGENERATE_SCALE_TOL or
                abs(insert.y_scale) < DEGENERATE_SCALE_TOL):
            converter.skipped['INSERT (degenerate scale)'] += 1
            continue
        geometry = resolve_block_geometry(doc, name, block_cache)
        if not any(geometry):
            converter.skipped['INSERT (empty block)'] += 1
            continue

        if name not in definitions:
            base = doc.blocks[name].base
            definitions[name] = {
                'key': next_key, 'name': name,
                'base_x': base[0], 'base_y': base[1],
                'lines': geometry[0], 'texts': geometry[1], 'polygons': geometry[2]}
            bbox_by_key[next_key] = geometry_bbox(geometry)
            next_key += 1
        key = definitions[name]['key']
        for cx, cy in array_cells(insert):
            asset_inserts.append((key, cx, cy, insert.x_scale, insert.y_scale,
                                  insert.rotation))
        converter.imported['INSERT'] += 1

    # Definitions are created lazily by their first insert, so every one is used.
    definition_list = list(definitions.values())

    # Recenter model geometry and the asset instances together, so block
    # instances keep their position relative to the plain drawing. Each
    # instance's extents proxy is its block bbox pushed through the placement
    # transform: the bare insert point can sit far from the actual geometry
    # when the block base point is away from its drawn content. Master
    # geometry, base points and the instance transforms stay in the block frame.
    defs_by_key = {d['key']: d for d in definition_list}
    anchors = []
    for key, cx, cy, sx, sy, rot in asset_inserts:
        definition = defs_by_key[key]
        min_x, min_y, max_x, max_y = bbox_by_key[key]
        theta = math.radians(rot)
        cos_r, sin_r = math.cos(theta), math.sin(theta)
        for px, py in ((min_x, min_y), (min_x, max_y), (max_x, min_y), (max_x, max_y)):
            qx = (px - definition['base_x']) * sx
            qy = (py - definition['base_y']) * sy
            anchors.append((cx + qx * cos_r - qy * sin_r, cy + qx * sin_r + qy * cos_r))
    offset_x, offset_y = converter.recenter(extra_points=anchors)
    if offset_x or offset_y:
        asset_inserts = [(k, x + offset_x, y + offset_y, sx, sy, rot)
                         for k, x, y, sx, sy, rot in asset_inserts]

    page_total = len(converter.lines) + len(converter.texts) + len(converter.polygons)
    total = page_total + len(definition_list) + len(asset_inserts)
    imported = ', '.join(f'{name} {count}' for name, count in converter.imported.most_common())
    skipped = ', '.join(f'{name} {count}' for name, count in sorted(
        (converter.skipped + doc.discarded_counts).items()))
    channel.send_log(
        f"Parsed '{request.file_name}': {page_total} Page2D elements "
        f"({len(converter.lines)} lines, {len(converter.texts)} texts, "
        f"{len(converter.polygons)} polygons), {len(definition_list)} asset "
        f"definitions with {len(asset_inserts)} instances from [{imported or 'nothing'}]"
        + (f"; recentered by ({offset_x:.3f}, {offset_y:.3f})"
           if offset_x or offset_y else "")
        + (f"; skipped [{skipped}]" if skipped else ""))

    for start in range(0, len(converter.lines), LINES_PER_BATCH):
        channel.send_page2d_batch(lines=converter.lines[start:start + LINES_PER_BATCH])
    for start in range(0, len(converter.texts), TEXTS_PER_BATCH):
        channel.send_page2d_batch(texts=converter.texts[start:start + TEXTS_PER_BATCH])
    for start in range(0, len(converter.polygons), POLYGONS_PER_BATCH):
        channel.send_page2d_batch(polygons=converter.polygons[start:start + POLYGONS_PER_BATCH])

    # One definition per batch (each is self-contained); inserts chunked.
    for definition in definition_list:
        channel.send_asset2d_batch(definitions=[definition])
    for start in range(0, len(asset_inserts), ASSET_INSERTS_PER_BATCH):
        channel.send_asset2d_batch(inserts=asset_inserts[start:start + ASSET_INSERTS_PER_BATCH])

    channel.send_result(True, "", total_elements=total)


if __name__ == "__main__":
    try:
        run()
    except Exception:
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)
