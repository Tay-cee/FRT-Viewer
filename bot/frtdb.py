from __future__ import annotations

import sqlite3
from contextlib import closing
from dataclasses import dataclass, field
from pathlib import Path

SUMMARY_FIELDS = [
    ("make", "Make"), ("model", "Model"), ("manufacturer", "Manufacturer"),
    ("type", "Type"), ("action", "Action"), ("country", "Country"),
    ("serial_numbering", "Serial Numbering"), ("level", "Level"),
    ("legal_classification", "Legal Classification"),
]


@dataclass
class Record:
    frn: int
    summary: dict
    removed: bool
    first_report: str | None
    last_report: str | None
    changed_report: str | None
    pdf_page: int | None
    calibres: list[dict] = field(default_factory=list)
    notes: list[dict] = field(default_factory=list)
    xrefs: list[dict] = field(default_factory=list)
    aliases: list[str] = field(default_factory=list)
    year_dates: list[str] = field(default_factory=list)
    importers: list[str] = field(default_factory=list)
    history: list[dict] = field(default_factory=list)

    @property
    def title(self) -> str:
        return f"{self.summary.get('make') or ''} {self.summary.get('model') or ''}".strip()


class FrtDB:
    def __init__(self, path: str | Path):
        self.path = Path(path).resolve()

    def _conn(self) -> sqlite3.Connection:
        c = sqlite3.connect(f"file:{self.path}?mode=ro", uri=True, timeout=10)
        c.row_factory = sqlite3.Row
        return c

    def exists(self) -> bool:
        return self.path.exists()


    def record(self, frn: int) -> Record | None:
        with closing(self._conn()) as c:
            r = c.execute(
                """SELECT f.*, a.valid_as_of AS first_v, b.valid_as_of AS last_v, d.valid_as_of AS changed_v
                   FROM firearms f
                   LEFT JOIN reports a ON a.id = f.first_report
                   LEFT JOIN reports b ON b.id = f.last_report
                   LEFT JOIN reports d ON d.id = f.changed_report
                   WHERE f.frn = ?""", (frn,)).fetchone()
            if not r:
                return None
            rec = Record(
                frn=frn,
                summary={k: r[k] for k, _ in SUMMARY_FIELDS},
                removed=bool(r["removed"]),
                first_report=r["first_v"], last_report=r["last_v"], changed_report=r["changed_v"],
                pdf_page=r["pdf_page"],
            )
            q = lambda sql: [dict(x) for x in c.execute(sql, (frn,))]
            rec.calibres = q("SELECT * FROM calibres WHERE frn=? ORDER BY rowid")
            rec.notes = q("SELECT category, text FROM notes WHERE frn=? ORDER BY seq")
            rec.xrefs = q("SELECT ref_frn, make, model, manufacturer, type, action FROM cross_refs"
                          " WHERE frn=? ORDER BY rowid")
            rec.aliases = [x["name"] for x in q("SELECT name FROM aliases WHERE frn=? ORDER BY rowid")]
            rec.year_dates = [x["text"] for x in q("SELECT text FROM year_dates WHERE frn=? ORDER BY rowid")]
            rec.importers = [x["name"] for x in q("SELECT name FROM importers WHERE frn=? ORDER BY rowid")]
            rec.history = q("""SELECT r.valid_as_of, c.kind, c.field, c.old_value, c.new_value
                               FROM changes c JOIN reports r ON r.id = c.report_id
                               WHERE c.frn=? ORDER BY c.id""")
            return rec

    def search(self, words: list[str], classification: str | None = None,
               type_: str | None = None, include_removed: bool = False, limit: int = 500,
               make: str | None = None, manufacturer: str | None = None) -> list[dict]:
        sql = ["SELECT frn, make, model, manufacturer, type, action, legal_classification, removed"
               " FROM firearms WHERE 1"]
        args: list = []
        if not include_removed:
            sql.append("AND removed = 0")
        for w in words:
            pat = f"%{w}%"
            sql.append("AND (make LIKE ? OR model LIKE ? OR manufacturer LIKE ? OR CAST(frn AS TEXT) = ?"
                       " OR frn IN (SELECT frn FROM aliases WHERE name LIKE ?))")
            args += [pat, pat, pat, w, pat]
        if classification:
            sql.append("AND legal_classification = ?")
            args.append(classification)
        if type_:
            sql.append("AND type = ? COLLATE NOCASE")
            args.append(type_)
        for col, val in (("make", make), ("manufacturer", manufacturer)):
            if val and val.strip():
                val = val.strip()
                sql.append(f"AND ({col} = ? COLLATE NOCASE OR (NOT EXISTS (SELECT 1 FROM firearms"
                           f" WHERE {col} = ? COLLATE NOCASE) AND {col} LIKE ?))")
                args += [val, val, f"%{val}%"]
        sql.append("ORDER BY make COLLATE NOCASE, model COLLATE NOCASE LIMIT ?")
        args.append(limit)
        with closing(self._conn()) as c:
            return [dict(x) for x in c.execute(" ".join(sql), args)]

    def suggest(self, text: str, limit: int = 25) -> list[dict]:
        text = text.strip()
        with closing(self._conn()) as c:
            if text.isdigit():
                rows = c.execute("SELECT frn, make, model FROM firearms WHERE CAST(frn AS TEXT) LIKE ?"
                                 " ORDER BY frn LIMIT ?", (f"{text}%", limit))
                return [dict(x) for x in rows]
        words = text.split()
        return self.search(words, limit=limit) if words else []

    def existing(self, frns: list[int]) -> set[int]:
        if not frns:
            return set()
        with closing(self._conn()) as c:
            marks = ",".join("?" * len(frns))
            return {r[0] for r in c.execute(f"SELECT frn FROM firearms WHERE frn IN ({marks})", frns)}

    def values(self, column: str, text: str, limit: int = 25) -> list[tuple[str, int]]:
        if column not in ("make", "manufacturer"):
            raise ValueError(column)
        text = text.strip()
        with closing(self._conn()) as c:
            rows = c.execute(
                f"SELECT {column}, count(*) n FROM firearms WHERE removed = 0 AND {column} LIKE ?"
                f" AND {column} <> '' GROUP BY {column} COLLATE NOCASE"
                f" ORDER BY {column} NOT LIKE ?, n DESC, {column} COLLATE NOCASE LIMIT ?",
                (f"%{text}%", f"{text}%", limit))
            return [(r[0], r[1]) for r in rows]

    def reports(self) -> list[dict]:
        with closing(self._conn()) as c:
            return [dict(x) for x in c.execute("SELECT * FROM reports ORDER BY valid_as_of DESC")]

    def changes(self, report: str | None = None, kind: str | None = None) -> tuple[dict | None, list[dict]]:
        with closing(self._conn()) as c:
            if report:
                rep = c.execute("SELECT * FROM reports WHERE valid_as_of = ? OR CAST(id AS TEXT) = ?",
                                (report, report)).fetchone()
            else:
                rep = c.execute("SELECT * FROM reports ORDER BY valid_as_of DESC LIMIT 1").fetchone()
            if not rep:
                return None, []
            rows = c.execute(
                """SELECT c.frn, c.kind, c.field, c.old_value, c.new_value, f.make, f.model
                   FROM changes c LEFT JOIN firearms f USING(frn)
                   WHERE c.report_id = ? AND (? IS NULL OR c.kind = ?)
                   ORDER BY c.kind, c.frn, c.id""", (rep["id"], kind, kind))
            return dict(rep), [dict(x) for x in rows]

    def stats(self) -> dict:
        with closing(self._conn()) as c:
            one = lambda sql: c.execute(sql).fetchone()[0]
            return {
                "active": one("SELECT count(*) FROM firearms WHERE removed=0"),
                "removed": one("SELECT count(*) FROM firearms WHERE removed=1"),
                "calibres": one("SELECT count(*) FROM calibres"),
                "latest": one("SELECT max(valid_as_of) FROM reports"),
                "by_class": [tuple(x) for x in c.execute(
                    "SELECT ifnull(legal_classification,'(blank)'), count(*) FROM firearms WHERE removed=0"
                    " GROUP BY 1 ORDER BY 2 DESC")],
                "by_type": [tuple(x) for x in c.execute(
                    "SELECT ifnull(type,'(blank)'), count(*) FROM firearms WHERE removed=0"
                    " GROUP BY 1 ORDER BY 2 DESC LIMIT 12")],
            }

    def imported_files(self) -> set[str]:
        with closing(self._conn()) as c:
            return {Path(x[0]).name for x in c.execute("SELECT source_file FROM reports") if x[0]}
