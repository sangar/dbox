#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/platform.h"

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
        for (i = 0; i < countof(units); i++) {
            size_t n = strlen(units[i].unit);
            if (strncmp(p, units[i].unit, n) == 0) {
                total += v * units[i].ns;
                p += n;
                break;
            }
        }
        if (i == countof(units)) return false;
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
    bool negative = *s == '-';
    if (*s == '-' || *s == '+') s++;
    if (*s < '0' || *s > '9') return false;
    uint64_t magnitude = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        uint64_t digit = (uint64_t)(*s - '0');
        if (magnitude > (UINT64_MAX - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    while (*s == ' ') s++;
    if (*s) return false;
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    if (magnitude > limit) return false;
    *out = negative ? (int64_t)(0 - magnitude) : (int64_t)magnitude;
    return true;
}

Error mkdir_p(const char *path, unsigned mode, Err *err) {
    char *p = xstrdup(path);
    Error e = ERR_OK;
    for (char *s = p + 1; e == ERR_OK && *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        e = dir_create(p, mode, err);
        *s = '/';
    }
    if (e == ERR_OK) e = dir_create(p, mode, err);
    xfree(p);
    return e;
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
    if (strcmp(path, "~") == 0) return xstrdup(env_home());
    if (has_prefix(path, "~/")) return path_join(env_home(), path + 2);
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
    FileStat info;
    return file_info_follow(path, &info, NULL) == ERR_OK && info.is_dir;
}

Error read_file(const char *path, StrBuf *out, Err *err) {
    int fd;
    Error e = file_open_read(path, &fd, err);
    if (e == ERR_NOT_FOUND) return err_set(err, e, "%s: no such file", path);
    if (e != ERR_OK) return e;
    char buf[65536];
    size_t n;
    while ((e = file_read(fd, buf, sizeof buf, &n, err)) == ERR_OK && n > 0) sb_append(out, buf, n);
    if (e != ERR_OK) e = err_set(err, e, "%s: read error", path);
    file_close(fd);
    sb_cstr(out);
    return e;
}

Error write_file(const char *path, const void *data, size_t len, unsigned mode, Err *err) {
    int fd;
    Error e = file_create(path, mode, &fd, err);
    if (e != ERR_OK) return e;
    if (!file_write_all(fd, data, len)) e = err_set(err, ERR_IO, "%s: write error", path);
    file_close(fd);
    return e;
}

Error write_file_atomic(const char *path, const void *data, size_t len, unsigned mode, Err *err) {
    char *dir = path_dir(path);
    Error e = mkdir_p(dir, 0755, err);
    xfree(dir);
    if (e != ERR_OK) return e;
    char *tmp = path_join(path, "");
    tmp[strlen(tmp) - 1] = '\0';
    size_t n = strlen(tmp);
    tmp = xrealloc(tmp, n + 5);
    memcpy(tmp + n, ".tmp", 5);
    e = write_file(tmp, data, len, mode, err);
    if (e == ERR_OK) e = file_rename(tmp, path, err);
    if (e != ERR_OK) (void)file_remove(tmp, NULL);
    xfree(tmp);
    return e;
}
