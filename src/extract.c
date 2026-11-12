#define _GNU_SOURCE
#include "frt.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

long pdf_page_count(const char *pdf)
{
    char cmd[4096];
    Str q = {0};
    str_appends(&q, "'");
    for (const char *s = pdf; *s; s++) {
        if (*s == '\'') str_appends(&q, "'\\''");
        else str_append(&q, s, 1);
    }
    str_appends(&q, "'");
    snprintf(cmd, sizeof cmd, "pdfinfo %s 2>/dev/null", str_c(&q));
    str_free(&q);

    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char line[1024];
    long pages = -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "Pages:", 6) == 0) pages = strtol(line + 6, NULL, 10);
    pclose(f);
    return pages;
}

typedef struct {
    pid_t pid;
    long  first, last;
    char  path[600];
} Chunk;

static pid_t spawn(const char *pdf, Chunk *c)
{
    char f[32], l[32];
    snprintf(f, sizeof f, "%ld", c->first);
    snprintf(l, sizeof l, "%ld", c->last);
    pid_t pid = fork();
    if (pid == 0) {
        execlp("pdftotext", "pdftotext", "-q", "-fixed", "3", "-enc", "UTF-8",
               "-f", f, "-l", l, pdf, c->path, (char *)NULL);
        perror("exec pdftotext");
        _exit(127);
    }
    return pid;
}

int pdf_extract_parse(const char *pdf, long pages, int workers, int chunk, Parser *p, int quiet)
{
    if (workers < 1) workers = 1;
    if (chunk < 1) chunk = 500;
    long nchunks = (pages + chunk - 1) / chunk;

    const char *tmp = getenv("TMPDIR");
    char dir[512];
    snprintf(dir, sizeof dir, "%s/frt-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return -1; }

    Chunk *ch = xcalloc((size_t)nchunks, sizeof *ch);
    for (long i = 0; i < nchunks; i++) {
        ch[i].first = i * chunk + 1;
        ch[i].last  = (i + 1) * chunk < pages ? (i + 1) * chunk : pages;
        snprintf(ch[i].path, sizeof ch[i].path, "%s/chunk-%06ld.txt", dir, i);
    }

    long next = 0;
    int  rc = 0;
    for (long i = 0; i < nchunks && rc == 0; i++) {
        while (next < nchunks && next < i + workers) {
            ch[next].pid = spawn(pdf, &ch[next]);
            if (ch[next].pid < 0) { perror("fork"); rc = -1; break; }
            next++;
        }
        if (rc) break;

        int st;
        while (waitpid(ch[i].pid, &st, 0) < 0 && errno == EINTR) {}
        ch[i].pid = 0;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            fprintf(stderr, "\npdftotext failed on pages %ld-%ld (status %d)\n",
                    ch[i].first, ch[i].last, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
            rc = -1;
            break;
        }
        if (parse_text_file(ch[i].path, p)) rc = -1;
        unlink(ch[i].path);
        if (!quiet) {
            fprintf(stderr, "\r  pages %ld / %ld  (%.1f%%)  records %ld   ",
                    ch[i].last, pages, 100.0 * ch[i].last / pages, parser_record_count(p));
            fflush(stderr);
        }
    }
    if (!quiet) fputc('\n', stderr);

    for (long i = 0; i < next; i++) {
        if (ch[i].pid > 0) { kill(ch[i].pid, SIGTERM); waitpid(ch[i].pid, NULL, 0); }
        unlink(ch[i].path);
    }
    rmdir(dir);
    free(ch);
    return rc;
}
