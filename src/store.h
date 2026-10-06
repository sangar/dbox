#ifndef DBOX_STORE_H
#define DBOX_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "config.h"
#include "ctx.h"
#include "sha256.h"
#include "util.h"

/*
 * A Store is the backend a folder is synced to: an S3-compatible bucket or a
 * directory. Keys are paths relative to the synced root with forward
 * slashes; each store applies its own prefix.
 */
#define ETAG_MAX 96

/* Object describes a stored file. sha256 is empty in List results from S3, which does not return metadata there; Head fills it in. */
typedef struct {
    const char *key;
    int64_t size;
    char etag[ETAG_MAX];
    char sha256[SHA256_HEX_LEN];
    int64_t mtime_ns; /* 0 when unknown */
} Object;

/* Meta travels with every Put so other machines and mirrors can compare content without downloading it. */
typedef struct {
    char sha256[SHA256_HEX_LEN];
    int64_t mtime_ns;
} Meta;

typedef struct Store Store;

/* Every operation returns ERR_OK, ERR_NOT_FOUND for a missing key, or the failure. */
typedef struct {
    /* put uploads size bytes from the start of the regular file fd. */
    Error (*put)(Store *s, Ctx *ctx, const char *key, int fd, int64_t size, const Meta *meta, char etag[ETAG_MAX], Err *err);
    /* get returns a descriptor positioned at the start of the content; the caller closes it. */
    Error (*get)(Store *s, Ctx *ctx, const char *key, Object *obj, int *fd, Err *err);
    Error (*head)(Store *s, Ctx *ctx, const char *key, Object *obj, Err *err);
    Error (*del)(Store *s, Ctx *ctx, const char *key, Err *err);
    /* list returns every object, allocated in a. */
    Error (*list)(Store *s, Ctx *ctx, Arena *a, Object **objects, size_t *count, Err *err);
    void (*close)(Store *s);
} StoreOps;

struct Store {
    const StoreOps *ops;
};

[[nodiscard]] static inline Error store_put(Store *s, Ctx *ctx, const char *key, int fd, int64_t size, const Meta *meta, char etag[ETAG_MAX], Err *err) {
    return s->ops->put(s, ctx, key, fd, size, meta, etag, err);
}
[[nodiscard]] static inline Error store_get(Store *s, Ctx *ctx, const char *key, Object *obj, int *fd, Err *err) { return s->ops->get(s, ctx, key, obj, fd, err); }
[[nodiscard]] static inline Error store_head(Store *s, Ctx *ctx, const char *key, Object *obj, Err *err) { return s->ops->head(s, ctx, key, obj, err); }
[[nodiscard]] static inline Error store_delete(Store *s, Ctx *ctx, const char *key, Err *err) { return s->ops->del(s, ctx, key, err); }
[[nodiscard]] static inline Error store_list(Store *s, Ctx *ctx, Arena *a, Object **objects, size_t *count, Err *err) {
    return s->ops->list(s, ctx, a, objects, count, err);
}
static inline void store_close(Store *s) {
    if (s) s->ops->close(s);
}

/* store_open builds the store described by cfg into *out; S3 stores stage downloads in tmp_dir. */
[[nodiscard]] Error store_open(Ctx *ctx, const StoreConfig *cfg, int64_t part_size, const char *tmp_dir, Store **out, Err *err);

/* StoreSet holds every configured store by name, in name order. */
typedef struct {
    size_t count;
    const char **names;
    Store **stores;
} StoreSet;

[[nodiscard]] Error store_open_all(Ctx *ctx, const Config *cfg, StoreSet *set, Err *err);
Store *storeset_get(const StoreSet *set, const char *name);
void storeset_close(StoreSet *set);

/* store_check writes, reads back and deletes a probe object. */
[[nodiscard]] Error store_check(Ctx *ctx, Store *s, Err *err);

/* s3_global_init readies the HTTP client; main calls it once before any thread starts. */
void s3_global_init(void);
Store *disk_open(const char *root, const char *prefix);
[[nodiscard]] Error s3_open(Ctx *ctx, const StoreConfig *cfg, int64_t part_size, const char *tmp_dir, Store **out, Err *err);
/* s3_create_bucket creates the bucket if it does not exist yet. */
[[nodiscard]] Error s3_create_bucket(Store *s, Ctx *ctx, Err *err);

/* temp_file creates an unlinked temporary file in dir, open for reading and writing, and stores its descriptor in *fd. */
[[nodiscard]] Error temp_file(const char *dir, int *fd, Err *err);

#endif
