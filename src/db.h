#ifndef FRT_DB_H
#define FRT_DB_H

#include "frt.h"
#include <sqlite3.h>

typedef struct {
    long records, added, modified, unchanged, removed, restored, duplicates;
} ImportStats;

typedef struct Importer Importer;

sqlite3    *db_open(const char *path);
int         db_exec(sqlite3 *db, const char *sql);

Importer   *importer_new(sqlite3 *db, sqlite3_int64 report_id, int baseline);
int         importer_record(const Record *r, void *user);
int         importer_finish(Importer *im);
ImportStats importer_stats(const Importer *im);
void        importer_free(Importer *im);

#endif
