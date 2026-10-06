#ifndef DBOX_UTIL_H
#define DBOX_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "alloc.h"
#include "error.h"
#include "strbuf.h"

#define countof(a) (sizeof(a) / sizeof((a)[0]))

static inline size_t min_size(size_t a, size_t b) { return a < b ? a : b; }
static inline size_t max_size(size_t a, size_t b) { return a > b ? a : b; }
static inline int64_t min_i64(int64_t a, int64_t b) { return a < b ? a : b; }

#define NS_PER_MS 1000000LL
#define NS_PER_SEC 1000000000LL


/* format_duration and parse_duration use Go's notation: 750ms, 30s, 10m0s, 1h0m0s. */
const char *format_duration(int64_t ns, char buf[48]);
bool parse_duration(const char *s, int64_t *ns);
/* format_time writes RFC 3339 local time with milliseconds, as slog does. */
const char *format_time(int64_t ns, char buf[48]);

bool has_prefix(const char *s, const char *prefix);
bool has_suffix(const char *s, const char *suffix);
bool parse_int64(const char *s, int64_t *out);
int compare_strings(const void *a, const void *b);

[[nodiscard]] Error mkdir_p(const char *path, unsigned mode, Err *err);
char *path_join(const char *dir, const char *name);
char *path_dir(const char *path);
const char *path_base(const char *path);
/* path_ext is the suffix from the last dot of the last element, or "". */
const char *path_ext(const char *path);
char *expand_home(const char *path);
/* rel_path returns path relative to root, or NULL when it is the root or outside it. */
char *rel_path(const char *root, const char *path);
bool is_dir(const char *path);
[[nodiscard]] Error read_file(const char *path, StrBuf *out, Err *err);
[[nodiscard]] Error write_file(const char *path, const void *data, size_t len, unsigned mode, Err *err);
[[nodiscard]] Error write_file_atomic(const char *path, const void *data, size_t len, unsigned mode, Err *err);

#endif
