# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
"""STAAD -> Vishwakarma steel profile name mapping.

`designation_for(member_profile)` turns one member-property record from
InteroperabilityWithSTDFile (model.member_profile_by_member[member_id]) into a
Vishwakarma catalog designation (the `designation` column of
Catalog/profiles_hot_*.csv), or "" when the member has no mappable rolled
section — the host keeps drawing those members as placeholder pipes.

VISHWAKARMA_DESIGNATION_BY_STD_NAME maps a STAAD table name (uppercase, exactly
as written in the .std) to our designation. ALL name mapping happens here in
Python; the C++ host only looks the resulting designation up in its embedded
catalog. The seed content was generated mechanically from the catalog
(designation with spaces stripped + upper-cased, plus the HEA/HEB/HEM prefix
forms Europeans use) — correct or extend it by hand.

A name absent from the dictionary is passed through unchanged, so STAAD names
that already equal a catalog designation exactly (e.g. American "W10X49") need
no entry. Names that match nothing simply fail the host-side catalog lookup
and stay pipes.

Known seeding gaps to correct over time: American angles (STAAD "L40404" vs
catalog "L4X4X1/4"), STAAD Indian NPB/WPB names that append mass, Japanese
partial H names, and the Chinese-vs-Russian plain "I" beams ("I10" resolves to
the Chinese GB row; the Russian "I 10" needs a distinct key of your choosing).
"""

# STAAD section-position specs (first token of a TABLE property). ST is a plain
# single section; TC/BC/TB carry top/bottom cover plates whose WP/TH parameters
# are deliberately ignored for now (user decision) — the base rolled section is
# still modeled. The remaining specs change the cross-section itself (tee-cut,
# doubles, reversed angle, composite), so they stay unmapped placeholder pipes.
_BASE_SECTION_SPECS = {"ST", "TC", "BC", "TB"}
_SHAPE_CHANGING_SPECS = {"T", "D", "LD", "SD", "RA", "CM"}


def designation_for(member_profile) -> str:
    """Vishwakarma designation for one member_profile_by_member record, or ""."""
    if not member_profile or member_profile.get("profile_type") != "TABLE":
        return ""  # PRIS / TAPERED / UPTABLE / GENERAL -> placeholder pipe (but see parametric_for).
    tokens = str(member_profile.get("profile_name", "")).split()
    if not tokens:
        return ""
    spec = tokens[0].upper()
    if spec in _BASE_SECTION_SPECS:
        if len(tokens) < 2:
            return ""
        name = tokens[1].upper()
    elif spec in _SHAPE_CHANGING_SPECS:
        return ""
    else:
        name = tokens[0].upper()  # Lenient: tolerate a missing ST spec.
    return VISHWAKARMA_DESIGNATION_BY_STD_NAME.get(name, name)


# STAAD prismatic parameter keys we understand. YB/ZB define prismatic tee /
# trapezoid shapes that RECT/CIRC cannot represent — such members stay
# placeholder pipes rather than silently dropping their taper.
_PRISMATIC_KEYS = {"YD", "ZD", "YB", "ZB"}


def parametric_for(member_profile):
    """(designation, parameter1, parameter2) in SI meters for one PRIS record,
    or None when the member is not a plain rectangular/circular prismatic.

    STAAD convention: `PRIS YD y ZD z` is a rectangle y deep (local y) by z
    wide; `PRIS YD y` alone is a solid circle of diameter y. Values arrive
    already unit-converted by the parser (values_si). OCT/HEX have no STAAD
    spelling, so they are never produced here.
    """
    if not member_profile or member_profile.get("profile_type") != "PRIS":
        return None
    values = member_profile.get("values_si") or member_profile.get("values") or []
    found = {}
    for token, value in zip(values, values[1:]):
        if isinstance(token, str) and token.upper() in _PRISMATIC_KEYS \
                and isinstance(value, (int, float)):
            found[token.upper()] = float(value)
    if "YB" in found or "ZB" in found:
        return None
    yd = found.get("YD", 0.0)
    if yd <= 0.0:
        return None  # AX/IZ-only prismatics carry no outline.
    zd = found.get("ZD", 0.0)
    if zd > 0.0:
        return ("RECT", yd, zd)  # parameter1 = depth YD, parameter2 = width ZD.
    return ("CIRC", yd, 0.0)


VISHWAKARMA_DESIGNATION_BY_STD_NAME = {
    # --- ANGLE : Australia/NZ ---
    "100X100X10EA": "100x100x10EA",
    "100X75X8UA": "100x75x8UA",
    "125X125X12EA": "125x125x12EA",
    "125X75X10UA": "125x75x10UA",
    "150X100X12UA": "150x100x12UA",
    "150X150X16EA": "150x150x16EA",
    "150X90X12UA": "150x90x12UA",
    "200X200X20EA": "200x200x20EA",
    "25X25X3EA": "25x25x3EA",
    "50X50X5EA": "50x50x5EA",
    "65X50X6UA": "65x50x6UA",
    "65X65X6EA": "65x65x6EA",
    "75X50X6UA": "75x50x6UA",
    "75X75X6EA": "75x75x6EA",
    "90X90X8EA": "90x90x8EA",
    # --- ANGLE : China ---
    "L100X10": "L 100x10",
    "L125X12": "L 125x12",
    "L160X16": "L 160x16",
    "L200X20": "L 200x20",
    "L25X3": "L 25x3",
    "L40X4": "L 40x4",
    "L50X5": "L 50x5",
    "L63X40X5": "L 63x40x5",
    "L63X6": "L 63x6",
    "L75X8": "L 75x8",
    # --- ANGLE : Europe ---
    "L100X50X8": "L 100x50x8",
    "L100X65X8": "L 100x65x8",
    "L120X120X12": "L 120x120x12",
    "L120X80X10": "L 120x80x10",
    "L150X100X12": "L 150x100x12",
    "L200X100X14": "L 200x100x14",
    "L200X150X15": "L 200x150x15",
    "L20X20X3": "L 20x20x3",
    "L250X250X28": "L 250x250x28",
    "L25X25X3": "L 25x25x3",
    "L30X30X3": "L 30x30x3",
    "L40X25X4": "L 40x25x4",
    "L40X40X4": "L 40x40x4",
    "L50X30X5": "L 50x30x5",
    "L60X40X6": "L 60x40x6",
    "L60X60X6": "L 60x60x6",
    "L70X70X7": "L 70x70x7",
    "L80X40X6": "L 80x40x6",
    "L80X80X8": "L 80x80x8",
    "L90X90X9": "L 90x90x9",
    # --- ANGLE : India ---
    "ISA100X100X10": "ISA 100x100x10",
    "ISA100X100X8": "ISA 100x100x8",
    "ISA100X65X8": "ISA 100x65x8",
    "ISA100X75X8": "ISA 100x75x8",
    "ISA110X110X10": "ISA 110x110x10",
    "ISA125X75X8": "ISA 125x75x8",
    "ISA130X130X10": "ISA 130x130x10",
    "ISA150X115X10": "ISA 150x115x10",
    "ISA150X150X12": "ISA 150x150x12",
    "ISA150X150X16": "ISA 150x150x16",
    "ISA150X75X10": "ISA 150x75x10",
    "ISA200X100X12": "ISA 200x100x12",
    "ISA200X150X16": "ISA 200x150x16",
    "ISA200X200X16": "ISA 200x200x16",
    "ISA200X200X25": "ISA 200x200x25",
    "ISA25X25X3": "ISA 25x25x3",
    "ISA30X20X3": "ISA 30x20x3",
    "ISA30X30X3": "ISA 30x30x3",
    "ISA40X25X4": "ISA 40x25x4",
    "ISA40X40X4": "ISA 40x40x4",
    "ISA50X30X5": "ISA 50x30x5",
    "ISA50X50X5": "ISA 50x50x5",
    "ISA50X50X6": "ISA 50x50x6",
    "ISA60X40X5": "ISA 60x40x5",
    "ISA60X60X6": "ISA 60x60x6",
    "ISA65X45X6": "ISA 65x45x6",
    "ISA65X65X6": "ISA 65x65x6",
    "ISA75X50X6": "ISA 75x50x6",
    "ISA75X75X8": "ISA 75x75x8",
    "ISA90X60X8": "ISA 90x60x8",
    "ISA90X90X8": "ISA 90x90x8",
    # --- ANGLE : Japan ---
    "L100X100X10": "L 100x100x10",
    "L100X75X10": "L 100x75x10",
    "L125X75X10": "L 125x75x10",
    "L130X130X12": "L 130x130x12",
    "L150X150X15": "L 150x150x15",
    "L150X90X12": "L 150x90x12",
    "L200X200X25": "L 200x200x25",
    "L200X90X9X14": "L 200x90x9x14",
    "L250X90X10X15": "L 250x90x10x15",
    "L300X90X11X16": "L 300x90x11x16",
    "L40X40X3": "L 40x40x3",
    "L40X40X5": "L 40x40x5",
    "L50X50X6": "L 50x50x6",
    "L65X65X6": "L 65x65x6",
    "L75X75X9": "L 75x75x9",
    "L90X75X9": "L 90x75x9",
    "L90X90X10": "L 90x90x10",
    # --- ANGLE : Russia ---
    "L100X63X8": "L 100x63x8",
    "L125X125X12": "L 125x125x12",
    "L125X80X10": "L 125x80x10",
    "L160X100X12": "L 160x100x12",
    "L160X160X16": "L 160x160x16",
    "L200X125X16": "L 200x125x16",
    "L200X200X20": "L 200x200x20",
    "L50X50X5": "L 50x50x5",
    "L63X40X6": "L 63x40x6",
    "L63X63X6": "L 63x63x6",
    "L75X50X6": "L 75x50x6",
    "L75X75X8": "L 75x75x8",
    # --- BAR : Europe ---
    "FL100X10": "FL 100x10",
    "FL50X6": "FL 50x6",
    "HEX32": "HEX 32",
    "RD20": "RD 20",
    "RD50": "RD 50",
    "SQ40": "SQ 40",
    # --- BAR : India ---
    "ISF100X8": "ISF 100x8",
    "ISRO25": "ISRO 25",
    # --- BAR : Russia ---
    "RD12": "RD 12",
    # --- BULB : Europe ---
    "HP100X6": "HP 100x6",
    "HP160X8": "HP 160x8",
    "HP200X10": "HP 200x10",
    "HP320X13": "HP 320x13",
    "HP430X20": "HP 430x20",
    # --- BULB : India ---
    "ISBF200X10": "ISBF 200x10",
    # --- CHANNEL : China ---
    "C16A": "C16a",
    "C20A": "C20a",
    "C25A": "C25a",
    "C32A": "C32a",
    "C40A": "C40a",
    # --- CHANNEL : Europe ---
    "U50X25": "U 50x25",
    "U65X42": "U 65x42",
    "UAP150": "UAP 150",
    "UAP250": "UAP 250",
    "UAP80": "UAP 80",
    "UPE100": "UPE 100",
    "UPE120": "UPE 120",
    "UPE160": "UPE 160",
    "UPE200": "UPE 200",
    "UPE240": "UPE 240",
    "UPE300": "UPE 300",
    "UPE400": "UPE 400",
    "UPE80": "UPE 80",
    "UPN100": "UPN 100",
    "UPN120": "UPN 120",
    "UPN140": "UPN 140",
    "UPN160": "UPN 160",
    "UPN200": "UPN 200",
    "UPN240": "UPN 240",
    "UPN300": "UPN 300",
    "UPN400": "UPN 400",
    "UPN80": "UPN 80",
    # --- CHANNEL : India ---
    "ISJC100": "ISJC 100",
    "ISJC150": "ISJC 150",
    "ISJC200": "ISJC 200",
    "ISLC100": "ISLC 100",
    "ISLC150": "ISLC 150",
    "ISLC200": "ISLC 200",
    "ISLC300": "ISLC 300",
    "ISLC400": "ISLC 400",
    "ISLC75": "ISLC 75",
    "ISMC100": "ISMC 100",
    "ISMC125": "ISMC 125",
    "ISMC150": "ISMC 150",
    "ISMC175": "ISMC 175",
    "ISMC200": "ISMC 200",
    "ISMC250": "ISMC 250",
    "ISMC300": "ISMC 300",
    "ISMC400": "ISMC 400",
    "ISMC75": "ISMC 75",
    "ISMCP200": "ISMCP 200",
    "ISMCP300": "ISMCP 300",
    # --- CHANNEL : Japan ---
    "C100X50X5X7.5": "C 100x50x5x7.5",
    "C125X65X6X8": "C 125x65x6x8",
    "C150X75X6.5X10": "C 150x75x6.5x10",
    "C200X80X7.5X11": "C 200x80x7.5x11",
    "C250X90X9X13": "C 250x90x9x13",
    "C300X90X9X13": "C 300x90x9x13",
    "C380X100X10.5X16": "C 380x100x10.5x16",
    # --- CHANNEL : UK ---
    "CH305X102X46": "CH 305x102x46",
    "PFC100X50X10": "PFC 100x50x10",
    "PFC150X75X18": "PFC 150x75x18",
    "PFC180X90X26": "PFC 180x90x26",
    "PFC200X90X30": "PFC 200x90x30",
    "PFC230X90X32": "PFC 230x90x32",
    "PFC260X90X35": "PFC 260x90x35",
    "PFC300X100X46": "PFC 300x100x46",
    "PFC430X100X64": "PFC 430x100x64",
    # --- CHS : Europe ---
    "CHS114.3X5": "CHS 114.3x5",
    "CHS219.1X8": "CHS 219.1x8",
    "CHS323.9X10": "CHS 323.9x10",
    "CHS48.3X3.2": "CHS 48.3x3.2",
    "CHS508X12.5": "CHS 508x12.5",
    # --- CHS : India ---
    "NB100H": "NB 100 H",
    "NB150M": "NB 150 M",
    "NB50M": "NB 50 M",
    # --- CHS : USA ---
    "HSS6.625X0.280": "HSS 6.625x0.280",
    # --- I : Brazil ---
    "CS300X76": "CS 300x76",
    "VS400X49": "VS 400x49",
    # --- I : Canada ---
    "WWF350X137": "WWF 350x137",
    "WWF900X293": "WWF 900x293",
    # --- I : China ---
    "HM340X250": "HM 340x250",
    "HM588X300": "HM 588x300",
    "HN400X200": "HN 400x200",
    "HN700X300": "HN 700x300",
    "HW100X100": "HW 100x100",
    "HW200X200": "HW 200x200",
    "HW300X300": "HW 300x300",
    "HW400X400": "HW 400x400",
    "I20A": "I20a",
    "I25A": "I25a",
    "I32A": "I32a",
    "I40A": "I40a",
    "I45A": "I45a",
    "I56A": "I56a",
    "I63C": "I63c",
    # --- I : Europe ---
    "HD400X187": "HD 400x187",
    "HD400X990": "HD 400x990",
    "HE100A": "HE 100 A",
    "HE100B": "HE 100 B",
    "HE100M": "HE 100 M",
    "HE140A": "HE 140 A",
    "HE140B": "HE 140 B",
    "HE160A": "HE 160 A",
    "HE160B": "HE 160 B",
    "HE200A": "HE 200 A",
    "HE200B": "HE 200 B",
    "HE200M": "HE 200 M",
    "HE240A": "HE 240 A",
    "HE240B": "HE 240 B",
    "HE300A": "HE 300 A",
    "HE300B": "HE 300 B",
    "HE300M": "HE 300 M",
    "HE400A": "HE 400 A",
    "HE400B": "HE 400 B",
    "HE500A": "HE 500 A",
    "HE500B": "HE 500 B",
    "HE600A": "HE 600 A",
    "HE600B": "HE 600 B",
    "HE600M": "HE 600 M",
    "HEA100": "HE 100 A",
    "HEA140": "HE 140 A",
    "HEA160": "HE 160 A",
    "HEA200": "HE 200 A",
    "HEA240": "HE 240 A",
    "HEA300": "HE 300 A",
    "HEA400": "HE 400 A",
    "HEA500": "HE 500 A",
    "HEA600": "HE 600 A",
    "HEB100": "HE 100 B",
    "HEB140": "HE 140 B",
    "HEB160": "HE 160 B",
    "HEB200": "HE 200 B",
    "HEB240": "HE 240 B",
    "HEB300": "HE 300 B",
    "HEB400": "HE 400 B",
    "HEB500": "HE 500 B",
    "HEB600": "HE 600 B",
    "HEM100": "HE 100 M",
    "HEM200": "HE 200 M",
    "HEM300": "HE 300 M",
    "HEM600": "HE 600 M",
    "HL920X342": "HL 920x342",
    "HP260X87": "HP 260x87",
    "HP360X109": "HP 360x109",
    "IPE100": "IPE 100",
    "IPE120": "IPE 120",
    "IPE140": "IPE 140",
    "IPE160": "IPE 160",
    "IPE180": "IPE 180",
    "IPE200": "IPE 200",
    "IPE220": "IPE 220",
    "IPE240": "IPE 240",
    "IPE270": "IPE 270",
    "IPE300": "IPE 300",
    "IPE330": "IPE 330",
    "IPE360": "IPE 360",
    "IPE400": "IPE 400",
    "IPE450": "IPE 450",
    "IPE500": "IPE 500",
    "IPE550": "IPE 550",
    "IPE600": "IPE 600",
    "IPE80": "IPE 80",
    "IPN100": "IPN 100",
    "IPN140": "IPN 140",
    "IPN200": "IPN 200",
    "IPN240": "IPN 240",
    "IPN300": "IPN 300",
    "IPN400": "IPN 400",
    "IPN500": "IPN 500",
    "IPN80": "IPN 80",
    # --- I : India ---
    "ISHB150": "ISHB 150",
    "ISHB200": "ISHB 200",
    "ISHB225": "ISHB 225",
    "ISHB250": "ISHB 250",
    "ISHB300": "ISHB 300",
    "ISHB350": "ISHB 350",
    "ISHB400": "ISHB 400",
    "ISHB450": "ISHB 450",
    "ISJB150": "ISJB 150",
    "ISJB175": "ISJB 175",
    "ISJB200": "ISJB 200",
    "ISJB225": "ISJB 225",
    "ISLB100": "ISLB 100",
    "ISLB125": "ISLB 125",
    "ISLB150": "ISLB 150",
    "ISLB175": "ISLB 175",
    "ISLB200": "ISLB 200",
    "ISLB250": "ISLB 250",
    "ISLB300": "ISLB 300",
    "ISLB350": "ISLB 350",
    "ISLB400": "ISLB 400",
    "ISLB450": "ISLB 450",
    "ISLB500": "ISLB 500",
    "ISLB600": "ISLB 600",
    "ISMB100": "ISMB 100",
    "ISMB125": "ISMB 125",
    "ISMB150": "ISMB 150",
    "ISMB175": "ISMB 175",
    "ISMB200": "ISMB 200",
    "ISMB225": "ISMB 225",
    "ISMB250": "ISMB 250",
    "ISMB300": "ISMB 300",
    "ISMB350": "ISMB 350",
    "ISMB400": "ISMB 400",
    "ISMB450": "ISMB 450",
    "ISMB500": "ISMB 500",
    "ISMB600": "ISMB 600",
    "ISWB150": "ISWB 150",
    "ISWB175": "ISWB 175",
    "ISWB200": "ISWB 200",
    "ISWB250": "ISWB 250",
    "ISWB300": "ISWB 300",
    "ISWB350": "ISWB 350",
    "ISWB400": "ISWB 400",
    "ISWB450": "ISWB 450",
    "ISWB500": "ISWB 500",
    "ISWB550": "ISWB 550",
    "ISWB600": "ISWB 600",
    "NPB120": "NPB 120",
    "NPB160": "NPB 160",
    "NPB200": "NPB 200",
    "NPB300": "NPB 300",
    "NPB400": "NPB 400",
    "NPB600": "NPB 600",
    "WPB160": "WPB 160",
    "WPB200": "WPB 200",
    "WPB240": "WPB 240",
    "WPB300": "WPB 300",
    "WPB450": "WPB 450",
    "WPB600": "WPB 600",
    # --- I : Japan ---
    "H100X100X6X8": "H 100x100x6x8",
    "H125X125X6.5X9": "H 125x125x6.5x9",
    "H150X150X7X10": "H 150x150x7x10",
    "H150X75X5X7": "H 150x75x5x7",
    "H200X100X5.5X8": "H 200x100x5.5x8",
    "H250X250X9X14": "H 250x250x9x14",
    "H300X150X6.5X9": "H 300x150x6.5x9",
    "H350X350X12X19": "H 350x350x12x19",
    "H400X400X13X21": "H 400x400x13x21",
    "H450X200X9X14": "H 450x200x9x14",
    "H500X200X10X16": "H 500x200x10x16",
    "H600X200X11X17": "H 600x200x11x17",
    "H700X300X13X24": "H 700x300x13x24",
    "H800X300X14X26": "H 800x300x14x26",
    "H900X300X16X28": "H 900x300x16x28",
    # --- I : Russia ---
    "30SH1": "30Sh1",
    "40SH1": "40Sh1",
    "I20": "I 20",
    "I30": "I 30",
    "I40": "I 40",
    "I60": "I 60",
    # --- I : South Korea ---
    "H200X200X8X12": "H 200x200x8x12",
    "H300X300X10X15": "H 300x300x10x15",
    "H400X200X8X13": "H 400x200x8x13",
    # --- I : UK ---
    "J127X114X27": "J 127x114x27",
    "J203X152X52": "J 203x152x52",
    "UB127X76X13": "UB 127x76x13",
    "UB203X133X25": "UB 203x133x25",
    "UB254X146X31": "UB 254x146x31",
    "UB305X165X40": "UB 305x165x40",
    "UB356X171X51": "UB 356x171x51",
    "UB406X178X60": "UB 406x178x60",
    "UB457X191X82": "UB 457x191x82",
    "UB533X210X92": "UB 533x210x92",
    "UB610X229X125": "UB 610x229x125",
    "UB686X254X140": "UB 686x254x140",
    "UB762X267X173": "UB 762x267x173",
    "UB838X292X194": "UB 838x292x194",
    "UB914X305X224": "UB 914x305x224",
    "UBP203X203X45": "UBP 203x203x45",
    "UBP305X305X110": "UBP 305x305x110",
    "UC152X152X30": "UC 152x152x30",
    "UC203X203X60": "UC 203x203x60",
    "UC254X254X89": "UC 254x254x89",
    "UC305X305X118": "UC 305x305x118",
    "UC356X368X153": "UC 356x368x153",
    "UC356X406X235": "UC 356x406x235",
    # --- RAIL : USA ---
    "ASCE60": "ASCE 60",
    # --- RHS : Europe ---
    "RHS100X50X4": "RHS 100x50x4",
    "RHS200X100X8": "RHS 200x100x8",
    "RHS300X200X10": "RHS 300x200x10",
    "SHS100X100X5": "SHS 100x100x5",
    "SHS150X150X8": "SHS 150x150x8",
    "SHS40X40X3": "SHS 40x40x3",
    # --- RHS : India ---
    "RHS150X100X5": "RHS 150x100x5",
    "SHS100X100X4": "SHS 100x100x4",
    # --- TEE : Europe ---
    "T100": "T 100",
    "T30": "T 30",
    "T40": "T 40",
    "T50": "T 50",
    "T80": "T 80",
    # --- TEE : India ---
    "ISNT100": "ISNT 100",
    "ISNT50": "ISNT 50",
    "ISST250": "ISST 250",
}
