#ifndef FRT_H
#define FRT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char  *p;
    size_t n, cap;
} Str;

void  str_append(Str *s, const char *src, size_t len);
void  str_appends(Str *s, const char *src);
void  str_append_word(Str *s, const char *src, size_t len);
void  str_clear(Str *s);
void  str_free(Str *s);
const char *str_c(const Str *s);

#define VEC(T) struct { T *v; size_t n, cap; }
#define VEC_PUSH(vec, item) do {                                           \
        if ((vec).n == (vec).cap) {                                        \
            (vec).cap = (vec).cap ? (vec).cap * 2 : 8;                     \
            (vec).v = xrealloc((vec).v, (vec).cap * sizeof(*(vec).v));     \
        }                                                                  \
        (vec).v[(vec).n++] = (item);                                       \
    } while (0)

void *xrealloc(void *p, size_t n);
void *xcalloc(size_t n, size_t sz);

enum { CAL_FRN, CAL_CALIBRE, CAL_SHOTS, CAL_BARREL, CAL_LEGAL, CAL_LEVEL, CAL_BTC, CAL_NCOLS };
enum { XR_FRN, XR_MAKE, XR_MODEL, XR_MANUF, XR_TYPE, XR_ACTION, XR_NCOLS };

typedef struct {
    int  sub;
    Str  calibre, shots, barrel_mm, legal_class, legal_authority, level, barrel_type_code;
} Calibre;

typedef struct {
    Str category;
    Str text;
} Note;

typedef struct {
    long ref_frn;
    Str  make, model, manufacturer, type, action;
} XRef;

enum {
    F_MAKE, F_MODEL, F_MANUFACTURER, F_LEVEL, F_TYPE, F_ACTION,
    F_COUNTRY, F_SERIAL, F_LEGAL, F_NFIELDS
};
extern const char *const FIELD_LABEL[F_NFIELDS];
extern const char *const FIELD_COLUMN[F_NFIELDS];

typedef struct {
    long frn;
    int  first_page, last_page;
    Str  field[F_NFIELDS];
    VEC(Calibre) calibres;
    VEC(Note)    notes;
    VEC(XRef)    xrefs;
    VEC(Str)     aliases;
    VEC(Str)     year_dates;
    VEC(Str)     importers;
} Record;

void record_free(Record *r);
void record_reset(Record *r);

enum { H_SUMMARY, H_CALIBRES, H_NOTES, H_XREFS, H_ALIASES, H_YEARS, H_IMPORTERS, H_NSECT };
extern const char *const SECTION_NAME[H_NSECT];
void     record_hashes(const Record *r, uint64_t out[H_NSECT]);
uint64_t record_hash_all(const uint64_t h[H_NSECT]);
void     serialize_calibres(const Record *r, Str *out);

typedef int (*record_cb)(const Record *r, void *user);

typedef struct Parser Parser;
Parser     *parser_new(record_cb cb, void *user);
int         parser_feed_line(Parser *p, char *line);
int         parser_finish(Parser *p);
const char *parser_valid_as_of(const Parser *p);
long        parser_record_count(const Parser *p);
long        parser_total_pages(const Parser *p);
void        parser_free(Parser *p);

int parse_text_file(const char *path, Parser *p);

long pdf_page_count(const char *pdf);
int  pdf_extract_parse(const char *pdf, long pages, int workers, int chunk, Parser *p, int quiet);

#endif
