#define _GNU_SOURCE
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *db_path = "frt.db";

static void usage(void)
{
    fprintf(stderr,
        "usage: frt [--db FILE] COMMAND ...\n"
        "\n"
        "commands:\n"
        "  import [--force] [--jobs N] FILE...   import report(s) (.pdf, or .txt from pdftotext -fixed 3)\n"
        "  show FRN                              print one firearm record\n"
        "  search [--class C] [--type T] [--all] [--limit N] WORDS...\n"
        "                                        search make/model/manufacturer/aliases\n"
        "  changes [--report ID|DATE] [--kind K] list changes introduced by a report (default: latest)\n"
        "  reports                               list imported reports\n"
        "  stats                                 summary counts\n"
        "  parse FILE.txt                        debug: dump parsed records\n"
        "\n"
        "default database: ./frt.db (override with --db or $FRT_DB)\n");
    exit(2);
}

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static const char *col(sqlite3_stmt *s, int i)
{
    const char *v = (const char *)sqlite3_column_text(s, i);
    return v ? v : "";
}

static int probe_date(const char *file, int is_pdf, char *out, size_t n)
{
    Parser *p = parser_new(NULL, NULL);
    FILE *f;
    if (is_pdf) {
        pid_t pid;
        int fd[2];
        if (pipe(fd)) { parser_free(p); return -1; }
        if ((pid = fork()) == 0) {
            dup2(fd[1], 1); close(fd[0]); close(fd[1]);
            execlp("pdftotext", "pdftotext", "-q", "-fixed", "3", "-enc", "UTF-8",
                   "-f", "1", "-l", "8", file, "-", (char *)NULL);
            _exit(127);
        }
        close(fd[1]);
        f = fdopen(fd[0], "r");
    } else {
        f = fopen(file, "r");
    }
    if (!f) { parser_free(p); return -1; }
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        parser_feed_line(p, line);
        if (parser_valid_as_of(p)[0]) break;
    }
    fclose(f);
    if (is_pdf) while (wait(NULL) > 0) {}
    snprintf(out, n, "%s", parser_valid_as_of(p));
    parser_free(p);
    return out[0] ? 0 : -1;
}

static long query_long(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s;
    long v = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        v = (long)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

static int import_one(sqlite3 *db, const char *file, int force, int jobs)
{
    int is_pdf = ends_with(file, ".pdf");
    struct stat sb;
    if (stat(file, &sb)) { perror(file); return 1; }

    char date[16];
    if (probe_date(file, is_pdf, date, sizeof date)) {
        fprintf(stderr, "%s: could not find 'Valid as of' date - is this an FRT report?\n", file);
        return 1;
    }
    printf("%s: FRT report valid as of %s\n", file, date);

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT id, valid_as_of FROM reports WHERE valid_as_of >= ?"
                           " ORDER BY valid_as_of DESC LIMIT 1", -1, &s, NULL);
    sqlite3_bind_text(s, 1, date, -1, SQLITE_STATIC);
    sqlite3_int64 existing = 0;
    char newest[16] = "";
    if (sqlite3_step(s) == SQLITE_ROW) {
        snprintf(newest, sizeof newest, "%s", col(s, 1));
        if (strcmp(newest, date) == 0) existing = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    if (newest[0] && !force) {
        if (existing) printf("  already imported (report #%lld) - skipping. Use --force to re-import.\n",
                             (long long)existing);
        else printf("  database already has a newer report (%s) - skipping. Use --force to apply anyway.\n",
                    newest);
        return 0;
    }

    long active_before = query_long(db, "SELECT count(*) FROM firearms WHERE removed = 0");
    int baseline = query_long(db, "SELECT count(*) FROM firearms") == 0;

    if (db_exec(db, "BEGIN IMMEDIATE")) return 1;

    sqlite3_int64 report_id;
    if (existing) {
        report_id = existing;
    } else {
        sqlite3_prepare_v2(db, "INSERT INTO reports(valid_as_of, source_file) VALUES(?, ?)", -1, &s, NULL);
        sqlite3_bind_text(s, 1, date, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, file, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
        report_id = sqlite3_last_insert_rowid(db);
    }

    Importer *im = importer_new(db, report_id, baseline);
    Parser *p = parser_new(importer_record, im);
    int rc;
    long pages;
    if (is_pdf) {
        pages = pdf_page_count(file);
        if (pages <= 0) { fprintf(stderr, "%s: pdfinfo could not read page count\n", file); rc = -1; }
        else {
            printf("  %ld pages, extracting with %d parallel pdftotext jobs...\n", pages, jobs);
            rc = pdf_extract_parse(file, pages, jobs, 500, p, !(isatty(2) || getenv("FRT_PROGRESS")));
        }
    } else {
        rc = parse_text_file(file, p);
    }
    if (rc == 0) rc = parser_finish(p);
    if (!is_pdf || pages <= 0) pages = parser_total_pages(p);

    ImportStats st = importer_stats(im);
    if (rc == 0 && st.records == 0) {
        fprintf(stderr, "  no records parsed - aborting\n");
        rc = -1;
    }
    if (rc == 0 && !baseline && !force && st.records < active_before / 2) {
        fprintf(stderr, "  only %ld records parsed vs %ld in database - looks truncated, aborting.\n"
                        "  (use --force if this is really correct)\n", st.records, active_before);
        rc = -1;
    }
    if (rc == 0) rc = importer_finish(im);
    st = importer_stats(im);

    if (rc == 0) {
        char sql[512];
        snprintf(sql, sizeof sql,
                 "UPDATE reports SET page_count=%ld, record_count=%ld, added=%ld, modified=%ld,"
                 " removed=%ld, restored=%ld, imported_at=datetime('now') WHERE id=%lld",
                 pages, st.records, st.added, st.modified, st.removed, st.restored,
                 (long long)report_id);
        rc = db_exec(db, sql);
    }
    parser_free(p);
    importer_free(im);

    if (rc) {
        db_exec(db, "ROLLBACK");
        fprintf(stderr, "  import failed - database unchanged\n");
        return 1;
    }
    if (db_exec(db, "COMMIT")) return 1;

    printf("  report #%lld: %ld records", (long long)report_id, st.records);
    if (baseline) printf(" (initial import)\n");
    else printf(" | added %ld, modified %ld, removed %ld, restored %ld, unchanged %ld\n",
                st.added, st.modified, st.removed, st.restored, st.unchanged);
    if (st.duplicates) printf("  note: %ld duplicate FRN blocks ignored\n", st.duplicates);
    return 0;
}

static int cmd_import(sqlite3 *db, int argc, char **argv)
{
    int force = 0, jobs = (int)sysconf(_SC_NPROCESSORS_ONLN), i, rc = 0;
    if (jobs > 16) jobs = 16;
    if (jobs < 1) jobs = 1;
    for (i = 0; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--jobs") && i + 1 < argc) jobs = atoi(argv[++i]);
        else usage();
    }
    if (i == argc) usage();
    for (; i < argc; i++) rc |= import_one(db, argv[i], force, jobs);
    if (rc == 0) db_exec(db, "PRAGMA optimize;");
    return rc;
}

static void print_wrapped(const char *indent, const char *text, int width)
{
    int col = 0;
    const char *s = text;
    fputs(indent, stdout);
    while (*s) {
        const char *e = s;
        while (*e && *e != ' ') e++;
        int len = (int)(e - s);
        if (col && col + 1 + len > width) { printf("\n%s", indent); col = 0; }
        else if (col) { putchar(' '); col++; }
        fwrite(s, 1, (size_t)len, stdout);
        col += len;
        s = *e ? e + 1 : e;
    }
    putchar('\n');
}

static int cmd_show(sqlite3 *db, int argc, char **argv)
{
    if (argc != 1) usage();
    long frn = atol(argv[0]);
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT f.make, f.model, f.manufacturer, f.level, f.type, f.action, f.country,"
        " f.serial_numbering, f.legal_classification, f.removed, a.valid_as_of, b.valid_as_of,"
        " c.valid_as_of, f.pdf_page FROM firearms f"
        " LEFT JOIN reports a ON a.id = f.first_report"
        " LEFT JOIN reports b ON b.id = f.last_report"
        " LEFT JOIN reports c ON c.id = f.changed_report WHERE f.frn = ?", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, frn);
    if (sqlite3_step(s) != SQLITE_ROW) {
        fprintf(stderr, "FRN %ld not found\n", frn);
        sqlite3_finalize(s);
        return 1;
    }
    printf("FRN %ld%s\n", frn, sqlite3_column_int(s, 9) ? "   ** REMOVED from latest report **" : "");
    static const char *labels[] = { "Make", "Model", "Manufacturer", "Level", "Type", "Action",
                                    "Country", "Serial Numbering", "Legal Classification" };
    for (int i = 0; i < 9; i++) printf("  %-22s %s\n", labels[i], col(s, i));
    printf("  %-22s first %s, last %s, changed %s (page %d)\n", "Reports",
           col(s, 10), col(s, 11), col(s, 12), sqlite3_column_int(s, 13));
    sqlite3_finalize(s);

    printf("\nCalibre, Shots and Barrel Length\n");
    sqlite3_prepare_v2(db, "SELECT sub, calibre, shots, barrel_mm, legal_classification,"
                           " legal_authority, level FROM calibres WHERE frn=? ORDER BY rowid", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, frn);
    int any = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!any++) printf("  %-4s %-28s %6s %8s  %-15s %s\n", "#", "Calibre", "Shots", "Barrel", "Class", "Authority / Level");
        printf("  %-4d %-28s %6s %6smm  %-15s %s\n       %59s%s\n", sqlite3_column_int(s, 0), col(s, 1),
               col(s, 2), col(s, 3), col(s, 4), col(s, 5), "", col(s, 6));
    }
    if (!any) printf("  No Data\n");
    sqlite3_finalize(s);

    printf("\nNotes\n");
    sqlite3_prepare_v2(db, "SELECT category, text FROM notes WHERE frn=? ORDER BY seq", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, frn);
    char last[256] = "\x01";
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (strcmp(last, col(s, 0))) { snprintf(last, sizeof last, "%s", col(s, 0)); printf("  %s\n", last); }
        Str t = {0};
        str_appends(&t, "- ");
        str_appends(&t, col(s, 1));
        print_wrapped("      ", str_c(&t), 100);
        str_free(&t);
    }
    sqlite3_finalize(s);

    printf("\nCross-References\n");
    sqlite3_prepare_v2(db, "SELECT ref_frn, make, model, manufacturer, type, action FROM cross_refs"
                           " WHERE frn=? ORDER BY rowid", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, frn);
    any = 0;
    while (sqlite3_step(s) == SQLITE_ROW && ++any)
        printf("  %-8s %-24s %-30s %-30s %s / %s\n", col(s, 0), col(s, 1), col(s, 2), col(s, 3), col(s, 4), col(s, 5));
    if (!any) printf("  No Data\n");
    sqlite3_finalize(s);

    static const struct { const char *title, *sql; } lists[] = {
        { "Also Known As/Product Code", "SELECT name FROM aliases WHERE frn=? ORDER BY rowid" },
        { "Year Dates",                 "SELECT text FROM year_dates WHERE frn=? ORDER BY rowid" },
        { "Importer",                   "SELECT name FROM importers WHERE frn=? ORDER BY rowid" },
    };
    for (int l = 0; l < 3; l++) {
        printf("\n%s\n", lists[l].title);
        sqlite3_prepare_v2(db, lists[l].sql, -1, &s, NULL);
        sqlite3_bind_int64(s, 1, frn);
        any = 0;
        while (sqlite3_step(s) == SQLITE_ROW && ++any) printf("  %s\n", col(s, 0));
        if (!any) printf("  No Data\n");
        sqlite3_finalize(s);
    }

    sqlite3_prepare_v2(db, "SELECT r.valid_as_of, c.kind, c.field, c.old_value, c.new_value FROM changes c"
                           " JOIN reports r ON r.id = c.report_id WHERE c.frn=? ORDER BY c.id", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, frn);
    any = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!any++) printf("\nHistory\n");
        printf("  %s  %-9s %s", col(s, 0), col(s, 1), col(s, 2));
        if (!strcmp(col(s, 1), "modified") &&
            (sqlite3_column_type(s, 3) != SQLITE_NULL || sqlite3_column_type(s, 4) != SQLITE_NULL))
            printf(": \"%s\" -> \"%s\"", col(s, 3), col(s, 4));
        putchar('\n');
    }
    sqlite3_finalize(s);
    return 0;
}

static int cmd_search(sqlite3 *db, int argc, char **argv)
{
    const char *cls = NULL, *type = NULL;
    int limit = 50, all = 0, i;
    for (i = 0; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--class") && i + 1 < argc) cls = argv[++i];
        else if (!strcmp(argv[i], "--type") && i + 1 < argc) type = argv[++i];
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--all")) all = 1;
        else usage();
    }
    int nwords = argc - i;
    if (nwords == 0 && !cls && !type) usage();

    Str sql = {0};
    str_appends(&sql, "SELECT frn, make, model, manufacturer, type, action, legal_classification, removed"
                      " FROM firearms WHERE 1");
    if (!all) str_appends(&sql, " AND removed = 0");
    for (int w = 0; w < nwords; w++)
        str_appends(&sql, " AND (make LIKE ? OR model LIKE ? OR manufacturer LIKE ? OR CAST(frn AS TEXT) = ?"
                          " OR frn IN (SELECT frn FROM aliases WHERE name LIKE ?))");
    if (cls)  str_appends(&sql, " AND legal_classification LIKE ?");
    if (type) str_appends(&sql, " AND type LIKE ?");
    str_appends(&sql, " ORDER BY make COLLATE NOCASE, model COLLATE NOCASE LIMIT ?");

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, str_c(&sql), -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "%s\n", sqlite3_errmsg(db));
        return 1;
    }
    str_free(&sql);
    int b = 1;
    for (int w = 0; w < nwords; w++) {
        char *pat = sqlite3_mprintf("%%%s%%", argv[i + w]);
        for (int k = 0; k < 3; k++) sqlite3_bind_text(s, b++, pat, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, b++, argv[i + w], -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, b++, pat, -1, SQLITE_TRANSIENT);
        sqlite3_free(pat);
    }
    if (cls)  { char *p = sqlite3_mprintf("%%%s%%", cls);  sqlite3_bind_text(s, b++, p, -1, sqlite3_free); }
    if (type) { char *p = sqlite3_mprintf("%%%s%%", type); sqlite3_bind_text(s, b++, p, -1, sqlite3_free); }
    sqlite3_bind_int(s, b, limit);

    int n = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!n++) printf("%-7s %-22s %-30s %-16s %-16s %s\n", "FRN", "Make", "Model", "Type", "Action", "Class");
        printf("%-7s %-22.22s %-30.30s %-16.16s %-16.16s %s%s\n", col(s, 0), col(s, 1), col(s, 2),
               col(s, 4), col(s, 5), col(s, 6), sqlite3_column_int(s, 7) ? " (removed)" : "");
    }
    sqlite3_finalize(s);
    fprintf(stderr, "%d result%s%s\n", n, n == 1 ? "" : "s", n == limit ? " (limit reached, use --limit)" : "");
    return n ? 0 : 1;
}

static int cmd_changes(sqlite3 *db, int argc, char **argv)
{
    const char *rep = NULL, *kind = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--report") && i + 1 < argc) rep = argv[++i];
        else if (!strcmp(argv[i], "--kind") && i + 1 < argc) kind = argv[++i];
        else usage();
    }
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, rep ? "SELECT id, valid_as_of FROM reports WHERE id = ?1 OR valid_as_of = ?1"
                               : "SELECT id, valid_as_of FROM reports ORDER BY valid_as_of DESC LIMIT 1",
                       -1, &s, NULL);
    if (rep) sqlite3_bind_text(s, 1, rep, -1, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) { fprintf(stderr, "no such report\n"); sqlite3_finalize(s); return 1; }
    sqlite3_int64 id = sqlite3_column_int64(s, 0);
    printf("Changes in report #%lld (valid as of %s)\n", (long long)id, col(s, 1));
    sqlite3_finalize(s);

    sqlite3_prepare_v2(db,
        "SELECT c.frn, c.kind, c.field, c.old_value, c.new_value, f.make, f.model FROM changes c"
        " LEFT JOIN firearms f USING(frn) WHERE c.report_id = ? AND (?2 IS NULL OR c.kind = ?2)"
        " ORDER BY c.kind, c.frn, c.id", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, id);
    if (kind) sqlite3_bind_text(s, 2, kind, -1, SQLITE_STATIC); else sqlite3_bind_null(s, 2);
    int n = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        n++;
        printf("  %-8s FRN %-7s %-20.20s %-28.28s", col(s, 1), col(s, 0), col(s, 5), col(s, 6));
        if (sqlite3_column_type(s, 2) != SQLITE_NULL) printf(" %s", col(s, 2));
        if (!strcmp(col(s, 1), "modified") &&
            (sqlite3_column_type(s, 3) != SQLITE_NULL || sqlite3_column_type(s, 4) != SQLITE_NULL))
            printf(": \"%s\" -> \"%s\"", col(s, 3), col(s, 4));
        putchar('\n');
    }
    sqlite3_finalize(s);
    if (!n) printf("  (none recorded - the first import is the baseline)\n");
    return 0;
}

static int print_query(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) { fprintf(stderr, "%s\n", sqlite3_errmsg(db)); return 1; }
    int nc = sqlite3_column_count(s), row = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!row++) {
            for (int i = 0; i < nc; i++) printf("%-*s", i == nc - 1 ? 0 : 14, sqlite3_column_name(s, i));
            putchar('\n');
        }
        for (int i = 0; i < nc; i++) printf("%-*s", i == nc - 1 ? 0 : 14, col(s, i));
        putchar('\n');
    }
    sqlite3_finalize(s);
    return 0;
}

static int cmd_reports(sqlite3 *db)
{
    return print_query(db, "SELECT id, valid_as_of AS valid, page_count AS pages, record_count AS records,"
                           " added, modified, removed, restored, imported_at FROM reports ORDER BY valid_as_of");
}

static int cmd_stats(sqlite3 *db)
{
    printf("Active firearms: %ld  (removed: %ld)\n\n",
           query_long(db, "SELECT count(*) FROM firearms WHERE removed=0"),
           query_long(db, "SELECT count(*) FROM firearms WHERE removed=1"));
    print_query(db, "SELECT count(*) AS count, ifnull(legal_classification,'(blank)') AS classification"
                    " FROM firearms WHERE removed=0 GROUP BY 2 ORDER BY 1 DESC");
    putchar('\n');
    print_query(db, "SELECT count(*) AS count, ifnull(type,'(blank)') AS type"
                    " FROM firearms WHERE removed=0 GROUP BY 2 ORDER BY 1 DESC LIMIT 15");
    return 0;
}

static int dump_record(const Record *r, void *user)
{
    (void)user;
    printf("FRN %ld (pages %d-%d)\n", r->frn, r->first_page, r->last_page);
    for (int i = 0; i < F_NFIELDS; i++) printf("  %-22s %s\n", FIELD_LABEL[i], str_c(&r->field[i]));
    for (size_t i = 0; i < r->calibres.n; i++) {
        const Calibre *c = &r->calibres.v[i];
        printf("  CAL %d | %s | %s | %s | %s | %s | %s | %s\n", c->sub, str_c(&c->calibre), str_c(&c->shots),
               str_c(&c->barrel_mm), str_c(&c->legal_class), str_c(&c->legal_authority),
               str_c(&c->level), str_c(&c->barrel_type_code));
    }
    for (size_t i = 0; i < r->notes.n; i++)
        printf("  NOTE [%s] %s\n", str_c(&r->notes.v[i].category), str_c(&r->notes.v[i].text));
    for (size_t i = 0; i < r->xrefs.n; i++) {
        const XRef *x = &r->xrefs.v[i];
        printf("  XREF %ld | %s | %s | %s | %s | %s\n", x->ref_frn, str_c(&x->make), str_c(&x->model),
               str_c(&x->manufacturer), str_c(&x->type), str_c(&x->action));
    }
    for (size_t i = 0; i < r->aliases.n; i++)    printf("  AKA  %s\n", str_c(&r->aliases.v[i]));
    for (size_t i = 0; i < r->year_dates.n; i++) printf("  YEAR %s\n", str_c(&r->year_dates.v[i]));
    for (size_t i = 0; i < r->importers.n; i++)  printf("  IMP  %s\n", str_c(&r->importers.v[i]));
    return 0;
}

static int cmd_parse(int argc, char **argv)
{
    if (argc != 1) usage();
    Parser *p = parser_new(dump_record, NULL);
    int rc = parse_text_file(argv[0], p);
    if (!rc) rc = parser_finish(p);
    fprintf(stderr, "valid as of %s, %ld records\n", parser_valid_as_of(p), parser_record_count(p));
    parser_free(p);
    return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (getenv("FRT_DB")) db_path = getenv("FRT_DB");
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "--db") && i + 1 < argc) { db_path = argv[i + 1]; i += 2; }
        else usage();
    }
    if (i >= argc) usage();
    const char *cmd = argv[i++];
    int sub_argc = argc - i;
    char **sub_argv = argv + i;

    if (!strcmp(cmd, "parse")) return cmd_parse(sub_argc, sub_argv);

    sqlite3 *db = db_open(db_path);
    if (!db) return 1;
    int rc;
    if      (!strcmp(cmd, "import"))  rc = cmd_import(db, sub_argc, sub_argv);
    else if (!strcmp(cmd, "show"))    rc = cmd_show(db, sub_argc, sub_argv);
    else if (!strcmp(cmd, "search"))  rc = cmd_search(db, sub_argc, sub_argv);
    else if (!strcmp(cmd, "changes")) rc = cmd_changes(db, sub_argc, sub_argv);
    else if (!strcmp(cmd, "reports")) rc = cmd_reports(db);
    else if (!strcmp(cmd, "stats"))   rc = cmd_stats(db);
    else { sqlite3_close(db); usage(); return 2; }
    sqlite3_close(db);
    return rc;
}
