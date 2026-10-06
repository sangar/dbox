#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void out_of_memory(void) {
    fputs("dbox: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void err_set(Err *err, const char *fmt, ...) {
    if (!err) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
}

void err_sys(Err *err, const char *fmt, ...) {
    if (!err) return;
    int saved = errno;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
    if (n >= 0 && (size_t)n < sizeof err->msg) snprintf(err->msg + n, sizeof err->msg - (size_t)n, ": %s", strerror(saved));
}

void sb_grow(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < sb->len + extra + 1) cap *= 2;
    sb->data = xrealloc(sb->data, cap);
    sb->cap = cap;
}

void sb_append(StrBuf *sb, const char *s, size_t n) {
    sb_grow(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s) { sb_append(sb, s, strlen(s)); }

void sb_putc(StrBuf *sb, char c) { sb_append(sb, &c, 1); }

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n > 0) {
        sb_grow(sb, (size_t)n);
        vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
        sb->len += (size_t)n;
    }
    va_end(ap);
}

void sb_clear(StrBuf *sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

const char *sb_cstr(StrBuf *sb) {
    sb_grow(sb, 0);
    sb->data[sb->len] = '\0';
    return sb->data;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    *sb = (StrBuf){0};
}

void strlist_push_owned(StrList *l, char *s) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = xrealloc(l->items, l->cap * sizeof *l->items);
    }
    l->items[l->len++] = s;
}

void strlist_push(StrList *l, const char *s) { strlist_push_owned(l, xstrdup(s)); }

void strlist_clear(StrList *l) {
    for (size_t i = 0; i < l->len; i++) free(l->items[i]);
    l->len = 0;
}

void strlist_free(StrList *l) {
    strlist_clear(l);
    free(l->items);
    *l = (StrList){0};
}

bool strlist_contains(const StrList *l, const char *s) {
    for (size_t i = 0; i < l->len; i++)
        if (strcmp(l->items[i], s) == 0) return true;
    return false;
}

int compare_strings(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

void strlist_sort(StrList *l) { qsort(l->items, l->len, sizeof *l->items, compare_strings); }

int64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

int64_t wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

int64_t stat_mtime_ns(const struct stat *st) {
#ifdef __APPLE__
    return (int64_t)st->st_mtimespec.tv_sec * NS_PER_SEC + st->st_mtimespec.tv_nsec;
#else
    return (int64_t)st->st_mtim.tv_sec * NS_PER_SEC + st->st_mtim.tv_nsec;
#endif
}

bool set_mtime_ns(const char *path, int64_t ns) {
    struct timespec times[2];
    times[0].tv_sec = times[1].tv_sec = (time_t)(ns / NS_PER_SEC);
    times[0].tv_nsec = times[1].tv_nsec = (long)(ns % NS_PER_SEC);
    return utimensat(AT_FDCWD, path, times, 0) == 0;
}

/* fraction writes v / 10^digits with trailing zeros removed, like Go's Duration.String. */
static void fraction(StrBuf *sb, int64_t whole, int64_t frac, int digits) {
    char f[32];
    snprintf(f, sizeof f, "%0*lld", digits, (long long)frac);
    size_t n = strlen(f);
    while (n > 0 && f[n - 1] == '0') n--;
    sb_printf(sb, "%lld", (long long)whole);
    if (n > 0) sb_printf(sb, ".%.*s", (int)n, f);
}

const char *format_duration(int64_t ns, char buf[48]) {
    StrBuf sb = {0};
    bool negative = ns < 0;
    if (negative) ns = -ns;
    if (negative) sb_putc(&sb, '-');
    if (ns < NS_PER_SEC) {
        if (ns == 0) {
            sb_puts(&sb, "0s");
        } else if (ns < 1000) {
            sb_printf(&sb, "%lldns", (long long)ns);
        } else if (ns < NS_PER_MS) {
            fraction(&sb, ns / 1000, ns % 1000, 3);
            sb_puts(&sb, "\xc2\xb5s");
        } else {
            fraction(&sb, ns / NS_PER_MS, ns % NS_PER_MS, 6);
            sb_puts(&sb, "ms");
        }
    } else {
        int64_t hours = ns / (3600 * NS_PER_SEC);
        int64_t minutes = ns / (60 * NS_PER_SEC) % 60;
        int64_t secs = ns % (60 * NS_PER_SEC);
        if (hours > 0) sb_printf(&sb, "%lldh", (long long)hours);
        if (hours > 0 || minutes > 0) sb_printf(&sb, "%lldm", (long long)minutes);
        fraction(&sb, secs / NS_PER_SEC, secs % NS_PER_SEC, 9);
        sb_putc(&sb, 's');
    }
    snprintf(buf, 48, "%s", sb_cstr(&sb));
    sb_free(&sb);
    return buf;
}

bool parse_duration(const char *s, int64_t *ns) {
    const char *p = s;
    bool negative = false;
    if (*p == '-' || *p == '+') negative = *p++ == '-';
    if (strcmp(p, "0") == 0) {
        *ns = 0;
        return true;
    }
    if (!*p) return false;
    double total = 0;
    while (*p) {
        char *end;
        double v = strtod(p, &end);
        if (end == p || v < 0) return false;
        p = end;
        static const struct {
            const char *unit;
            double ns;
        } units[] = {{"ns", 1}, {"us", 1e3}, {"\xc2\xb5s", 1e3}, {"\xce\xbcs", 1e3}, {"ms", 1e6}, {"s", 1e9}, {"m", 60e9}, {"h", 3600e9}};
        size_t i;
        for (i = 0; i < ARRAY_LEN(units); i++) {
            size_t n = strlen(units[i].unit);
            if (strncmp(p, units[i].unit, n) == 0) {
                total += v * units[i].ns;
                p += n;
                break;
            }
        }
        if (i == ARRAY_LEN(units)) return false;
    }
    *ns = (int64_t)(negative ? -total : total);
    return true;
}

const char *format_time(int64_t ns, char buf[48]) {
    time_t secs = (time_t)(ns / NS_PER_SEC);
    struct tm tm;
    localtime_r(&secs, &tm);
    size_t n = strftime(buf, 48, "%Y-%m-%dT%H:%M:%S", &tm);
    n += (size_t)snprintf(buf + n, 48 - n, ".%03lld", (long long)(ns % NS_PER_SEC / NS_PER_MS));
    long off = tm.tm_gmtoff;
    if (off == 0) {
        snprintf(buf + n, 48 - n, "Z");
    } else {
        char sign = off < 0 ? '-' : '+';
        if (off < 0) off = -off;
        snprintf(buf + n, 48 - n, "%c%02ld:%02ld", sign, off / 3600, off / 60 % 60);
    }
    return buf;
}

bool has_prefix(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

bool has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

bool parse_int64(const char *s, int64_t *out) {
    while (*s == ' ') s++;
    if (!*s) return false;
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    while (*end == ' ') end++;
    if (errno || *end) return false;
    *out = v;
    return true;
}

const char *home_dir(void) {
    const char *home = getenv("HOME");
    if (home && *home) return home;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : ".";
}

bool mkdir_p(const char *path, mode_t mode, Err *err) {
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        if (mkdir(p, mode) != 0 && errno != EEXIST) {
            err_sys(err, "mkdir %s", p);
            free(p);
            return false;
        }
        *s = '/';
    }
    bool ok = mkdir(p, mode) == 0 || errno == EEXIST;
    if (!ok) err_sys(err, "mkdir %s", p);
    free(p);
    return ok;
}

char *path_join(const char *dir, const char *name) {
    size_t n = strlen(dir);
    StrBuf sb = {0};
    sb_append(&sb, dir, n);
    if (n == 0 || dir[n - 1] != '/') sb_putc(&sb, '/');
    sb_puts(&sb, name);
    return sb.data;
}

char *path_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    return xstrndup(path, (size_t)(slash - path));
}

const char *path_base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

const char *path_ext(const char *path) {
    const char *base = path_base(path);
    const char *dot = strrchr(base, '.');
    return dot ? dot : "";
}

char *expand_home(const char *path) {
    if (strcmp(path, "~") == 0) return xstrdup(home_dir());
    if (has_prefix(path, "~/")) return path_join(home_dir(), path + 2);
    return xstrdup(path);
}

char *rel_path(const char *root, const char *path) {
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    if (strncmp(path, root, n) != 0) return NULL;
    const char *rest = path + n;
    if (*rest == '\0') return NULL;
    if (*rest != '/') return NULL;
    while (*rest == '/') rest++;
    if (*rest == '\0') return NULL;
    return xstrdup(rest);
}

bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool read_file(const char *path, StrBuf *out, Err *err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        err_sys(err, "%s", path);
        return false;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_append(out, buf, n);
    bool ok = !ferror(f);
    if (!ok) err_set(err, "%s: read error", path);
    fclose(f);
    sb_cstr(out);
    return ok;
}

bool write_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    return true;
}

bool write_file(const char *path, const void *data, size_t len, mode_t mode, Err *err) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        err_sys(err, "%s", path);
        return false;
    }
    bool ok = write_all(fd, data, len);
    if (!ok) err_sys(err, "%s", path);
    close(fd);
    return ok;
}

bool write_file_atomic(const char *path, const void *data, size_t len, mode_t mode, Err *err) {
    char *dir = path_dir(path);
    bool ok = mkdir_p(dir, 0755, err);
    free(dir);
    if (!ok) return false;
    char *tmp = path_join(path, "");
    tmp[strlen(tmp) - 1] = '\0';
    size_t n = strlen(tmp);
    tmp = xrealloc(tmp, n + 5);
    memcpy(tmp + n, ".tmp", 5);
    ok = write_file(tmp, data, len, mode, err);
    if (ok && rename(tmp, path) != 0) {
        err_sys(err, "rename %s", path);
        ok = false;
    }
    if (!ok) unlink(tmp);
    free(tmp);
    return ok;
}

const char *short_hostname(char buf[256]) {
    if (gethostname(buf, 256) != 0) buf[0] = '\0';
    buf[255] = '\0';
    char *dot = strchr(buf, '.');
    if (dot && dot != buf) *dot = '\0';
    return buf;
}
