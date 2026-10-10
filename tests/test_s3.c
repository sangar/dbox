/* The S3 store against the two docker-compose MinIOs; run `tools/minio.sh` first. */
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "http.h"
#include "store.h"

/* T is one test run: its counters, the shared cancellation token and the staging directory. */
typedef struct {
    int failures, checks;
    Arena *arena;
    Cancel cancel;
    String tmp_dir;
} T;

/* The check macros stay macros for __FILE__, __LINE__ and the expression text. */
#define CHECK(cond)                                                                                /* modern-c: allow function-macro */ \
    do {                                                                                           \
        t->checks++;                                                                               \
        if (!(cond)) {                                                                             \
            t->failures++;                                                                         \
            fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, __LINE__, __func__, #cond); \
        }                                                                                          \
    } while (0)

#define CHECK_STR(got, want)                                                                                                                      /* modern-c: allow function-macro */ \
    do {                                                                                                                                          \
        t->checks++;                                                                                                                              \
        String g_ = (got), w_ = (want);                                                                                                           \
        if (!str_equal(g_, w_)) {                                                                                                                 \
            t->failures++;                                                                                                                        \
            fprintf(stderr, "%s:%d: %s: got \"%.*s\", want \"%.*s\"\n", __FILE__, __LINE__, __func__, (int)g_.len, g_.data, (int)w_.len, w_.data); \
        }                                                                                                                                         \
    } while (0)

/* minio returns a store on the docker-compose MinIO under a fresh prefix. */
static Store *minio(T *t, const char *endpoint) {
    StoreConfig config = {
        .name = S("m"),
        .kind = KIND_S3,
        .role = ROLE_PRIMARY,
        .bucket = S("dbox"),
        .region = S("us-east-1"),
        .endpoint = S(endpoint),
        .path_style = true,
        .access_key = S("minioadmin"),
        .secret_key = S("minioadmin"),
        .storage_class = S(""),
        .prefix = str_format(t->arena, "test-%lld/", (long long)clock_wall_ns()),
    };
    Err err;
    Store *s;
    if (s3_open(t->arena, &config, 5 << 20, t->tmp_dir, &s, &err) != ERR_OK) {
        fprintf(stderr, "%s: %s\n", endpoint, err.msg);
        exit(1);
    }
    if (s3_create_bucket(s, &t->cancel, t->arena, &err) != ERR_OK) {
        fprintf(stderr, "%s: %s (run `tools/minio.sh`)\n", endpoint, err.msg);
        exit(1);
    }
    return s;
}

static int file_with(T *t, String content) {
    Err err;
    int fd = -1;
    CHECK(temp_file(t->arena, t->tmp_dir, &fd, &err) == ERR_OK);
    CHECK(file_write(fd, content, &err) == ERR_OK);
    return fd;
}

static String read_fd(T *t, int fd) {
    StringBuilder out = str_builder_create(t->arena, 64);
    char buffer[4096];
    size_t n;
    while (file_read(fd, buffer, sizeof buffer, &n, nullptr) == ERR_OK && n > 0) str_builder_append(&out, (String){buffer, n});
    return str_builder_finish(&out);
}

static void test_round_trip_keeps_metadata(T *t) {
    Store *s = minio(t, "http://localhost:9200");
    Err err;
    Error checked = store_check(&t->cancel, t->arena, s, &err);
    CHECK(checked == ERR_OK);
    if (checked != ERR_OK) fprintf(stderr, "check: %s\n", err.msg);

    String key = S("dir/a.txt");
    Meta meta = {.sha256 = S("abc"), .mtime_ns = 1700000000LL * NS_PER_SECOND + 123};
    int fd = file_with(t, S("hello"));
    String etag = S("");
    bool ok = store_put(s, &t->cancel, t->arena, key, fd, 5, meta, &etag, &err) == ERR_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "put: %s\n", err.msg);
    file_close(fd);

    Object head;
    CHECK(store_head(s, &t->cancel, t->arena, key, &head, &err) == ERR_OK);
    CHECK_STR(head.etag, etag);
    CHECK_STR(head.sha256, S("abc"));
    CHECK(head.mtime_ns == meta.mtime_ns);
    CHECK(head.size == 5);

    Object got;
    int body;
    CHECK(store_get(s, &t->cancel, t->arena, key, &got, &body, &err) == ERR_OK);
    CHECK_STR(read_fd(t, body), S("hello"));
    CHECK_STR(got.sha256, S("abc"));
    file_close(body);

    ObjectList list;
    CHECK(store_list(s, &t->cancel, t->arena, &list, &err) == ERR_OK);
    CHECK(list.count == 1);
    if (list.count == 1) {
        CHECK_STR(list.items[0].key, key);
        CHECK_STR(list.items[0].etag, etag);
        CHECK(list.items[0].size == 5);
    }

    CHECK(store_remove(s, &t->cancel, t->arena, key, &err) == ERR_OK);
    CHECK(store_head(s, &t->cancel, t->arena, key, &head, &err) == ERR_NOT_FOUND);
    CHECK(store_get(s, &t->cancel, t->arena, key, &got, &body, &err) == ERR_NOT_FOUND);
    CHECK(store_remove(s, &t->cancel, t->arena, key, &err) == ERR_OK);
}

static void test_multipart_copy_between_stores(T *t) {
    Store *a = minio(t, "http://localhost:9200"), *b = minio(t, "http://localhost:9300");
    const int64_t size = 12 << 20;
    String key = S("big.bin");
    Err err;
    int fd;
    CHECK(temp_file(t->arena, t->tmp_dir, &fd, &err) == ERR_OK);
    bool written = true;
    for (int64_t i = 0; written && i < size / 16; i++) written = file_write(fd, S("0123456789abcdef"), &err) == ERR_OK;
    CHECK(written);
    Meta meta = {.sha256 = S("big")};
    String etag = S("");
    bool ok = store_put(a, &t->cancel, t->arena, key, fd, size, meta, &etag, &err) == ERR_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "multipart put: %s\n", err.msg);
    file_close(fd);
    CHECK(str_contains(etag, S("-")));

    /* A mirror copy takes one store's Get descriptor straight into another's Put. */
    Object object;
    int body;
    CHECK(store_get(a, &t->cancel, t->arena, key, &object, &body, &err) == ERR_OK);
    CHECK(object.size == size);
    ok = store_put(b, &t->cancel, t->arena, key, body, size, meta, &etag, &err) == ERR_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "copy put: %s\n", err.msg);
    file_close(body);
    Object head;
    CHECK(store_head(b, &t->cancel, t->arena, key, &head, &err) == ERR_OK);
    CHECK(head.size == size);
    CHECK_STR(head.sha256, S("big"));
    CHECK(store_remove(a, &t->cancel, t->arena, key, &err) == ERR_OK);
    CHECK(store_remove(b, &t->cancel, t->arena, key, &err) == ERR_OK);
}

static void test_keys_with_special_characters(T *t) {
    Store *s = minio(t, "http://localhost:9200");
    Err err;
    String key = S("dir with spaces/caf\xc3\xa9 & co+plus=eq.txt");
    int fd = file_with(t, S("x"));
    Meta meta = {.sha256 = S("x")};
    String etag;
    bool ok = store_put(s, &t->cancel, t->arena, key, fd, 1, meta, &etag, &err) == ERR_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "put: %s\n", err.msg);
    file_close(fd);
    ObjectList list;
    CHECK(store_list(s, &t->cancel, t->arena, &list, &err) == ERR_OK);
    CHECK(list.count == 1);
    if (list.count == 1) CHECK_STR(list.items[0].key, key);
    CHECK(store_remove(s, &t->cancel, t->arena, key, &err) == ERR_OK);
}

int main(void) {
    T t = {.arena = arena_create(0)};
    http_global_init();
    cancel_init(&t.cancel);
    if (dir_create_temp(t.arena, S("dbox-s3-test-"), &t.tmp_dir, nullptr) != ERR_OK) return 1;
    test_round_trip_keeps_metadata(&t);
    test_multipart_copy_between_stores(&t);
    test_keys_with_special_characters(&t);
    (void)dir_remove_all(t.tmp_dir, nullptr);
    fprintf(stderr, "%d checks, %d failures\n", t.checks, t.failures);
    cancel_destroy(&t.cancel);
    arena_destroy(t.arena);
    return t.failures ? 1 : 0;
}
