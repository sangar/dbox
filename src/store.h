#pragma once

#include "config.h"
#include "mc/concurrency/cancel.h"

/*
 * A Store is the backend a folder is synced to: an S3-compatible bucket or a
 * directory. Keys are paths relative to the synced root with forward
 * slashes; each store applies its own prefix. Stores live in the arena they
 * were opened in; store_close releases what else they hold, such as an S3
 * store's connections.
 */

/* Object describes a stored file. sha256 is empty in List results from S3, which does not return metadata there; Head fills it in. */
typedef struct Object {
    String key;
    int64_t size;
    String etag;
    String sha256;
    int64_t mtime_ns; /* 0 when unknown */
} Object;

typedef struct ObjectList {
    Object *items;
    size_t count, capacity;
} ObjectList;

/* Meta travels with every Put so other machines and mirrors can compare content without downloading it. */
typedef struct Meta {
    String sha256;
    int64_t mtime_ns;
} Meta;

typedef struct Store Store;

/*
 * Every operation returns ERR_OK, ERR_NOT_FOUND for a missing key, or the
 * failure. Strings in what it hands back live in the arena it is given.
 */
typedef struct StoreOps {
    /* put uploads size bytes from the start of the regular file fd. */
    Error (*put)(Store *store, Cancel *cancel, Arena *arena, String key, int fd, int64_t size, Meta meta, String *etag, Err *err);
    /* get returns a descriptor positioned at the start of the content; the caller closes it. */
    Error (*get)(Store *store, Cancel *cancel, Arena *arena, String key, Object *object, int *fd, Err *err);
    Error (*head)(Store *store, Cancel *cancel, Arena *arena, String key, Object *object, Err *err);
    Error (*remove)(Store *store, Cancel *cancel, Arena *arena, String key, Err *err);
    Error (*list)(Store *store, Cancel *cancel, Arena *arena, ObjectList *objects, Err *err);
    /* close releases what the store holds besides its arena; nullptr when nothing. */
    void (*close)(Store *store);
} StoreOps;

struct Store {
    const StoreOps *ops;
};

[[nodiscard]] static inline Error store_put(Store *store, Cancel *cancel, Arena *arena, String key, int fd, int64_t size, Meta meta, String *etag, Err *err) {
    return store->ops->put(store, cancel, arena, key, fd, size, meta, etag, err);
}
[[nodiscard]] static inline Error store_get(Store *store, Cancel *cancel, Arena *arena, String key, Object *object, int *fd, Err *err) {
    return store->ops->get(store, cancel, arena, key, object, fd, err);
}
[[nodiscard]] static inline Error store_head(Store *store, Cancel *cancel, Arena *arena, String key, Object *object, Err *err) {
    return store->ops->head(store, cancel, arena, key, object, err);
}
[[nodiscard]] static inline Error store_remove(Store *store, Cancel *cancel, Arena *arena, String key, Err *err) {
    return store->ops->remove(store, cancel, arena, key, err);
}
[[nodiscard]] static inline Error store_list(Store *store, Cancel *cancel, Arena *arena, ObjectList *objects, Err *err) {
    return store->ops->list(store, cancel, arena, objects, err);
}

static inline void store_close(Store *store) {
    if (store && store->ops->close) store->ops->close(store);
}

void objectlist_push(Arena *arena, ObjectList *list, Object object);

/* store_open builds the store described by config in the arena; S3 stores stage downloads in tmp_dir. */
[[nodiscard]] Error store_open(Arena *arena, const StoreConfig *config, int64_t part_size, String tmp_dir, Store **store, Err *err);

/* StoreSet holds every configured store by name, in name order. */
typedef struct StoreSet {
    StringList names;
    Store **stores;
} StoreSet;

[[nodiscard]] Error store_open_all(Arena *arena, const Config *config, StoreSet *set, Err *err);
Store *storeset_get(const StoreSet *set, String name);
void storeset_close(StoreSet *set);

/* store_check writes, reads back and deletes a probe object. */
[[nodiscard]] Error store_check(Cancel *cancel, Arena *scratch, Store *store, Err *err);

Store *disk_open(Arena *arena, String root, String prefix);
[[nodiscard]] Error s3_open(Arena *arena, const StoreConfig *config, int64_t part_size, String tmp_dir, Store **store, Err *err);
/* s3_store_create_bucket creates an S3 store's bucket if it does not exist yet. */
[[nodiscard]] Error s3_store_create_bucket(Store *store, Cancel *cancel, Arena *scratch, Err *err);

/* temp_file creates an unlinked temporary file in dir, open for reading and writing. */
[[nodiscard]] Error temp_file(Arena *scratch, String dir, int *fd, Err *err);
