#include "store.h"

#include "content.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

void objectlist_push(Arena *arena, ObjectList *list, Object object) {
    list->items = arena_grow(arena, list->items, &list->capacity, list->count, sizeof *list->items);
    list->items[list->count++] = object;
}

Error store_open(Arena *arena, const StoreConfig *config, int64_t part_size, String tmp_dir, Store **store, Err *err) {
    *store = nullptr;
    switch (config->kind) {
    case KIND_S3: return s3_open(arena, config, part_size, tmp_dir, store, err);
    case KIND_DISK: *store = disk_open(arena, config->root, config->prefix); return ERR_OK;
    default: return err_set(err, ERR_INVALID_ARGUMENT, "unknown store kind \"%s\"", kind_name(config->kind));
    }
}

Error store_open_all(Arena *arena, const Config *config, StoreSet *set, Err *err) {
    *set = (StoreSet){0};
    for (size_t i = 0; i < config->store_count; i++) strlist_push(arena, &set->names, config->stores[i].name);
    strlist_sort(&set->names);
    set->stores = arena_push(arena, arena_size_mul(set->names.count + 1, sizeof *set->stores));
    String tmp = path_join(arena, config_state_dir(arena, config), S("tmp"));
    for (size_t i = 0; i < set->names.count; i++) {
        String name = set->names.items[i];
        Err inner;
        Error e = store_open(arena, config_store(config, name), config->sync.part_size, tmp, &set->stores[i], &inner);
        if (e != ERR_OK) {
            storeset_close(set);
            return err_set(err, e, "store %.*s: %s", (int)name.len, name.data, inner.msg);
        }
    }
    return ERR_OK;
}

void storeset_close(StoreSet *set) {
    for (size_t i = 0; i < set->names.count; i++) {
        store_close(set->stores[i]);
        set->stores[i] = nullptr;
    }
}

Store *storeset_get(const StoreSet *set, String name) {
    for (size_t i = 0; i < set->names.count; i++)
        if (str_equal(set->names.items[i], name)) return set->stores[i];
    return nullptr;
}

Error temp_file(Arena *scratch, String dir, int *fd, Err *err) {
    Error e = dir_create_all(dir, 0755, err);
    if (e != ERR_OK) return e;
    String path;
    e = file_create_unique(scratch, path_join(scratch, dir, S(".dbox-tmp-XXXXXX")), &path, fd, err);
    if (e == ERR_OK) (void)file_remove(path, nullptr);
    return e;
}

Error store_check(Cancel *cancel, Arena *scratch, Store *store, Err *err) {
    String content = S("dbox probe");
    String key = str_format(scratch, ".dbox-probe-%lld", (long long)clock_wall_ns());
    Meta meta = {.sha256 = content_hash(scratch, content), .mtime_ns = clock_wall_ns()};
    String tmpdir;
    if (!env_get(scratch, S("TMPDIR"), &tmpdir) || tmpdir.len == 0) tmpdir = S("/tmp");
    int fd;
    Error e = temp_file(scratch, tmpdir, &fd, err);
    if (e != ERR_OK) return e;
    Err inner;
    String etag;
    if ((e = file_write(fd, content, &inner)) != ERR_OK) {
        e = err_set(err, e, "write probe: %s", inner.msg);
    } else if ((e = store_put(store, cancel, scratch, key, fd, (int64_t)content.len, meta, &etag, &inner)) != ERR_OK) {
        e = err_set(err, e, "write: %s", inner.msg);
    } else {
        Object object;
        int body;
        if ((e = store_get(store, cancel, scratch, key, &object, &body, &inner)) != ERR_OK) {
            e = err_set(err, e, "read back: %s", inner.msg);
        } else {
            char got[64] = {0};
            size_t n;
            e = file_read(body, got, sizeof got - 1, &n, &inner);
            file_close(body);
            if (e != ERR_OK) e = err_set(err, e, "read back: %s", inner.msg);
            else if (!str_equal((String){got, n}, content)) e = err_set(err, ERR_NETWORK, "read back different content than was written");
            else if ((e = store_remove(store, cancel, scratch, key, &inner)) != ERR_OK) e = err_set(err, e, "delete: %s", inner.msg);
        }
    }
    file_close(fd);
    return e;
}
