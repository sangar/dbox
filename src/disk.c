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

static StoreStatus disk_put(Store *s, Ctx *ctx, const char *key, int fd, int64_t size, const Meta *meta, char etag[ETAG_MAX], Err *err) {
    Disk *d = (Disk *)s;
    char *target = object_path(d, key);
    char *dir = path_dir(target);
    StoreStatus status = STORE_ERROR;
    char *tmp = NULL;
    int out = -1;
    if (!mkdir_p(dir, 0755, err)) goto out;
    tmp = path_join(dir, DISK_TEMP_PREFIX "XXXXXX");
    out = mkstemp(tmp);
    if (out < 0) {
        err_sys(err, "%s", tmp);
        goto out;
    }
    if (lseek(fd, 0, SEEK_SET) < 0 || copy_fd(fd, out, NULL, err) < 0) goto out;
    close(out);
    out = -1;
    if (meta->mtime_ns != 0 && !set_mtime_ns(tmp, meta->mtime_ns)) {
        err_sys(err, "set mtime %s", tmp);
        goto out;
    }
    if (rename(tmp, target) != 0) {
        err_sys(err, "rename %s", target);
        goto out;
    }
    struct stat st;
    if (stat(target, &st) != 0) {
        err_sys(err, "%s", target);
        goto out;
    }
    Object obj;
    describe(&obj, key, &st);
    memcpy(etag, obj.etag, ETAG_MAX);
    status = STORE_OK;
out:
    if (out >= 0) close(out);
    if (status != STORE_OK && tmp) unlink(tmp);
    free(tmp);
    free(dir);
    free(target);
    return status;
}

static StoreStatus open_object(Disk *d, const char *key, Object *obj, int *fd, Err *err) {
    char *path = object_path(d, key);
    int f = open(path, O_RDONLY | O_CLOEXEC);
    StoreStatus status = STORE_ERROR;
    if (f < 0) {
        if (errno == ENOENT || errno == ENOTDIR) status = STORE_NOT_FOUND;
        else err_sys(err, "%s", path);
        goto out;
    }
    struct stat st;
    if (fstat(f, &st) != 0) {
        err_sys(err, "%s", path);
        close(f);
        goto out;
    }
    if (!S_ISREG(st.st_mode)) {
        close(f);
        status = STORE_NOT_FOUND;
        goto out;
    }
    describe(obj, key, &st);
    *fd = f;
    status = STORE_OK;
out:
    free(path);
    return status;
}

static StoreStatus disk_get(Store *s, Ctx *ctx, const char *key, Object *obj, int *fd, Err *err) { return open_object((Disk *)s, key, obj, fd, err); }

static StoreStatus disk_head(Store *s, Ctx *ctx, const char *key, Object *obj, Err *err) {
    int fd;
    StoreStatus status = open_object((Disk *)s, key, obj, &fd, err);
    if (status != STORE_OK) return status;
    Sha256 h;
    sha256_init(&h);
    bool ok = copy_fd(fd, -1, &h, err) >= 0;
    close(fd);
    if (!ok) return STORE_ERROR;
    sha256_hex(&h, obj->sha256);
    return STORE_OK;
}

static StoreStatus disk_delete(Store *s, Ctx *ctx, const char *key, Err *err) {
    char *path = object_path((Disk *)s, key);
    StoreStatus status = STORE_OK;
    if (unlink(path) != 0 && errno != ENOENT && errno != ENOTDIR) {
        err_sys(err, "%s", path);
        status = STORE_ERROR;
    }
    free(path);
    return status;
}

typedef struct {
    Arena *arena;
    Object *objects;
    size_t count, cap;
} Listing;

static bool walk(Disk *d, Listing *l, const char *dir, const char *rel, Err *err) {
    struct dirent **entries;
    int n = scandir(dir, &entries, NULL, alphasort);
    if (n < 0) {
        err_sys(err, "%s", dir);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < n; i++) {
        const char *name = entries[i]->d_name;
        if (!ok || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char *path = path_join(dir, name);
        char *key = *rel ? path_join(rel, name) : xstrdup(name);
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                ok = walk(d, l, path, key, err);
            } else if (S_ISREG(st.st_mode) && !has_prefix(name, DISK_TEMP_PREFIX)) {
                if (l->count == l->cap) l->objects = xrealloc(l->objects, (l->cap = l->cap ? l->cap * 2 : 64) * sizeof *l->objects);
                describe(&l->objects[l->count++], arena_strdup(l->arena, key), &st);
            }
        }
        free(key);
        free(path);
    }
    for (int i = 0; i < n; i++) free(entries[i]);
    free(entries);
    return ok;
}

static StoreStatus disk_list(Store *s, Ctx *ctx, Arena *a, Object **objects, size_t *count, Err *err) {
    Disk *d = (Disk *)s;
    Listing l = {.arena = a};
    struct stat st;
    bool ok = true;
    if (stat(d->dir, &st) == 0) ok = walk(d, &l, d->dir, "", err);
    else if (errno != ENOENT) {
        err_sys(err, "%s", d->dir);
        ok = false;
    }
    Object *out = arena_alloc(a, (l.count + 1) * sizeof *out);
    memcpy(out, l.objects, l.count * sizeof *out);
    free(l.objects);
    *objects = out;
    *count = l.count;
    return ok ? STORE_OK : STORE_ERROR;
}

static void disk_close(Store *s) {
    Disk *d = (Disk *)s;
    free(d->dir);
    free(d);
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
