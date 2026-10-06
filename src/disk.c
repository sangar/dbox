#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "store.h"

#define DISK_TEMP_PREFIX ".dbox-upload-"

/* Disk stores objects as files under root/prefix. The ETag is size and modification time; the content hash is computed on Head. */
typedef struct {
    Store base;
    char *dir;
} Disk;

static char *object_path(Disk *d, const char *key) { return path_join(d->dir, key); }

static void describe(Object *obj, const char *key, const FileStat *st) {
    memset(obj, 0, sizeof *obj);
    obj->key = key;
    obj->size = st->size;
    obj->mtime_ns = st->mtime_ns;
    snprintf(obj->etag, sizeof obj->etag, "%lld-%lld", (long long)st->size, (long long)obj->mtime_ns);
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
    if ((e = file_mkstemp(tmp, &out, err)) != ERR_OK) goto out;
    if ((e = file_seek_start(fd, err)) != ERR_OK) goto out;
    if ((e = copy_fd(fd, out, NULL, &copied, err)) != ERR_OK) goto out;
    file_close(out);
    out = -1;
    if (meta->mtime_ns != 0 && (e = file_set_mtime(tmp, meta->mtime_ns, err)) != ERR_OK) goto out;
    if ((e = file_rename(tmp, target, err)) != ERR_OK) goto out;
    FileStat st;
    if ((e = file_info_follow(target, &st, err)) != ERR_OK) goto out;
    if (!st.exists) {
        e = err_set(err, ERR_IO, "%s: vanished after rename", target);
        goto out;
    }
    Object obj;
    describe(&obj, key, &st);
    memcpy(etag, obj.etag, ETAG_MAX);
out:
    if (out >= 0) file_close(out);
    if (e != ERR_OK && tmp) (void)file_remove(tmp, NULL);
    xfree(tmp);
    xfree(dir);
    xfree(target);
    return e;
}

[[nodiscard]] static Error open_object(Disk *d, const char *key, Object *obj, int *fd, Err *err) {
    char *path = object_path(d, key);
    int f;
    Error e = file_open_read(path, &f, err);
    if (e != ERR_OK) goto out;
    FileStat st;
    e = file_info_fd(f, &st, err);
    if (e == ERR_OK && !st.is_regular) e = ERR_NOT_FOUND;
    if (e != ERR_OK) {
        file_close(f);
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
    file_close(fd);
    if (e == ERR_OK) sha256_hex(&h, obj->sha256);
    return e;
}

[[nodiscard]] static Error disk_delete(Store *s, Ctx *ctx, const char *key, Err *err) {
    char *path = object_path((Disk *)s, key);
    Error e = file_remove(path, err);
    if (e == ERR_NOT_FOUND) e = ERR_OK;
    xfree(path);
    return e;
}

typedef struct {
    Arena *arena;
    Object *objects;
    size_t count, cap;
} Listing;

[[nodiscard]] static Error walk(Disk *d, Listing *l, const char *dir, const char *rel, Err *err) {
    StrList names = {0};
    Error e = dir_list(dir, &names, err);
    if (e == ERR_NOT_FOUND) e = err_set(err, e, "%s: no such directory", dir);
    for (size_t i = 0; e == ERR_OK && i < names.len; i++) {
        const char *name = names.items[i];
        char *path = path_join(dir, name);
        char *key = *rel ? path_join(rel, name) : xstrdup(name);
        FileStat st;
        if (file_info(path, &st, NULL) == ERR_OK && st.exists) {
            if (st.is_dir) {
                e = walk(d, l, path, key, err);
            } else if (st.is_regular && !has_prefix(name, DISK_TEMP_PREFIX)) {
                if (l->count == l->cap) l->objects = xrealloc(l->objects, (l->cap = l->cap ? l->cap * 2 : 64) * sizeof *l->objects);
                describe(&l->objects[l->count++], arena_strdup(l->arena, key), &st);
            }
        }
        xfree(key);
        xfree(path);
    }
    strlist_free(&names);
    return e;
}

[[nodiscard]] static Error disk_list(Store *s, Ctx *ctx, Arena *a, Object **objects, size_t *count, Err *err) {
    Disk *d = (Disk *)s;
    Listing l = {.arena = a};
    FileStat st;
    Error e = file_info_follow(d->dir, &st, err);
    if (e == ERR_OK && st.exists) e = walk(d, &l, d->dir, "", err);
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
