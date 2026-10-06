/* The S3 store against the two docker-compose MinIOs; run `make minio` first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "ctx.h"
#include "platform/platform.h"
#include "store.h"

/* T is one test run: its counters, the shared context and the staging directory. */
typedef struct {
    int failures, checks;
    Ctx ctx;
    char tmp_dir[sizeof "/tmp/dbox-s3-test-XXXXXX"];
} T;

/* The check macros stay macros for __FILE__, __LINE__ and the expression text. */
#define CHECK(cond)                                                                                      /* modern-c: allow function-macro */ \
    do {                                                                                                 \
        t->checks++;                                                                                        \
        if (!(cond)) {                                                                                   \
            t->failures++;                                                                                  \
            fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, __LINE__, __func__, #cond);       \
        }                                                                                                \
    } while (0)

#define CHECK_STR(got, want)                                                                                       /* modern-c: allow function-macro */ \
    do {                                                                                                           \
        t->checks++;                                                                                                  \
        const char *g_ = (got), *w_ = (want);                                                                      \
        if (strcmp(g_ ? g_ : "(null)", w_) != 0) {                                                                 \
            t->failures++;                                                                                            \
            fprintf(stderr, "%s:%d: %s: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, __func__, g_ ? g_ : "(null)", w_); \
        }                                                                                                          \
    } while (0)

/* minio returns a store on the docker-compose MinIO under a fresh prefix. */
static Store *minio(T *t, const char *endpoint) {
    Config cfg;
    config_init(&cfg);
    StoreConfig *sc = config_add_store(&cfg, "m");
    sc->kind = KIND_S3;
    sc->role = ROLE_PRIMARY;
    sc->bucket = "dbox";
    sc->region = "us-east-1";
    sc->endpoint = endpoint;
    sc->path_style = true;
    sc->access_key = "minioadmin";
    sc->secret_key = "minioadmin";
    sc->prefix = arena_printf(&cfg.arena, "test-%lld/", (long long)wall_ns());
    Err err;
    Store *s;
    if (s3_open(&t->ctx, sc, 5 << 20, t->tmp_dir, &s, &err) != ERR_OK) {
        fprintf(stderr, "%s: %s\n", endpoint, err.msg);
        exit(1);
    }
    if (s3_create_bucket(s, &t->ctx, &err) != ERR_OK) {
        fprintf(stderr, "%s: %s (run `make minio`)\n", endpoint, err.msg);
        exit(1);
    }
    config_free(&cfg);
    return s;
}

static int file_with(T *t, const char *content, size_t len) {
    Err err;
    int fd;
    CHECK(temp_file(t->tmp_dir, &fd, &err) == ERR_OK);
    CHECK(file_write_all(fd, content, len));
    return fd;
}

static char *read_fd(int fd) {
    StrBuf sb = {0};
    char buf[4096];
    size_t n;
    while (file_read(fd, buf, sizeof buf, &n, NULL) == ERR_OK && n > 0) sb_append(&sb, buf, n);
    sb_cstr(&sb);
    return sb.data;
}

static void test_round_trip_keeps_metadata(T *t) {
    Store *s = minio(t, "http://localhost:9200");
    Err err;
    Error checked = store_check(&t->ctx, s, &err);
    CHECK(checked == ERR_OK);
    if (checked != ERR_OK) fprintf(stderr, "check: %s\n", err.msg);

    Meta meta = {.sha256 = "abc", .mtime_ns = 1700000000LL * NS_PER_SEC + 123};
    int fd = file_with(t, "hello", 5);
    char etag[ETAG_MAX];
    bool ok = store_put(s, &t->ctx, "dir/a.txt", fd, 5, &meta, etag, &err) == ERR_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "put: %s\n", err.msg);
    file_close(fd);

    Object head;
    CHECK(store_head(s, &t->ctx, "dir/a.txt", &head, &err) == ERR_OK);
    CHECK_STR(head.etag, etag);
    CHECK_STR(head.sha256, "abc");
    CHECK(head.mtime_ns == meta.mtime_ns);
    CHECK(head.size == 5);

    Object got;
    int body;
    CHECK(store_get(s, &t->ctx, "dir/a.txt", &got, &body, &err) == ERR_OK);
    char *content = read_fd(body);
    CHECK_STR(content, "hello");
    CHECK_STR(got.sha256, "abc");
    xfree(content);
    file_close(body);

    Arena a;
    arena_init(&a, 4096);
    Object *list;
    size_t n;
    CHECK(store_list(s, &t->ctx, &a, &list, &n, &err) == ERR_OK);
    CHECK(n == 1);
    if (n == 1) {
        CHECK_STR(list[0].key, "dir/a.txt");
        CHECK_STR(list[0].etag, etag);
        CHECK(list[0].size == 5);
    }
    arena_free(&a);

    CHECK(store_delete(s, &t->ctx, "dir/a.txt", &err) == ERR_OK);
    CHECK(store_head(s, &t->ctx, "dir/a.txt", &head, &err) == ERR_NOT_FOUND);
    CHECK(store_get(s, &t->ctx, "dir/a.txt", &got, &body, &err) == ERR_NOT_FOUND);
    CHECK(store_delete(s, &t->ctx, "dir/a.txt", &err) == ERR_OK);
    store_close(s);
}

static void test_multipart_copy_between_stores(T *t) {
    Store *a = minio(t, "http://localhost:9200"), *b = minio(t, "http://localhost:9300");
    const int64_t size = 12 << 20;
    Err err;
    int fd;
    CHECK(temp_file(t->tmp_dir, &fd, &err) == ERR_OK);
    static const char chunk[] = "0123456789abcdef";
    bool written = true;
    for (int64_t i = 0; written && i < size / 16; i++) written = file_write_all(fd, chunk, 16);
    CHECK(written);
    Meta meta = {.sha256 = "big"};
    char etag[ETAG_MAX];
    bool ok = store_put(a, &t->ctx, "big.bin", fd, size, &meta, etag, &err) == ERR_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "multipart put: %s\n", err.msg);
    file_close(fd);
    CHECK(strchr(etag, '-') != NULL);

    /* A mirror copy takes one store's Get descriptor straight into another's Put. */
    Object obj;
    int body;
    CHECK(store_get(a, &t->ctx, "big.bin", &obj, &body, &err) == ERR_OK);
    CHECK(obj.size == size);
    ok = store_put(b, &t->ctx, "big.bin", body, size, &meta, etag, &err) == ERR_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "copy put: %s\n", err.msg);
    file_close(body);
    Object head;
    CHECK(store_head(b, &t->ctx, "big.bin", &head, &err) == ERR_OK);
    CHECK(head.size == size);
    CHECK_STR(head.sha256, "big");
    CHECK(store_delete(a, &t->ctx, "big.bin", &err) == ERR_OK);
    CHECK(store_delete(b, &t->ctx, "big.bin", &err) == ERR_OK);
    store_close(a);
    store_close(b);
}

static void test_keys_with_special_characters(T *t) {
    Store *s = minio(t, "http://localhost:9200");
    Err err;
    const char *key = "dir with spaces/caf\xc3\xa9 & co+plus=eq.txt";
    int fd = file_with(t, "x", 1);
    Meta meta = {.sha256 = "x"};
    char etag[ETAG_MAX];
    bool ok = store_put(s, &t->ctx, key, fd, 1, &meta, etag, &err) == ERR_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "put: %s\n", err.msg);
    file_close(fd);
    Arena a;
    arena_init(&a, 4096);
    Object *list;
    size_t n;
    CHECK(store_list(s, &t->ctx, &a, &list, &n, &err) == ERR_OK);
    CHECK(n == 1);
    if (n == 1) CHECK_STR(list[0].key, key);
    arena_free(&a);
    CHECK(store_delete(s, &t->ctx, key, &err) == ERR_OK);
    store_close(s);
}

int main(void) {
    T t = {.tmp_dir = "/tmp/dbox-s3-test-XXXXXX"};
    s3_global_init();
    ctx_init(&t.ctx);
    if (dir_make_temp(t.tmp_dir, NULL) != ERR_OK) return 1;
    test_round_trip_keeps_metadata(&t);
    test_multipart_copy_between_stores(&t);
    test_keys_with_special_characters(&t);
    (void)dir_remove(t.tmp_dir, NULL);
    fprintf(stderr, "%d checks, %d failures\n", t.checks, t.failures);
    return t.failures ? 1 : 0;
}
