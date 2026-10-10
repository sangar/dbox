#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "daemon.h"
#include "engine.h"
#include "http.h"
#include "ignore.h"
#include "index.h"
#include "mc/text/glob.h"
#include "mc/text/path.h"
#include "store.h"

/* T is one test run: its counters, scratch directory and the fakes tests share. */
typedef struct {
    int failures, checks;
    Arena *arena;
    String scratch;
    int scratch_count;
    Cancel background;
    Logger *discard, *verbose;
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

#define CHECK_STR(got, want)                                                                                                   /* modern-c: allow function-macro */ \
    do {                                                                                                                       \
        t->checks++;                                                                                                           \
        String g_ = (got);                                                                                                     \
        const char *w_ = (want);                                                                                               \
        if (!str_equal(g_, S(w_))) {                                                                                           \
            t->failures++;                                                                                                     \
            fprintf(stderr, "%s:%d: %s: got \"%.*s\", want \"%s\"\n", __FILE__, __LINE__, __func__, (int)g_.len, g_.data, w_); \
        }                                                                                                                      \
    } while (0)

/* ---- t->scratch files ---- */

static String temp_dir(T *t) {
    String dir = str_format(t->arena, "%.*s/d%d", (int)t->scratch.len, t->scratch.data, t->scratch_count++);
    (void)dir_create(dir, 0755, nullptr);
    return dir;
}

static String join(T *t, String dir, const char *name) { return path_join(t->arena, dir, S(name)); }

/* count_matching counts the entries of dir whose name matches pattern and hands the first one's path to *first when asked. */
static size_t count_matching(T *t, String dir, const char *pattern, String *first) {
    StringList names = {0};
    size_t n = 0;
    if (first) *first = S("");
    if (dir_list(t->arena, dir, &names, nullptr) != ERR_OK) return 0;
    for (size_t i = 0; i < names.count; i++) {
        if (!glob_match(S(pattern), names.items[i])) continue;
        if (first && n == 0) *first = path_join(t->arena, dir, names.items[i]);
        n++;
    }
    return n;
}

static void write_text(T *t, String path, const char *content) {
    Err err;
    CHECK(dir_create_all(path_dir(path), 0755, &err) == ERR_OK);
    CHECK(file_write_all(path, S(content), 0644, &err) == ERR_OK);
}

/* read_text returns the file's content, or "<missing>". */
static String read_text(Arena *arena, String path) {
    String content;
    return file_read_all(arena, path, &content, nullptr) == ERR_OK ? content : S("<missing>");
}

static void expect_file(T *t, String path, const char *want) {
    String got = read_text(t->arena, path);
    t->checks++;
    if (!str_equal(got, S(want))) {
        t->failures++;
        fprintf(stderr, "%.*s = \"%.*s\", want \"%s\"\n", (int)path.len, path.data, (int)got.len, got.data, want);
    }
}

static bool eventually(bool (*ok)(void *), void *argument) {
    for (int64_t deadline = clock_monotonic_ns() + 5LL * NS_PER_SECOND; clock_monotonic_ns() < deadline; clock_sleep_ms(20))
        if (ok(argument)) return true;
    return false;
}

static StringList list_of(T *t, size_t count, const char *const *items) { return strlist_of(t->arena, count, items); }

/* ---- config ---- */

[[nodiscard]] static Error load(T *t, const char *yaml, StringList environment, Config *config, Err *err) {
    return config_parse(t->arena, S("config.yaml"), S(yaml), environment, config, err);
}

static void test_config_loads_stores_with_defaults_and_expanded_secrets(T *t) {
    String nas = temp_dir(t);
    String yaml = str_format(t->arena,
                             "stores:\n"
                             "  minio: {kind: s3, role: primary, bucket: dbox, region: us-east-1, prefix: me, secret_key: \"${SECRET}\"}\n"
                             "  nas: {kind: disk, role: mirror, root: %.*s}\n"
                             "sync: {root: /tmp/box}\n",
                             (int)nas.len, nas.data);
    Config config;
    Err err;
    Error e = load(t, yaml.data, list_of(t, 1, (const char *const[]){"SECRET=s3cr3t"}), &config, &err);
    CHECK(e == ERR_OK);
    if (e != ERR_OK) {
        fprintf(stderr, "load: %s\n", err.msg);
        return;
    }
    StoreConfig *minio = config_store(&config, S("minio"));
    CHECK_STR(minio->secret_key, "s3cr3t");
    CHECK_STR(minio->prefix, "me/");
    CHECK(minio->workers == 4);
    CHECK(config_store(&config, S("nas"))->workers == 2);
    CHECK_STR(config_primary(&config), "minio");
    StringList mirrors = config_mirrors(t->arena, &config);
    CHECK(mirrors.count == 1 && str_equal(mirrors.items[0], S("nas")));
    CHECK(config.sync.pull_interval_ns == 30LL * NS_PER_SECOND);
    CHECK(config.sync.part_size == 8 << 20);
    CHECK(config.sync.delete_remote);
}

static void test_config_references_expand_in_values_but_not_comments(T *t) {
    Config config;
    Err err;
    Error e = load(t, "# secret_key: ${UNSET}\nstores:\n  a: {kind: s3, role: primary, bucket: b, region: r, workers: \"${N}\"}\nsync: {root: /tmp/box}\n",
                   list_of(t, 1, (const char *const[]){"N=3"}), &config, &err);
    CHECK(e == ERR_OK);
    if (e == ERR_OK) CHECK(config_store(&config, S("a"))->workers == 3);
    else fprintf(stderr, "load: %s\n", err.msg);
}

static void test_config_environment_overrides_nested_keys(T *t) {
    Config config;
    Err err;
    Error e = load(t, "sync: {root: /a}\n", list_of(t, 2, (const char *const[]){"DBOX_SYNC__ROOT=/b", "DBOX_SYNC__DELETE_LOCAL=false"}), &config, &err);
    CHECK(e == ERR_OK);
    if (e != ERR_OK) return;
    CHECK_STR(config.sync.root, "/b");
    CHECK(!config.sync.delete_local);
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
        Config config;
        Err err;
        t->checks++;
        if (load(t, cases[i].yaml, (StringList){0}, &config, &err) == ERR_OK) {
            t->failures++;
            fprintf(stderr, "%s: loaded without error\n", cases[i].name);
        }
    }
}

static void test_config_errors_name_the_line(T *t) {
    Config config;
    Err err;
    CHECK(load(t, "sync:\n  root: /a\n  part_size: lots\n", (StringList){0}, &config, &err) == ERR_INVALID_ARGUMENT);
    CHECK_STR(S(err.msg), "config.yaml: line 3: part_size: size lots: want a number with an optional KiB, MiB or GiB suffix");
}

static void test_config_no_stores_is_accepted(T *t) {
    Config config;
    Err err;
    Error e = load(t, "", (StringList){0}, &config, &err);
    CHECK(e == ERR_OK);
    if (e != ERR_OK) return;
    CHECK(config_primary(&config).len == 0);
    CHECK_STR(config.daemon.listen, "127.0.0.1:7878");
}

static void test_config_promote_swaps_roles_and_keeps_comments(T *t) {
    String path = join(t, temp_dir(t), "config.yaml");
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
               "    role: 'mirror'\n"
               "    bucket: dbox\n"
               "    region: auto\n"
               "  old:\n"
               "    kind: s3\n"
               "    role: detached\n"
               "    bucket: dbox\n"
               "    region: auto\n");
    Err err;
    CHECK(config_promote(t->arena, path, S("r2"), &err) == ERR_OK);
    Config config;
    Error e = config_load(t->arena, path, &config, &err);
    CHECK(e == ERR_OK);
    if (e == ERR_OK) {
        CHECK(config_store(&config, S("r2"))->role == ROLE_PRIMARY);
        CHECK(config_store(&config, S("minio"))->role == ROLE_MIRROR);
        CHECK(config_store(&config, S("old"))->role == ROLE_DETACHED);
    }
    String raw = read_text(t->arena, path);
    CHECK(str_contains(raw, S("# my stores")) && str_contains(raw, S("role: mirror # home server")) && str_contains(raw, S("role: 'primary'")));
    CHECK(config_promote(t->arena, path, S("nope"), &err) == ERR_NOT_FOUND);
}

static void test_config_default_path_honours_environment(T *t) {
    Err err;
    CHECK(env_set(S("DBOX_CONFIG"), S("/elsewhere/dbox.yaml"), &err) == ERR_OK);
    CHECK_STR(config_default_path(t->arena), "/elsewhere/dbox.yaml");
    env_unset(S("DBOX_CONFIG"));

    String dir = temp_dir(t);
    CHECK(env_set(S("XDG_CONFIG_HOME"), dir, &err) == ERR_OK);
    String yml = join(t, dir, "dbox/config.yml"), yaml = join(t, dir, "dbox/config.yaml");
    CHECK(str_equal(config_default_path(t->arena), yml));
    write_text(t, yaml, "");
    CHECK(str_equal(config_default_path(t->arena), yaml));
    env_unset(S("XDG_CONFIG_HOME"));
}

static void test_config_starter_loads_and_renders_without_secrets(T *t) {
    String path = join(t, temp_dir(t), "config.yml");
    bool created;
    Err err;
    CHECK(config_write_starter(path, &created, &err) == ERR_OK && created);
    Config config;
    Error e = config_load(t->arena, path, &config, &err);
    CHECK(e == ERR_OK);
    if (e != ERR_OK) {
        fprintf(stderr, "starter: %s\n", err.msg);
        return;
    }
    StoreConfig *s = config_add_store(t->arena, &config, S("a"));
    s->kind = KIND_S3;
    s->role = ROLE_PRIMARY;
    s->bucket = S("b");
    s->region = S("r");
    s->secret_key = S("s3cr3t");
    s->workers = 4;
    String out = config_render(t->arena, &config);
    CHECK(!str_contains(out, S("s3cr3t")));
    CHECK(str_contains(out, S("part_size: 8MiB")));
    CHECK(str_contains(out, S("secret_key: '***'")));
    Config again;
    e = config_parse(t->arena, path, out, (StringList){0}, &again, &err);
    CHECK(e == ERR_OK);
    if (e == ERR_OK) {
        CHECK_STR(config_store(&again, S("a"))->secret_key, "***");
        CHECK(again.sync.ignore.count == 5);
    } else {
        fprintf(stderr, "rendered config does not load: %s\n", err.msg);
    }
}

static void test_sizes(T *t) {
    int64_t size;
    CHECK(parse_size(S("8MiB"), &size) && size == 8 << 20);
    CHECK(parse_size(S(" 12 "), &size) && size == 12);
    CHECK(!parse_size(S("8MB"), &size));
    CHECK_STR(format_size(t->arena, 8 << 20), "8MiB");
    CHECK_STR(format_size(t->arena, 1500), "1500");
}

/* ---- ignore ---- */

static void test_ignore_match(T *t) {
    Ignore ignore = ignore_create(t->arena, list_of(t, 5, (const char *const[]){".git/", ".DS_Store", "*.swp", "~$*", "build/out/*"}));
    static const struct {
        const char *path;
        bool is_dir, want;
    } cases[] = {
        {".dbox", true, true},          {".dbox/index.db", false, true}, {"src/.git", true, true},            {"src/.git/HEAD", false, true},
        {".git", false, false},         {"docs/.DS_Store", false, true}, {"notes/.todo.md.swp", false, true},
        {"~$report.docx", false, true}, {"build/out/app", false, true},  {"other/build/out/app", false, false}, {"notes/todo.md", false, false},
    };
    for (size_t i = 0; i < countof(cases); i++) {
        bool got = ignore_match(&ignore, S(cases[i].path), cases[i].is_dir);
        t->checks++;
        if (got != cases[i].want) {
            t->failures++;
            fprintf(stderr, "ignore_match(%s, %d) = %d, want %d\n", cases[i].path, cases[i].is_dir, got, cases[i].want);
        }
    }
}

/* ---- index ---- */

static Index *open_index(T *t) {
    Err err;
    Index *index = nullptr;
    if (index_open(t->arena, join(t, temp_dir(t), "index.db"), &index, &err) != ERR_OK) fprintf(stderr, "index_open: %s\n", err.msg);
    return index;
}

static StringList one_mirror(T *t) { return list_of(t, 1, (const char *const[]){"m"}); }

static void test_index_tombstone_is_removed_once_no_store_holds_a_copy(T *t) {
    Index *index = open_index(t);
    Err err;
    IndexFile f = {.path = S("a"), .size = 1, .sha256 = S("x")};
    CHECK(index_record_synced(index, &f, S("p"), S("e1"), one_mirror(t), &err) == ERR_OK);
    CHECK(index_tombstone(index, S("a"), S("p"), false, &err) == ERR_OK);
    IndexFile got;
    bool found;
    CHECK(index_file(index, t->arena, S("a"), &got, &found, &err) == ERR_OK && found && got.deleted);
    CHECK(index_drop_replica(index, S("a"), S("m"), &err) == ERR_OK);
    CHECK(index_file(index, t->arena, S("a"), &got, &found, &err) == ERR_OK && !found);
    index_close(index);
}

static void test_index_mark_verified_ignores_stale_content(T *t) {
    Index *index = open_index(t);
    Err err;
    IndexFile f = {.path = S("a"), .sha256 = S("new")};
    CHECK(index_record_synced(index, &f, S("p"), S("e"), one_mirror(t), &err) == ERR_OK);
    bool verified;
    CHECK(index_mark_verified(index, S("a"), S("m"), S("e"), S("old"), &verified, &err) == ERR_OK && !verified);
    CHECK(index_mark_verified(index, S("a"), S("m"), S("e"), S("new"), &verified, &err) == ERR_OK && verified);
    index_close(index);
}

static void test_index_backfill_and_stats(T *t) {
    Index *index = open_index(t);
    Err err;
    IndexFile a = {.path = S("a"), .size = 10, .sha256 = S("x")}, b = {.path = S("b"), .size = 20, .sha256 = S("y")};
    CHECK(index_record_synced(index, &a, S("p"), S("e"), (StringList){0}, &err) == ERR_OK);
    CHECK(index_record_synced(index, &b, S("p"), S("e"), (StringList){0}, &err) == ERR_OK);
    int64_t n;
    CHECK(index_backfill(index, S("new"), &n, &err) == ERR_OK && n == 2);
    CHECK(index_backfill(index, S("new"), &n, &err) == ERR_OK && n == 0);
    bool verified;
    CHECK(index_mark_verified(index, S("a"), S("new"), S("e"), S("x"), &verified, &err) == ERR_OK);
    Stats s;
    CHECK(index_stats(index, S("new"), &s, &err) == ERR_OK);
    CHECK(s.files == 2 && s.bytes == 30 && s.verified == 1 && s.verified_bytes == 10 && s.pending == 1 && s.failed == 0);
    CHECK(stats_unverified(&s) == 1);
    index_close(index);
}

static void test_index_files_under_a_directory_with_unicode_names(T *t) {
    Index *index = open_index(t);
    Err err;
    const char *const paths[] = {"bilder/ål.jpg", "bilder/ø/ny.jpg", "bilder-andre/x.jpg", "bilder"};
    for (size_t i = 0; i < countof(paths); i++) {
        IndexFile f = {.path = S(paths[i]), .sha256 = S("x")};
        CHECK(index_record_synced(index, &f, S("p"), S("e"), (StringList){0}, &err) == ERR_OK);
    }
    IndexFileList inside;
    CHECK(index_files_under(index, t->arena, S("bilder"), &inside, &err) == ERR_OK && inside.count == 2);
    index_close(index);
}

static void test_index_upload_failures_are_due_after_backoff_until_given_up(T *t) {
    Index *index = open_index(t);
    Err err;
    int64_t now = clock_wall_ns();
    CHECK(index_mark_upload_failed(index, S("a"), S("504"), 1, now + 60LL * NS_PER_SECOND, &err) == ERR_OK);
    CHECK(index_mark_upload_failed(index, S("b"), S("403"), MAX_ATTEMPTS, now, &err) == ERR_OK);
    StringList due;
    CHECK(index_due_uploads(index, t->arena, now, &due, &err) == ERR_OK && due.count == 0);
    CHECK(index_due_uploads(index, t->arena, now + 60LL * NS_PER_SECOND, &due, &err) == ERR_OK && due.count == 1 && str_equal(due.items[0], S("a")));
    int64_t retried;
    CHECK(index_retry_failed_uploads(index, &retried, &err) == ERR_OK && retried == 2);
    CHECK(index_due_uploads(index, t->arena, now, &due, &err) == ERR_OK && due.count == 2 && str_equal(due.items[0], S("a")) && str_equal(due.items[1], S("b")));
    IndexFile f = {.path = S("a"), .size = 1, .sha256 = S("x")};
    CHECK(index_record_synced(index, &f, S("p"), S("e1"), (StringList){0}, &err) == ERR_OK);
    UploadFailureList failed;
    CHECK(index_failed_uploads(index, t->arena, &failed, &err) == ERR_OK && failed.count == 1 && str_equal(failed.items[0].path, S("b")) &&
          str_equal(failed.items[0].last_error, S("403")));
    index_close(index);
}

/* ---- engine ---- */

/* A Machine is one synced folder with its own index, sharing store directories with other machines in the same test. */
typedef struct {
    const char *name;
    String dir;
} StoreDir;

typedef struct {
    const char *name;
    StoreRole role;
} RoleSpec;

typedef struct {
    String root;
    Config config;
    Index *index;
    StoreSet stores;
} Machine;

static void new_stores(T *t, StoreDir *dirs, size_t n) {
    for (size_t i = 0; i < n; i++) dirs[i].dir = temp_dir(t);
}

static String dir_of(const StoreDir *dirs, size_t n, String name) {
    for (size_t i = 0; i < n; i++)
        if (str_equal(S(dirs[i].name), name)) return dirs[i].dir;
    return S("");
}

static void set_roles(T *t, Machine *m, const StoreDir *dirs, size_t ndirs, const RoleSpec *roles, size_t nroles) {
    m->config.store_count = 0;
    m->stores = (StoreSet){.stores = arena_push(t->arena, (nroles + 1) * sizeof *m->stores.stores)};
    for (size_t i = 0; i < nroles; i++) {
        StoreConfig *s = config_add_store(t->arena, &m->config, S(roles[i].name));
        s->kind = KIND_DISK;
        s->role = roles[i].role;
        s->root = dir_of(dirs, ndirs, s->name);
        s->workers = 2;
        strlist_push(t->arena, &m->stores.names, s->name);
    }
    strlist_sort(&m->stores.names);
    for (size_t i = 0; i < nroles; i++) m->stores.stores[i] = disk_open(t->arena, config_store(&m->config, m->stores.names.items[i])->root, S(""));
}

static Machine *new_machine(T *t, const StoreDir *dirs, size_t ndirs, const RoleSpec *roles, size_t nroles) {
    Machine *m = arena_push(t->arena, sizeof *m);
    m->root = temp_dir(t);
    config_init(t->arena, &m->config);
    m->config.sync.root = m->root;
    m->config.sync.pull_interval_ns = 50LL * NS_PER_MILLISECOND;
    m->config.sync.backfill_interval_ns = 3600LL * NS_PER_SECOND;
    m->config.sync.debounce_ns = 20LL * NS_PER_MILLISECOND;
    m->config.sync.ignore = list_of(t, 1, (const char *const[]){".git/"});
    set_roles(t, m, dirs, ndirs, roles, nroles);
    Err err;
    if (index_open(t->arena, path_join(t->arena, config_state_dir(t->arena, &m->config), S("index.db")), &m->index, &err) != ERR_OK) fprintf(stderr, "index: %s\n", err.msg);
    return m;
}

static void free_machine(Machine *m) { index_close(m->index); }

static Engine *machine_engine(T *t, Machine *m) {
    Logger *log = getenv("DBOX_TEST_VERBOSE") ? t->verbose : t->discard;
    Engine *engine = nullptr;
    Err err;
    if (engine_create(t->arena, &m->config, m->index, &m->stores, (EngineOptions){.log = log}, &engine, &err) != ERR_OK) {
        fprintf(stderr, "engine: %s\n", err.msg);
        abort();
    }
    return engine;
}

static void once(T *t, Machine *m) {
    Engine *engine = machine_engine(t, m);
    Err err;
    Error e = engine_once(engine, &t->background, &err);
    t->checks++;
    if (e != ERR_OK) {
        t->failures++;
        fprintf(stderr, "once: %s\n", err.msg);
    }
    engine_destroy(engine);
}

static void mwrite(T *t, Machine *m, const char *rel, const char *content) { write_text(t, join(t, m->root, rel), content); }
static void mexpect(T *t, Machine *m, const char *rel, const char *want) { expect_file(t, join(t, m->root, rel), want); }
static void expect_in(T *t, String dir, const char *rel, const char *want) { expect_file(t, join(t, dir, rel), want); }
static void remove_in(T *t, String dir, const char *rel) { (void)file_remove(join(t, dir, rel), nullptr); }

static Store *replace_store(Machine *m, const char *name, Store *wrapper) {
    for (size_t i = 0; i < m->stores.names.count; i++) {
        if (str_equal(m->stores.names.items[i], S(name))) {
            Store *inner = m->stores.stores[i];
            m->stores.stores[i] = wrapper;
            return inner;
        }
    }
    return nullptr;
}

static const RoleSpec primary_and_mirror[] = {{"home", ROLE_PRIMARY}, {"nas", ROLE_MIRROR}};
static const RoleSpec primary_only[] = {{"home", ROLE_PRIMARY}};

static void test_engine_new_files_reach_primary_and_mirror(T *t) {
    StoreDir dirs[] = {{"home", {0}}, {"nas", {0}}};
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
    CHECK(index_stats(m->index, S("nas"), &s, &err) == ERR_OK && stats_unverified(&s) == 0);
    free_machine(m);
}

static void test_engine_local_delete_reaches_every_store(T *t) {
    StoreDir dirs[] = {{"home", {0}}, {"nas", {0}}};
    new_stores(t, dirs, 2);
    Machine *m = new_machine(t, dirs, 2, primary_and_mirror, 2);
    mwrite(t, m, "a.txt", "x");
    once(t, m);
    remove_in(t, m->root, "a.txt");
    once(t, m);
    for (size_t i = 0; i < 2; i++) expect_in(t, dirs[i].dir, "a.txt", "<missing>");
    IndexFile f;
    bool found;
    Err err;
    CHECK(index_file(m->index, t->arena, S("a.txt"), &f, &found, &err) == ERR_OK && !found);
    free_machine(m);
}

static void test_engine_changes_made_on_one_machine_arrive_on_another(T *t) {
    StoreDir dirs[] = {{"home", {0}}, {"nas", {0}}};
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
    remove_in(t, a->root, "shared.txt");
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
    Mutex mutex;
    int in_flight;
    bool released;
    bool *rejecting;
    String edit_local;
    bool edited;
    T *t;
} Wrapped;

[[nodiscard]] static Error w_put(Store *s, Cancel *cancel, Arena *arena, String key, int fd, int64_t size, Meta meta, String *etag, Err *err) {
    Wrapped *w = (Wrapped *)s;
    if (w->rejecting && *w->rejecting) return err_set(err, ERR_NETWORK, "503 slow down");
    if (w->edit_local.len > 0 && !w->edited) {
        w->edited = true;
        write_text(w->t, w->edit_local, "second, longer version");
    }
    return store_put(w->inner, cancel, arena, key, fd, size, meta, etag, err);
}

[[nodiscard]] static Error w_get(Store *s, Cancel *cancel, Arena *arena, String key, Object *object, int *fd, Err *err) {
    Wrapped *w = (Wrapped *)s;
    if (w->want_gets > 0) {
        mutex_lock(&w->mutex);
        if (++w->in_flight >= w->want_gets) w->released = true;
        int64_t deadline = clock_monotonic_ns() + 2LL * NS_PER_SECOND;
        while (!w->released && clock_monotonic_ns() < deadline) {
            mutex_unlock(&w->mutex);
            clock_sleep_ms(20);
            mutex_lock(&w->mutex);
        }
        bool released = w->released;
        mutex_unlock(&w->mutex);
        if (!released) return err_set(err, ERR_NETWORK, "downloads did not overlap");
    }
    return store_get(w->inner, cancel, arena, key, object, fd, err);
}

[[nodiscard]] static Error w_head(Store *s, Cancel *cancel, Arena *arena, String key, Object *object, Err *err) {
    return store_head(((Wrapped *)s)->inner, cancel, arena, key, object, err);
}

[[nodiscard]] static Error w_remove(Store *s, Cancel *cancel, Arena *arena, String key, Err *err) { return store_remove(((Wrapped *)s)->inner, cancel, arena, key, err); }

[[nodiscard]] static Error w_list(Store *s, Cancel *cancel, Arena *arena, ObjectList *objects, Err *err) {
    Wrapped *w = (Wrapped *)s;
    if (w->online && !atomic_load(w->online)) return err_set(err, ERR_NETWORK, "network is unreachable");
    Error status = store_list(w->inner, cancel, arena, objects, err);
    if (status != ERR_OK || !w->hidden) return status;
    size_t kept = 0;
    for (size_t i = 0; i < objects->count; i++)
        if (!str_equal(objects->items[i].key, S(w->hidden))) objects->items[kept++] = objects->items[i];
    objects->count = kept;
    return ERR_OK;
}

static const StoreOps wrapped_ops = {w_put, w_get, w_head, w_remove, w_list};

static Wrapped *wrap(T *t, Machine *m, const char *name) {
    Wrapped *w = arena_push(t->arena, sizeof *w);
    w->base.ops = &wrapped_ops;
    w->t = t;
    mutex_init(&w->mutex);
    w->inner = replace_store(m, name, &w->base);
    return w;
}

static void test_engine_file_missing_from_stale_listing_is_not_deleted_locally(T *t) {
    StoreDir dirs[] = {{"home", {0}}};
    new_stores(t, dirs, 1);
    Machine *m = new_machine(t, dirs, 1, primary_only, 1);
    mwrite(t, m, "fresh.txt", "just uploaded");
    once(t, m);
    Wrapped *w = wrap(t, m, "home");
    w->hidden = "fresh.txt";
    once(t, m);
    mexpect(t, m, "fresh.txt", "just uploaded");
    IndexFile f;
    bool found;
    Err err;
    CHECK(index_file(m->index, t->arena, S("fresh.txt"), &f, &found, &err) == ERR_OK && found && !f.deleted);
    free_machine(m);
}

static void test_engine_identical_file_on_second_machine_is_adopted_not_conflicted(T *t) {
    StoreDir dirs[] = {{"home", {0}}};
    new_stores(t, dirs, 1);
    Machine *a = new_machine(t, dirs, 1, primary_only, 1);
    Machine *b = new_machine(t, dirs, 1, primary_only, 1);
    mwrite(t, a, "same.txt", "same");
    mwrite(t, b, "same.txt", "same");
    once(t, a);
    once(t, b);
    CHECK(count_matching(t, b->root, "*conflict*", nullptr) == 0);
    Stats s;
    Err err;
    CHECK(index_stats(b->index, S("home"), &s, &err) == ERR_OK && s.files == 1 && s.verified == 1);
    free_machine(a);
    free_machine(b);
}

static void test_engine_edit_on_both_sides_keeps_local_and_saves_remote_beside(T *t) {
    StoreDir dirs[] = {{"home", {0}}};
    new_stores(t, dirs, 1);
    Machine *a = new_machine(t, dirs, 1, primary_only, 1);
    Machine *b = new_machine(t, dirs, 1, primary_only, 1);
    mwrite(t, a, "notes.md", "base");
    once(t, a);
    once(t, b);
    clock_sleep_ms(20);
    mwrite(t, a, "notes.md", "from a");
    once(t, a);
    mwrite(t, b, "notes.md", "from b, longer");
    once(t, b);
    mexpect(t, b, "notes.md", "from b, longer");
    expect_in(t, dirs[0].dir, "notes.md", "from b, longer");
    String copy;
    CHECK(count_matching(t, b->root, "notes.conflict-*.md", &copy) == 1);
    if (copy.len > 0) expect_file(t, copy, "from a");
    free_machine(a);
    free_machine(b);
}

static void test_engine_migration_by_promoting_a_mirror(T *t) {
    StoreDir dirs[] = {{"minio", {0}}, {"r2", {0}}};
    new_stores(t, dirs, 2);
    RoleSpec only_minio[] = {{"minio", ROLE_PRIMARY}};
    Machine *m = new_machine(t, dirs, 2, only_minio, 1);
    mwrite(t, m, "old.txt", "before r2 existed");
    once(t, m);

    RoleSpec with_mirror[] = {{"minio", ROLE_PRIMARY}, {"r2", ROLE_MIRROR}};
    set_roles(t, m, dirs, 2, with_mirror, 2);
    once(t, m);
    expect_in(t, dirs[1].dir, "old.txt", "before r2 existed");
    Stats s;
    Err err;
    CHECK(index_stats(m->index, S("r2"), &s, &err) == ERR_OK && stats_unverified(&s) == 0);

    RoleSpec promoted[] = {{"r2", ROLE_PRIMARY}, {"minio", ROLE_MIRROR}};
    set_roles(t, m, dirs, 2, promoted, 2);
    mwrite(t, m, "new.txt", "after promotion");
    once(t, m);
    expect_in(t, dirs[0].dir, "new.txt", "after promotion");
    expect_in(t, dirs[1].dir, "new.txt", "after promotion");

    RoleSpec detached[] = {{"r2", ROLE_PRIMARY}, {"minio", ROLE_DETACHED}};
    set_roles(t, m, dirs, 2, detached, 2);
    mwrite(t, m, "later.txt", "detached gets nothing");
    once(t, m);
    expect_in(t, dirs[0].dir, "later.txt", "<missing>");
    expect_in(t, dirs[0].dir, "old.txt", "before r2 existed");
    free_machine(m);
}

static void test_engine_mirror_reconcile_adopts_copies_made_by_other_tools(T *t) {
    StoreDir dirs[] = {{"home", {0}}, {"nas", {0}}};
    new_stores(t, dirs, 2);
    Machine *m = new_machine(t, dirs, 2, primary_only, 1);
    mwrite(t, m, "big.bin", "pretend this is large");
    once(t, m);
    String copy = join(t, dirs[1].dir, "big.bin");
    write_text(t, copy, "pretend this is large");
    FileInfo before, after;
    CHECK(file_info_follow(copy, &before, nullptr) == ERR_OK && before.exists);
    set_roles(t, m, dirs, 2, primary_and_mirror, 2);
    Engine *e = machine_engine(t, m);
    Err err;
    CHECK(engine_reconcile(e, &t->background, &err) == ERR_OK);
    engine_destroy(e);
    Stats s;
    CHECK(index_stats(m->index, S("nas"), &s, &err) == ERR_OK && s.verified == 1);
    CHECK(file_info_follow(copy, &after, nullptr) == ERR_OK && after.exists);
    CHECK(before.mtime_ns == after.mtime_ns);
    free_machine(m);
}

static void test_engine_mirror_falls_back_to_local_file_when_primary_lacks_it(T *t) {
    StoreDir dirs[] = {{"home", {0}}, {"nas", {0}}};
    new_stores(t, dirs, 2);
    Machine *m = new_machine(t, dirs, 2, primary_only, 1);
    mwrite(t, m, "a.txt", "only local and in index");
    once(t, m);
    remove_in(t, dirs[0].dir, "a.txt");
    set_roles(t, m, dirs, 2, primary_and_mirror, 2);
    int64_t n;
    Err err;
    CHECK(index_backfill(m->index, S("nas"), &n, &err) == ERR_OK);
    Engine *e = machine_engine(t, m);
    CHECK(engine_drain_mirror(e, &t->background, S("nas"), &err) == ERR_OK);
    engine_destroy(e);
    expect_in(t, dirs[1].dir, "a.txt", "only local and in index");
    free_machine(m);
}

typedef struct {
    Engine *e;
    Cancel cancel;
    Error result;
    Err err;
} DaemonRun;

static void *daemon_main(void *argument) {
    DaemonRun *d = argument;
    d->result = engine_run(d->e, &d->cancel, &d->err);
    return nullptr;
}

/* Expectation is a file that should come to have want as its content; want "<missing>" waits for it to be gone. */
typedef struct {
    String path;
    const char *want;
} Expectation;

static bool file_is(void *argument) {
    const Expectation *x = argument;
    Arena *arena = arena_create(4096);
    bool ok = str_equal(read_text(arena, x->path), S(x->want));
    arena_destroy(arena);
    return ok;
}

static bool both_missing(void *argument) {
    const String *paths = argument;
    Expectation first = {paths[0], "<missing>"}, second = {paths[1], "<missing>"};
    return file_is(&first) && file_is(&second);
}

[[nodiscard]] static Error start_daemon(T *t, Machine *m, DaemonRun *run, Thread *thread) {
    *run = (DaemonRun){.e = machine_engine(t, m)};
    cancel_init(&run->cancel);
    return thread_start(thread, daemon_main, run, nullptr);
}

static void stop_daemon(T *t, DaemonRun *run, Thread *thread) {
    cancel_request(&run->cancel);
    thread_join(thread);
    CHECK(run->result == ERR_OK);
    engine_destroy(run->e);
    cancel_destroy(&run->cancel);
}

static void test_engine_daemon_pushes_edits_as_they_happen(T *t) {
    StoreDir dirs[] = {{"home", {0}}, {"nas", {0}}};
    new_stores(t, dirs, 2);
    Machine *m = new_machine(t, dirs, 2, primary_and_mirror, 2);
    DaemonRun run;
    Thread thread;
    if (start_daemon(t, m, &run, &thread) != ERR_OK) {
        CHECK(false);
        return;
    }
    clock_sleep_ms(300);

    mwrite(t, m, "new/dir/a.txt", "created in a new directory");
    /* Atomic save, the way most editors write: temp file renamed over the original. */
    mwrite(t, m, "new/dir/.a.txt.tmp", "saved by editor");
    (void)file_rename(join(t, m->root, "new/dir/.a.txt.tmp"), join(t, m->root, "new/dir/a.txt"), nullptr);
    String nas_copy = join(t, dirs[1].dir, "new/dir/a.txt");
    Expectation saved = {nas_copy, "saved by editor"};
    CHECK(eventually(file_is, &saved));

    write_text(t, join(t, dirs[0].dir, "from-elsewhere.txt"), "pulled");
    Expectation pulled = {join(t, m->root, "from-elsewhere.txt"), "pulled"};
    CHECK(eventually(file_is, &pulled));

    (void)dir_remove_all(join(t, m->root, "new"), nullptr);
    String gone[] = {join(t, dirs[0].dir, "new/dir/a.txt"), nas_copy};
    CHECK(eventually(both_missing, gone));

    stop_daemon(t, &run, &thread);
    free_machine(m);
}

static void test_engine_daemon_retries_reconcile_instead_of_exiting(T *t) {
    StoreDir dirs[] = {{"home", {0}}};
    new_stores(t, dirs, 1);
    Machine *m = new_machine(t, dirs, 1, primary_only, 1);
    atomic_bool online;
    atomic_init(&online, false);
    Wrapped *w = wrap(t, m, "home");
    w->online = &online;
    mwrite(t, m, "written-while-offline.txt", "queued");
    DaemonRun run;
    Thread thread;
    if (start_daemon(t, m, &run, &thread) != ERR_OK) {
        CHECK(false);
        return;
    }
    clock_sleep_ms(150);
    expect_in(t, dirs[0].dir, "written-while-offline.txt", "<missing>");
    atomic_store(&online, true);
    Expectation uploaded = {join(t, dirs[0].dir, "written-while-offline.txt"), "queued"};
    CHECK(eventually(file_is, &uploaded));
    stop_daemon(t, &run, &thread);
    free_machine(m);
}

static void test_engine_poll_downloads_in_parallel(T *t) {
    StoreDir dirs[] = {{"home", {0}}};
    new_stores(t, dirs, 1);
    Machine *m = new_machine(t, dirs, 1, primary_only, 1);
    Wrapped *w = wrap(t, m, "home");
    w->want_gets = 2;
    write_text(t, join(t, dirs[0].dir, "one.txt"), "1");
    write_text(t, join(t, dirs[0].dir, "two.txt"), "2");
    once(t, m);
    mexpect(t, m, "one.txt", "1");
    mexpect(t, m, "two.txt", "2");
    free_machine(m);
}

static void test_engine_failed_upload_is_recorded_and_forgotten_once_it_succeeds(T *t) {
    StoreDir dirs[] = {{"home", {0}}};
    new_stores(t, dirs, 1);
    Machine *m = new_machine(t, dirs, 1, primary_only, 1);
    bool rejecting = true;
    Wrapped *w = wrap(t, m, "home");
    w->rejecting = &rejecting;
    mwrite(t, m, "flaky.txt", "content");
    once(t, m);
    UploadFailureList failed;
    Err err;
    CHECK(index_failed_uploads(m->index, t->arena, &failed, &err) == ERR_OK);
    CHECK(failed.count == 1 && str_equal(failed.items[0].path, S("flaky.txt")) && failed.items[0].attempts == 1 && str_contains(failed.items[0].last_error, S("503")));
    rejecting = false;
    once(t, m);
    expect_in(t, dirs[0].dir, "flaky.txt", "content");
    CHECK(index_failed_uploads(m->index, t->arena, &failed, &err) == ERR_OK && failed.count == 0);
    free_machine(m);
}

static void test_engine_file_changed_during_upload_is_not_recorded_until_resent(T *t) {
    StoreDir dirs[] = {{"home", {0}}};
    new_stores(t, dirs, 1);
    Machine *m = new_machine(t, dirs, 1, primary_only, 1);
    Wrapped *w = wrap(t, m, "home");
    w->edit_local = join(t, m->root, "saving.txt");
    mwrite(t, m, "saving.txt", "first");
    clock_sleep_ms(20);
    once(t, m);
    IndexFile f;
    bool found;
    Err err;
    CHECK(index_file(m->index, t->arena, S("saving.txt"), &f, &found, &err) == ERR_OK && !found);
    once(t, m);
    expect_in(t, dirs[0].dir, "saving.txt", "second, longer version");
    CHECK(index_file(m->index, t->arena, S("saving.txt"), &f, &found, &err) == ERR_OK && found && f.size == (int64_t)S("second, longer version").len);
    free_machine(m);
}

/* ---- daemon ---- */

typedef struct {
    Cancel cancel;
    String address;
} ServeRun;

[[nodiscard]] static Error no_metrics(void *context, Arena *arena, StringBuilder *out, Err *err) { return ERR_OK; }
static int64_t seven(void *context) { return 7; }

static void *serve_main(void *argument) {
    ServeRun *s = argument;
    Err err;
    if (daemon_serve(&s->cancel, s->address, no_metrics, seven, nullptr, &err) != ERR_OK) fprintf(stderr, "serve: %s\n", err.msg);
    return nullptr;
}

/* free_address finds a loopback address whose port nothing listens on. */
static String free_address(T *t) {
    int fd;
    String address = S("");
    if (net_listen(S("127.0.0.1:0"), &fd, nullptr) != ERR_OK) return address;
    if (net_local_address(t->arena, fd, &address, nullptr) != ERR_OK) address = S("");
    net_close(fd);
    return address;
}

static void test_daemon_ask_returns_what_serve_reports(T *t) {
    ServeRun run = {.address = free_address(t)};
    CHECK(run.address.len > 0);
    int64_t backlog;
    CHECK(!daemon_ask(t->arena, run.address, &backlog));
    cancel_init(&run.cancel);
    Thread thread;
    CHECK(thread_start(&thread, serve_main, &run, nullptr) == ERR_OK);
    bool answered = false;
    for (int64_t deadline = clock_monotonic_ns() + 2LL * NS_PER_SECOND; !answered && clock_monotonic_ns() < deadline; clock_sleep_ms(10))
        answered = daemon_ask(t->arena, run.address, &backlog);
    CHECK(answered && backlog == 7);
    cancel_request(&run.cancel);
    thread_join(&thread);
    cancel_destroy(&run.cancel);
}

static void test_daemon_pid_file(T *t) {
    String pid = join(t, temp_dir(t), "daemon.pid");
    Err err;
    int got;
    CHECK(!daemon_running(t->arena, pid, &got));
    CHECK(daemon_write_pid(t->arena, pid, &err) == ERR_OK);
    CHECK(daemon_running(t->arena, pid, &got) && got == process_id());
    CHECK(daemon_write_pid(t->arena, pid, &err) != ERR_OK);
    daemon_remove_pid(pid);
    CHECK(!daemon_running(t->arena, pid, &got));
}

int main(void) {
    T t = {.arena = arena_create(0)};
    if (dir_create_temp(t.arena, S("dbox-test-"), &t.scratch, nullptr) != ERR_OK) return 1;
    cancel_init(&t.background);
    FILE *null_out = fopen("/dev/null", "w");
    if (!null_out) return 1;
    t.discard = logger_create(LOG_ERROR, LOG_TEXT, null_out);
    t.verbose = logger_create(LOG_DEBUG, LOG_TEXT, stderr);
    http_global_init();
    env_unset(S("DBOX_CONFIG"));

    test_config_loads_stores_with_defaults_and_expanded_secrets(&t);
    test_config_references_expand_in_values_but_not_comments(&t);
    test_config_environment_overrides_nested_keys(&t);
    test_config_rejects_invalid_configs(&t);
    test_config_errors_name_the_line(&t);
    test_config_no_stores_is_accepted(&t);
    test_config_promote_swaps_roles_and_keeps_comments(&t);
    test_config_default_path_honours_environment(&t);
    test_config_starter_loads_and_renders_without_secrets(&t);
    test_sizes(&t);
    test_ignore_match(&t);
    test_index_tombstone_is_removed_once_no_store_holds_a_copy(&t);
    test_index_mark_verified_ignores_stale_content(&t);
    test_index_backfill_and_stats(&t);
    test_index_files_under_a_directory_with_unicode_names(&t);
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

    (void)dir_remove_all(t.scratch, nullptr);
    fprintf(stderr, "%d checks, %d failures\n", t.checks, t.failures);
    logger_destroy(t.discard);
    logger_destroy(t.verbose);
    fclose(null_out);
    cancel_destroy(&t.background);
    arena_destroy(t.arena);
    return t.failures ? 1 : 0;
}
