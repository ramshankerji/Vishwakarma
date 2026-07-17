#!/usr/bin/env python3
"""
catalog_editor.py -- minimal editor for an immutable-ID CSV catalog.

IDs are drawn uniformly at random from [2**32, 2**40 - 1] and are globally
unique across the whole catalog. There are no type tags: the file an entry
lives in determines its type. The floor at 2**32 is deliberate -- every valid
id has a bit set above bit 31, so any uint64->uint32 truncation lands below
the minimum and is caught by a range check instead of silently aliasing.

Layout:
    Catalog/
        profiles_hot_i.csv      one file per profile family ("country" and
        profiles_hot_c.csv       "code" are just columns) -- schema documented
        profiles_hot_l.csv       in website/content/civil/SteelTable.md
        profiles_hot_t.csv      (_hot = hot-rolled/hot-finished; cold-formed
        profiles_hot_rhs.csv     will be a separate profiles_cold_* set)
        profiles_hot_chs.csv
        profiles_hot_bar.csv
        profiles_hot_bulb.csv
        profiles_hot_rail.csv
    (no allocation file -- random draws need no central counter, which is
     exactly what keeps branches from conflicting)

Invariants the editor enforces (this is the whole point):
    * the `id` column is never editable
    * rows are never deleted, only superseded
    * IDs are allocated only by draw-and-check against every file in the
      catalog, never by hand
    * every id lies in [2**32, 2**40 - 1]
    * saving fails if an id present on disk would disappear, or if an id
      collides with one in any other file

Run:  python3 catalog_editor.py [catalog_dir]    (default: the script's folder)
      python3 catalog_editor.py --init [dir]     # write a sample catalog
"""

import csv
import secrets
import sys
from pathlib import Path

# ---------------------------------------------------------------- pure core

LOCKED = {"id", "status", "superseded_by"}  # not hand-editable

ID_MIN = 2**32              # floor: makes uint32 truncation detectable
ID_MAX = 2**40 - 1
ID_SPAN = ID_MAX - ID_MIN + 1

def parse_id(s):
    return int(str(s).strip(), 16 if str(s).strip().lower().startswith("0x") else 10)

def fmt_id(n): return f"0x{n:010X}"

def id_ok(n): return ID_MIN <= n <= ID_MAX

def read_csv(path):
    with open(path, newline="", encoding="utf-8") as f:
        r = csv.DictReader(f)
        return list(r.fieldnames or []), [dict(row) for row in r]

def write_csv(path, fields, rows):
    """Deterministic output: stable field order, \\n endings, sorted by key.
    Sorted by `key`, not `id`: random ids carry no meaning, so ordering by
    them would scatter IPE200/IPE300/IPE400 through the file at random and
    make every human read of the csv miserable.
    """
    if "key" in fields:
        rows = sorted(rows, key=lambda r: (r.get("key", ""), r.get("id", "")))
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        w.writeheader()
        for row in rows:
            w.writerow({k: row.get(k, "") for k in fields})

def catalog_files(catalog):
    return sorted(p for p in catalog.glob("*.csv") if not p.name.startswith("_"))

def used_ids(catalog, exclude=None):
    """Every id currently on disk, per file. The registry IS the catalog."""
    out = {}
    for p in catalog_files(catalog):
        if exclude and p.name == exclude:
            continue
        _, rows = read_csv(p)
        for r in rows:
            try:
                out[parse_id(r["id"])] = p.name
            except (ValueError, KeyError):
                pass
    return out

def allocate(catalog, live_ids=()):
    """Draw uniformly from [ID_MIN, ID_MAX], reject anything already used.

    Re-reads the whole catalog each call, so a colleague's id committed since
    you opened the file is still respected. `live_ids` covers unsaved rows in
    the open table. Expected draws: 1.0000005 at 10^5 objects.
    """
    taken = set(used_ids(catalog)) | set(live_ids)
    for _ in range(1000):
        n = ID_MIN + secrets.randbelow(ID_SPAN)
        if n not in taken:
            return n
    raise RuntimeError("1000 collisions in a row -- the RNG is broken, "
                       "or the space is full (it is not)")

def validate(rows, on_disk_ids, foreign_ids=None):
    """Return a list of problems. Empty list == safe to save."""
    foreign_ids = foreign_ids or {}
    problems = []
    seen = {}
    for row in rows:
        try:
            i = parse_id(row["id"])
        except (ValueError, KeyError):
            problems.append(f"unparseable id: {row.get('id')!r}")
            continue
        if i in seen:
            problems.append(f"duplicate id {fmt_id(i)}")
        seen[i] = row
        if not id_ok(i):
            problems.append(f"{fmt_id(i)} outside [0x{ID_MIN:010X}, 0x{ID_MAX:010X}]")
        if i in foreign_ids:
            problems.append(f"{fmt_id(i)} collides with an id in {foreign_ids[i]}")
        if not row.get("key", "").strip():
            problems.append(f"{fmt_id(i)} has empty key")
    for i in on_disk_ids:
        if i not in seen:
            problems.append(f"{fmt_id(i)} exists on disk but not in this table "
                            f"(IDs are immutable -- supersede, never delete)")
    return problems

# ---------------------------------------------------------------- sample data
SAMPLE_I_FIELDS = ["id", "key", "status", "superseded_by", "country", "code", "series",
                   "designation", "alt_designation", "h", "b", "tw", "tf", "r1", "r2",
                   "flange_slope", "mass", "availability"]
SAMPLE_I = [
    {"id": "0x3A7F1C0E92", "key": "EN10365:IPE 200", "status": "active",
     "superseded_by": "", "country": "Europe", "code": "EN 10365", "series": "IPE",
     "designation": "IPE 200", "alt_designation": "", "h": "200", "b": "100",
     "tw": "5.6", "tf": "8.5", "r1": "12", "r2": "0", "flange_slope": "0",
     "mass": "22.4", "availability": "current"},
    {"id": "0x91B40D77A3", "key": "EN10365:IPE 300", "status": "active",
     "superseded_by": "", "country": "Europe", "code": "EN 10365", "series": "IPE",
     "designation": "IPE 300", "alt_designation": "", "h": "300", "b": "150",
     "tw": "7.1", "tf": "10.7", "r1": "15", "r2": "0", "flange_slope": "0",
     "mass": "42.2", "availability": "current"},
]

def init_catalog(catalog):
    catalog.mkdir(parents=True, exist_ok=True)
    if not (catalog / "profiles_hot_i.csv").exists():
        write_csv(catalog / "profiles_hot_i.csv", SAMPLE_I_FIELDS, SAMPLE_I)
    return catalog

# ---------------------------------------------------------------- gui
def run_gui(catalog):
    import tkinter as tk
    from tkinter import ttk, messagebox, simpledialog

    class Editor(tk.Tk):
        def __init__(self, catalog):
            super().__init__()
            self.catalog = catalog
            self.title(f"Catalog Editor -- {catalog}")
            self.geometry("1100x620")
            self.path = None
            self.fields = []
            self.rows = []
            self.disk_ids = set()
            self.dirty = False
            self.editor = None
            self._build()
            self._load_files()

        # ---- layout
        def _build(self):
            left = ttk.Frame(self, padding=6)
            left.pack(side="left", fill="y")
            ttk.Label(left, text="Shape tables").pack(anchor="w")
            self.files = tk.Listbox(left, width=22, exportselection=False)
            self.files.pack(fill="y", expand=True)
            self.files.bind("<<ListboxSelect>>", self._on_pick)

            right = ttk.Frame(self, padding=6)
            right.pack(side="right", fill="both", expand=True)

            bar = ttk.Frame(right)
            bar.pack(fill="x", pady=(0, 6))
            ttk.Button(bar, text="Add row", command=self.add_row).pack(side="left")
            ttk.Button(bar, text="Supersede", command=self.supersede).pack(side="left", padx=4)
            ttk.Button(bar, text="Save", command=self.save).pack(side="left")
            ttk.Button(bar, text="Reload", command=self.reload).pack(side="left", padx=4)

            self.tree = ttk.Treeview(right, show="headings", selectmode="browse")
            vs = ttk.Scrollbar(right, orient="vertical", command=self.tree.yview)
            self.tree.configure(yscrollcommand=vs.set)
            vs.pack(side="right", fill="y")
            self.tree.pack(fill="both", expand=True)
            self.tree.bind("<Double-1>", self._begin_edit)
            self.tree.tag_configure("superseded", foreground="#999999")

            self.status = tk.StringVar(value="ready")
            ttk.Label(self, textvariable=self.status, relief="sunken",
                      anchor="w", padding=3).pack(side="bottom", fill="x")

        def _load_files(self):
            self.files.delete(0, "end")
            for p in sorted(self.catalog.glob("*.csv")):
                if not p.name.startswith("_"):
                    self.files.insert("end", p.name)

        # ---- table io
        def _on_pick(self, _evt):
            sel = self.files.curselection()
            if not sel:
                return
            if self.dirty and not messagebox.askokcancel(
                    "Unsaved changes", "Discard unsaved changes?"):
                return
            self.open(self.catalog / self.files.get(sel[0]))

        def open(self, path):
            self._kill_editor()
            self.path = path
            self.fields, self.rows = read_csv(path)
            self.disk_ids = {parse_id(r["id"]) for r in self.rows}
            self.tree["columns"] = self.fields
            for f in self.fields:
                self.tree.heading(f, text=f + (" *" if f in LOCKED else ""))
                wide = ("id", "key", "superseded_by", "code", "designation", "alt_designation")
                self.tree.column(f, width=150 if f in wide else 70, anchor="w")
            self.refresh()
            self.dirty = False
            self.status.set(f"{path.name}: {len(self.rows)} rows, "
                            f"{len(used_ids(self.catalog))} ids in catalog "
                            f"-- columns marked * are locked")

        def refresh(self):
            self.tree.delete(*self.tree.get_children())
            for r in sorted(self.rows, key=lambda r: (r.get("key", ""), r.get("id", ""))):
                tags = ("superseded",) if r.get("status") == "superseded" else ()
                self.tree.insert("", "end", iid=r["id"],
                                 values=[r.get(f, "") for f in self.fields], tags=tags)

        def reload(self):
            if self.path:
                self.open(self.path)

        def _find(self, id_str):
            return next(r for r in self.rows if r["id"] == id_str)

        def _live_ids(self):
            """Unsaved rows in the open table -- not yet visible on disk."""
            out = set()
            for r in self.rows:
                try:
                    out.add(parse_id(r["id"]))
                except (ValueError, KeyError):
                    pass
            return out

        # ---- cell editing
        def _kill_editor(self):
            if self.editor:
                self.editor.destroy()
                self.editor = None

        def _begin_edit(self, event):
            self._kill_editor()
            if self.tree.identify_region(event.x, event.y) != "cell":
                return
            iid = self.tree.identify_row(event.y)
            col = self.tree.identify_column(event.x)
            idx = int(col[1:]) - 1
            field = self.fields[idx]
            if field in LOCKED:
                self.status.set(f"'{field}' is not hand-editable -- use the buttons")
                return
            x, y, w, h = self.tree.bbox(iid, col)
            self.editor = tk.Entry(self.tree)
            self.editor.insert(0, self.tree.set(iid, field))
            self.editor.select_range(0, "end")
            self.editor.place(x=x, y=y, width=w, height=h)
            self.editor.focus_set()
            self.editor.bind("<Return>", lambda e: self._commit(iid, field))
            self.editor.bind("<FocusOut>", lambda e: self._commit(iid, field))
            self.editor.bind("<Escape>", lambda e: self._kill_editor())

        def _commit(self, iid, field):
            if not self.editor:
                return
            val = self.editor.get().strip()
            self._kill_editor()
            if self.tree.set(iid, field) == val:
                return
            self.tree.set(iid, field, val)
            self._find(iid)[field] = val
            self.dirty = True
            self.status.set(f"{iid} {field} -> {val!r}  (unsaved)")

        # ---- row ops
        def add_row(self):
            if not self.path:
                return
            new = fmt_id(allocate(self.catalog, self._live_ids()))
            row = {f: "" for f in self.fields}
            row.update(id=new, key="NEW", status="active")
            self.rows.append(row)
            self.refresh()
            self.tree.selection_set(new)
            self.tree.see(new)
            self.dirty = True
            self.status.set(f"allocated {new} -- set its key before saving")

        def supersede(self):
            """Geometry actually changed: clone to a fresh id, retire the old."""
            sel = self.tree.selection()
            if not sel or not self.path:
                return
            old = self._find(sel[0])
            if old.get("status") == "superseded":
                self.status.set("already superseded")
                return
            if not messagebox.askokcancel(
                    "Supersede",
                    f"Retire {old['id']} ({old.get('key')}) and clone it to a new ID?\n\n"
                    "Only do this if the real-world geometry changed.\n"
                    "For a typo, just edit the cell -- same object, same ID."):
                return
            new = fmt_id(allocate(self.catalog, self._live_ids()))
            clone = dict(old)
            clone.update(id=new, status="active", superseded_by="")
            old.update(status="superseded", superseded_by=new)
            self.rows.append(clone)
            self.refresh()
            self.tree.selection_set(new)
            self.tree.see(new)
            self.dirty = True
            self.status.set(f"{old['id']} superseded by {new}")

        # ---- save
        def save(self):
            if not self.path:
                return
            self._kill_editor()
            foreign = used_ids(self.catalog, exclude=self.path.name)
            problems = validate(self.rows, self.disk_ids, foreign)
            if problems:
                messagebox.showerror("Refusing to save", "\n".join(problems[:12]))
                self.status.set(f"{len(problems)} problem(s) -- not saved")
                return
            write_csv(self.path, self.fields, self.rows)
            self.disk_ids = {parse_id(r["id"]) for r in self.rows}
            self.dirty = False
            self.status.set(f"saved {self.path.name} ({len(self.rows)} rows)")

    Editor(catalog).mainloop()


if __name__ == "__main__":
    args = [a for a in sys.argv[1:]]
    if "--init" in args:
        args.remove("--init")
        d = init_catalog(Path(args[0]) if args else Path(__file__).parent)
        print(f"wrote sample catalog to {d.resolve()}")
        sys.exit(0)
    # default: the folder this script lives in -- the CSVs sit next to it
    run_gui(Path(args[0]) if args else Path(__file__).parent)
