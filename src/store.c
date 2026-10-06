#include "store.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Error store_open(Ctx *ctx, const StoreConfig *cfg, int64_t part_size, const char *tmp_dir, Store **out, Err *err) {
    *out = NULL;
    switch (cfg->kind) {
    case KIND_S3: return s3_open(ctx, cfg, part_size, tmp_dir, out, err);
    case KIND_DISK: *out = disk_open(cfg->root, cfg->prefix); return ERR_OK;
    default: return err_set(err, ERR_INVALID_ARGUMENT, "unknown store kind \"%s\"", kind_name(cfg->kind));
    }
}

Error store_open_all(Ctx *ctx, const Config *cfg, StoreSet *set, Err *err) {
    memset(set, 0, sizeof *set);
    set->names = xcalloc(cfg->store_count + 1, sizeof *set->names);
    set->stores = xcalloc(cfg->store_count + 1, sizeof *set->stores);
    for (size_t i = 0; i < cfg->store_count; i++) set->names[i] = cfg->stores[i].name;
    qsort(set->names, cfg->store_count, sizeof *set->names, compare_strings);
    char *state = config_state_dir(cfg);
    char *tmp = path_join(state, "tmp");
    xfree(state);
    Error e = ERR_OK;
    for (size_t i = 0; e == ERR_OK && i < cfg->store_count; i++) {
        Err inner;
        e = store_open(ctx, config_store(cfg, set->names[i]), cfg->sync.part_size, tmp, &set->stores[i], &inner);
        if (e != ERR_OK) e = err_set(err, e, "store %s: %s", set->names[i], inner.msg);
        set->count = i + 1;
    }
    xfree(tmp);
    if (e != ERR_OK) storeset_close(set);
    return e;
}

Store *storeset_get(const StoreSet *set, const char *name) {
    for (size_t i = 0; i < set->count; i++)
        if (strcmp(set->names[i], name) == 0) return set->stores[i];
    return NULL;
}

void storeset_close(StoreSet *set) {
    for (size_t i = 0; i < set->count; i++) store_close(set->stores[i]);
    xfree(set->names);
    xfree(set->stores);
    memset(set, 0, sizeof *set);
}

Error temp_file(const char *dir, int *fd, Err *err) {
    Error e = mkdir_p(dir, 0755, err);
    if (e != ERR_OK) return e;
    char *tmpl = path_join(dir, ".dbox-tmp-XXXXXX");
    *fd = mkstemp(tmpl);
    if (*fd < 0) e = err_sys(err, "%s", tmpl);
    else unlink(tmpl);
    xfree(tmpl);
    return e;
}

Error store_check(Ctx *ctx, Store *s, Err *err) {
    static const char content[] = "dbox probe";
    char key[64];
    snprintf(key, sizeof key, ".dbox-probe-%lld", (long long)wall_ns());
    Meta meta = {.mtime_ns = wall_ns()};
    sha256_of(content, sizeof content - 1, meta.sha256);
    const char *tmpdir = getenv("TMPDIR");
    int fd;
    Error e = temp_file(tmpdir && *tmpdir ? tmpdir : "/tmp", &fd, err);
    if (e != ERR_OK) return e;
    Err inner;
    char etag[ETAG_MAX];
    if (!write_all(fd, content, sizeof content - 1)) {
        e = err_sys(err, "write probe");
    } else if ((e = store_put(s, ctx, key, fd, (int64_t)sizeof content - 1, &meta, etag, &inner)) != ERR_OK) {
        e = err_set(err, e, "write: %s", inner.msg);
    } else {
        Object obj;
        int body;
        if ((e = store_get(s, ctx, key, &obj, &body, &inner)) != ERR_OK) {
            e = err_set(err, e, "read back: %s", inner.msg);
        } else {
            char got[64] = {0};
            ssize_t n = read(body, got, sizeof got - 1);
            close(body);
            if (n < 0) e = err_sys(err, "read back");
            else if (strcmp(got, content) != 0) e = err_set(err, ERR_REMOTE, "read back different content than was written");
            else if ((e = store_delete(s, ctx, key, &inner)) != ERR_OK) e = err_set(err, e, "delete: %s", inner.msg);
        }
    }
    close(fd);
    return e;
}
