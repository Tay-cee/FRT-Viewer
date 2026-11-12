#define _GNU_SOURCE
#include "frt.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    S_NONE,
    S_PRE,
    S_SUMMARY, S_CALIBRE, S_NOTES, S_XREF, S_AKA, S_YEARS, S_IMPORTER
};

static const struct { const char *name; int sect; } SECTIONS[] = {
    { "Summary",                           S_SUMMARY  },
    { "Calibre, Shots and Barrel Length",  S_CALIBRE  },
    { "Notes",                             S_NOTES    },
    { "Cross-References",                  S_XREF     },
    { "Also Known As/Product Code",        S_AKA      },
    { "Year Dates",                        S_YEARS    },
    { "Importer",                          S_IMPORTER },
};

struct Parser {
    record_cb cb;
    void     *user;
    Record    cur;
    int       section;
    int       item_open;
    int       last_field;
    int       prev_cat_line;
    long      cat_first_note;
    int       notes_blank;
    long      cal_idx, xref_idx;
    int       cal_cols[CAL_NCOLS];
    int       xr_cols[XR_NCOLS];
    Str       note_cat;
    char      valid[16];
    long      records, total_pages;
    int       page;
    int       aborted;
};

#define SECTION_MAX_INDENT 9
#define NOTE_CAT_MAX_INDENT 30

static void collapse(const char *s, char *out, size_t n)
{
    size_t o = 0;
    int sp = 0;
    while (*s == ' ') s++;
    for (; *s && o + 1 < n; s++) {
        if (*s == ' ') { sp = 1; continue; }
        if (sp && o) out[o++] = ' ';
        if (o + 1 < n) out[o++] = *s;
        sp = 0;
    }
    out[o] = '\0';
}

static int col_of(const char *line, const char *at)
{
    int c = 0;
    for (const char *s = line; s < at; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) c++;
    return c;
}

typedef struct { int col; const char *s; size_t len; } Seg;

static int segments(const char *line, Seg *out, int max)
{
    int n = 0;
    const char *s = line;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        const char *b = s, *e = s;
        while (*s) {
            if (s[0] == ' ' && (s[1] == ' ' || s[1] == '\0')) break;
            s++;
            e = s;
        }
        if (n < max) { out[n].col = col_of(line, b); out[n].s = b; out[n].len = (size_t)(e - b); n++; }
    }
    return n;
}

static void append_collapsed(Str *dst, const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        while (i < len && isspace((unsigned char)s[i])) i++;
        size_t b = i;
        while (i < len && !isspace((unsigned char)s[i])) i++;
        if (i > b) str_append_word(dst, s + b, i - b);
    }
}

static const char *skip_ws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = '\0';
}

static int col_for(const int *cols, int ncols, int c)
{
    int best = 0;
    for (int i = 0; i < ncols; i++)
        if (cols[i] <= c + 3) best = i;
    return best;
}

static int section_header(const char *line, const char *flat)
{
    const char *t = skip_ws(line);
    if (!*t || col_of(line, t) > SECTION_MAX_INDENT) return -1;
    for (size_t i = 0; i < sizeof SECTIONS / sizeof SECTIONS[0]; i++) {
        size_t n = strlen(SECTIONS[i].name);
        if (strncmp(flat, SECTIONS[i].name, n) == 0 && (flat[n] == '\0' || flat[n] == ' '))
            return SECTIONS[i].sect;
    }
    return -1;
}

static int emit(Parser *p)
{
    int rc = 0;
    if (p->cur.frn) {
        p->records++;
        if (p->cb) rc = p->cb(&p->cur, p->user);
    }
    record_reset(&p->cur);
    return rc;
}

static void close_item(Parser *p) { p->item_open = 0; }

static void start_record(Parser *p, long frn)
{
    p->cur.frn = frn;
    p->cur.first_page = p->cur.last_page = p->page;
    p->section = S_PRE;
    p->item_open = 0;
    p->last_field = -1;
    p->cal_idx = p->xref_idx = -1;
    p->prev_cat_line = 0;
    p->cat_first_note = -1;
    p->notes_blank = 0;
    str_clear(&p->note_cat);
}

static void do_summary(Parser *p, const char *line)
{
    const char *t = skip_ws(line);
    for (int i = 0; i < F_NFIELDS; i++) {
        size_t n = strlen(FIELD_LABEL[i]);
        if (strncmp(t, FIELD_LABEL[i], n) == 0) {
            p->last_field = i;
            str_clear(&p->cur.field[i]);
            append_collapsed(&p->cur.field[i], t + n, strlen(t + n));
            return;
        }
    }
    if (p->last_field >= 0) append_collapsed(&p->cur.field[p->last_field], t, strlen(t));
}

static void set_cols(int *cols, int ncols, const char *line, const char *const *names)
{
    for (int i = 0; i < ncols; i++) {
        const char *at = strstr(line, names[i]);
        if (at) cols[i] = col_of(line, at);
    }
}

static int parse_row_frn(const char *t, long *frn, int *sub, const char **after)
{
    char *e;
    if (!isdigit((unsigned char)*t)) return 0;
    long f = strtol(t, &e, 10);
    if (sub) {
        const char *d = e;
        while (*d == ' ') d++;
        if (*d != '-') return 0;
        d++;
        while (*d == ' ') d++;
        if (!isdigit((unsigned char)*d)) return 0;
        *sub = (int)strtol(d, &e, 10);
    }
    if (*e != ' ' && *e != '\0') return 0;
    *frn = f;
    if (after) *after = e;
    return 1;
}

static void do_calibre(Parser *p, const char *line)
{
    static const char *const names[CAL_NCOLS] =
        { "FRN", "Calibre", "Shots", "(mm)", "Legal Authority", "Level", "Barrel Type Code" };
    const char *t = skip_ws(line);
    if (!*t || strcmp(t, "No Data") == 0 || strcmp(t, "Legal Classification") == 0 || strcmp(t, "Barrel") == 0)
        return;
    if (strncmp(t, "FRN ", 4) == 0 && strstr(t, "Calibre") && strstr(t, "Shots")) {
        set_cols(p->cal_cols, CAL_NCOLS, line, names);
        return;
    }
    Seg seg[16];
    int ns = segments(line, seg, 16);
    long frn; int sub; const char *after;
    int first = 0;
    if (parse_row_frn(t, &frn, &sub, &after)) {
        Calibre c; memset(&c, 0, sizeof c);
        c.sub = sub;
        VEC_PUSH(p->cur.calibres, c);
        p->cal_idx = (long)p->cur.calibres.n - 1;
        first = 1;
    }
    if (p->cal_idx < 0) return;
    Calibre *c = &p->cur.calibres.v[p->cal_idx];
    for (int i = first ? 1 : 0; i < ns; i++) {
        Str *dst = NULL;
        switch (col_for(p->cal_cols, CAL_NCOLS, seg[i].col)) {
        case CAL_FRN:
        case CAL_CALIBRE: dst = &c->calibre; break;
        case CAL_SHOTS:   dst = &c->shots; break;
        case CAL_BARREL:  dst = &c->barrel_mm; break;
        case CAL_LEGAL:   dst = first ? &c->legal_class : &c->legal_authority; break;
        case CAL_LEVEL:   dst = &c->level; break;
        case CAL_BTC:     dst = &c->barrel_type_code; break;
        }
        append_collapsed(dst, seg[i].s, seg[i].len);
    }
}

static void new_note(Parser *p, const char *text, size_t len)
{
    Note n; memset(&n, 0, sizeof n);
    str_appends(&n.category, str_c(&p->note_cat));
    append_collapsed(&n.text, text, len);
    VEC_PUSH(p->cur.notes, n);
}

static void do_notes(Parser *p, const char *line)
{
    const char *t = skip_ws(line);
    int prev_cat = p->prev_cat_line, after_blank = p->notes_blank;
    p->prev_cat_line = 0;
    p->notes_blank = !*t;
    if (!*t) return;
    int indent = col_of(line, t);
    const char *body = t;

    if (indent <= NOTE_CAT_MAX_INDENT && *t != '-') {
        Seg seg[2];
        int ns = segments(line, seg, 2);
        int wrapped = (prev_cat && (ns < 2 || seg[1].s[0] != '-')) ||
                      (ns < 2 && !after_blank &&
                       p->cat_first_note >= 0 && p->cat_first_note < (long)p->cur.notes.n);
        if (wrapped) {
            append_collapsed(&p->note_cat, seg[0].s, seg[0].len);
            for (long i = p->cat_first_note < 0 ? 0 : p->cat_first_note; i < (long)p->cur.notes.n; i++) {
                str_clear(&p->cur.notes.v[i].category);
                str_appends(&p->cur.notes.v[i].category, str_c(&p->note_cat));
            }
        } else {
            str_clear(&p->note_cat);
            append_collapsed(&p->note_cat, seg[0].s, seg[0].len);
            p->prev_cat_line = 1;
            p->cat_first_note = (long)p->cur.notes.n;
        }
        if (ns < 2) return;
        body = seg[1].s;
    }
    if (body[0] == '-' && (body[1] == ' ' || body[1] == '\0')) {
        const char *b = skip_ws(body + 1);
        new_note(p, b, strlen(b));
    } else if (p->cur.notes.n &&
               strcmp(str_c(&p->cur.notes.v[p->cur.notes.n - 1].category), str_c(&p->note_cat)) == 0) {
        append_collapsed(&p->cur.notes.v[p->cur.notes.n - 1].text, body, strlen(body));
    } else {
        new_note(p, body, strlen(body));
    }
}

static void do_xref(Parser *p, const char *line)
{
    static const char *const names[XR_NCOLS] =
        { "FRN", "Make", "Model", "Manufacturer", "Type", "Action" };
    const char *t = skip_ws(line);
    if (!*t || strcmp(t, "No Data") == 0) return;
    if (strncmp(t, "FRN ", 4) == 0 && strstr(t, "Make") && strstr(t, "Model")) {
        set_cols(p->xr_cols, XR_NCOLS, line, names);
        return;
    }
    Seg seg[16];
    int ns = segments(line, seg, 16);
    long frn;
    int first = 0;
    if (parse_row_frn(t, &frn, NULL, NULL) && ns >= 2) {
        XRef x; memset(&x, 0, sizeof x);
        x.ref_frn = frn;
        VEC_PUSH(p->cur.xrefs, x);
        p->xref_idx = (long)p->cur.xrefs.n - 1;
        first = 1;
    }
    if (p->xref_idx < 0) return;
    XRef *x = &p->cur.xrefs.v[p->xref_idx];
    for (int i = first ? 1 : 0; i < ns; i++) {
        Str *dst = NULL;
        switch (col_for(p->xr_cols, XR_NCOLS, seg[i].col)) {
        case XR_FRN:
        case XR_MAKE:   dst = &x->make; break;
        case XR_MODEL:  dst = &x->model; break;
        case XR_MANUF:  dst = &x->manufacturer; break;
        case XR_TYPE:   dst = &x->type; break;
        case XR_ACTION: dst = &x->action; break;
        }
        append_collapsed(dst, seg[i].s, seg[i].len);
    }
}

static void do_list(Parser *p, const char *line, void *vecp)
{
    VEC(Str) *vec = vecp;
    const char *t = skip_ws(line);
    if (!*t) { close_item(p); return; }
    if (strcmp(t, "No Data") == 0) return;
    if (!p->item_open) {
        Str s = {0};
        VEC_PUSH(*vec, s);
        p->item_open = 1;
    }
    append_collapsed(&vec->v[vec->n - 1], t, strlen(t));
}

Parser *parser_new(record_cb cb, void *user)
{
    Parser *p = xcalloc(1, sizeof *p);
    p->cb = cb;
    p->user = user;
    p->section = S_NONE;
    p->last_field = -1;
    p->cal_idx = p->xref_idx = -1;
    int cal[CAL_NCOLS] = { 11, 33, 88, 100, 113, 188, 227 };
    int xr[XR_NCOLS]   = { 11, 35, 85, 134, 196, 225 };
    memcpy(p->cal_cols, cal, sizeof cal);
    memcpy(p->xr_cols, xr, sizeof xr);
    return p;
}

int parser_feed_line(Parser *p, char *line)
{
    if (p->aborted) return -1;
    while (*line == '\f') line++;
    rtrim(line);

    char flat[1024];
    collapse(line, flat, sizeof flat);

    const char *s;
    if ((s = strstr(flat, "Valid as of:")) && strstr(flat, "FRT Report")) {
        if (!p->valid[0]) {
            s = skip_ws(s + 12);
            snprintf(p->valid, sizeof p->valid, "%.10s", s);
        }
        close_item(p);
        return 0;
    }
    if ((s = strstr(flat, "Firearm Reference Number (FRN):"))) {
        line = flat;
        long frn = strtol(s + 31, NULL, 10);
        const char *pg = strstr(line, "Page:");
        if (pg) {
            p->page = (int)strtol(pg + 5, (char **)&pg, 10);
            const char *sl = strchr(pg, '/');
            if (sl) p->total_pages = strtol(sl + 1, NULL, 10);
        }
        close_item(p);
        if (frn != p->cur.frn) {
            if (emit(p)) { p->aborted = 1; return -1; }
            start_record(p, frn);
        }
        p->cur.last_page = p->page;
        return 0;
    }
    if (strstr(flat, "Firearms Reference Table, National Specialized Services")) return 0;

    if (p->section == S_NONE) return 0;

    int sect = section_header(line, flat);
    if (sect >= 0) {
        p->section = sect;
        close_item(p);
        return 0;
    }

    switch (p->section) {
    case S_PRE:      break;
    case S_SUMMARY:  if (*skip_ws(line)) do_summary(p, line); break;
    case S_CALIBRE:  do_calibre(p, line); break;
    case S_NOTES:    do_notes(p, line); break;
    case S_XREF:     do_xref(p, line); break;
    case S_AKA:      do_list(p, line, &p->cur.aliases); break;
    case S_YEARS:    do_list(p, line, &p->cur.year_dates); break;
    case S_IMPORTER: do_list(p, line, &p->cur.importers); break;
    }
    return 0;
}

int parser_finish(Parser *p)
{
    if (p->aborted) return -1;
    return emit(p) ? -1 : 0;
}

const char *parser_valid_as_of(const Parser *p) { return p->valid; }
long parser_record_count(const Parser *p) { return p->records; }
long parser_total_pages(const Parser *p) { return p->total_pages; }

void parser_free(Parser *p)
{
    if (!p) return;
    record_free(&p->cur);
    str_free(&p->note_cat);
    free(p);
}

int parse_text_file(const char *path, Parser *p)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int rc = 0;
    while ((n = getline(&line, &cap, f)) >= 0) {
        if (n && line[n - 1] == '\n') line[n - 1] = '\0';
        if (parser_feed_line(p, line)) { rc = -1; break; }
    }
    free(line);
    fclose(f);
    return rc;
}
