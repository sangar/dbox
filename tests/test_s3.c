/* The S3 store against the two docker-compose MinIOs; run `make minio` first. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "ctx.h"
#include "store.h"

static int failures, checks;

/* The check macros stay macros for __FILE__, __LINE__ and the expression text. */
#define CHECK(cond)                                                                                      /* modern-c: allow function-macro */ \
    do {                                                                                                 \
        checks++;                                                                                        \
        if (!(cond)) {                                                                                   \
            failures++;                                                                                  \
            fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, __LINE__, __func__, #cond);       \
        }                                                                                                \
    } while (0)

#define CHECK_STR(got, want)                                                                                       /* modern-c: allow function-macro */ \
    do {                                                                                                           \
        checks++;                                                                                                  \
        const char *g_ = (got), *w_ = (want);                                                                      \
        if (strcmp(g_ ? g_ : "(null)", w_) != 0) {                                                                 \
            failures++;                                                                                            \
            fprintf(stderr, "%s:%d: %s: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, __func__, g_ ? g_ : "(null)", w_); \
        }                                                                                                          \
    } while (0)

static Ctx ctx;
static char tmp_dir[] = "/tmp/dbox-s3-test-XXXXXX";

/* minio returns a store on the docker-compose MinIO under a fresh prefix. */
static Store *minio(const char *endpoint) {
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
    Store *s = s3_open(&ctx, sc, 5 << 20, tmp_dir, &err);
    if (!s) {
        fprintf(stderr, "%s: %s\n", endpoint, err.msg);
        exit(1);
    }
    if (!s3_create_bucket(s, &ctx, &err)) {
        fprintf(stderr, "%s: %s (run `make minio`)\n", endpoint, err.msg);
        exit(1);
    }
    config_free(&cfg);
    return s;
}

static int file_with(const char *content, size_t len) {
    Err err;
    int fd = temp_file(tmp_dir, &err);
    CHECK(fd >= 0);
    CHECK(write_all(fd, content, len));
    return fd;
}

static char *read_fd(int fd) {
    StrBuf sb = {0};
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) sb_append(&sb, buf, (size_t)n);
    sb_cstr(&sb);
    return sb.data;
}

static void test_round_trip_keeps_metadata(void) {
    Store *s = minio("http://localhost:9200");
    Err err;
    bool ok = store_check(&ctx, s, &err);
    CHECK(ok);
    if (!ok) fprintf(stderr, "check: %s\n", err.msg);

    Meta meta = {.sha256 = "abc", .mtime_ns = 1700000000LL * NS_PER_SEC + 123};
    int fd = file_with("hello", 5);
    char etag[ETAG_MAX];
    ok = store_put(s, &ctx, "dir/a.txt", fd, 5, &meta, etag, &err) == STORE_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "put: %s\n", err.msg);
    close(fd);

    Object head;
    CHECK(store_head(s, &ctx, "dir/a.txt", &head, &err) == STORE_OK);
    CHECK_STR(head.etag, etag);
    CHECK_STR(head.sha256, "abc");
    CHECK(head.mtime_ns == meta.mtime_ns);
    CHECK(head.size == 5);

    Object got;
    int body;
    CHECK(store_get(s, &ctx, "dir/a.txt", &got, &body, &err) == STORE_OK);
    char *content = read_fd(body);
    CHECK_STR(content, "hello");
    CHECK_STR(got.sha256, "abc");
    free(content);
    close(body);

    Arena a;
    arena_init(&a, 4096);
    Object *list;
    size_t n;
    CHECK(store_list(s, &ctx, &a, &list, &n, &err) == STORE_OK);
    CHECK(n == 1);
    if (n == 1) {
        CHECK_STR(list[0].key, "dir/a.txt");
        CHECK_STR(list[0].etag, etag);
        CHECK(list[0].size == 5);
    }
    arena_free(&a);

    CHECK(store_delete(s, &ctx, "dir/a.txt", &err) == STORE_OK);
    CHECK(store_head(s, &ctx, "dir/a.txt", &head, &err) == STORE_NOT_FOUND);
    CHECK(store_get(s, &ctx, "dir/a.txt", &got, &body, &err) == STORE_NOT_FOUND);
    CHECK(store_delete(s, &ctx, "dir/a.txt", &err) == STORE_OK);
    store_close(s);
}

static void test_multipart_copy_between_stores(void) {
    Store *a = minio("http://localhost:9200"), *b = minio("http://localhost:9300");
    const int64_t size = 12 << 20;
    Err err;
    int fd = temp_file(tmp_dir, &err);
    static const char chunk[] = "0123456789abcdef";
    bool written = true;
    for (int64_t i = 0; written && i < size / 16; i++) written = write_all(fd, chunk, 16);
    CHECK(written);
    Meta meta = {.sha256 = "big"};
    char etag[ETAG_MAX];
    bool ok = store_put(a, &ctx, "big.bin", fd, size, &meta, etag, &err) == STORE_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "multipart put: %s\n", err.msg);
    close(fd);
    CHECK(strchr(etag, '-') != NULL);

    /* A mirror copy takes one store's Get descriptor straight into another's Put. */
    Object obj;
    int body;
    CHECK(store_get(a, &ctx, "big.bin", &obj, &body, &err) == STORE_OK);
    CHECK(obj.size == size);
    ok = store_put(b, &ctx, "big.bin", body, size, &meta, etag, &err) == STORE_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "copy put: %s\n", err.msg);
    close(body);
    Object head;
    CHECK(store_head(b, &ctx, "big.bin", &head, &err) == STORE_OK);
    CHECK(head.size == size);
    CHECK_STR(head.sha256, "big");
    CHECK(store_delete(a, &ctx, "big.bin", &err) == STORE_OK);
    CHECK(store_delete(b, &ctx, "big.bin", &err) == STORE_OK);
    store_close(a);
    store_close(b);
}

static void test_keys_with_special_characters(void) {
    Store *s = minio("http://localhost:9200");
    Err err;
    const char *key = "dir with spaces/caf\xc3\xa9 & co+plus=eq.txt";
    int fd = file_with("x", 1);
    Meta meta = {.sha256 = "x"};
    char etag[ETAG_MAX];
    bool ok = store_put(s, &ctx, key, fd, 1, &meta, etag, &err) == STORE_OK;
    CHECK(ok);
    if (!ok) fprintf(stderr, "put: %s\n", err.msg);
    close(fd);
    Arena a;
    arena_init(&a, 4096);
    Object *list;
    size_t n;
    CHECK(store_list(s, &ctx, &a, &list, &n, &err) == STORE_OK);
    CHECK(n == 1);
    if (n == 1) CHECK_STR(list[0].key, key);
    arena_free(&a);
    CHECK(store_delete(s, &ctx, key, &err) == STORE_OK);
    store_close(s);
}

int main(void) {
    ctx_init(&ctx);
    if (!mkdtemp(tmp_dir)) return 1;
    test_round_trip_keeps_metadata();
    test_multipart_copy_between_stores();
    test_keys_with_special_characters();
    rmdir(tmp_dir);
    fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
