#ifndef DBOX_UTIL_H
#define DBOX_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "alloc.h"

#define countof(a) (sizeof(a) / sizeof((a)[0]))

static inline size_t min_size(size_t a, size_t b) { return a < b ? a : b; }
static inline size_t max_size(size_t a, size_t b) { return a > b ? a : b; }
static inline int64_t min_i64(int64_t a, int64_t b) { return a < b ? a : b; }

#define NS_PER_MS 1000000LL
#define NS_PER_SEC 1000000000LL


/* Err carries a human readable message up to whoever can report it. */
typedef struct {
    char msg[512];
} Err;

void err_set(Err *err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* err_sys formats the message and appends ": strerror(errno)". */
void err_sys(Err *err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

typedef struct {
    char *data;
    size_t len, cap;
} StrBuf;

void sb_grow(StrBuf *sb, size_t extra);
void sb_append(StrBuf *sb, const char *s, size_t n);
void sb_puts(StrBuf *sb, const char *s);
void sb_putc(StrBuf *sb, char c);
void sb_printf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_clear(StrBuf *sb);
const char *sb_cstr(StrBuf *sb);
void sb_free(StrBuf *sb);

/* StrList owns its strings. */
typedef struct {
    char **items;
    size_t len, cap;
} StrList;

void strlist_push(StrList *l, const char *s);
void strlist_push_owned(StrList *l, char *s);
void strlist_clear(StrList *l);
void strlist_free(StrList *l);
bool strlist_contains(const StrList *l, const char *s);
void strlist_sort(StrList *l);

int64_t monotonic_ns(void);
int64_t wall_ns(void);
int64_t stat_mtime_ns(const struct stat *st);
bool set_mtime_ns(const char *path, int64_t ns);

/* format_duration and parse_duration use Go's notation: 750ms, 30s, 10m0s, 1h0m0s. */
const char *format_duration(int64_t ns, char buf[48]);
bool parse_duration(const char *s, int64_t *ns);
/* format_time writes RFC 3339 local time with milliseconds, as slog does. */
const char *format_time(int64_t ns, char buf[48]);

bool has_prefix(const char *s, const char *prefix);
bool has_suffix(const char *s, const char *suffix);
bool parse_int64(const char *s, int64_t *out);
int compare_strings(const void *a, const void *b);

const char *home_dir(void);
bool mkdir_p(const char *path, mode_t mode, Err *err);
char *path_join(const char *dir, const char *name);
char *path_dir(const char *path);
const char *path_base(const char *path);
/* path_ext is the suffix from the last dot of the last element, or "". */
const char *path_ext(const char *path);
char *expand_home(const char *path);
/* rel_path returns path relative to root, or NULL when it is the root or outside it. */
char *rel_path(const char *root, const char *path);
bool is_dir(const char *path);
bool read_file(const char *path, StrBuf *out, Err *err);
bool write_file(const char *path, const void *data, size_t len, mode_t mode, Err *err);
bool write_file_atomic(const char *path, const void *data, size_t len, mode_t mode, Err *err);
bool write_all(int fd, const void *data, size_t len);
const char *short_hostname(char buf[256]);

#endif
