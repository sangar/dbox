#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "store.h"

#define DISK_TEMP_PREFIX ".dbox-upload-"

/* Disk stores objects as files under root/prefix. The ETag is size and modification time; the content hash is computed on Head. */
typedef struct {
    Store base;
    char *dir;
} Disk;

static char *object_path(Disk *d, const char *key) { return path_join(d->dir, key); }

static void describe(Object *obj, const char *key, const struct stat *st) {
    memset(obj, 0, sizeof *obj);
    obj->key = key;
    obj->size = st->st_size;
    obj->mtime_ns = stat_mtime_ns(st);
    snprintf(obj->etag, sizeof obj->etag, "%lld-%lld", (long long)st->st_size, (long long)obj->mtime_ns);
}

[[nodiscard]] static Error disk_put(Store *s, Ctx *ctx, const char *key, int fd, int64_t size, const Meta *meta, char etag[ETAG_MAX], Err *err) {
    Disk *d = (Disk *)s;
    char *target = object_path(d, key);
    char *dir = path_dir(target);
    char *tmp = NULL;
    int out = -1;
    int64_t copied;
    Error e = mkdir_p(dir, 0755, err);
    if (e != ERR_OK) goto out;
    tmp = path_join(dir, DISK_TEMP_PREFIX "XXXXXX");
    out = mkstemp(tmp);
    if (out < 0) {
        e = err_sys(err, "%s", tmp);
        goto out;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        e = err_sys(err, "seek");
        goto out;
    }
    if ((e = copy_fd(fd, out, NULL, &copied, err)) != ERR_OK) goto out;
    close(out);
    out = -1;
    if (meta->mtime_ns != 0 && !set_mtime_ns(tmp, meta->mtime_ns)) {
        e = err_sys(err, "set mtime %s", tmp);
        goto out;
    }
    if (rename(tmp, target) != 0) {
        e = err_sys(err, "rename %s", target);
        goto out;
    }
    struct stat st;
    if (stat(target, &st) != 0) {
        e = err_sys(err, "%s", target);
        goto out;
    }
    Object obj;
    describe(&obj, key, &st);
    memcpy(etag, obj.etag, ETAG_MAX);
out:
    if (out >= 0) close(out);
    if (e != ERR_OK && tmp) unlink(tmp);
    xfree(tmp);
    xfree(dir);
    xfree(target);
    return e;
}

[[nodiscard]] static Error open_object(Disk *d, const char *key, Object *obj, int *fd, Err *err) {
    char *path = object_path(d, key);
    int f = open(path, O_RDONLY | O_CLOEXEC);
    Error e = ERR_OK;
    if (f < 0) {
        e = errno == ENOENT || errno == ENOTDIR ? ERR_NOT_FOUND : err_sys(err, "%s", path);
        goto out;
    }
    struct stat st;
    if (fstat(f, &st) != 0) {
        e = err_sys(err, "%s", path);
        close(f);
        goto out;
    }
    if (!S_ISREG(st.st_mode)) {
        close(f);
        e = ERR_NOT_FOUND;
        goto out;
    }
    describe(obj, key, &st);
    *fd = f;
out:
    xfree(path);
    return e;
}

[[nodiscard]] static Error disk_get(Store *s, Ctx *ctx, const char *key, Object *obj, int *fd, Err *err) { return open_object((Disk *)s, key, obj, fd, err); }

[[nodiscard]] static Error disk_head(Store *s, Ctx *ctx, const char *key, Object *obj, Err *err) {
    int fd;
    Error e = open_object((Disk *)s, key, obj, &fd, err);
    if (e != ERR_OK) return e;
    Sha256 h;
    sha256_init(&h);
    int64_t copied;
    e = copy_fd(fd, -1, &h, &copied, err);
    close(fd);
    if (e == ERR_OK) sha256_hex(&h, obj->sha256);
    return e;
}

[[nodiscard]] static Error disk_delete(Store *s, Ctx *ctx, const char *key, Err *err) {
    char *path = object_path((Disk *)s, key);
    Error e = ERR_OK;
    if (unlink(path) != 0 && errno != ENOENT && errno != ENOTDIR) e = err_sys(err, "%s", path);
    xfree(path);
    return e;
}

typedef struct {
    Arena *arena;
    Object *objects;
    size_t count, cap;
} Listing;

[[nodiscard]] static Error walk(Disk *d, Listing *l, const char *dir, const char *rel, Err *err) {
    struct dirent **entries;
    int n = scandir(dir, &entries, NULL, alphasort);
    if (n < 0) return err_sys(err, "%s", dir);
    Error e = ERR_OK;
    for (int i = 0; i < n; i++) {
        const char *name = entries[i]->d_name;
        if (e != ERR_OK || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char *path = path_join(dir, name);
        char *key = *rel ? path_join(rel, name) : xstrdup(name);
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                e = walk(d, l, path, key, err);
            } else if (S_ISREG(st.st_mode) && !has_prefix(name, DISK_TEMP_PREFIX)) {
                if (l->count == l->cap) l->objects = xrealloc(l->objects, (l->cap = l->cap ? l->cap * 2 : 64) * sizeof *l->objects);
                describe(&l->objects[l->count++], arena_strdup(l->arena, key), &st);
            }
        }
        xfree(key);
        xfree(path);
    }
    for (int i = 0; i < n; i++) xfree(entries[i]);
    xfree(entries);
    return e;
}

[[nodiscard]] static Error disk_list(Store *s, Ctx *ctx, Arena *a, Object **objects, size_t *count, Err *err) {
    Disk *d = (Disk *)s;
    Listing l = {.arena = a};
    struct stat st;
    Error e = ERR_OK;
    if (stat(d->dir, &st) == 0) e = walk(d, &l, d->dir, "", err);
    else if (errno != ENOENT) e = err_sys(err, "%s", d->dir);
    Object *out = arena_alloc(a, (l.count + 1) * sizeof *out);
    memcpy(out, l.objects, l.count * sizeof *out);
    xfree(l.objects);
    *objects = out;
    *count = l.count;
    return e;
}

static void disk_close(Store *s) {
    Disk *d = (Disk *)s;
    xfree(d->dir);
    xfree(d);
}

static const StoreOps disk_ops = {disk_put, disk_get, disk_head, disk_delete, disk_list, disk_close};

Store *disk_open(const char *root, const char *prefix) {
    Disk *d = xcalloc(1, sizeof *d);
    d->base.ops = &disk_ops;
    d->dir = *prefix ? path_join(root, prefix) : xstrdup(root);
    size_t n = strlen(d->dir);
    while (n > 1 && d->dir[n - 1] == '/') d->dir[--n] = '\0';
    return &d->base;
}
