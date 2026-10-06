#include "store.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Store *store_open(Ctx *ctx, const StoreConfig *cfg, int64_t part_size, const char *tmp_dir, Err *err) {
    switch (cfg->kind) {
    case KIND_S3: return s3_open(ctx, cfg, part_size, tmp_dir, err);
    case KIND_DISK: return disk_open(cfg->root, cfg->prefix);
    default: err_set(err, "unknown store kind \"%s\"", kind_name(cfg->kind)); return NULL;
    }
}

bool store_open_all(Ctx *ctx, const Config *cfg, StoreSet *set, Err *err) {
    memset(set, 0, sizeof *set);
    set->names = xcalloc(cfg->store_count + 1, sizeof *set->names);
    set->stores = xcalloc(cfg->store_count + 1, sizeof *set->stores);
    for (size_t i = 0; i < cfg->store_count; i++) set->names[i] = cfg->stores[i].name;
    qsort(set->names, cfg->store_count, sizeof *set->names, compare_strings);
    char *state = config_state_dir(cfg);
    char *tmp = path_join(state, "tmp");
    free(state);
    bool ok = true;
    for (size_t i = 0; ok && i < cfg->store_count; i++) {
        Err inner;
        set->stores[i] = store_open(ctx, config_store(cfg, set->names[i]), cfg->sync.part_size, tmp, &inner);
        if (!set->stores[i]) {
            err_set(err, "store %s: %s", set->names[i], inner.msg);
            ok = false;
        }
        set->count = i + 1;
    }
    free(tmp);
    if (!ok) storeset_close(set);
    return ok;
}

Store *storeset_get(const StoreSet *set, const char *name) {
    for (size_t i = 0; i < set->count; i++)
        if (strcmp(set->names[i], name) == 0) return set->stores[i];
    return NULL;
}

void storeset_close(StoreSet *set) {
    for (size_t i = 0; i < set->count; i++) store_close(set->stores[i]);
    free(set->names);
    free(set->stores);
    memset(set, 0, sizeof *set);
}

int temp_file(const char *dir, Err *err) {
    if (!mkdir_p(dir, 0755, err)) return -1;
    char *tmpl = path_join(dir, ".dbox-tmp-XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) err_sys(err, "%s", tmpl);
    else unlink(tmpl);
    free(tmpl);
    return fd;
}

bool store_check(Ctx *ctx, Store *s, Err *err) {
    static const char content[] = "dbox probe";
    char key[64];
    snprintf(key, sizeof key, ".dbox-probe-%lld", (long long)wall_ns());
    Meta meta = {.mtime_ns = wall_ns()};
    sha256_of(content, sizeof content - 1, meta.sha256);
    const char *tmpdir = getenv("TMPDIR");
    int fd = temp_file(tmpdir && *tmpdir ? tmpdir : "/tmp", err);
    if (fd < 0) return false;
    bool ok = false;
    Err inner;
    char etag[ETAG_MAX];
    if (!write_all(fd, content, sizeof content - 1)) {
        err_sys(err, "write probe");
    } else if (store_put(s, ctx, key, fd, (int64_t)sizeof content - 1, &meta, etag, &inner) != STORE_OK) {
        err_set(err, "write: %s", inner.msg);
    } else {
        Object obj;
        int body;
        if (store_get(s, ctx, key, &obj, &body, &inner) != STORE_OK) {
            err_set(err, "read back: %s", inner.msg);
        } else {
            char got[64] = {0};
            ssize_t n = read(body, got, sizeof got - 1);
            close(body);
            if (n < 0) err_sys(err, "read back");
            else if (strcmp(got, content) != 0) err_set(err, "read back different content than was written");
            else if (store_delete(s, ctx, key, &inner) != STORE_OK) err_set(err, "delete: %s", inner.msg);
            else ok = true;
        }
    }
    close(fd);
    return ok;
}
