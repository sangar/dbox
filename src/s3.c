/*
 * S3 stores objects in a bucket under a prefix, through libmc's S3 client.
 * Content hash and modification time ride along as object metadata. Uploads
 * above part_size go as multipart uploads, and downloads are staged in a temp
 * directory because callers want a readable descriptor.
 */
#include "mc/net/s3.h"

#include "mc/platform/platform.h"
#include "store.h"

#define META_SHA256 "sha256"
#define META_MTIME "mtime"

typedef struct {
    Store base;
    S3Client *client;
    String prefix, storage_class;
    String tmp_dir;
} S3;

static String full_key(Arena *arena, const S3 *s, String key) { return str_concat(arena, s->prefix, key); }

/* describe turns what S3 says about an object into an Object, the modification time from our metadata when it has one. */
static Object describe(String key, const S3Object *object) {
    String sha256 = S(""), mtime;
    int64_t mtime_ns = 0;
    (void)s3_metadata_get(object, S(META_SHA256), &sha256);
    if (!s3_metadata_get(object, S(META_MTIME), &mtime) || !str_parse_i64(mtime, &mtime_ns)) mtime_ns = 0;
    return (Object){
        .key = key,
        .size = object->size,
        .etag = object->etag,
        .sha256 = sha256,
        .mtime_ns = mtime_ns ? mtime_ns : object->last_modified_ns,
    };
}

[[nodiscard]] static Error s3_store_put(Store *store, Cancel *cancel, Arena *arena, String key, int fd, int64_t size, Meta meta, String *etag, Err *err) {
    S3 *s = (S3 *)store;
    HttpHeader metadata[2] = {{S(META_SHA256), meta.sha256}};
    size_t count = 1;
    if (meta.mtime_ns != 0) metadata[count++] = (HttpHeader){S(META_MTIME), str_format(arena, "%lld", (long long)meta.mtime_ns)};
    S3PutOptions options = {.metadata = {metadata, count}, .storage_class = s->storage_class};
    return s3_put_file(s->client, cancel, arena, full_key(arena, s, key), fd, size, &options, etag, err);
}

[[nodiscard]] static Error s3_store_get(Store *store, Cancel *cancel, Arena *arena, String key, Object *object, int *fd, Err *err) {
    S3 *s = (S3 *)store;
    int tmp;
    Error e = temp_file(arena, s->tmp_dir, &tmp, err);
    if (e != ERR_OK) return e;
    S3Object found;
    e = s3_get_file(s->client, cancel, arena, full_key(arena, s, key), tmp, &found, err);
    if (e == ERR_OK) e = file_seek(tmp, 0, err);
    if (e != ERR_OK) {
        file_close(tmp);
        return e;
    }
    *object = describe(key, &found);
    *fd = tmp;
    return ERR_OK;
}

[[nodiscard]] static Error s3_store_head(Store *store, Cancel *cancel, Arena *arena, String key, Object *object, Err *err) {
    S3 *s = (S3 *)store;
    S3Object found;
    Error e = s3_head(s->client, cancel, arena, full_key(arena, s, key), &found, err);
    if (e == ERR_OK) *object = describe(key, &found);
    return e;
}

[[nodiscard]] static Error s3_store_remove(Store *store, Cancel *cancel, Arena *arena, String key, Err *err) {
    S3 *s = (S3 *)store;
    return s3_delete(s->client, cancel, arena, full_key(arena, s, key), err);
}

[[nodiscard]] static Error s3_store_list(Store *store, Cancel *cancel, Arena *arena, ObjectList *objects, Err *err) {
    S3 *s = (S3 *)store;
    *objects = (ObjectList){0};
    S3ObjectList found;
    Error e = s3_list(s->client, cancel, arena, s->prefix, &found, err);
    if (e != ERR_OK) return e;
    for (size_t i = 0; i < found.count; i++) {
        const S3Object *item = &found.items[i];
        String key = str_trim_prefix(item->key, s->prefix);
        /* Folder markers that consoles create are not files. */
        if (key.len == 0 || str_ends_with(key, S("/"))) continue;
        objectlist_push(arena, objects, (Object){.key = key, .size = item->size, .etag = item->etag, .sha256 = S(""), .mtime_ns = item->last_modified_ns});
    }
    return ERR_OK;
}

static void s3_store_close(Store *store) { s3_client_destroy(((S3 *)store)->client); }

Error s3_store_create_bucket(Store *store, Cancel *cancel, Arena *scratch, Err *err) { return s3_create_bucket(((S3 *)store)->client, cancel, scratch, err); }

static const StoreOps s3_ops = {s3_store_put, s3_store_get, s3_store_head, s3_store_remove, s3_store_list, s3_store_close};

Error s3_open(Arena *arena, const StoreConfig *config, int64_t part_size, String tmp_dir, Store **store, Err *err) {
    *store = nullptr;
    S3Credentials credentials = {.access_key = config->access_key, .secret_key = config->secret_key, .session_token = S("")};
    if (config->access_key.len == 0) {
        Error e = s3_credentials_from_environment(arena, &credentials, err);
        if (e != ERR_OK) return err_set(err, ERR_INVALID_ARGUMENT, "no credentials: set access_key and secret_key, AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, or ~/.aws/credentials");
    }
    S3Config s3_config = {
        .endpoint = config->endpoint,
        .region = config->region,
        .bucket = config->bucket,
        .path_style = config->path_style,
        .credentials = credentials,
        .part_size = part_size,
        .user_agent = S("dbox"),
    };
    S3 *s = arena_push(arena, sizeof *s);
    s->base.ops = &s3_ops;
    s->prefix = str_copy(arena, config->prefix);
    s->storage_class = str_copy(arena, config->storage_class);
    s->tmp_dir = str_copy(arena, tmp_dir);
    Error e = s3_client_create(&s3_config, &s->client, err);
    if (e != ERR_OK) return e;
    *store = &s->base;
    return ERR_OK;
}
