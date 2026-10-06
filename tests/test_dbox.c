#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "config.h"
#include "ctx.h"
#include "daemon.h"
#include "debounce.h"
#include "engine.h"
#include "ignore.h"
#include "index.h"
#include "log.h"
#include "service.h"
#include "store.h"

/* T is one test run: its counters, scratch directory and the fakes tests share. */
typedef struct {
    int failures, checks;
    char scratch[sizeof "/tmp/dbox-test-XXXXXX"];
    int scratch_count;
    Ctx background;
    Logger discard, verbose;
    StrList commands;
    bool loaded;
} T;

/* The check macros stay macros for __FILE__, __LINE__ and the expression text. */
#define CHECK(cond)                                                                                /* modern-c: allow function-macro */ \
    do {                                                                                           \
        t->checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            t->failures++;                                                                            \
            fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, __LINE__, __func__, #cond); \
        }                                                                                          \
    } while (0)

#define CHECK_STR(got, want)                                                                                                 /* modern-c: allow function-macro */ \
    do {                                                                                                                     \
        t->checks++;                                                                                                            \
        const char *g_ = (got), *w_ = (want);                                                                                \
        if (strcmp(g_ ? g_ : "(null)", w_) != 0) {                                                                           \
            t->failures++;                                                                                                      \
            fprintf(stderr, "%s:%d: %s: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, __func__, g_ ? g_ : "(null)", w_); \
        }                                                                                                                    \
    } while (0)

#define CHECK_OK(call)                                                                                   /* modern-c: allow function-macro */ \
    do {                                                                                                 \
        Err e_;                                                                                          \
        e_.msg[0] = '\0';                                                                                \
        t->checks++;                                                                                        \
        if (!(call)) {                                                                                   \
            t->failures++;                                                                                  \
            fprintf(stderr, "%s:%d: %s: %s failed: %s\n", __FILE__, __LINE__, __func__, #call, e_.msg); \
        }                                                                                                \
    } while (0)

/* ---- t->scratch files ---- */

static char *temp_dir(T *t) {
    char *dir = xmalloc(strlen(t->scratch) + 16);
    snprintf(dir, strlen(t->scratch) + 16, "%s/d%d", t->scratch, t->scratch_count++);
    mkdir(dir, 0755);
    return dir;
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) { return remove(path); }

static void remove_tree(const char *path) { nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS); }

static void write_text(T *t, const char *path, const char *content) {
    char *dir = path_dir(path);
    Err err;
    CHECK(mkdir_p(dir, 0755, &err));
    xfree(dir);
    CHECK(write_file(path, content, strlen(content), 0644, &err));
}

/* read_text returns the file's content, or "<missing>". The result is malloc'd. */
static char *read_text(const char *path) {
    StrBuf sb = {0};
    Err err;
    if (!read_file(path, &sb, &err)) {
        sb_free(&sb);
        return xstrdup("<missing>");
    }
    return sb.data ? sb.data : xstrdup("");
}

static void expect_file(T *t, const char *path, const char *want) {
    char *got = read_text(path);
    t->checks++;
    if (strcmp(got, want) != 0) {
        t->failures++;
        fprintf(stderr, "%s = \"%s\", want \"%s\"\n", path, got, want);
    }
    xfree(got);
}

static char *join(const char *a, const char *b) { return path_join(a, b); }

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

static bool eventually(bool (*ok)(void *), void *arg) {
    for (int64_t deadline = monotonic_ns() + 5 * NS_PER_SEC; monotonic_ns() < deadline; sleep_ms(20))
        if (ok(arg)) return true;
    return false;
}

/* ---- config ---- */

static bool load(const char *yaml, char *const *env, Config *cfg, Err *err) { return config_parse("config.yaml", yaml, strlen(yaml), env, cfg, err); }

static void test_config_loads_stores_with_defaults_and_expanded_secrets(T *t) {
    char *nas = temp_dir(t);
    StrBuf yaml = {0};
    sb_printf(&yaml,
              "stores:\n"
              "  minio: {kind: s3, role: primary, bucket: dbox, region: us-east-1, prefix: me, secret_key: \"${SECRET}\"}\n"
              "  nas: {kind: disk, role: mirror, root: %s}\n"
              "sync: {root: /tmp/box}\n",
              nas);
    char *env[] = {"SECRET=s3cr3t", NULL};
    Config cfg;
    Err err;
    bool ok = load(yaml.data, env, &cfg, &err);
    CHECK(ok);
    if (!ok) {
        fprintf(stderr, "load: %s\n", err.msg);
        goto out;
    }
    StoreConfig *minio = config_store(&cfg, "minio");
    CHECK_STR(minio->secret_key, "s3cr3t");
    CHECK_STR(minio->prefix, "me/");
    CHECK(minio->workers == 4);
    CHECK(config_store(&cfg, "nas")->workers == 2);
    CHECK_STR(config_primary(&cfg), "minio");
    Arena a;
    arena_init(&a, 1024);
    size_t n;
    const char **mirrors = config_mirrors(&cfg, &a, &n);
    CHECK(n == 1 && strcmp(mirrors[0], "nas") == 0);
    arena_free(&a);
    CHECK(cfg.sync.pull_interval_ns == 30 * NS_PER_SEC);
    CHECK(cfg.sync.part_size == 8 << 20);
    CHECK(cfg.sync.delete_remote);
    config_free(&cfg);
out:
    sb_free(&yaml);
    xfree(nas);
}

static void test_config_references_expand_in_values_but_not_comments(T *t) {
    char *env[] = {"N=3", NULL};
    Config cfg;
    Err err;
    bool ok = load("# secret_key: ${UNSET}\nstores:\n  a: {kind: s3, role: primary, bucket: b, region: r, workers: \"${N}\"}\nsync: {root: /tmp/box}\n", env, &cfg, &err);
    CHECK(ok);
    if (ok) {
        CHECK(config_store(&cfg, "a")->workers == 3);
        config_free(&cfg);
    } else {
        fprintf(stderr, "load: %s\n", err.msg);
    }
}

static void test_config_environment_overrides_nested_keys(T *t) {
    char *env[] = {"DBOX_SYNC__ROOT=/b", "DBOX_SYNC__DELETE_LOCAL=false", NULL};
    Config cfg;
    Err err;
    bool ok = load("sync: {root: /a}\n", env, &cfg, &err);
    CHECK(ok);
    if (ok) {
        CHECK_STR(cfg.sync.root, "/b");
        CHECK(!cfg.sync.delete_local);
        config_free(&cfg);
    }
}

static void test_config_rejects_invalid_configs(T *t) {
    static const struct {
        const char *name, *yaml;
    } cases[] = {
        {"two primaries", "stores: {a: {kind: s3, bucket: b, region: r, role: primary}, b: {kind: s3, bucket: b, region: r, role: primary}}"},
        {"no primary", "stores: {a: {kind: s3, bucket: b, region: r, role: mirror}}"},
        {"bad name", "stores: {Big: {kind: s3, bucket: b, region: r, role: primary}}"},
        {"unknown role", "stores: {a: {kind: s3, bucket: b, region: r, role: backup}}"},
        {"s3 lacks bucket", "stores: {a: {kind: s3, region: r, role: primary}}"},
        {"missing disk", "stores: {a: {kind: disk, root: /does/not/exist, role: primary}}"},
        {"missing env", "stores: {a: {kind: s3, bucket: b, region: r, role: primary, secret_key: '${NOPE}'}}"},
        {"unknown key", "sync: {rooot: /x}"},
        {"bad duration", "sync: {pull_interval: soon}"},
        {"invalid yaml", "sync: [unclosed"},
    };
    for (size_t i = 0; i < countof(cases); i++) {
        Config cfg;
        Err err;
        bool ok = load(cases[i].yaml, NULL, &cfg, &err);
        t->checks++;
        if (ok) {
            t->failures++;
            fprintf(stderr, "%s: loaded without error\n", cases[i].name);
            config_free(&cfg);
        }
    }
}

static void test_config_no_stores_is_accepted(T *t) {
    Config cfg;
    Err err;
    bool ok = load("", NULL, &cfg, &err);
    CHECK(ok);
    if (ok) {
        CHECK(config_primary(&cfg) == NULL);
        CHECK_STR(cfg.daemon.listen, "127.0.0.1:7878");
        config_free(&cfg);
    }
}

static void test_config_promote_swaps_roles_and_keeps_comments(T *t) {
    char *dir = temp_dir(t);
    char *path = join(dir, "config.yaml");
    write_text(t, path,
               "# my stores\n"
               "stores:\n"
               "  minio:\n"
               "    kind: s3\n"
               "    role: primary # home server\n"
               "    bucket: dbox\n"
               "    region: us-east-1\n"
               "  r2:\n"
               "    kind: s3\n"
               "    role: mirror\n"
               "    bucket: dbox\n"
               "    region: auto\n"
               "  old:\n"
               "    kind: s3\n"
               "    role: detached\n"
               "    bucket: dbox\n"
               "    region: auto\n");
    Err err;
    CHECK(config_promote(path, "r2", &err));
    Config cfg;
    bool ok = config_load(path, &cfg, &err);
    CHECK(ok);
    if (ok) {
        CHECK(config_store(&cfg, "r2")->role == ROLE_PRIMARY);
        CHECK(config_store(&cfg, "minio")->role == ROLE_MIRROR);
        CHECK(config_store(&cfg, "old")->role == ROLE_DETACHED);
        config_free(&cfg);
    }
    char *raw = read_text(path);
    CHECK(strstr(raw, "# my stores") && strstr(raw, "# home server"));
    xfree(raw);
    CHECK(!config_promote(path, "nope", &err));
    xfree(path);
    xfree(dir);
}

static void test_config_default_path_honours_environment(T *t) {
    setenv("DBOX_CONFIG", "/elsewhere/dbox.yaml", 1);
    char *got = config_default_path();
    CHECK_STR(got, "/elsewhere/dbox.yaml");
    xfree(got);
    unsetenv("DBOX_CONFIG");

    char *dir = temp_dir(t);
    setenv("XDG_CONFIG_HOME", dir, 1);
    char *yml = join(dir, "dbox/config.yml"), *yaml = join(dir, "dbox/config.yaml");
    got = config_default_path();
    CHECK_STR(got, yml);
    xfree(got);
    write_text(t, yaml, "");
    got = config_default_path();
    CHECK_STR(got, yaml);
    xfree(got);
    unsetenv("XDG_CONFIG_HOME");
    xfree(yml);
    xfree(yaml);
    xfree(dir);
}

static void test_config_starter_loads_and_renders_without_secrets(T *t) {
    char *dir = temp_dir(t);
    char *path = join(dir, "config.yml");
    bool created;
    Err err;
    CHECK(config_write_starter(path, &created, &err) && created);
    Config cfg;
    bool ok = config_load(path, &cfg, &err);
    CHECK(ok);
    if (!ok) {
        fprintf(stderr, "starter: %s\n", err.msg);
        goto out;
    }
    StoreConfig *s = config_add_store(&cfg, "a");
    s->kind = KIND_S3;
    s->role = ROLE_PRIMARY;
    s->bucket = "b";
    s->region = "r";
    s->secret_key = "s3cr3t";
    s->workers = 4;
    StrBuf out = {0};
    config_render(&cfg, &out);
    CHECK(!strstr(out.data, "s3cr3t"));
    CHECK(strstr(out.data, "part_size: 8MiB") != NULL);
    CHECK(strstr(out.data, "secret_key: '***'") != NULL);
    Config again;
    ok = config_parse(path, out.data, out.len, NULL, &again, &err);
    CHECK(ok);
    if (ok) {
        CHECK_STR(config_store(&again, "a")->secret_key, "***");
        CHECK(again.sync.ignore_count == 5);
        config_free(&again);
    } else {
        fprintf(stderr, "rendered config does not load: %s\n", err.msg);
    }
    sb_free(&out);
    config_free(&cfg);
out:
    xfree(path);
    xfree(dir);
}

static void test_durations_and_sizes(T *t) {
    char buf[48];
    CHECK_STR(format_duration(30 * NS_PER_SEC, buf), "30s");
    CHECK_STR(format_duration(10 * 60 * NS_PER_SEC, buf), "10m0s");
    CHECK_STR(format_duration(750 * NS_PER_MS, buf), "750ms");
    CHECK_STR(format_duration(90 * NS_PER_SEC + 500 * NS_PER_MS, buf), "1m30.5s");
    int64_t ns;
    CHECK(parse_duration("1m30s", &ns) && ns == 90 * NS_PER_SEC);
    CHECK(parse_duration("1.5s", &ns) && ns == 1500 * NS_PER_MS);
    CHECK(!parse_duration("5", &ns));
    Err err;
    int64_t size;
    CHECK(parse_size("8MiB", &size, &err) && size == 8 << 20);
    CHECK(parse_size(" 12 ", &size, &err) && size == 12);
    CHECK(!parse_size("8MB", &size, &err));
    CHECK_STR(format_size(8 << 20, buf), "8MiB");
    CHECK_STR(format_size(1500, buf), "1500");
}

/* ---- ignore ---- */

static void test_ignore_match(T *t) {
    const char *patterns[] = {".git/", ".DS_Store", "*.swp", "~$*", "build/out/*"};
    Ignore m;
    ignore_init(&m, patterns, countof(patterns));
    static const struct {
        const char *path;
        bool is_dir, want;
    } cases[] = {
        {".dbox", true, true},         {".dbox/index.db", false, true},  {"src/.git", true, true},   {"src/.git/HEAD", false, true},
        {".git", false, false},        {"docs/.DS_Store", false, true},  {"notes/.todo.md.swp", false, true},
        {"~$report.docx", false, true}, {"build/out/app", false, true},  {"other/build/out/app", false, false}, {"notes/todo.md", false, false},
    };
    for (size_t i = 0; i < countof(cases); i++) {
        bool got = ignore_match(&m, cases[i].path, cases[i].is_dir);
        t->checks++;
        if (got != cases[i].want) {
            t->failures++;
            fprintf(stderr, "ignore_match(%s, %d) = %d, want %d\n", cases[i].path, cases[i].is_dir, got, cases[i].want);
        }
    }
    ignore_free(&m);
}

/* ---- debounce ---- */

typedef struct {
    pthread_mutex_t mu;
    int a, b;
} Calls;

static void count_call(void *ctx, const char *key) {
    Calls *c = ctx;
    pthread_mutex_lock(&c->mu);
    if (strcmp(key, "a") == 0) c->a++;
    if (strcmp(key, "b") == 0) c->b++;
    pthread_mutex_unlock(&c->mu);
}

static void test_debounce_burst_is_coalesced_per_key(T *t) {
    Calls calls = {PTHREAD_MUTEX_INITIALIZER, 0, 0};
    Debouncer *d = debounce_new(30 * NS_PER_MS, count_call, &calls);
    for (int i = 0; i < 5; i++) {
        debounce_trigger(d, "a");
        sleep_ms(5);
    }
    debounce_trigger(d, "b");
    CHECK(debounce_pending(d) == 2);
    sleep_ms(100);
    pthread_mutex_lock(&calls.mu);
    CHECK(calls.a == 1 && calls.b == 1);
    pthread_mutex_unlock(&calls.mu);
    CHECK(debounce_pending(d) == 0);
    debounce_stop(d);
}

/* ---- index ---- */

static Index *open_index(T *t) {
    char *dir = temp_dir(t);
    char *path = join(dir, "index.db");
    Err err;
    Index *idx = index_open(path, &err);
    if (!idx) fprintf(stderr, "index_open: %s\n", err.msg);
    xfree(path);
    xfree(dir);
    return idx;
}

static void test_index_tombstone_is_removed_once_no_store_holds_a_copy(T *t) {
    Index *idx = open_index(t);
    Arena a;
    arena_init(&a, 4096);
    Err err;
    IndexFile f = {.path = "a", .size = 1, .sha256 = "x"};
    const char *mirrors[] = {"m"};
    CHECK(index_record_synced(idx, &f, "p", "e1", mirrors, 1, &err));
    CHECK(index_tombstone(idx, "a", "p", false, &err));
    IndexFile got;
    bool found;
    CHECK(index_file(idx, &a, "a", &got, &found, &err) && found && got.deleted);
    CHECK(index_drop_replica(idx, "a", "m", &err));
    CHECK(index_file(idx, &a, "a", &got, &found, &err) && !found);
    arena_free(&a);
    index_close(idx);
}

static void test_index_mark_verified_ignores_stale_content(T *t) {
    Index *idx = open_index(t);
    Err err;
    IndexFile f = {.path = "a", .sha256 = "new"};
    const char *mirrors[] = {"m"};
    CHECK(index_record_synced(idx, &f, "p", "e", mirrors, 1, &err));
    bool verified;
    CHECK(index_mark_verified(idx, "a", "m", "e", "old", &verified, &err) && !verified);
    CHECK(index_mark_verified(idx, "a", "m", "e", "new", &verified, &err) && verified);
    index_close(idx);
}

static void test_index_backfill_and_stats(T *t) {
    Index *idx = open_index(t);
    Err err;
    IndexFile a = {.path = "a", .size = 10, .sha256 = "x"}, b = {.path = "b", .size = 20, .sha256 = "y"};
    CHECK(index_record_synced(idx, &a, "p", "e", NULL, 0, &err));
    CHECK(index_record_synced(idx, &b, "p", "e", NULL, 0, &err));
    int64_t n;
    CHECK(index_backfill(idx, "new", &n, &err) && n == 2);
    CHECK(index_backfill(idx, "new", &n, &err) && n == 0);
    bool verified;
    CHECK(index_mark_verified(idx, "a", "new", "e", "x", &verified, &err));
    Stats s;
    CHECK(index_stats(idx, "new", &s, &err));
    CHECK(s.files == 2 && s.bytes == 30 && s.verified == 1 && s.verified_bytes == 10 && s.pending == 1 && s.failed == 0);
    CHECK(stats_unverified(&s) == 1);
    index_close(idx);
}

static void test_index_upload_failures_are_due_after_backoff_until_given_up(T *t) {
    Index *idx = open_index(t);
    Arena a;
    arena_init(&a, 4096);
    Err err;
    int64_t now = wall_ns();
    CHECK(index_mark_upload_failed(idx, "a", "504", 1, now + 60 * NS_PER_SEC, &err));
    CHECK(index_mark_upload_failed(idx, "b", "403", MAX_ATTEMPTS, now, &err));
    const char **due;
    size_t n;
    CHECK(index_due_uploads(idx, &a, now, &due, &n, &err) && n == 0);
    CHECK(index_due_uploads(idx, &a, now + 60 * NS_PER_SEC, &due, &n, &err) && n == 1 && strcmp(due[0], "a") == 0);
    int64_t retried;
    CHECK(index_retry_failed_uploads(idx, &retried, &err) && retried == 2);
    CHECK(index_due_uploads(idx, &a, now, &due, &n, &err) && n == 2 && strcmp(due[0], "a") == 0 && strcmp(due[1], "b") == 0);
    IndexFile f = {.path = "a", .size = 1, .sha256 = "x"};
    CHECK(index_record_synced(idx, &f, "p", "e1", NULL, 0, &err));
    UploadFailure *failed;
    CHECK(index_failed_uploads(idx, &a, &failed, &n, &err) && n == 1 && strcmp(failed[0].path, "b") == 0 && strcmp(failed[0].last_error, "403") == 0);
    arena_free(&a);
    index_close(idx);
}

/* ---- engine ---- */

/* A Machine is one synced folder with its own index, sharing store directories with other machines in the same test. */
typedef struct {
    const char *name;
    char *dir;
} StoreDir;

typedef struct {
    const char *name;
    StoreRole role;
} RoleSpec;

typedef struct {
    char *root;
    Config cfg;
    Index *idx;
    StoreSet stores;
} Machine;

static void new_stores(T *t, StoreDir *dirs, size_t n) {
    for (size_t i = 0; i < n; i++) dirs[i].dir = temp_dir(t);
}

static const char *dir_of(StoreDir *dirs, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(dirs[i].name, name) == 0) return dirs[i].dir;
    return NULL;
}

static void set_roles(Machine *m, StoreDir *dirs, size_t ndirs, const RoleSpec *roles, size_t nroles) {
    m->cfg.store_count = 0;
    storeset_close(&m->stores);
    m->stores.names = xcalloc(nroles + 1, sizeof *m->stores.names);
    m->stores.stores = xcalloc(nroles + 1, sizeof *m->stores.stores);
    for (size_t i = 0; i < nroles; i++) {
        StoreConfig *s = config_add_store(&m->cfg, roles[i].name);
        s->kind = KIND_DISK;
        s->role = roles[i].role;
        s->root = dir_of(dirs, ndirs, roles[i].name);
        s->workers = 2;
        m->stores.names[i] = s->name;
    }
    qsort(m->stores.names, nroles, sizeof *m->stores.names, compare_strings);
    for (size_t i = 0; i < nroles; i++) m->stores.stores[i] = disk_open(config_store(&m->cfg, m->stores.names[i])->root, "");
    m->stores.count = nroles;
}

static Machine *new_machine(T *t, StoreDir *dirs, size_t ndirs, const RoleSpec *roles, size_t nroles) {
    Machine *m = xcalloc(1, sizeof *m);
    m->root = temp_dir(t);
    config_init(&m->cfg);
    m->cfg.sync.root = m->root;
    m->cfg.sync.pull_interval_ns = 50 * NS_PER_MS;
    m->cfg.sync.backfill_interval_ns = 3600 * NS_PER_SEC;
    m->cfg.sync.debounce_ns = 20 * NS_PER_MS;
    const char **ignore = arena_alloc(&m->cfg.arena, sizeof *ignore);
    ignore[0] = ".git/";
    m->cfg.sync.ignore = ignore;
    m->cfg.sync.ignore_count = 1;
    set_roles(m, dirs, ndirs, roles, nroles);
    char *state = config_state_dir(&m->cfg);
    char *path = join(state, "index.db");
    Err err;
    m->idx = index_open(path, &err);
    if (!m->idx) fprintf(stderr, "index: %s\n", err.msg);
    xfree(path);
    xfree(state);
    return m;
}

static void free_machine(Machine *m) {
    index_close(m->idx);
    storeset_close(&m->stores);
    config_free(&m->cfg);
    xfree(m->root);
    xfree(m);
}

static Engine *machine_engine(T *t, Machine *m) {
    Logger *log = getenv("DBOX_TEST_VERBOSE") ? &t->verbose : &t->discard;
    return engine_new(&m->cfg, m->idx, &m->stores, (EngineOptions){.log = log});
}

static void once(T *t, Machine *m) {
    Engine *e = machine_engine(t, m);
    Err err;
    bool ok = engine_once(e, &t->background, &err);
    t->checks++;
    if (!ok) {
        t->failures++;
        fprintf(stderr, "once: %s\n", err.msg);
    }
    engine_free(e);
}

static void mwrite(T *t, Machine *m, const char *rel, const char *content) {
    char *p = join(m->root, rel);
    write_text(t, p, content);
    xfree(p);
}

static void mexpect(T *t, Machine *m, const char *rel, const char *want) {
    char *p = join(m->root, rel);
    expect_file(t, p, want);
    xfree(p);
}

static void expect_in(T *t, const char *dir, const char *rel, const char *want) {
    char *p = join(dir, rel);
    expect_file(t, p, want);
    xfree(p);
}

static void remove_in(const char *dir, const char *rel) {
    char *p = join(dir, rel);
    unlink(p);
    xfree(p);
}

static Store *replace_store(Machine *m, const char *name, Store *wrapper) {
    for (size_t i = 0; i < m->stores.count; i++) {
        if (strcmp(m->stores.names[i], name) == 0) {
            Store *inner = m->stores.stores[i];
            m->stores.stores[i] = wrapper;
            return inner;
        }
    }
    return NULL;
}

static const RoleSpec primary_and_mirror[] = {{"home", ROLE_PRIMARY}, {"nas", ROLE_MIRROR}};
static const RoleSpec primary_only[] = {{"home", ROLE_PRIMARY}};

static void test_engine_new_files_reach_primary_and_mirror(T *t) {
    StoreDir dirs[] = {{"home", NULL}, {"nas", NULL}};
    new_stores(t, dirs, 2);
    Machine *m = new_machine(t, dirs, 2, primary_and_mirror, 2);
    mwrite(t, m, "docs/a.txt", "hello");
    mwrite(t, m, ".git/HEAD", "ignored");
    once(t, m);
    for (size_t i = 0; i < 2; i++) {
        expect_in(t, dirs[i].dir, "docs/a.txt", "hello");
        expect_in(t, dirs[i].dir, ".git/HEAD", "<missing>");
    }
    Stats s;
    Err err;
    CHECK(index_stats(m->idx, "nas", &s, &err) && stats_unverified(&s) == 0);
    free_machine(m);
}

static void test_engine_local_delete_reaches_every_store(T *t) {
    StoreDir dirs[] = {{"home", NULL}, {"nas", NULL}};
    new_stores(t, dirs, 2);
    Machine *m = new_machine(t, dirs, 2, primary_and_mirror, 2);
    mwrite(t, m, "a.txt", "x");
    once(t, m);
    remove_in(m->root, "a.txt");
    once(t, m);
    for (size_t i = 0; i < 2; i++) expect_in(t, dirs[i].dir, "a.txt", "<missing>");
    Arena a;
    arena_init(&a, 1024);
    IndexFile f;
    bool found;
    Err err;
    CHECK(index_file(m->idx, &a, "a.txt", &f, &found, &err) && !found);
    arena_free(&a);
    free_machine(m);
}

static void test_engine_changes_made_on_one_machine_arrive_on_another(T *t) {
    StoreDir dirs[] = {{"home", NULL}, {"nas", NULL}};
    new_stores(t, dirs, 2);
    Machine *a = new_machine(t, dirs, 2, primary_and_mirror, 2);
    Machine *b = new_machine(t, dirs, 2, primary_and_mirror, 2);
    mwrite(t, a, "shared.txt", "v1");
    once(t, a);
    once(t, b);
    mexpect(t, b, "shared.txt", "v1");
    mwrite(t, a, "shared.txt", "version 2");
    once(t, a);
    once(t, b);
    mexpect(t, b, "shared.txt", "version 2");
    remove_in(a->root, "shared.txt");
    once(t, a);
    once(t, b);
    mexpect(t, b, "shared.txt", "<missing>");
    free_machine(a);
    free_machine(b);
}

/*
 * Wrapped decorates a store for one test: it can hide a key from listings,
 * fail listings until online, hold Gets until enough overlap, reject Puts,
 * or edit the local file during the first Put.
 */
typedef struct {
    Store base;
    Store *inner;
    const char *hidden;
    atomic_bool *online;
    int want_gets;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int in_flight;
    bool released;
    bool *rejecting;
    const char *edit_local;
    bool edited;
    T *t;
} Wrapped;

static StoreStatus w_put(Store *s, Ctx *ctx, const char *key, int fd, int64_t size, const Meta *meta, char etag[ETAG_MAX], Err *err) {
    Wrapped *w = (Wrapped *)s;
    if (w->rejecting && *w->rejecting) {
        err_set(err, "503 slow down");
        return STORE_ERROR;
    }
    if (w->edit_local && !w->edited) {
        w->edited = true;
        write_text(w->t, w->edit_local, "second, longer version");
    }
    return store_put(w->inner, ctx, key, fd, size, meta, etag, err);
}

static StoreStatus w_get(Store *s, Ctx *ctx, const char *key, Object *obj, int *fd, Err *err) {
    Wrapped *w = (Wrapped *)s;
    if (w->want_gets > 0) {
        pthread_mutex_lock(&w->mu);
        if (++w->in_flight >= w->want_gets) {
            w->released = true;
            pthread_cond_broadcast(&w->cv);
        }
        int64_t deadline = monotonic_ns() + 2 * NS_PER_SEC;
        while (!w->released && monotonic_ns() < deadline) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * NS_PER_MS};
            pthread_mutex_unlock(&w->mu);
            nanosleep(&ts, NULL);
            pthread_mutex_lock(&w->mu);
        }
        bool released = w->released;
        pthread_mutex_unlock(&w->mu);
        if (!released) {
            err_set(err, "downloads did not overlap");
            return STORE_ERROR;
        }
    }
    return store_get(w->inner, ctx, key, obj, fd, err);
}

static StoreStatus w_head(Store *s, Ctx *ctx, const char *key, Object *obj, Err *err) { return store_head(((Wrapped *)s)->inner, ctx, key, obj, err); }
static StoreStatus w_del(Store *s, Ctx *ctx, const char *key, Err *err) { return store_delete(((Wrapped *)s)->inner, ctx, key, err); }

static StoreStatus w_list(Store *s, Ctx *ctx, Arena *a, Object **objects, size_t *count, Err *err) {
    Wrapped *w = (Wrapped *)s;
    if (w->online && !atomic_load(w->online)) {
        err_set(err, "network is unreachable");
        return STORE_ERROR;
    }
    StoreStatus status = store_list(w->inner, ctx, a, objects, count, err);
    if (status == STORE_OK && w->hidden) {
        size_t n = 0;
        for (size_t i = 0; i < *count; i++)
            if (strcmp((*objects)[i].key, w->hidden) != 0) (*objects)[n++] = (*objects)[i];
        *count = n;
    }
    return status;
}

static void w_close(Store *s) {
    Wrapped *w = (Wrapped *)s;
    store_close(w->inner);
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
    xfree(w);
}

static const StoreOps wrapped_ops = {w_put, w_get, w_head, w_del, w_list, w_close};

static Wrapped *wrap(T *t, Machine *m, const char *name) {
    Wrapped *w = xcalloc(1, sizeof *w);
    w->base.ops = &wrapped_ops;
    w->t = t;
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    w->inner = replace_store(m, name, &w->base);
    return w;
}

static void test_engine_file_missing_from_stale_listing_is_not_deleted_locally(T *t) {
    StoreDir dirs[] = {{"home", NULL}};
    new_stores(t, dirs, 1);
    Machine *m = new_machine(t, dirs, 1, primary_only, 1);
    mwrite(t, m, "fresh.txt", "just uploaded");
    once(t, m);
    Wrapped *w = wrap(t, m, "home");
    w->hidden = "fresh.txt";
    once(t, m);
    mexpect(t, m, "fresh.txt", "just uploaded");
    Arena a;
    arena_init(&a, 1024);
    IndexFile f;
    bool found;
    Err err;
    CHECK(index_file(m->idx, &a, "fresh.txt", &f, &found, &err) && found && !f.deleted);
    arena_free(&a);
    free_machine(m);
}

static void test_engine_identical_file_on_second_machine_is_adopted_not_conflicted(T *t) {
    StoreDir dirs[] = {{"home", NULL}};
    new_stores(t, dirs, 1);
    Machine *a = new_machine(t, dirs, 1, primary_only, 1);
    Machine *b = new_machine(t, dirs, 1, primary_only, 1);
    mwrite(t, a, "same.txt", "same");
    mwrite(t, b, "same.txt", "same");
    once(t, a);
    once(t, b);
    char *pattern = join(b->root, "*conflict*");
    glob_t g;
    CHECK(glob(pattern, 0, NULL, &g) == GLOB_NOMATCH);
    globfree(&g);
    xfree(pattern);
    Stats s;
    Err err;
    CHECK(index_stats(b->idx, "home", &s, &err) && s.files == 1 && s.verified == 1);
    free_machine(a);
    free_machine(b);
}

static void test_engine_edit_on_both_sides_keeps_local_and_saves_remote_beside(T *t) {
    StoreDir dirs[] = {{"home", NULL}};
    new_stores(t, dirs, 1);
    Machine *a = new_machine(t, dirs, 1, primary_only, 1);
    Machine *b = new_machine(t, dirs, 1, primary_only, 1);
    mwrite(t, a, "notes.md", "base");
    once(t, a);
    once(t, b);
    sleep_ms(20);
    mwrite(t, a, "notes.md", "from a");
    once(t, a);
    mwrite(t, b, "notes.md", "from b, longer");
    once(t, b);
    mexpect(t, b, "notes.md", "from b, longer");
    expect_in(t, dirs[0].dir, "notes.md", "from b, longer");
    char *pattern = join(b->root, "notes.conflict-*.md");
    glob_t g;
    int rc = glob(pattern, 0, NULL, &g);
    CHECK(rc == 0 && g.gl_pathc == 1);
    if (rc == 0 && g.gl_pathc == 1) expect_file(t, g.gl_pathv[0], "from a");
    globfree(&g);
    xfree(pattern);
    free_machine(a);
    free_machine(b);
}

static void test_engine_migration_by_promoting_a_mirror(T *t) {
    StoreDir dirs[] = {{"minio", NULL}, {"r2", NULL}};
    new_stores(t, dirs, 2);
    RoleSpec only_minio[] = {{"minio", ROLE_PRIMARY}};
    Machine *m = new_machine(t, dirs, 2, only_minio, 1);
    mwrite(t, m, "old.txt", "before r2 existed");
    once(t, m);

    RoleSpec with_mirror[] = {{"minio", ROLE_PRIMARY}, {"r2", ROLE_MIRROR}};
    set_roles(m, dirs, 2, with_mirror, 2);
    once(t, m);
    expect_in(t, dirs[1].dir, "old.txt", "before r2 existed");
    Stats s;
    Err err;
    CHECK(index_stats(m->idx, "r2", &s, &err) && stats_unverified(&s) == 0);

    RoleSpec promoted[] = {{"r2", ROLE_PRIMARY}, {"minio", ROLE_MIRROR}};
    set_roles(m, dirs, 2, promoted, 2);
    mwrite(t, m, "new.txt", "after promotion");
    once(t, m);
    expect_in(t, dirs[0].dir, "new.txt", "after promotion");
    expect_in(t, dirs[1].dir, "new.txt", "after promotion");

    RoleSpec detached[] = {{"r2", ROLE_PRIMARY}, {"minio", ROLE_DETACHED}};
    set_roles(m, dirs, 2, detached, 2);
    mwrite(t, m, "later.txt", "detached gets nothing");
    once(t, m);
    expect_in(t, dirs[0].dir, "later.txt", "<missing>");
    expect_in(t, dirs[0].dir, "old.txt", "before r2 existed");
    free_machine(m);
}

static void test_engine_mirror_reconcile_adopts_copies_made_by_other_tools(T *t) {
    StoreDir dirs[] = {{"home", NULL}, {"nas", NULL}};
    new_stores(t, dirs, 2);
    Machine *m = new_machine(t, dirs, 2, primary_only, 1);
    mwrite(t, m, "big.bin", "pretend this is large");
    once(t, m);
    char *copy = join(dirs[1].dir, "big.bin");
    write_text(t, copy, "pretend this is large");
    struct stat before, after;
    stat(copy, &before);
    set_roles(m, dirs, 2, primary_and_mirror, 2);
    Engine *e = machine_engine(t, m);
    Err err;
    CHECK(engine_reconcile(e, &t->background, &err));
    engine_free(e);
    Stats s;
    CHECK(index_stats(m->idx, "nas", &s, &err) && s.verified == 1);
    stat(copy, &after);
    CHECK(stat_mtime_ns(&before) == stat_mtime_ns(&after));
    xfree(copy);
    free_machine(m);
}

static void test_engine_mirror_falls_back_to_local_file_when_primary_lacks_it(T *t) {
    StoreDir dirs[] = {{"home", NULL}, {"nas", NULL}};
    new_stores(t, dirs, 2);
    Machine *m = new_machine(t, dirs, 2, primary_only, 1);
    mwrite(t, m, "a.txt", "only local and in index");
    once(t, m);
    remove_in(dirs[0].dir, "a.txt");
    set_roles(m, dirs, 2, primary_and_mirror, 2);
    int64_t n;
    Err err;
    CHECK(index_backfill(m->idx, "nas", &n, &err));
    Engine *e = machine_engine(t, m);
    CHECK(engine_drain_mirror(e, &t->background, "nas", &err));
    engine_free(e);
    expect_in(t, dirs[1].dir, "a.txt", "only local and in index");
    free_machine(m);
}

typedef struct {
    Engine *e;
    Ctx ctx;
    bool ok;
    Err err;
} DaemonRun;

static void *daemon_main(void *arg) {
    DaemonRun *d = arg;
    d->ok = engine_run(d->e, &d->ctx, &d->err);
    return NULL;
}

static bool file_is(void *arg) {
    const char **pair = arg;
    char *got = read_text(pair[0]);
    bool ok = strcmp(got, pair[1]) == 0;
    xfree(got);
    return ok;
}

static bool both_missing(void *arg) {
    const char **pair = arg;
    const char *missing[] = {pair[0], "<missing>"}, *other[] = {pair[1], "<missing>"};
    return file_is(missing) && file_is(other);
}

static void test_engine_daemon_pushes_edits_as_they_happen(T *t) {
    StoreDir dirs[] = {{"home", NULL}, {"nas", NULL}};
    new_stores(t, dirs, 2);
    Machine *m = new_machine(t, dirs, 2, primary_and_mirror, 2);
    DaemonRun run = {.e = machine_engine(t, m)};
    ctx_init(&run.ctx);
    pthread_t thread;
    pthread_create(&thread, NULL, daemon_main, &run);
    sleep_ms(300);

    mwrite(t, m, "new/dir/a.txt", "created in a new directory");
    /* Atomic save, the way most editors write: temp file renamed over the original. */
    mwrite(t, m, "new/dir/.a.txt.tmp", "saved by editor");
    char *tmp = join(m->root, "new/dir/.a.txt.tmp"), *target = join(m->root, "new/dir/a.txt");
    rename(tmp, target);
    char *nas_copy = join(dirs[1].dir, "new/dir/a.txt");
    const char *saved[] = {nas_copy, "saved by editor"};
    CHECK(eventually(file_is, saved));

    char *remote = join(dirs[0].dir, "from-elsewhere.txt");
    write_text(t, remote, "pulled");
    char *pulled_local = join(m->root, "from-elsewhere.txt");
    const char *pulled[] = {pulled_local, "pulled"};
    CHECK(eventually(file_is, pulled));

    char *new_dir = join(m->root, "new");
    remove_tree(new_dir);
    char *home_copy = join(dirs[0].dir, "new/dir/a.txt");
    const char *gone[] = {home_copy, nas_copy};
    CHECK(eventually(both_missing, gone));

    ctx_cancel(&run.ctx);
    pthread_join(thread, NULL);
    CHECK(run.ok);
    engine_free(run.e);
    ctx_destroy(&run.ctx);
    xfree(tmp);
    xfree(target);
    xfree(nas_copy);
    xfree(remote);
    xfree(pulled_local);
    xfree(new_dir);
    xfree(home_copy);
    free_machine(m);
}

static void test_engine_daemon_retries_reconcile_instead_of_exiting(T *t) {
    StoreDir dirs[] = {{"home", NULL}};
    new_stores(t, dirs, 1);
    Machine *m = new_machine(t, dirs, 1, primary_only, 1);
    atomic_bool online;
    atomic_init(&online, false);
    Wrapped *w = wrap(t, m, "home");
    w->online = &online;
    mwrite(t, m, "written-while-offline.txt", "queued");
    DaemonRun run = {.e = machine_engine(t, m)};
    ctx_init(&run.ctx);
    pthread_t thread;
    pthread_create(&thread, NULL, daemon_main, &run);
    sleep_ms(150);
    expect_in(t, dirs[0].dir, "written-while-offline.txt", "<missing>");
    atomic_store(&online, true);
    char *copy = join(dirs[0].dir, "written-while-offline.txt");
    const char *uploaded[] = {copy, "queued"};
    CHECK(eventually(file_is, uploaded));
    ctx_cancel(&run.ctx);
    pthread_join(thread, NULL);
    CHECK(run.ok);
    engine_free(run.e);
    ctx_destroy(&run.ctx);
    xfree(copy);
    free_machine(m);
}

static void test_engine_poll_downloads_in_parallel(T *t) {
    StoreDir dirs[] = {{"home", NULL}};
    new_stores(t, dirs, 1);
    Machine *m = new_machine(t, dirs, 1, primary_only, 1);
    Wrapped *w = wrap(t, m, "home");
    w->want_gets = 2;
    char *one = join(dirs[0].dir, "one.txt"), *two = join(dirs[0].dir, "two.txt");
    write_text(t, one, "1");
    write_text(t, two, "2");
    once(t, m);
    mexpect(t, m, "one.txt", "1");
    mexpect(t, m, "two.txt", "2");
    xfree(one);
    xfree(two);
    free_machine(m);
}

static void test_engine_failed_upload_is_recorded_and_forgotten_once_it_succeeds(T *t) {
    StoreDir dirs[] = {{"home", NULL}};
    new_stores(t, dirs, 1);
    Machine *m = new_machine(t, dirs, 1, primary_only, 1);
    bool rejecting = true;
    Wrapped *w = wrap(t, m, "home");
    w->rejecting = &rejecting;
    mwrite(t, m, "flaky.txt", "content");
    once(t, m);
    Arena a;
    arena_init(&a, 4096);
    UploadFailure *failed;
    size_t n;
    Err err;
    CHECK(index_failed_uploads(m->idx, &a, &failed, &n, &err));
    CHECK(n == 1 && strcmp(failed[0].path, "flaky.txt") == 0 && failed[0].attempts == 1 && strstr(failed[0].last_error, "503"));
    rejecting = false;
    once(t, m);
    expect_in(t, dirs[0].dir, "flaky.txt", "content");
    CHECK(index_failed_uploads(m->idx, &a, &failed, &n, &err) && n == 0);
    arena_free(&a);
    free_machine(m);
}

static void test_engine_file_changed_during_upload_is_not_recorded_until_resent(T *t) {
    StoreDir dirs[] = {{"home", NULL}};
    new_stores(t, dirs, 1);
    Machine *m = new_machine(t, dirs, 1, primary_only, 1);
    Wrapped *w = wrap(t, m, "home");
    char *local = join(m->root, "saving.txt");
    w->edit_local = local;
    mwrite(t, m, "saving.txt", "first");
    sleep_ms(20);
    once(t, m);
    Arena a;
    arena_init(&a, 4096);
    IndexFile f;
    bool found;
    Err err;
    CHECK(index_file(m->idx, &a, "saving.txt", &f, &found, &err) && !found);
    once(t, m);
    expect_in(t, dirs[0].dir, "saving.txt", "second, longer version");
    CHECK(index_file(m->idx, &a, "saving.txt", &f, &found, &err) && found && f.size == (int64_t)strlen("second, longer version"));
    arena_free(&a);
    xfree(local);
    free_machine(m);
}

/* ---- daemon ---- */

typedef struct {
    Ctx ctx;
    char addr[64];
} ServeRun;

static bool no_metrics(void *arg, StrBuf *out, Err *err) { return true; }
static long seven(void *arg) { return 7; }

static void *serve_main(void *arg) {
    ServeRun *s = arg;
    Err err;
    if (!daemon_serve(&s->ctx, s->addr, no_metrics, seven, NULL, &err)) fprintf(stderr, "serve: %s\n", err.msg);
    return NULL;
}

static void test_daemon_ask_returns_what_serve_reports(T *t) {
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(probe, (struct sockaddr *)&sa, sizeof sa) == 0);
    socklen_t len = sizeof sa;
    getsockname(probe, (struct sockaddr *)&sa, &len);
    close(probe);
    ServeRun run;
    snprintf(run.addr, sizeof run.addr, "127.0.0.1:%d", ntohs(sa.sin_port));
    long backlog;
    CHECK(!daemon_ask(run.addr, &backlog));
    ctx_init(&run.ctx);
    pthread_t thread;
    pthread_create(&thread, NULL, serve_main, &run);
    bool answered = false;
    for (int64_t deadline = monotonic_ns() + 2 * NS_PER_SEC; !answered && monotonic_ns() < deadline; sleep_ms(10)) answered = daemon_ask(run.addr, &backlog);
    CHECK(answered && backlog == 7);
    ctx_cancel(&run.ctx);
    pthread_join(thread, NULL);
    ctx_destroy(&run.ctx);
}

static void test_daemon_pid_file(T *t) {
    char *dir = temp_dir(t);
    char *pid = join(dir, "daemon.pid");
    Err err;
    pid_t got;
    CHECK(!daemon_running(pid, &got));
    CHECK(daemon_write_pid(pid, &err));
    CHECK(daemon_running(pid, &got) && got == getpid());
    CHECK(!daemon_write_pid(pid, &err));
    daemon_remove_pid(pid);
    CHECK(!daemon_running(pid, &got));
    xfree(pid);
    xfree(dir);
}

/* ---- service ---- */

/* capture records service manager commands. launchctl print reports a service as gone, and bootout of a missing service fails like launchctl does. */
static bool capture(void *user, char *const argv[], Err *err) {
    T *t = user;
    StrBuf line = {0};
    for (size_t i = 0; argv[i]; i++) sb_printf(&line, "%s%s", i ? " " : "", argv[i]);
    strlist_push(&t->commands, sb_cstr(&line));
    sb_free(&line);
    bool launchctl = strcmp(argv[0], "launchctl") == 0;
    if (launchctl && strcmp(argv[1], "bootstrap") == 0) t->loaded = true;
    else if (launchctl && strcmp(argv[1], "bootout") == 0 && !t->loaded) {
        err_set(err, "Boot-out failed: 3: No such process");
        return false;
    } else if (launchctl && strcmp(argv[1], "bootout") == 0) t->loaded = false;
    else if (launchctl && strcmp(argv[1], "print") == 0 && !t->loaded) {
        err_set(err, "Could not find service");
        return false;
    }
    return true;
}

static char *joined_calls(T *t) {
    StrBuf sb = {0};
    for (size_t i = 0; i < t->commands.len; i++) sb_printf(&sb, "%s%s", i ? "\n" : "", t->commands.items[i]);
    sb_cstr(&sb);
    return sb.data;
}

static void test_service_enable_on_mac_writes_agent_and_bootstraps_it(T *t) {
    strlist_clear(&t->commands);
    t->loaded = false;
    char *home = temp_dir(t);
    ServiceManager m = {.os = OS_DARWIN, .home = home, .config_home = home, .uid = 501, .run = capture, .run_user = t};
    Err err;
    CHECK(service_manager_enable(&m, "/opt/homebrew/bin/dbox", &err));
    char *unit = service_unit_path(&m);
    char *plist = read_text(unit);
    CHECK(strstr(plist, "<string>dbox</string>") != NULL);
    CHECK(strstr(plist, "<string>/opt/homebrew/bin/dbox</string><string>run</string>") != NULL);
    CHECK(strstr(plist, "<key>KeepAlive</key><true/>") != NULL);
    char *got = joined_calls(t);
    StrBuf want = {0};
    sb_printf(&want, "launchctl bootout gui/501/dbox\nlaunchctl bootstrap gui/501 %s", unit);
    CHECK_STR(got, want.data);
    xfree(got);
    sb_free(&want);
    strlist_clear(&t->commands);
    CHECK(service_manager_disable(&m, &err));
    got = joined_calls(t);
    CHECK_STR(got, "launchctl bootout gui/501/dbox\nlaunchctl print gui/501/dbox");
    xfree(got);
    xfree(plist);
    xfree(unit);
    xfree(home);
}

static void test_service_enable_on_linux_writes_unit_and_enables_it(T *t) {
    strlist_clear(&t->commands);
    char *home = temp_dir(t), *config_home = temp_dir(t);
    ServiceManager m = {.os = OS_LINUX, .home = home, .config_home = config_home, .uid = 1000, .run = capture, .run_user = t};
    Err err;
    CHECK(service_manager_enable(&m, "/home/me/go/bin/dbox", &err));
    char *unit = service_unit_path(&m);
    char *text = read_text(unit);
    CHECK(strstr(text, "ExecStart=\"/home/me/go/bin/dbox\" run") != NULL);
    CHECK(strstr(text, "WantedBy=default.target") != NULL);
    char *got = joined_calls(t);
    CHECK_STR(got, "systemctl --user daemon-reload\nsystemctl --user enable --now dbox.service");
    xfree(got);
    strlist_clear(&t->commands);
    CHECK(service_manager_disable(&m, &err));
    CHECK(access(unit, F_OK) != 0);
    got = joined_calls(t);
    CHECK_STR(got, "systemctl --user disable --now dbox.service\nsystemctl --user daemon-reload");
    xfree(got);
    CHECK(!service_manager_disable(&m, &err));
    xfree(text);
    xfree(unit);
    xfree(home);
    xfree(config_home);
}

static void test_service_plist_escapes_paths(T *t) {
    char *plist = service_launchd_plist("/Apps/a&b/dbox", "/l.log");
    CHECK(strstr(plist, "/Apps/a&amp;b/dbox") != NULL);
    xfree(plist);
}

int main(void) {
    T t = {.scratch = "/tmp/dbox-test-XXXXXX"};
    if (!mkdtemp(t.scratch)) return 1;
    ctx_init(&t.background);
    logger_init(&t.discard, "error", "text", NULL);
    logger_init(&t.verbose, "debug", "text", stderr);
    s3_global_init();
    unsetenv("DBOX_CONFIG");

    test_config_loads_stores_with_defaults_and_expanded_secrets(&t);
    test_config_references_expand_in_values_but_not_comments(&t);
    test_config_environment_overrides_nested_keys(&t);
    test_config_rejects_invalid_configs(&t);
    test_config_no_stores_is_accepted(&t);
    test_config_promote_swaps_roles_and_keeps_comments(&t);
    test_config_default_path_honours_environment(&t);
    test_config_starter_loads_and_renders_without_secrets(&t);
    test_durations_and_sizes(&t);
    test_ignore_match(&t);
    test_debounce_burst_is_coalesced_per_key(&t);
    test_index_tombstone_is_removed_once_no_store_holds_a_copy(&t);
    test_index_mark_verified_ignores_stale_content(&t);
    test_index_backfill_and_stats(&t);
    test_index_upload_failures_are_due_after_backoff_until_given_up(&t);
    test_engine_new_files_reach_primary_and_mirror(&t);
    test_engine_local_delete_reaches_every_store(&t);
    test_engine_changes_made_on_one_machine_arrive_on_another(&t);
    test_engine_file_missing_from_stale_listing_is_not_deleted_locally(&t);
    test_engine_identical_file_on_second_machine_is_adopted_not_conflicted(&t);
    test_engine_edit_on_both_sides_keeps_local_and_saves_remote_beside(&t);
    test_engine_migration_by_promoting_a_mirror(&t);
    test_engine_mirror_reconcile_adopts_copies_made_by_other_tools(&t);
    test_engine_mirror_falls_back_to_local_file_when_primary_lacks_it(&t);
    test_engine_daemon_pushes_edits_as_they_happen(&t);
    test_engine_daemon_retries_reconcile_instead_of_exiting(&t);
    test_engine_poll_downloads_in_parallel(&t);
    test_engine_failed_upload_is_recorded_and_forgotten_once_it_succeeds(&t);
    test_engine_file_changed_during_upload_is_not_recorded_until_resent(&t);
    test_daemon_ask_returns_what_serve_reports(&t);
    test_daemon_pid_file(&t);
    test_service_enable_on_mac_writes_agent_and_bootstraps_it(&t);
    test_service_enable_on_linux_writes_unit_and_enables_it(&t);
    test_service_plist_escapes_paths(&t);

    strlist_free(&t.commands);
    remove_tree(t.scratch);
    fprintf(stderr, "%d checks, %d failures\n", t.checks, t.failures);
    return t.failures ? 1 : 0;
}
