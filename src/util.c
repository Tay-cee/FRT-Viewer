#define _GNU_SOURCE
#include "frt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { perror("realloc"); exit(1); }
    return q;
}

void *xcalloc(size_t n, size_t sz)
{
    void *q = calloc(n ? n : 1, sz ? sz : 1);
    if (!q) { perror("calloc"); exit(1); }
    return q;
}

void str_append(Str *s, const char *src, size_t len)
{
    if (s->n + len + 1 > s->cap) {
        size_t c = s->cap ? s->cap : 32;
        while (c < s->n + len + 1) c *= 2;
        s->p = xrealloc(s->p, c);
        s->cap = c;
    }
    memcpy(s->p + s->n, src, len);
    s->n += len;
    s->p[s->n] = '\0';
}

void str_appends(Str *s, const char *src) { str_append(s, src, strlen(src)); }

void str_append_word(Str *s, const char *src, size_t len)
{
    if (!len) return;
    if (s->n) str_append(s, " ", 1);
    str_append(s, src, len);
}

void str_clear(Str *s) { s->n = 0; if (s->p) s->p[0] = '\0'; }
void str_free(Str *s)  { free(s->p); s->p = NULL; s->n = s->cap = 0; }
const char *str_c(const Str *s) { return s->p ? s->p : ""; }

const char *const FIELD_LABEL[F_NFIELDS] = {
    "Make:", "Model:", "Manufacturer:", "Level:", "Type:", "Action:",
    "Country of Manufacturer:", "Serial Numbering:", "Legal Classification:"
};
const char *const FIELD_COLUMN[F_NFIELDS] = {
    "make", "model", "manufacturer", "level", "type", "action",
    "country", "serial_numbering", "legal_classification"
};
const char *const SECTION_NAME[H_NSECT] = {
    "summary", "calibres", "notes", "cross_refs", "aliases", "year_dates", "importers"
};

void record_reset(Record *r)
{
    for (int i = 0; i < F_NFIELDS; i++) str_free(&r->field[i]);
    for (size_t i = 0; i < r->calibres.n; i++) {
        Calibre *c = &r->calibres.v[i];
        str_free(&c->calibre); str_free(&c->shots); str_free(&c->barrel_mm);
        str_free(&c->legal_class); str_free(&c->legal_authority);
        str_free(&c->level); str_free(&c->barrel_type_code);
    }
    for (size_t i = 0; i < r->notes.n; i++) { str_free(&r->notes.v[i].category); str_free(&r->notes.v[i].text); }
    for (size_t i = 0; i < r->xrefs.n; i++) {
        XRef *x = &r->xrefs.v[i];
        str_free(&x->make); str_free(&x->model); str_free(&x->manufacturer);
        str_free(&x->type); str_free(&x->action);
    }
    for (size_t i = 0; i < r->aliases.n; i++)    str_free(&r->aliases.v[i]);
    for (size_t i = 0; i < r->year_dates.n; i++) str_free(&r->year_dates.v[i]);
    for (size_t i = 0; i < r->importers.n; i++)  str_free(&r->importers.v[i]);
    r->calibres.n = r->notes.n = r->xrefs.n = 0;
    r->aliases.n = r->year_dates.n = r->importers.n = 0;
    r->frn = 0; r->first_page = r->last_page = 0;
}

void record_free(Record *r)
{
    record_reset(r);
    free(r->calibres.v); free(r->notes.v); free(r->xrefs.v);
    free(r->aliases.v); free(r->year_dates.v); free(r->importers.v);
    memset(r, 0, sizeof *r);
}

static uint64_t fnv(uint64_t h, const char *s)
{
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    h ^= 0x1f; h *= 1099511628211ULL;
    return h;
}
#define FNV0 1469598103934665603ULL

void record_hashes(const Record *r, uint64_t h[H_NSECT])
{
    char buf[32];
    for (int i = 0; i < H_NSECT; i++) h[i] = FNV0;
    for (int i = 0; i < F_NFIELDS; i++) h[H_SUMMARY] = fnv(h[H_SUMMARY], str_c(&r->field[i]));
    for (size_t i = 0; i < r->calibres.n; i++) {
        const Calibre *c = &r->calibres.v[i];
        snprintf(buf, sizeof buf, "%d", c->sub);
        uint64_t x = h[H_CALIBRES];
        x = fnv(x, buf); x = fnv(x, str_c(&c->calibre)); x = fnv(x, str_c(&c->shots));
        x = fnv(x, str_c(&c->barrel_mm)); x = fnv(x, str_c(&c->legal_class));
        x = fnv(x, str_c(&c->legal_authority)); x = fnv(x, str_c(&c->level));
        x = fnv(x, str_c(&c->barrel_type_code));
        h[H_CALIBRES] = x;
    }
    for (size_t i = 0; i < r->notes.n; i++) {
        h[H_NOTES] = fnv(h[H_NOTES], str_c(&r->notes.v[i].category));
        h[H_NOTES] = fnv(h[H_NOTES], str_c(&r->notes.v[i].text));
    }
    for (size_t i = 0; i < r->xrefs.n; i++) {
        const XRef *x = &r->xrefs.v[i];
        snprintf(buf, sizeof buf, "%ld", x->ref_frn);
        uint64_t y = h[H_XREFS];
        y = fnv(y, buf); y = fnv(y, str_c(&x->make)); y = fnv(y, str_c(&x->model));
        y = fnv(y, str_c(&x->manufacturer)); y = fnv(y, str_c(&x->type)); y = fnv(y, str_c(&x->action));
        h[H_XREFS] = y;
    }
    for (size_t i = 0; i < r->aliases.n; i++)    h[H_ALIASES]   = fnv(h[H_ALIASES],   str_c(&r->aliases.v[i]));
    for (size_t i = 0; i < r->year_dates.n; i++) h[H_YEARS]     = fnv(h[H_YEARS],     str_c(&r->year_dates.v[i]));
    for (size_t i = 0; i < r->importers.n; i++)  h[H_IMPORTERS] = fnv(h[H_IMPORTERS], str_c(&r->importers.v[i]));
}

uint64_t record_hash_all(const uint64_t h[H_NSECT])
{
    uint64_t x = FNV0;
    for (int i = 0; i < H_NSECT; i++) { x ^= h[i]; x *= 1099511628211ULL; }
    return x;
}

void serialize_calibres(const Record *r, Str *out)
{
    char buf[32];
    for (size_t i = 0; i < r->calibres.n; i++) {
        const Calibre *c = &r->calibres.v[i];
        if (i) str_appends(out, "; ");
        snprintf(buf, sizeof buf, "#%d ", c->sub);
        str_appends(out, buf);
        str_appends(out, str_c(&c->calibre));   str_appends(out, " | ");
        str_appends(out, str_c(&c->shots));     str_appends(out, " shots | ");
        str_appends(out, str_c(&c->barrel_mm)); str_appends(out, "mm | ");
        str_appends(out, str_c(&c->legal_class));
        if (c->legal_authority.n) { str_appends(out, " ("); str_appends(out, str_c(&c->legal_authority)); str_appends(out, ")"); }
    }
}
