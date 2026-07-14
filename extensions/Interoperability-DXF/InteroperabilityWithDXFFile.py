# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
# DXF Format is a registered trademark of Autodesk, Inc. in the USA and/or other countries.
# This file has been developed by Claude Fable 5 using DXF Specification released by Autodesk.
# autocad_2012_pdf_dxf-reference_enu.pdf and some sample DXF files from Internet.
# We intend to support a very small subset of DXF entities and features, 
# enough to import simple 2D drawings into Vishwakarma.

"""Pure-Python reader for ASCII DXF files (AutoCAD 2012-era tag set).

Reads model-space entities plus BLOCK definitions and their INSERT references.
Supported entity types are converted to plain Python objects (see the
dataclasses below); everything else is counted and discarded. Deliberately
discarded by design:
  - ATTRIB / ATTDEF block attributes (INSERT geometry is kept, text is not)
  - Embedded OLE objects (OLE2FRAME)
  - Paper-space / plot-layout entities (group code 67 == 1)

Symbol tables read: LAYER, LTYPE (line types), STYLE (text styles) and
DIMSTYLE (dimension styles). All HEADER variables are kept in a dict.

Command line usage prints simple statistics:
    python InteroperabilityWithDXFFile.py <drawing.dxf>

Future use: entities read here will be imported into Page2D logical
container elements.

Security posture (input files are untrusted):
  - Hard caps on file size and tag count bound memory before any allocation.
  - The parser is fully iterative (no recursion) and every loop advances a
    monotonically increasing index, so no crafted input can hang it.
  - Numeric values are range-checked: non-finite floats (inf/nan) and
    out-of-int32 integers fall back to safe defaults.
  - C0/C1 control characters are stripped from the whole file up front, so
    terminal escape sequences cannot survive into names, text or CLI output.
  - Lines are split on '\n' only (never splitlines()), so exotic Unicode
    line separators cannot desynchronise the code/value tag pairing.
  - Group codes must be ASCII decimal in the DXF-defined range 0..1071;
    anything else fails fast with a DxfError that truncates file content
    before echoing it.
String values (text, names) are passed through verbatim apart from control
character stripping; consumers must treat them as untrusted data.
"""

from __future__ import annotations

import math
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# Resource caps for untrusted input. Exceeding either raises DxfError.
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_TAGS = 25_000_000

_MAX_GROUP_CODE = 1071        # highest group code defined by the DXF spec
_INT32_MIN, _INT32_MAX = -2**31, 2**31 - 1

# C0 control characters (except \t \n \r) plus DEL: stripped from the input.
_CTRL_CHARS_RE = re.compile('[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]')
_CTRL_STRIP_TABLE = {c: None for c in (*range(0x00, 0x09), 0x0b, 0x0c,
                                       *range(0x0e, 0x20), 0x7f)}


class DxfError(Exception):
    """Raised when the file is not a readable ASCII DXF file."""


# ---------------------------------------------------------------------------
# Tolerant numeric converters: a malformed or hostile value must never crash
# the reader nor smuggle non-finite / out-of-range numbers downstream.
# ---------------------------------------------------------------------------

def _f(value: str, default: float = 0.0) -> float:
    try:
        result = float(value)
    except ValueError:
        return default
    return result if math.isfinite(result) else default


def _i(value: str, default: int = 0) -> int:
    try:
        result = int(value.strip())
    except ValueError:
        return default
    return result if _INT32_MIN <= result <= _INT32_MAX else default


# ---------------------------------------------------------------------------
# Symbol table records
# ---------------------------------------------------------------------------

@dataclass
class Layer:
    name: str = ''
    color: int = 7            # ACI color; negative means the layer is off
    linetype: str = 'Continuous'
    flags: int = 0            # bit 1 = frozen, bit 4 = locked
    lineweight: int = -3      # 1/100 mm; -3 = default
    plottable: bool = True
    true_color: Optional[int] = None  # 24-bit RGB if present (code 420)

    @property
    def is_off(self) -> bool:
        return self.color < 0

    @property
    def is_frozen(self) -> bool:
        return bool(self.flags & 1)

    @property
    def is_locked(self) -> bool:
        return bool(self.flags & 4)


@dataclass
class Linetype:
    name: str = ''
    description: str = ''
    pattern_length: float = 0.0
    dashes: List[float] = field(default_factory=list)  # code 49 elements


@dataclass
class TextStyle:
    name: str = ''
    font: str = ''            # primary font file (code 3)
    bigfont: str = ''         # big font file for Asian text (code 4)
    height: float = 0.0       # 0.0 = not fixed
    width_factor: float = 1.0
    oblique_angle: float = 0.0
    flags: int = 0


@dataclass
class DimStyle:
    name: str = ''
    # All DIM* variable overrides kept raw, keyed by DXF group code.
    overrides: Dict[int, str] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Entities. Coordinates are drawing units (see header['$INSUNITS']).
# ---------------------------------------------------------------------------

Point3 = Tuple[float, float, float]


@dataclass
class Entity:
    layer: str = '0'
    linetype: str = 'ByLayer'
    color: int = 256          # ACI; 256 = ByLayer, 0 = ByBlock
    lineweight: int = -1      # 1/100 mm; -1 = ByLayer
    linetype_scale: float = 1.0
    handle: str = ''


@dataclass
class Line(Entity):
    start: Point3 = (0.0, 0.0, 0.0)
    end: Point3 = (0.0, 0.0, 0.0)


@dataclass
class DxfPoint(Entity):
    location: Point3 = (0.0, 0.0, 0.0)


@dataclass
class Circle(Entity):
    center: Point3 = (0.0, 0.0, 0.0)
    radius: float = 0.0


@dataclass
class Arc(Circle):
    start_angle: float = 0.0  # degrees, counter-clockwise
    end_angle: float = 0.0


@dataclass
class Ellipse(Entity):
    center: Point3 = (0.0, 0.0, 0.0)
    major_axis: Point3 = (0.0, 0.0, 0.0)  # endpoint relative to center
    ratio: float = 1.0                    # minor/major axis ratio
    start_param: float = 0.0              # radians
    end_param: float = 6.283185307179586


@dataclass
class LWPolyline(Entity):
    closed: bool = False
    const_width: float = 0.0
    elevation: float = 0.0
    vertices: List[Tuple[float, float, float]] = field(default_factory=list)  # (x, y, bulge)


@dataclass
class Polyline(Entity):
    """Heavy POLYLINE with its VERTEX children already absorbed."""
    flags: int = 0            # bit 1 = closed, bit 8 = 3D polyline
    vertices: List[Tuple[float, float, float, float]] = field(default_factory=list)  # (x, y, z, bulge)

    @property
    def closed(self) -> bool:
        return bool(self.flags & 1)


@dataclass
class Text(Entity):
    text: str = ''
    insert: Point3 = (0.0, 0.0, 0.0)
    align_point: Point3 = (0.0, 0.0, 0.0)
    height: float = 1.0
    rotation: float = 0.0     # degrees
    width_factor: float = 1.0
    oblique_angle: float = 0.0
    style: str = 'Standard'
    halign: int = 0
    valign: int = 0


@dataclass
class MText(Entity):
    text: str = ''            # raw MTEXT string incl. inline formatting codes
    insert: Point3 = (0.0, 0.0, 0.0)
    height: float = 1.0
    ref_width: float = 0.0    # reference column width
    rotation: float = 0.0     # degrees
    attachment: int = 1       # 1 = top-left ... 9 = bottom-right
    style: str = 'Standard'


@dataclass
class Dimension(Entity):
    dim_style: str = 'Standard'
    dim_type: int = 0         # raw code 70 incl. flag bits; & 15 gives base type
    text: str = ''            # text override; '' or '<>' means measured value
    measurement: float = 0.0  # actual measurement (code 42)
    defpoint: Point3 = (0.0, 0.0, 0.0)       # dimension line definition point
    text_midpoint: Point3 = (0.0, 0.0, 0.0)
    defpoint2: Point3 = (0.0, 0.0, 0.0)      # extension line 1 origin
    defpoint3: Point3 = (0.0, 0.0, 0.0)      # extension line 2 origin
    angle: float = 0.0        # rotation of linear dimensions (code 50)


@dataclass
class Solid(Entity):
    # Four corners in DXF order (3rd and 4th are swapped versus visual order).
    corners: List[Point3] = field(default_factory=list)


@dataclass
class Spline(Entity):
    flags: int = 0            # bit 1 = closed
    degree: int = 3
    knots: List[float] = field(default_factory=list)
    control_points: List[Point3] = field(default_factory=list)
    fit_points: List[Point3] = field(default_factory=list)


@dataclass
class Leader(Entity):
    dim_style: str = 'Standard'
    vertices: List[Point3] = field(default_factory=list)


@dataclass
class Insert(Entity):
    """Block reference. Placement of a named BLOCK definition, optionally as a
    rectangular array (col/row counts and spacings)."""
    name: str = ''
    insert: Point3 = (0.0, 0.0, 0.0)
    x_scale: float = 1.0
    y_scale: float = 1.0
    z_scale: float = 1.0
    rotation: float = 0.0     # degrees, counter-clockwise
    col_count: int = 1
    row_count: int = 1
    col_spacing: float = 0.0
    row_spacing: float = 0.0


# ---------------------------------------------------------------------------
# Block definition (BLOCKS section)
# ---------------------------------------------------------------------------

@dataclass
class Block:
    name: str = ''
    base: Point3 = (0.0, 0.0, 0.0)  # insert base point (aligns with an INSERT point)
    flags: int = 0                  # bit 1 = anonymous, bits 4/8/16/32 = xref-related
    entities: List[Entity] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Document
# ---------------------------------------------------------------------------

@dataclass
class DxfDocument:
    name: str = ''
    header: Dict[str, object] = field(default_factory=dict)
    layers: Dict[str, Layer] = field(default_factory=dict)
    linetypes: Dict[str, Linetype] = field(default_factory=dict)
    text_styles: Dict[str, TextStyle] = field(default_factory=dict)
    dim_styles: Dict[str, DimStyle] = field(default_factory=dict)
    entities: List[Entity] = field(default_factory=list)
    blocks: Dict[str, Block] = field(default_factory=dict)
    read_counts: Counter = field(default_factory=Counter)
    discarded_counts: Counter = field(default_factory=Counter)

    @property
    def acadver(self) -> str:
        return str(self.header.get('$ACADVER', ''))


# ---------------------------------------------------------------------------
# Tag stream
# ---------------------------------------------------------------------------

Tag = Tuple[int, str]


def _load_tags(data: bytes) -> List[Tag]:
    if len(data) > MAX_FILE_BYTES:
        raise DxfError(f'File exceeds the {MAX_FILE_BYTES} byte limit.')
    if data[:32].lstrip()[:18] == b'AutoCAD Binary DXF':
        raise DxfError('Binary DXF files are not supported; save as ASCII DXF.')
    try:
        text = data.decode('utf-8-sig')
    except UnicodeDecodeError:
        text = data.decode('cp1252', errors='replace')
    # Strip C0 controls / DEL so escape sequences cannot reach names, text
    # values or the terminal. Scan first: clean files skip the rewrite.
    if _CTRL_CHARS_RE.search(text):
        text = text.translate(_CTRL_STRIP_TABLE)
    # Split on '\n' only: splitlines() also splits on \x1c-\x1e, \x85,
    #  etc., which AutoCAD does not, and that mismatch would let a
    # crafted file desynchronise the code/value pairing.
    lines = text.replace('\r\n', '\n').split('\n')
    del text
    if len(lines) // 2 > MAX_TAGS:
        raise DxfError(f'File exceeds the {MAX_TAGS} tag limit.')
    tags: List[Tag] = []
    for i in range(0, len(lines) - 1, 2):
        code_line = lines[i].strip()
        if not code_line:
            # Tolerate blank line(s) at end of file only.
            if any(lines[k].strip() for k in range(i + 1, len(lines))):
                raise DxfError(f'Blank group code at line {i + 1}')
            break
        if not code_line.isascii():
            raise DxfError(f'Non-ASCII group code {code_line[:32]!r} at line {i + 1}')
        try:
            code = int(code_line)
        except ValueError:
            raise DxfError(f'Malformed group code {code_line[:32]!r} at line {i + 1}')
        if not 0 <= code <= _MAX_GROUP_CODE:
            raise DxfError(f'Group code {code} out of range at line {i + 1}')
        tags.append((code, lines[i + 1]))
    if not tags:
        raise DxfError('Empty or non-DXF file.')
    return tags


def _skip_to_next_zero(tags: List[Tag], i: int) -> int:
    n = len(tags)
    while i < n and tags[i][0] != 0:
        i += 1
    return i


# ---------------------------------------------------------------------------
# HEADER section
# ---------------------------------------------------------------------------

def _header_value(values: List[Tag]) -> object:
    """Convert the tags of one $VARIABLE into a Python value."""
    def convert(code: int, raw: str):
        if 10 <= code <= 59 or 110 <= code <= 149 or 210 <= code <= 239:
            return _f(raw)
        if 60 <= code <= 99 or 160 <= code <= 179 or 270 <= code <= 299:
            return _i(raw)
        return raw.strip()

    if len(values) == 1:
        return convert(*values[0])
    if all(c in (10, 20, 30) for c, _ in values):  # 2D/3D point variable
        return tuple(_f(v) for _, v in values)
    return [convert(c, v) for c, v in values]


def _parse_header(tags: List[Tag], i: int, doc: DxfDocument) -> int:
    n = len(tags)
    name = ''
    values: List[Tag] = []

    def flush() -> None:
        if name:
            doc.header[name] = _header_value(values) if values else ''

    while i < n:
        code, value = tags[i]
        if code == 0:
            stripped = value.strip()
            if stripped == 'ENDSEC':
                flush()
                return i + 1
            if stripped == 'SECTION':  # unterminated section: hand back
                flush()
                return i
        if code == 9:
            flush()
            name = value.strip()
            values = []
        else:
            values.append((code, value))
        i += 1
    flush()
    return i


# ---------------------------------------------------------------------------
# TABLES section
# ---------------------------------------------------------------------------

def _parse_layer(rec: List[Tag], doc: DxfDocument) -> None:
    layer = Layer()
    for code, value in rec:
        if code == 2:
            layer.name = value.strip()
        elif code == 62:
            layer.color = _i(value, 7)
        elif code == 6:
            layer.linetype = value.strip()
        elif code == 70:
            layer.flags = _i(value)
        elif code == 370:
            layer.lineweight = _i(value, -3)
        elif code == 290:
            layer.plottable = _i(value, 1) != 0
        elif code == 420:
            layer.true_color = _i(value)
    if layer.name:
        doc.layers[layer.name] = layer


def _parse_ltype(rec: List[Tag], doc: DxfDocument) -> None:
    lt = Linetype()
    for code, value in rec:
        if code == 2:
            lt.name = value.strip()
        elif code == 3:
            lt.description = value.strip()
        elif code == 40:
            lt.pattern_length = _f(value)
        elif code == 49:
            lt.dashes.append(_f(value))
    if lt.name:
        doc.linetypes[lt.name] = lt


def _parse_style(rec: List[Tag], doc: DxfDocument) -> None:
    st = TextStyle()
    for code, value in rec:
        if code == 2:
            st.name = value.strip()
        elif code == 3:
            st.font = value.strip()
        elif code == 4:
            st.bigfont = value.strip()
        elif code == 40:
            st.height = _f(value)
        elif code == 41:
            st.width_factor = _f(value, 1.0)
        elif code == 50:
            st.oblique_angle = _f(value)
        elif code == 70:
            st.flags = _i(value)
    # Skip shape-file records (flag bit 1) which have an empty style name.
    if st.name:
        doc.text_styles[st.name] = st


def _parse_dimstyle(rec: List[Tag], doc: DxfDocument) -> None:
    ds = DimStyle()
    for code, value in rec:
        if code == 2:
            ds.name = value.strip()
        elif code not in (105, 100, 330, 102):  # skip handle/subclass/owner noise
            ds.overrides[code] = value.strip()
    if ds.name:
        doc.dim_styles[ds.name] = ds


_TABLE_PARSERS = {
    'LAYER': _parse_layer,
    'LTYPE': _parse_ltype,
    'STYLE': _parse_style,
    'DIMSTYLE': _parse_dimstyle,
}


def _parse_tables(tags: List[Tag], i: int, doc: DxfDocument) -> int:
    n = len(tags)
    while i < n:
        code, value = tags[i]
        name = value.strip()
        if code == 0 and name == 'ENDSEC':
            return i + 1
        if code == 0 and name == 'SECTION':  # unterminated section: hand back
            return i
        if code == 0 and name in _TABLE_PARSERS:
            j = _skip_to_next_zero(tags, i + 1)
            _TABLE_PARSERS[name](tags[i + 1:j], doc)
            i = j
        else:
            i += 1
    return i


# ---------------------------------------------------------------------------
# Entity parsers. Each receives the tags between the "0 <TYPE>" line and the
# next 0-code line, with common attributes already applied.
# ---------------------------------------------------------------------------

def _apply_common(entity: Entity, code: int, value: str) -> bool:
    if code == 8:
        entity.layer = value.strip()
    elif code == 6:
        entity.linetype = value.strip()
    elif code == 62:
        entity.color = _i(value, 256)
    elif code == 370:
        entity.lineweight = _i(value, -1)
    elif code == 48:
        entity.linetype_scale = _f(value, 1.0)
    elif code == 5:
        entity.handle = value.strip()
    else:
        return False
    return True


def _xyz(fields: Dict[int, str], x: int, y: int, z: int) -> Point3:
    return (_f(fields.get(x, '0')), _f(fields.get(y, '0')), _f(fields.get(z, '0')))


def _scalar_fields(entity: Entity, rec: List[Tag]) -> Dict[int, str]:
    """Apply common codes and return remaining codes as a last-wins dict."""
    fields: Dict[int, str] = {}
    for code, value in rec:
        if not _apply_common(entity, code, value):
            fields[code] = value
    return fields


def _parse_line(rec: List[Tag]) -> Line:
    e = Line()
    fld = _scalar_fields(e, rec)
    e.start = _xyz(fld, 10, 20, 30)
    e.end = _xyz(fld, 11, 21, 31)
    return e


def _parse_point(rec: List[Tag]) -> DxfPoint:
    e = DxfPoint()
    fld = _scalar_fields(e, rec)
    e.location = _xyz(fld, 10, 20, 30)
    return e


def _parse_circle(rec: List[Tag]) -> Circle:
    e = Circle()
    fld = _scalar_fields(e, rec)
    e.center = _xyz(fld, 10, 20, 30)
    e.radius = _f(fld.get(40, '0'))
    return e


def _parse_arc(rec: List[Tag]) -> Arc:
    e = Arc()
    fld = _scalar_fields(e, rec)
    e.center = _xyz(fld, 10, 20, 30)
    e.radius = _f(fld.get(40, '0'))
    e.start_angle = _f(fld.get(50, '0'))
    e.end_angle = _f(fld.get(51, '0'))
    return e


def _parse_ellipse(rec: List[Tag]) -> Ellipse:
    e = Ellipse()
    fld = _scalar_fields(e, rec)
    e.center = _xyz(fld, 10, 20, 30)
    e.major_axis = _xyz(fld, 11, 21, 31)
    e.ratio = _f(fld.get(40, '1'), 1.0)
    e.start_param = _f(fld.get(41, '0'))
    e.end_param = _f(fld.get(42, '6.283185307179586'), 6.283185307179586)
    return e


def _parse_lwpolyline(rec: List[Tag]) -> LWPolyline:
    e = LWPolyline()
    x = y = bulge = None
    for code, value in rec:
        if _apply_common(e, code, value):
            continue
        if code == 10:  # starts a new vertex
            if x is not None:
                e.vertices.append((x, y, bulge or 0.0))
            x, y, bulge = _f(value), 0.0, 0.0
        elif code == 20:
            y = _f(value)
        elif code == 42:
            bulge = _f(value)
        elif code == 70:
            e.closed = bool(_i(value) & 1)
        elif code == 43:
            e.const_width = _f(value)
        elif code == 38:
            e.elevation = _f(value)
    if x is not None:
        e.vertices.append((x, y, bulge or 0.0))
    return e


def _parse_polyline(rec: List[Tag], vertex_recs: List[List[Tag]]) -> Polyline:
    e = Polyline()
    for code, value in rec:
        if not _apply_common(e, code, value) and code == 70:
            e.flags = _i(value)
    for vrec in vertex_recs:
        vx = vy = vz = vb = 0.0
        has_location = False
        for code, value in vrec:
            if code == 10:
                vx, has_location = _f(value), True
            elif code == 20:
                vy = _f(value)
            elif code == 30:
                vz = _f(value)
            elif code == 42:
                vb = _f(value)
        if has_location:
            e.vertices.append((vx, vy, vz, vb))
    return e


def _parse_text(rec: List[Tag]) -> Text:
    e = Text()
    fld = _scalar_fields(e, rec)
    e.text = fld.get(1, '')
    e.insert = _xyz(fld, 10, 20, 30)
    e.align_point = _xyz(fld, 11, 21, 31)
    e.height = _f(fld.get(40, '1'), 1.0)
    e.rotation = _f(fld.get(50, '0'))
    e.width_factor = _f(fld.get(41, '1'), 1.0)
    e.oblique_angle = _f(fld.get(51, '0'))
    e.style = fld.get(7, 'Standard').strip() or 'Standard'
    e.halign = _i(fld.get(72, '0'))
    e.valign = _i(fld.get(73, '0'))
    return e


def _parse_mtext(rec: List[Tag]) -> MText:
    e = MText()
    chunks: List[str] = []
    tail = ''
    for code, value in rec:
        if _apply_common(e, code, value):
            continue
        if code == 3:      # additional text chunks, in order, before code 1
            chunks.append(value)
        elif code == 1:    # final text chunk
            tail = value
        elif code == 10:
            e.insert = (_f(value), e.insert[1], e.insert[2])
        elif code == 20:
            e.insert = (e.insert[0], _f(value), e.insert[2])
        elif code == 30:
            e.insert = (e.insert[0], e.insert[1], _f(value))
        elif code == 40:
            e.height = _f(value, 1.0)
        elif code == 41:
            e.ref_width = _f(value)
        elif code == 50:
            e.rotation = _f(value)
        elif code == 71:
            e.attachment = _i(value, 1)
        elif code == 7:
            e.style = value.strip() or 'Standard'
    e.text = ''.join(chunks) + tail
    return e


def _parse_dimension(rec: List[Tag]) -> Dimension:
    e = Dimension()
    fld = _scalar_fields(e, rec)
    e.dim_style = fld.get(3, 'Standard').strip() or 'Standard'
    e.dim_type = _i(fld.get(70, '0'))
    e.text = fld.get(1, '')
    e.measurement = _f(fld.get(42, '0'))
    e.defpoint = _xyz(fld, 10, 20, 30)
    e.text_midpoint = _xyz(fld, 11, 21, 31)
    e.defpoint2 = _xyz(fld, 13, 23, 33)
    e.defpoint3 = _xyz(fld, 14, 24, 34)
    e.angle = _f(fld.get(50, '0'))
    return e


def _parse_solid(rec: List[Tag]) -> Solid:
    e = Solid()
    fld = _scalar_fields(e, rec)
    e.corners = [_xyz(fld, 10 + k, 20 + k, 30 + k) for k in range(4)]
    return e


def _parse_spline(rec: List[Tag]) -> Spline:
    e = Spline()
    control: List[List[float]] = []
    fit: List[List[float]] = []
    for code, value in rec:
        if _apply_common(e, code, value):
            continue
        if code == 70:
            e.flags = _i(value)
        elif code == 71:
            e.degree = _i(value, 3)
        elif code == 40:
            e.knots.append(_f(value))
        elif code == 10:
            control.append([_f(value), 0.0, 0.0])
        elif code == 20 and control:
            control[-1][1] = _f(value)
        elif code == 30 and control:
            control[-1][2] = _f(value)
        elif code == 11:
            fit.append([_f(value), 0.0, 0.0])
        elif code == 21 and fit:
            fit[-1][1] = _f(value)
        elif code == 31 and fit:
            fit[-1][2] = _f(value)
    e.control_points = [tuple(p) for p in control]
    e.fit_points = [tuple(p) for p in fit]
    return e


def _parse_leader(rec: List[Tag]) -> Leader:
    e = Leader()
    pts: List[List[float]] = []
    for code, value in rec:
        if _apply_common(e, code, value):
            continue
        if code == 3:
            e.dim_style = value.strip() or 'Standard'
        elif code == 10:
            pts.append([_f(value), 0.0, 0.0])
        elif code == 20 and pts:
            pts[-1][1] = _f(value)
        elif code == 30 and pts:
            pts[-1][2] = _f(value)
    e.vertices = [tuple(p) for p in pts]
    return e


def _parse_insert(rec: List[Tag]) -> Insert:
    e = Insert()
    fld = _scalar_fields(e, rec)
    e.name = fld.get(2, '').strip()
    e.insert = _xyz(fld, 10, 20, 30)
    e.x_scale = _f(fld.get(41, '1'), 1.0)
    e.y_scale = _f(fld.get(42, '1'), 1.0)
    e.z_scale = _f(fld.get(43, '1'), 1.0)
    e.rotation = _f(fld.get(50, '0'))
    e.col_count = max(1, _i(fld.get(70, '1'), 1))
    e.row_count = max(1, _i(fld.get(71, '1'), 1))
    e.col_spacing = _f(fld.get(44, '0'))
    e.row_spacing = _f(fld.get(45, '0'))
    return e


_ENTITY_PARSERS = {
    'LINE': _parse_line,
    'POINT': _parse_point,
    'CIRCLE': _parse_circle,
    'ARC': _parse_arc,
    'ELLIPSE': _parse_ellipse,
    'LWPOLYLINE': _parse_lwpolyline,
    'TEXT': _parse_text,
    'MTEXT': _parse_mtext,
    'DIMENSION': _parse_dimension,
    'SOLID': _parse_solid,
    'SPLINE': _parse_spline,
    'LEADER': _parse_leader,
    'INSERT': _parse_insert,
    # POLYLINE is handled specially (absorbs VERTEX/SEQEND children).
}


_TYPE_NAME_OK = frozenset('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$*-')


def _safe_type_name(name: str) -> str:
    """Entity type names come from the file and are later printed: keep only
    the character set valid in DXF type names, and cap the length."""
    return ''.join(c if c in _TYPE_NAME_OK else '?' for c in name[:24]) or '?'


def _is_paper_space(rec: List[Tag]) -> bool:
    return any(code == 67 and _i(value) == 1 for code, value in rec)


def _parse_entity_run(tags: List[Tag], i: int, doc: DxfDocument, sink: List[Entity],
                      stop_names: Tuple[str, ...], count_read: bool = True) -> int:
    """Parse a run of entities into `sink`, stopping at (without consuming) the
    first 0-code tag whose value is in `stop_names`. Shared by the ENTITIES
    section and BLOCK bodies. Discarded/paper-space entities are counted on doc;
    model-space reads are counted only when `count_read` is True."""
    n = len(tags)
    while i < n:
        code, value = tags[i]
        etype = value.strip()
        if code != 0:  # stray tag, should not happen
            i += 1
            continue
        if etype in stop_names:
            return i

        j = _skip_to_next_zero(tags, i + 1)
        rec = tags[i + 1:j]
        i = j

        if etype == 'POLYLINE':
            # Absorb the VERTEX children and the closing SEQEND.
            vertex_recs: List[List[Tag]] = []
            while i < n and tags[i][1].strip() == 'VERTEX':
                j = _skip_to_next_zero(tags, i + 1)
                vertex_recs.append(tags[i + 1:j])
                i = j
            if i < n and tags[i][1].strip() == 'SEQEND':
                i = _skip_to_next_zero(tags, i + 1)
            if _is_paper_space(rec):
                doc.discarded_counts['POLYLINE'] += 1
                doc.discarded_counts['VERTEX'] += len(vertex_recs)
            else:
                sink.append(_parse_polyline(rec, vertex_recs))
                if count_read:
                    doc.read_counts['POLYLINE'] += 1
            continue

        parser = _ENTITY_PARSERS.get(etype)
        if parser is None or _is_paper_space(rec):
            doc.discarded_counts[_safe_type_name(etype)] += 1
            continue
        sink.append(parser(rec))
        if count_read:
            doc.read_counts[etype] += 1
    return i


def _parse_entities(tags: List[Tag], i: int, doc: DxfDocument) -> int:
    i = _parse_entity_run(tags, i, doc, doc.entities, ('ENDSEC', 'SECTION'))
    if i < len(tags) and tags[i][1].strip() == 'ENDSEC':
        return i + 1  # consume ENDSEC; a bare SECTION is handed back for the caller
    return i


def _parse_one_block(tags: List[Tag], i: int, doc: DxfDocument) -> int:
    """Parse a single 0 BLOCK ... 0 ENDBLK record. `i` points at the BLOCK tag."""
    n = len(tags)
    j = _skip_to_next_zero(tags, i + 1)
    rec = tags[i + 1:j]
    i = j

    block = Block()
    bx = by = bz = 0.0
    for code, value in rec:
        if code == 2:
            block.name = value.strip()
        elif code == 10:
            bx = _f(value)
        elif code == 20:
            by = _f(value)
        elif code == 30:
            bz = _f(value)
        elif code == 70:
            block.flags = _i(value)
    block.base = (bx, by, bz)

    # Block-body entities (may include nested INSERTs) are not model space, so
    # they do not inflate the model read statistics.
    i = _parse_entity_run(tags, i, doc, block.entities,
                          ('ENDBLK', 'ENDSEC', 'SECTION'), count_read=False)
    if i < n and tags[i][1].strip() == 'ENDBLK':
        i = _skip_to_next_zero(tags, i + 1)  # consume the ENDBLK record

    if block.name:
        doc.blocks[block.name] = block
    return i


def _parse_blocks(tags: List[Tag], i: int, doc: DxfDocument) -> int:
    """Parse BLOCK definitions (with their entities) into doc.blocks."""
    n = len(tags)
    while i < n:
        code, value = tags[i]
        name = value.strip()
        if code == 0 and name == 'ENDSEC':
            return i + 1
        if code == 0 and name == 'SECTION':  # unterminated section: hand back
            return i
        if code == 0 and name == 'BLOCK':
            i = _parse_one_block(tags, i, doc)
        else:
            i += 1
    return i


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def read_dxf_bytes(data: bytes, name: str = '<bytes>') -> DxfDocument:
    tags = _load_tags(data)
    doc = DxfDocument(name=name)
    n = len(tags)
    i = 0
    while i < n:
        code, value = tags[i]
        if code == 0 and value.strip() == 'SECTION' and i + 1 < n and tags[i + 1][0] == 2:
            section = tags[i + 1][1].strip()
            body = i + 2
            if section == 'HEADER':
                i = _parse_header(tags, body, doc)
            elif section == 'TABLES':
                i = _parse_tables(tags, body, doc)
            elif section == 'BLOCKS':
                i = _parse_blocks(tags, body, doc)
            elif section == 'ENTITIES':
                i = _parse_entities(tags, body, doc)
            else:  # CLASSES, OBJECTS, THUMBNAILIMAGE, ...
                i = body
        else:
            i += 1
    return doc


def read_dxf_file(path: str) -> DxfDocument:
    with open(path, 'rb') as fh:
        # Read one byte past the cap so _load_tags can reject oversized
        # files without ever buffering more than the limit.
        data = fh.read(MAX_FILE_BYTES + 1)
    doc = read_dxf_bytes(data, name=path)
    return doc


# ---------------------------------------------------------------------------
# Command line: print simple statistics
# ---------------------------------------------------------------------------

def _print_counter(title: str, counts: Counter) -> None:
    print(title)
    if not counts:
        print('  (none)')
        return
    for etype, count in counts.most_common():
        print(f'  {etype:<14}{count:>8}')
    print(f'  {"Total":<14}{sum(counts.values()):>8}')


def main(argv: List[str]) -> int:
    if len(argv) != 2:
        print(f'Usage: python {argv[0]} <drawing.dxf>')
        return 2
    try:
        doc = read_dxf_file(argv[1])
    except (OSError, DxfError) as exc:
        print(f'Error: {exc}')
        return 1
    except MemoryError:
        print('Error: out of memory while parsing the file.')
        return 1

    units = doc.header.get('$INSUNITS', '?')
    print(f'DXF file : {doc.name}')
    print(f'Version  : {doc.acadver or "unknown"}   $INSUNITS: {units}')
    print(f'Tables   : {len(doc.layers)} layers, {len(doc.linetypes)} linetypes, '
          f'{len(doc.text_styles)} text styles, {len(doc.dim_styles)} dimension styles')
    print()
    _print_counter('Entities read (model space):', doc.read_counts)
    print()
    _print_counter('Entities discarded (unsupported / paper space):', doc.discarded_counts)
    inserts = sum(1 for e in doc.entities if isinstance(e, Insert))
    print(f'\nBlock definitions: {len(doc.blocks)}   Model-space INSERTs: {inserts}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
