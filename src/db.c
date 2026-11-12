#define _GNU_SOURCE
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SCHEMA =
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS reports("
    "  id INTEGER PRIMARY KEY,"
    "  valid_as_of TEXT NOT NULL UNIQUE,"
    "  source_file TEXT,"
    "  page_count INTEGER,"
    "  record_count INTEGER,"
    "  added INTEGER DEFAULT 0, modified INTEGER DEFAULT 0,"
    "  removed INTEGER DEFAULT 0, restored INTEGER DEFAULT 0,"
    "  imported_at TEXT DEFAULT (datetime('now')));"
    "CREATE TABLE IF NOT EXISTS firearms("
    "  frn INTEGER PRIMARY KEY,"
    "  make TEXT, model TEXT, manufacturer TEXT, level TEXT, type TEXT, action TEXT,"
    "  country TEXT, serial_numbering TEXT, legal_classification TEXT,"
    "  h_summary INTEGER, h_calibres INTEGER, h_notes INTEGER, h_cross_refs INTEGER,"
    "  h_aliases INTEGER, h_year_dates INTEGER, h_importers INTEGER,"
    "  first_report INTEGER REFERENCES reports(id),"
    "  last_report INTEGER REFERENCES reports(id),"
    "  changed_report INTEGER REFERENCES reports(id),"
    "  removed INTEGER NOT NULL DEFAULT 0,"
    "  pdf_page INTEGER);"
    "CREATE TABLE IF NOT EXISTS calibres("
    "  frn INTEGER NOT NULL REFERENCES firearms(frn) ON DELETE CASCADE, sub INTEGER,"
    "  calibre TEXT, shots TEXT, barrel_mm TEXT, legal_classification TEXT,"
    "  legal_authority TEXT, level TEXT, barrel_type_code TEXT);"
    "CREATE TABLE IF NOT EXISTS notes("
    "  frn INTEGER NOT NULL REFERENCES firearms(frn) ON DELETE CASCADE,"
    "  seq INTEGER, category TEXT, text TEXT);"
    "CREATE TABLE IF NOT EXISTS cross_refs("
    "  frn INTEGER NOT NULL REFERENCES firearms(frn) ON DELETE CASCADE,"
    "  ref_frn INTEGER, make TEXT, model TEXT, manufacturer TEXT, type TEXT, action TEXT);"
    "CREATE TABLE IF NOT EXISTS aliases("
    "  frn INTEGER NOT NULL REFERENCES firearms(frn) ON DELETE CASCADE, name TEXT);"
    "CREATE TABLE IF NOT EXISTS year_dates("
    "  frn INTEGER NOT NULL REFERENCES firearms(frn) ON DELETE CASCADE, text TEXT);"
    "CREATE TABLE IF NOT EXISTS importers("
    "  frn INTEGER NOT NULL REFERENCES firearms(frn) ON DELETE CASCADE, name TEXT);"
    "CREATE TABLE IF NOT EXISTS changes("
    "  id INTEGER PRIMARY KEY,"
    "  report_id INTEGER NOT NULL REFERENCES reports(id) ON DELETE CASCADE,"
    "  frn INTEGER NOT NULL,"
    "  kind TEXT NOT NULL,"
    "  field TEXT,"
    "  old_value TEXT, new_value TEXT);"
    "CREATE INDEX IF NOT EXISTS calibres_frn   ON calibres(frn);"
    "CREATE INDEX IF NOT EXISTS calibres_cal   ON calibres(calibre);"
    "CREATE INDEX IF NOT EXISTS notes_frn      ON notes(frn);"
    "CREATE INDEX IF NOT EXISTS xrefs_frn      ON cross_refs(frn);"
    "CREATE INDEX IF NOT EXISTS aliases_frn    ON aliases(frn);"
    "CREATE INDEX IF NOT EXISTS aliases_name   ON aliases(name COLLATE NOCASE);"
    "CREATE INDEX IF NOT EXISTS years_frn      ON year_dates(frn);"
    "CREATE INDEX IF NOT EXISTS importers_frn  ON importers(frn);"
    "CREATE INDEX IF NOT EXISTS changes_report ON changes(report_id, kind);"
    "CREATE INDEX IF NOT EXISTS changes_frn    ON changes(frn);"
    "CREATE INDEX IF NOT EXISTS firearms_make  ON firearms(make COLLATE NOCASE);"
    "CREATE INDEX IF NOT EXISTS firearms_model ON firearms(model COLLATE NOCASE);"
    "CREATE INDEX IF NOT EXISTS firearms_class ON firearms(legal_classification);"
    "CREATE VIEW IF NOT EXISTS firearm_calibres AS"
    "  SELECT f.frn, f.make, f.model, f.manufacturer, f.type, f.action,"
    "         f.legal_classification AS firearm_class, c.sub, c.calibre, c.shots, c.barrel_mm,"
    "         c.legal_classification AS row_class, c.legal_authority, c.level"
    "  FROM firearms f LEFT JOIN calibres c USING(frn) WHERE f.removed = 0;";

int db_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sqlite: %s\n", err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

sqlite3 *db_open(const char *path)
{
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "cannot open %s: %s\n", path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    if (db_exec(db, "PRAGMA journal_mode = WAL;") || db_exec(db, SCHEMA)) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static sqlite3_stmt *prep(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "sqlite prepare: %s\n  %s\n", sqlite3_errmsg(db), sql);
        exit(1);
    }
    return st;
}

static void bind_str(sqlite3_stmt *st, int i, const Str *s)
{
    if (s->n) sqlite3_bind_text(st, i, s->p, (int)s->n, SQLITE_TRANSIENT);
    else      sqlite3_bind_null(st, i);
}

static int step_done(sqlite3 *db, sqlite3_stmt *st)
{
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        fprintf(stderr, "sqlite step: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

enum {
    Q_SEEN, Q_SELECT, Q_UPSERT, Q_TOUCH,
    Q_DEL_CAL, Q_DEL_NOTES, Q_DEL_XREF, Q_DEL_ALIAS, Q_DEL_YEARS, Q_DEL_IMP,
    Q_INS_CAL, Q_INS_NOTE, Q_INS_XREF, Q_INS_ALIAS, Q_INS_YEAR, Q_INS_IMP,
    Q_CHANGE, Q_OLD_CAL, Q_COUNT
};

struct Importer {
    sqlite3      *db;
    sqlite3_int64 report;
    int           baseline;
    sqlite3_stmt *q[Q_COUNT];
    ImportStats   st;
};

static void log_change(Importer *im, long frn, const char *kind, const char *field,
                       const char *oldv, const char *newv)
{
    if (im->baseline) return;
    sqlite3_stmt *s = im->q[Q_CHANGE];
    sqlite3_bind_int64(s, 1, im->report);
    sqlite3_bind_int64(s, 2, frn);
    sqlite3_bind_text(s, 3, kind, -1, SQLITE_STATIC);
    if (field) sqlite3_bind_text(s, 4, field, -1, SQLITE_STATIC); else sqlite3_bind_null(s, 4);
    if (oldv && *oldv) sqlite3_bind_text(s, 5, oldv, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(s, 5);
    if (newv && *newv) sqlite3_bind_text(s, 6, newv, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(s, 6);
    step_done(im->db, s);
}

Importer *importer_new(sqlite3 *db, sqlite3_int64 report_id, int baseline)
{
    Importer *im = xcalloc(1, sizeof *im);
    im->db = db;
    im->report = report_id;
    im->baseline = baseline;
    char sql[2048];

    db_exec(db, "CREATE TEMP TABLE IF NOT EXISTS seen(frn INTEGER PRIMARY KEY); DELETE FROM temp.seen;");
    im->q[Q_SEEN] = prep(db, "INSERT OR IGNORE INTO temp.seen(frn) VALUES(?)");
    im->q[Q_SELECT] = prep(db,
        "SELECT make, model, manufacturer, level, type, action, country, serial_numbering,"
        " legal_classification, h_summary, h_calibres, h_notes, h_cross_refs, h_aliases,"
        " h_year_dates, h_importers, removed FROM firearms WHERE frn = ?");
    snprintf(sql, sizeof sql,
        "INSERT INTO firearms(frn, make, model, manufacturer, level, type, action, country,"
        " serial_numbering, legal_classification, h_summary, h_calibres, h_notes, h_cross_refs,"
        " h_aliases, h_year_dates, h_importers, first_report, last_report, changed_report,"
        " removed, pdf_page)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?18,?18,0,?19)"
        " ON CONFLICT(frn) DO UPDATE SET make=excluded.make, model=excluded.model,"
        " manufacturer=excluded.manufacturer, level=excluded.level, type=excluded.type,"
        " action=excluded.action, country=excluded.country,"
        " serial_numbering=excluded.serial_numbering,"
        " legal_classification=excluded.legal_classification,"
        " h_summary=excluded.h_summary, h_calibres=excluded.h_calibres, h_notes=excluded.h_notes,"
        " h_cross_refs=excluded.h_cross_refs, h_aliases=excluded.h_aliases,"
        " h_year_dates=excluded.h_year_dates, h_importers=excluded.h_importers,"
        " last_report=excluded.last_report, changed_report=excluded.changed_report,"
        " removed=0, pdf_page=excluded.pdf_page");
    im->q[Q_UPSERT] = prep(db, sql);
    im->q[Q_TOUCH]  = prep(db, "UPDATE firearms SET last_report=?, pdf_page=? WHERE frn=?");
    im->q[Q_DEL_CAL]   = prep(db, "DELETE FROM calibres WHERE frn=?");
    im->q[Q_DEL_NOTES] = prep(db, "DELETE FROM notes WHERE frn=?");
    im->q[Q_DEL_XREF]  = prep(db, "DELETE FROM cross_refs WHERE frn=?");
    im->q[Q_DEL_ALIAS] = prep(db, "DELETE FROM aliases WHERE frn=?");
    im->q[Q_DEL_YEARS] = prep(db, "DELETE FROM year_dates WHERE frn=?");
    im->q[Q_DEL_IMP]   = prep(db, "DELETE FROM importers WHERE frn=?");
    im->q[Q_INS_CAL]   = prep(db, "INSERT INTO calibres VALUES(?,?,?,?,?,?,?,?,?)");
    im->q[Q_INS_NOTE]  = prep(db, "INSERT INTO notes VALUES(?,?,?,?)");
    im->q[Q_INS_XREF]  = prep(db, "INSERT INTO cross_refs VALUES(?,?,?,?,?,?,?)");
    im->q[Q_INS_ALIAS] = prep(db, "INSERT INTO aliases VALUES(?,?)");
    im->q[Q_INS_YEAR]  = prep(db, "INSERT INTO year_dates VALUES(?,?)");
    im->q[Q_INS_IMP]   = prep(db, "INSERT INTO importers VALUES(?,?)");
    im->q[Q_CHANGE]    = prep(db,
        "INSERT INTO changes(report_id, frn, kind, field, old_value, new_value) VALUES(?,?,?,?,?,?)");
    im->q[Q_OLD_CAL]   = prep(db,
        "SELECT sub, calibre, shots, barrel_mm, legal_classification, legal_authority"
        " FROM calibres WHERE frn=? ORDER BY rowid");
    return im;
}

static void old_calibres_text(Importer *im, long frn, Str *out)
{
    Record tmp; memset(&tmp, 0, sizeof tmp);
    sqlite3_stmt *s = im->q[Q_OLD_CAL];
    sqlite3_bind_int64(s, 1, frn);
    while (sqlite3_step(s) == SQLITE_ROW) {
        Calibre c; memset(&c, 0, sizeof c);
        c.sub = sqlite3_column_int(s, 0);
        const char *v;
        if ((v = (const char *)sqlite3_column_text(s, 1))) str_appends(&c.calibre, v);
        if ((v = (const char *)sqlite3_column_text(s, 2))) str_appends(&c.shots, v);
        if ((v = (const char *)sqlite3_column_text(s, 3))) str_appends(&c.barrel_mm, v);
        if ((v = (const char *)sqlite3_column_text(s, 4))) str_appends(&c.legal_class, v);
        if ((v = (const char *)sqlite3_column_text(s, 5))) str_appends(&c.legal_authority, v);
        VEC_PUSH(tmp.calibres, c);
    }
    sqlite3_reset(s);
    serialize_calibres(&tmp, out);
    record_free(&tmp);
}

static int write_children(Importer *im, const Record *r)
{
    sqlite3 *db = im->db;
    for (int q = Q_DEL_CAL; q <= Q_DEL_IMP; q++) {
        sqlite3_bind_int64(im->q[q], 1, r->frn);
        if (step_done(db, im->q[q])) return -1;
    }
    sqlite3_stmt *s = im->q[Q_INS_CAL];
    for (size_t i = 0; i < r->calibres.n; i++) {
        const Calibre *c = &r->calibres.v[i];
        sqlite3_bind_int64(s, 1, r->frn);
        sqlite3_bind_int(s, 2, c->sub);
        bind_str(s, 3, &c->calibre); bind_str(s, 4, &c->shots); bind_str(s, 5, &c->barrel_mm);
        bind_str(s, 6, &c->legal_class); bind_str(s, 7, &c->legal_authority);
        bind_str(s, 8, &c->level); bind_str(s, 9, &c->barrel_type_code);
        if (step_done(db, s)) return -1;
    }
    s = im->q[Q_INS_NOTE];
    for (size_t i = 0; i < r->notes.n; i++) {
        sqlite3_bind_int64(s, 1, r->frn);
        sqlite3_bind_int(s, 2, (int)i);
        bind_str(s, 3, &r->notes.v[i].category);
        bind_str(s, 4, &r->notes.v[i].text);
        if (step_done(db, s)) return -1;
    }
    s = im->q[Q_INS_XREF];
    for (size_t i = 0; i < r->xrefs.n; i++) {
        const XRef *x = &r->xrefs.v[i];
        sqlite3_bind_int64(s, 1, r->frn);
        sqlite3_bind_int64(s, 2, x->ref_frn);
        bind_str(s, 3, &x->make); bind_str(s, 4, &x->model); bind_str(s, 5, &x->manufacturer);
        bind_str(s, 6, &x->type); bind_str(s, 7, &x->action);
        if (step_done(db, s)) return -1;
    }
    struct { int q; const Str *v; size_t n; } lists[] = {
        { Q_INS_ALIAS, r->aliases.v,    r->aliases.n },
        { Q_INS_YEAR,  r->year_dates.v, r->year_dates.n },
        { Q_INS_IMP,   r->importers.v,  r->importers.n },
    };
    for (int l = 0; l < 3; l++) {
        s = im->q[lists[l].q];
        for (size_t i = 0; i < lists[l].n; i++) {
            sqlite3_bind_int64(s, 1, r->frn);
            bind_str(s, 2, &lists[l].v[i]);
            if (step_done(db, s)) return -1;
        }
    }
    return 0;
}

static void count_text(char *buf, size_t n, size_t count, const char *what)
{
    snprintf(buf, n, "%zu %s", count, what);
}

int importer_record(const Record *r, void *user)
{
    Importer *im = user;
    sqlite3 *db = im->db;

    sqlite3_bind_int64(im->q[Q_SEEN], 1, r->frn);
    if (step_done(db, im->q[Q_SEEN])) return -1;
    if (sqlite3_changes(db) == 0) { im->st.duplicates++; return 0; }
    im->st.records++;

    uint64_t h[H_NSECT];
    record_hashes(r, h);

    sqlite3_stmt *sel = im->q[Q_SELECT];
    sqlite3_bind_int64(sel, 1, r->frn);
    int exists = sqlite3_step(sel) == SQLITE_ROW;
    int removed = 0, changed[H_NSECT] = {0}, any = 0;

    if (exists) {
        removed = sqlite3_column_int(sel, 16);
        for (int i = 0; i < H_NSECT; i++) {
            changed[i] = (uint64_t)sqlite3_column_int64(sel, 9 + i) != h[i];
            any |= changed[i];
        }
        if (changed[H_SUMMARY]) {
            for (int f = 0; f < F_NFIELDS; f++) {
                const char *old = (const char *)sqlite3_column_text(sel, f);
                const char *nw  = str_c(&r->field[f]);
                if (strcmp(old ? old : "", nw) != 0)
                    log_change(im, r->frn, "modified", FIELD_COLUMN[f], old, nw);
            }
        }
    }
    sqlite3_reset(sel);

    if (exists && !any && !removed) {
        sqlite3_stmt *t = im->q[Q_TOUCH];
        sqlite3_bind_int64(t, 1, im->report);
        sqlite3_bind_int(t, 2, r->first_page);
        sqlite3_bind_int64(t, 3, r->frn);
        im->st.unchanged++;
        return step_done(db, t);
    }

    if (!exists) {
        im->st.added++;
        log_change(im, r->frn, "added", NULL, NULL, str_c(&r->field[F_MAKE]));
    } else {
        if (removed) { im->st.restored++; log_change(im, r->frn, "restored", NULL, NULL, NULL); }
        if (any) {
            im->st.modified++;
            char a[64], b[64];
            for (int i = H_CALIBRES; i < H_NSECT; i++) {
                if (!changed[i]) continue;
                if (i == H_CALIBRES) {
                    Str o = {0}, n = {0};
                    old_calibres_text(im, r->frn, &o);
                    serialize_calibres(r, &n);
                    log_change(im, r->frn, "modified", SECTION_NAME[i], str_c(&o), str_c(&n));
                    str_free(&o); str_free(&n);
                } else {
                    size_t cnt = i == H_NOTES ? r->notes.n : i == H_XREFS ? r->xrefs.n :
                                 i == H_ALIASES ? r->aliases.n : i == H_YEARS ? r->year_dates.n :
                                 r->importers.n;
                    char *q = sqlite3_mprintf("SELECT count(*) FROM %s WHERE frn = %ld",
                                              SECTION_NAME[i], r->frn);
                    sqlite3_stmt *cs;
                    long old = 0;
                    if (sqlite3_prepare_v2(db, q, -1, &cs, NULL) == SQLITE_OK && sqlite3_step(cs) == SQLITE_ROW)
                        old = (long)sqlite3_column_int64(cs, 0);
                    sqlite3_finalize(cs);
                    sqlite3_free(q);
                    count_text(a, sizeof a, (size_t)old, "entries");
                    count_text(b, sizeof b, cnt, "entries (content changed)");
                    log_change(im, r->frn, "modified", SECTION_NAME[i], a, b);
                }
            }
        }
    }

    sqlite3_stmt *u = im->q[Q_UPSERT];
    sqlite3_bind_int64(u, 1, r->frn);
    for (int f = 0; f < F_NFIELDS; f++) bind_str(u, 2 + f, &r->field[f]);
    for (int i = 0; i < H_NSECT; i++) sqlite3_bind_int64(u, 11 + i, (sqlite3_int64)h[i]);
    sqlite3_bind_int64(u, 18, im->report);
    sqlite3_bind_int(u, 19, r->first_page);
    if (step_done(db, u)) return -1;
    return write_children(im, r);
}

int importer_finish(Importer *im)
{
    sqlite3 *db = im->db;
    sqlite3_stmt *s = prep(db,
        "SELECT frn, make, model FROM firearms WHERE removed = 0"
        " AND frn NOT IN (SELECT frn FROM temp.seen)");
    while (sqlite3_step(s) == SQLITE_ROW) {
        long frn = (long)sqlite3_column_int64(s, 0);
        char buf[512];
        snprintf(buf, sizeof buf, "%s %s",
                 sqlite3_column_text(s, 1) ? (const char *)sqlite3_column_text(s, 1) : "",
                 sqlite3_column_text(s, 2) ? (const char *)sqlite3_column_text(s, 2) : "");
        log_change(im, frn, "removed", NULL, buf, NULL);
        im->st.removed++;
    }
    sqlite3_finalize(s);

    char sql[256];
    snprintf(sql, sizeof sql,
             "UPDATE firearms SET removed = 1, changed_report = %lld"
             " WHERE removed = 0 AND frn NOT IN (SELECT frn FROM temp.seen)",
             (long long)im->report);
    return db_exec(db, sql);
}

ImportStats importer_stats(const Importer *im) { return im->st; }

void importer_free(Importer *im)
{
    if (!im) return;
    for (int i = 0; i < Q_COUNT; i++) sqlite3_finalize(im->q[i]);
    free(im);
}
