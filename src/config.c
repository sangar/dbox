#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "yml.h"

static const char starter_yml[] =
    "# dbox configuration. Every key: https://github.com/OWNER/dbox/blob/master/docs/configuration.md\n"
    "#\n"
    "# Any value can be ${ENV_VAR}, expanded when the config loads, so secrets can\n"
    "# stay out of this file. Any key can also be overridden from the environment\n"
    "# as DBOX_SECTION__KEY, e.g. DBOX_SYNC__PULL_INTERVAL=1m.\n"
    "#\n"
    "# After editing, check a store with `dbox check NAME`. A running daemon picks\n"
    "# up changes on SIGHUP, which `dbox config edit` sends for you.\n"
    "\n"
    "stores: {}\n"
    "#  minio:                      # name: a-z, 0-9, _ and -\n"
    "#    kind: s3                  # s3 | disk\n"
    "#    role: primary             # exactly one primary; others mirror | detached\n"
    "#    bucket: dbox\n"
    "#    region: us-east-1\n"
    "#    endpoint: http://localhost:9000\n"
    "#    path_style: true\n"
    "#    access_key: ${MINIO_ACCESS_KEY}\n"
    "#    secret_key: ${MINIO_SECRET_KEY}\n"
    "#\n"
    "#  nas:\n"
    "#    kind: disk\n"
    "#    role: mirror\n"
    "#    root: /Volumes/backup/dbox\n"
    "\n"
    "sync:\n"
    "  root: ~/dbox\n"
    "  pull_interval: 30s\n"
    "  backfill_interval: 10m\n"
    "  debounce: 750ms\n"
    "  part_size: 8MiB\n"
    "  delete_remote: true\n"
    "  delete_local: true\n"
    "\n"
    "daemon:\n"
    "  log_level: info             # debug | info | warn | error\n"
    "  log_format: text            # text | json\n"
    "  listen: 127.0.0.1:7878\n";

static const char *default_ignore[] = {".git/", ".DS_Store", "*.swp", "*.tmp", "~$*"};

void config_init(Config *c) {
    memset(c, 0, sizeof *c);
    arena_init(&c->arena, 16 * 1024);
    c->sync.root = "~/dbox";
    c->sync.pull_interval_ns = 30 * NS_PER_SEC;
    c->sync.backfill_interval_ns = 10 * 60 * NS_PER_SEC;
    c->sync.debounce_ns = 750 * NS_PER_MS;
    c->sync.part_size = 8 << 20;
    c->sync.delete_remote = true;
    c->sync.delete_local = true;
    c->sync.ignore = default_ignore;
    c->sync.ignore_count = countof(default_ignore);
    c->daemon.log_level = "info";
    c->daemon.log_format = "text";
    c->daemon.listen = "127.0.0.1:7878";
    c->path = "";
}

void config_free(Config *c) {
    xfree(c->stores);
    arena_free(&c->arena);
    memset(c, 0, sizeof *c);
}

StoreConfig *config_add_store(Config *c, const char *name) {
    if (c->store_count == c->store_cap) {
        c->store_cap = c->store_cap ? c->store_cap * 2 : 4;
        c->stores = xrealloc(c->stores, c->store_cap * sizeof *c->stores);
    }
    StoreConfig *s = &c->stores[c->store_count++];
    memset(s, 0, sizeof *s);
    s->name = arena_strdup(&c->arena, name);
    s->bucket = s->prefix = s->region = s->endpoint = s->access_key = s->secret_key = s->storage_class = s->root = "";
    return s;
}

StoreConfig *config_store(const Config *c, const char *name) {
    for (size_t i = 0; i < c->store_count; i++)
        if (strcmp(c->stores[i].name, name) == 0) return &c->stores[i];
    return NULL;
}

const char *config_primary(const Config *c) {
    for (size_t i = 0; i < c->store_count; i++)
        if (c->stores[i].role == ROLE_PRIMARY) return c->stores[i].name;
    return NULL;
}

static const char **names_with_role(const Config *c, Arena *a, StoreRole role, size_t *count) {
    const char **names = arena_alloc(a, (c->store_count + 1) * sizeof *names);
    size_t n = 0;
    for (size_t i = 0; i < c->store_count; i++)
        if (c->stores[i].role == role) names[n++] = c->stores[i].name;
    qsort(names, n, sizeof *names, compare_strings);
    *count = n;
    return names;
}

const char **config_mirrors(const Config *c, Arena *a, size_t *count) { return names_with_role(c, a, ROLE_MIRROR, count); }

const char **config_store_names(const Config *c, Arena *a, size_t *count) {
    const char **names = arena_alloc(a, (c->store_count + 1) * sizeof *names);
    size_t n = 0;
    StoreRole order[] = {ROLE_PRIMARY, ROLE_MIRROR, ROLE_DETACHED};
    for (size_t r = 0; r < countof(order); r++) {
        size_t m;
        const char **group = names_with_role(c, a, order[r], &m);
        for (size_t i = 0; i < m; i++) names[n++] = group[i];
    }
    *count = n;
    return names;
}

char *config_state_dir(const Config *c) { return path_join(c->sync.root, ".dbox"); }

const char *role_name(StoreRole r) {
    switch (r) {
    case ROLE_PRIMARY: return "primary";
    case ROLE_MIRROR: return "mirror";
    case ROLE_DETACHED: return "detached";
    default: return "";
    }
}

const char *kind_name(StoreKind k) {
    switch (k) {
    case KIND_S3: return "s3";
    case KIND_DISK: return "disk";
    default: return "";
    }
}

/* ---- sizes ---- */

static const struct {
    const char *suffix;
    int64_t factor;
} size_units[] = {{"GiB", 1 << 30}, {"MiB", 1 << 20}, {"KiB", 1 << 10}, {"B", 1}};

Error parse_size(const char *s, int64_t *out, Err *err) {
    const char *original = s;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') n--;
    int64_t factor = 1;
    for (size_t i = 0; i < countof(size_units); i++) {
        size_t m = strlen(size_units[i].suffix);
        if (n >= m && memcmp(s + n - m, size_units[i].suffix, m) == 0) {
            factor = size_units[i].factor;
            n -= m;
            break;
        }
    }
    char *digits = xstrndup(s, n);
    int64_t v;
    bool ok = parse_int64(digits, &v);
    xfree(digits);
    if (!ok) return err_set(err, ERR_INVALID_ARGUMENT, "size %s: want a number with an optional KiB, MiB or GiB suffix", original);
    *out = v * factor;
    return ERR_OK;
}

const char *format_size(int64_t n, char buf[32]) {
    for (size_t i = 0; i < countof(size_units); i++) {
        int64_t f = size_units[i].factor;
        if (f > 1 && n >= f && n % f == 0) {
            snprintf(buf, 32, "%lld%s", (long long)(n / f), size_units[i].suffix);
            return buf;
        }
    }
    snprintf(buf, 32, "%lld", (long long)n);
    return buf;
}

/* ---- loading ---- */

char *config_default_path(void) {
    const char *env = getenv("DBOX_CONFIG");
    if (env && *env) return xstrdup(env);
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char *dir = xdg && *xdg ? xstrdup(xdg) : path_join(env_home(), ".config");
    char *yml = path_join(dir, "dbox/config.yml");
    char *yaml = path_join(dir, "dbox/config.yaml");
    xfree(dir);
    FileStat yml_info, yaml_info;
    bool yml_missing = file_info_follow(yml, &yml_info, NULL) == ERR_OK && !yml_info.exists;
    if (yml_missing && file_info_follow(yaml, &yaml_info, NULL) == ERR_OK && yaml_info.exists) {
        xfree(yml);
        return yaml;
    }
    xfree(yaml);
    return yml;
}

Error config_write_starter(const char *path, bool *created, Err *err) {
    FileStat info;
    *created = false;
    if (file_info_follow(path, &info, NULL) == ERR_OK && info.exists) return ERR_OK;
    char *dir = path_dir(path);
    Error e = mkdir_p(dir, 0755, err);
    xfree(dir);
    if (e != ERR_OK) return e;
    /* Private to the user because it may come to hold store secrets. */
    e = write_file(path, starter_yml, sizeof starter_yml - 1, 0600, err);
    if (e != ERR_OK) return e;
    *created = true;
    return ERR_OK;
}

typedef struct {
    Arena *arena;
    char *const *environ;
    StrList missing;
} Expander;

static const char *env_value(char *const *environ, const char *name, size_t n) {
    for (size_t i = 0; environ[i]; i++)
        if (strncmp(environ[i], name, n) == 0 && environ[i][n] == '=') return environ[i] + n + 1;
    return NULL;
}

static bool name_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool name_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* expand_string replaces ${VAR} references, noting unset names, and reports whether anything changed. */
static bool expand_string(Expander *x, const char *s, char **out) {
    StrBuf sb = {0};
    bool changed = false;
    for (const char *p = s; *p;) {
        if (p[0] == '$' && p[1] == '{' && name_start(p[2])) {
            const char *name = p + 2, *q = name;
            while (name_char(*q)) q++;
            if (*q == '}') {
                size_t n = (size_t)(q - name);
                const char *v = env_value(x->environ, name, n);
                if (v) {
                    sb_puts(&sb, v);
                } else {
                    char *missing = xstrndup(name, n);
                    if (!strlist_contains(&x->missing, missing)) strlist_push_owned(&x->missing, missing);
                    else xfree(missing);
                }
                changed = true;
                p = q + 1;
                continue;
            }
        }
        sb_putc(&sb, *p++);
    }
    if (changed) *out = arena_strdup(x->arena, sb_cstr(&sb));
    sb_free(&sb);
    return changed;
}

/* expand_references rewrites every scalar in the tree; keys and comments are left alone. */
static void expand_references(Expander *x, YmlNode *n) {
    if (!n) return;
    switch (n->kind) {
    case YML_SCALAR: {
        char *expanded;
        if (expand_string(x, n->value, &expanded)) {
            n->value = expanded;
            /* An expanded value is read as YAML would have, so "workers: ${N}" is a number. */
            n->quoted = false;
            n->start = n->end = 0;
        }
        break;
    }
    case YML_MAP:
    case YML_SEQ:
        for (size_t i = 0; i < n->count; i++) expand_references(x, n->items[i]);
        break;
    default: break;
    }
}

/* apply_overrides sets DBOX_SECTION__KEY=value as tree[section][key]. */
static void apply_overrides(Arena *a, YmlNode *root, char *const *environ) {
    StrList keys = {0};
    for (size_t i = 0; environ[i]; i++) {
        const char *eq = strchr(environ[i], '=');
        if (!eq || !has_prefix(environ[i], "DBOX_")) continue;
        char *key = xstrndup(environ[i], (size_t)(eq - environ[i]));
        if (strstr(key, "__")) strlist_push_owned(&keys, key);
        else xfree(key);
    }
    strlist_sort(&keys);
    for (size_t i = 0; i < keys.len; i++) {
        const char *value = env_value(environ, keys.items[i], strlen(keys.items[i]));
        char *lower = xstrdup(keys.items[i] + strlen("DBOX_"));
        for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
        YmlNode *node = root;
        char *part = lower;
        for (;;) {
            char *sep = strstr(part, "__");
            if (!sep) break;
            *sep = '\0';
            YmlNode *next = yml_get(node, part);
            if (!next || next->kind != YML_MAP) {
                next = yml_new_map(a);
                yml_set(a, node, part, next);
            }
            node = next;
            part = sep + 2;
        }
        yml_set(a, node, part, yml_new_scalar(a, value ? value : "", false));
        xfree(lower);
    }
    strlist_free(&keys);
}

/* ---- decoding ---- */

typedef struct {
    Config *cfg;
    Err *err;
    bool failed;
} Decoder;

static void decode_fail(Decoder *d, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void decode_fail(Decoder *d, int line, const char *fmt, ...) {
    if (d->failed) return;
    d->failed = true;
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (line > 0) (void)err_set(d->err, ERR_INVALID_ARGUMENT, "line %d: %s", line, msg);
    else (void)err_set(d->err, ERR_INVALID_ARGUMENT, "%s", msg);
}

static const char *scalar(Decoder *d, const YmlNode *n, const char *key) {
    if (yml_is_null(n)) return "";
    if (n->kind != YML_SCALAR) {
        decode_fail(d, n->line, "%s: expected a single value", key);
        return "";
    }
    return n->value;
}

static const char *string_field(Decoder *d, const YmlNode *n, const char *key) { return arena_strdup(&d->cfg->arena, scalar(d, n, key)); }

static bool bool_field(Decoder *d, const YmlNode *n, const char *key, bool current) {
    const char *v = scalar(d, n, key);
    if (strcmp(v, "true") == 0) return true;
    if (strcmp(v, "false") == 0) return false;
    decode_fail(d, n->line, "%s: cannot read %s as a bool", key, v);
    return current;
}

static int64_t int_field(Decoder *d, const YmlNode *n, const char *key, int64_t current) {
    const char *v = scalar(d, n, key);
    int64_t out;
    if (parse_int64(v, &out)) return out;
    decode_fail(d, n->line, "%s: cannot read %s as a number", key, v);
    return current;
}

static int64_t duration_field(Decoder *d, const YmlNode *n, const char *key, int64_t current) {
    const char *v = scalar(d, n, key);
    int64_t ns;
    if (parse_duration(v, &ns)) return ns;
    decode_fail(d, n->line, "%s: invalid duration %s", key, v);
    return current;
}

static int64_t size_field(Decoder *d, const YmlNode *n, const char *key, int64_t current) {
    Err err;
    int64_t out;
    if (parse_size(scalar(d, n, key), &out, &err) == ERR_OK) return out;
    decode_fail(d, n->line, "%s: %s", key, err.msg);
    return current;
}

static void list_field(Decoder *d, const YmlNode *n, const char *key, const char ***out, size_t *count) {
    if (yml_is_null(n)) {
        *out = NULL;
        *count = 0;
        return;
    }
    if (n->kind != YML_SEQ) {
        decode_fail(d, n->line, "%s: expected a list", key);
        return;
    }
    const char **items = arena_alloc(&d->cfg->arena, (n->count + 1) * sizeof *items);
    for (size_t i = 0; i < n->count; i++) items[i] = string_field(d, n->items[i], key);
    *out = items;
    *count = n->count;
}

static bool expect_map(Decoder *d, const YmlNode *n, const char *key) {
    if (n->kind == YML_MAP) return true;
    decode_fail(d, n->line, "%s: expected a mapping", key);
    return false;
}

static void decode_store(Decoder *d, const char *name, const YmlNode *n, int line) {
    StoreConfig *s = config_add_store(d->cfg, name);
    if (yml_is_null(n) || !expect_map(d, n, name)) return;
    for (size_t i = 0; i < n->count && !d->failed; i++) {
        const char *k = n->keys[i];
        const YmlNode *v = n->items[i];
        if (strcmp(k, "kind") == 0) {
            const char *kind = scalar(d, v, k);
            s->kind = strcmp(kind, "s3") == 0 ? KIND_S3 : strcmp(kind, "disk") == 0 ? KIND_DISK : KIND_NONE;
            if (s->kind == KIND_NONE) decode_fail(d, 0, "store \"%s\": kind must be s3 or disk", name);
        } else if (strcmp(k, "role") == 0) {
            const char *role = scalar(d, v, k);
            s->role = strcmp(role, "primary") == 0 ? ROLE_PRIMARY : strcmp(role, "mirror") == 0 ? ROLE_MIRROR : strcmp(role, "detached") == 0 ? ROLE_DETACHED : ROLE_NONE;
            if (s->role == ROLE_NONE && *role) decode_fail(d, 0, "store \"%s\": unknown role \"%s\"", name, role);
        } else if (strcmp(k, "bucket") == 0) s->bucket = string_field(d, v, k);
        else if (strcmp(k, "prefix") == 0) s->prefix = string_field(d, v, k);
        else if (strcmp(k, "region") == 0) s->region = string_field(d, v, k);
        else if (strcmp(k, "endpoint") == 0) s->endpoint = string_field(d, v, k);
        else if (strcmp(k, "path_style") == 0) s->path_style = bool_field(d, v, k, false);
        else if (strcmp(k, "access_key") == 0) s->access_key = string_field(d, v, k);
        else if (strcmp(k, "secret_key") == 0) s->secret_key = string_field(d, v, k);
        else if (strcmp(k, "storage_class") == 0) s->storage_class = string_field(d, v, k);
        else if (strcmp(k, "root") == 0) s->root = string_field(d, v, k);
        else if (strcmp(k, "workers") == 0) s->workers = (int)int_field(d, v, k, 0);
        else decode_fail(d, line, "field %s not found in store %s", k, name);
    }
}

static void decode_sync(Decoder *d, const YmlNode *n) {
    SyncConfig *s = &d->cfg->sync;
    if (yml_is_null(n) || !expect_map(d, n, "sync")) return;
    for (size_t i = 0; i < n->count && !d->failed; i++) {
        const char *k = n->keys[i];
        const YmlNode *v = n->items[i];
        if (strcmp(k, "root") == 0) s->root = string_field(d, v, k);
        else if (strcmp(k, "pull_interval") == 0) s->pull_interval_ns = duration_field(d, v, k, s->pull_interval_ns);
        else if (strcmp(k, "backfill_interval") == 0) s->backfill_interval_ns = duration_field(d, v, k, s->backfill_interval_ns);
        else if (strcmp(k, "debounce") == 0) s->debounce_ns = duration_field(d, v, k, s->debounce_ns);
        else if (strcmp(k, "part_size") == 0) s->part_size = size_field(d, v, k, s->part_size);
        else if (strcmp(k, "delete_remote") == 0) s->delete_remote = bool_field(d, v, k, s->delete_remote);
        else if (strcmp(k, "delete_local") == 0) s->delete_local = bool_field(d, v, k, s->delete_local);
        else if (strcmp(k, "ignore") == 0) list_field(d, v, k, &s->ignore, &s->ignore_count);
        else decode_fail(d, n->line, "field %s not found in sync", k);
    }
}

static void decode_daemon(Decoder *d, const YmlNode *n) {
    DaemonConfig *dm = &d->cfg->daemon;
    if (yml_is_null(n) || !expect_map(d, n, "daemon")) return;
    for (size_t i = 0; i < n->count && !d->failed; i++) {
        const char *k = n->keys[i];
        const YmlNode *v = n->items[i];
        if (strcmp(k, "log_level") == 0) dm->log_level = string_field(d, v, k);
        else if (strcmp(k, "log_format") == 0) dm->log_format = string_field(d, v, k);
        else if (strcmp(k, "listen") == 0) dm->listen = string_field(d, v, k);
        else decode_fail(d, n->line, "field %s not found in daemon", k);
    }
}

static void decode(Decoder *d, const YmlNode *root) {
    if (yml_is_null(root)) return;
    if (!expect_map(d, root, "config")) return;
    for (size_t i = 0; i < root->count && !d->failed; i++) {
        const char *k = root->keys[i];
        const YmlNode *v = root->items[i];
        if (strcmp(k, "stores") == 0) {
            if (yml_is_null(v)) continue;
            if (!expect_map(d, v, "stores")) return;
            for (size_t j = 0; j < v->count && !d->failed; j++) decode_store(d, v->keys[j], v->items[j], v->line);
        } else if (strcmp(k, "sync") == 0) decode_sync(d, v);
        else if (strcmp(k, "daemon") == 0) decode_daemon(d, v);
        else decode_fail(d, root->line, "field %s not found in config", k);
    }
}

/* ---- validation ---- */

static bool valid_store_name(const char *name) {
    if (!*name) return false;
    for (; *name; name++)
        if (!(islower((unsigned char)*name) || isdigit((unsigned char)*name) || *name == '_' || *name == '-')) return false;
    return true;
}

static const char *expand_home_into(Arena *a, const char *p) {
    char *expanded = expand_home(p);
    const char *out = arena_strdup(a, expanded);
    xfree(expanded);
    return out;
}

[[nodiscard]] static Error finish(Config *c, Err *err) {
    c->sync.root = expand_home_into(&c->arena, c->sync.root);
    if (!*c->sync.root) return err_set(err, ERR_INVALID_ARGUMENT, "sync.root is required");
    int primaries = 0;
    for (size_t i = 0; i < c->store_count; i++) {
        StoreConfig *s = &c->stores[i];
        if (!valid_store_name(s->name)) return err_set(err, ERR_INVALID_ARGUMENT, "store \"%s\": names may only contain a-z, 0-9, _ and -", s->name);
        if (s->role == ROLE_PRIMARY) primaries++;
        if (s->role == ROLE_NONE) return err_set(err, ERR_INVALID_ARGUMENT, "store \"%s\": role is required (primary, mirror or detached)", s->name);
        switch (s->kind) {
        case KIND_S3:
            if (!*s->bucket || !*s->region) return err_set(err, ERR_INVALID_ARGUMENT, "store \"%s\": s3 stores need bucket and region", s->name);
            break;
        case KIND_DISK:
            s->root = expand_home_into(&c->arena, s->root);
            if (!is_dir(s->root)) return err_set(err, ERR_INVALID_ARGUMENT, "store \"%s\": root \"%s\" is not an existing directory", s->name, s->root);
            break;
        default: return err_set(err, ERR_INVALID_ARGUMENT, "store \"%s\": kind must be s3 or disk", s->name);
        }
        if (*s->prefix && !has_suffix(s->prefix, "/")) s->prefix = arena_printf(&c->arena, "%s/", s->prefix);
        if (s->workers <= 0) s->workers = s->role == ROLE_PRIMARY ? 4 : 2;
    }
    if (c->store_count > 0 && primaries != 1) return err_set(err, ERR_INVALID_ARGUMENT, "exactly one store must be primary, found %d", primaries);
    if (c->sync.part_size < 5 << 20) return err_set(err, ERR_INVALID_ARGUMENT, "sync.part_size must be at least 5MiB, the S3 minimum");
    return ERR_OK;
}

Error config_parse(const char *path, const char *text, size_t len, char *const *environ, Config *c, Err *err) {
    static char *const no_env[] = {NULL};
    if (!environ) environ = no_env;
    config_init(c);
    c->path = arena_strdup(&c->arena, path);
    Arena tree;
    arena_init(&tree, 16 * 1024);
    Err inner;
    YmlNode *root;
    Error e = yml_load(&tree, text, len, &root, &inner);
    if (e != ERR_OK) {
        e = err_set(err, e, "%s: %s", path, inner.msg);
        goto out;
    }
    if (root->kind == YML_NULL) root = yml_new_map(&tree);
    Expander x = {.arena = &tree, .environ = environ};
    expand_references(&x, root);
    if (x.missing.len > 0) {
        strlist_sort(&x.missing);
        StrBuf names = {0};
        for (size_t i = 0; i < x.missing.len; i++) sb_printf(&names, "%s%s", i ? ", " : "", x.missing.items[i]);
        e = err_set(err, ERR_INVALID_ARGUMENT, "%s: environment variables not set: %s", path, sb_cstr(&names));
        sb_free(&names);
        strlist_free(&x.missing);
        goto out;
    }
    apply_overrides(&tree, root, environ);
    Decoder d = {.cfg = c, .err = &inner};
    decode(&d, root);
    e = d.failed ? ERR_INVALID_ARGUMENT : finish(c, &inner);
    if (e != ERR_OK) e = err_set(err, e, "%s: %s", path, inner.msg);
out:
    arena_free(&tree);
    if (e != ERR_OK) config_free(c);
    return e;
}

Error config_load(const char *path, Config *c, Err *err) {
    StrBuf raw = {0};
    Error e = read_file(path, &raw, err);
    if (e == ERR_OK) e = config_parse(path, raw.data ? raw.data : "", raw.len, env_all(), c, err);
    sb_free(&raw);
    return e;
}

/* ---- rendering ---- */

static bool looks_special(const char *s) {
    if (!*s) return true;
    if (strchr("*&!|>'\"%@`#-?:[]{},", s[0])) return true;
    if (s[0] == ' ' || s[strlen(s) - 1] == ' ') return true;
    if (strstr(s, ": ") || strstr(s, " #") || has_suffix(s, ":")) return true;
    static const char *words[] = {"true", "false", "null", "~", "yes", "no", "on", "off"};
    for (size_t i = 0; i < countof(words); i++)
        if (strcmp(s, words[i]) == 0) return true;
    char *end;
    strtod(s, &end);
    return *end == '\0';
}

static void yaml_scalar(StrBuf *out, const char *s) {
    if (!looks_special(s)) {
        sb_puts(out, s);
        return;
    }
    sb_putc(out, '\'');
    for (; *s; s++) {
        if (*s == '\'') sb_putc(out, '\'');
        sb_putc(out, *s);
    }
    sb_putc(out, '\'');
}

static void field(StrBuf *out, int indent, const char *key, const char *value) {
    sb_printf(out, "%*s%s: ", indent, "", key);
    yaml_scalar(out, value);
    sb_putc(out, '\n');
}

void config_render(const Config *c, StrBuf *out) {
    char buf[48];
    Arena a;
    arena_init(&a, 4096);
    const char **all = arena_alloc(&a, (c->store_count + 1) * sizeof *all);
    for (size_t i = 0; i < c->store_count; i++) all[i] = c->stores[i].name;
    qsort(all, c->store_count, sizeof *all, compare_strings);
    sb_puts(out, c->store_count ? "stores:\n" : "stores: {}\n");
    for (size_t i = 0; i < c->store_count; i++) {
        const StoreConfig *s = config_store(c, all[i]);
        sb_printf(out, "  %s:\n", s->name);
        field(out, 4, "kind", kind_name(s->kind));
        field(out, 4, "role", role_name(s->role));
        field(out, 4, "bucket", s->bucket);
        field(out, 4, "prefix", s->prefix);
        field(out, 4, "region", s->region);
        field(out, 4, "endpoint", s->endpoint);
        sb_printf(out, "    path_style: %s\n", s->path_style ? "true" : "false");
        field(out, 4, "access_key", *s->access_key ? "***" : "");
        field(out, 4, "secret_key", *s->secret_key ? "***" : "");
        field(out, 4, "storage_class", s->storage_class);
        field(out, 4, "root", s->root);
        sb_printf(out, "    workers: %d\n", s->workers);
    }
    sb_puts(out, "sync:\n");
    field(out, 2, "root", c->sync.root);
    field(out, 2, "pull_interval", format_duration(c->sync.pull_interval_ns, buf));
    field(out, 2, "backfill_interval", format_duration(c->sync.backfill_interval_ns, buf));
    field(out, 2, "debounce", format_duration(c->sync.debounce_ns, buf));
    field(out, 2, "part_size", format_size(c->sync.part_size, buf));
    sb_printf(out, "  delete_remote: %s\n", c->sync.delete_remote ? "true" : "false");
    sb_printf(out, "  delete_local: %s\n", c->sync.delete_local ? "true" : "false");
    if (c->sync.ignore_count == 0) {
        sb_puts(out, "  ignore: []\n");
    } else {
        sb_puts(out, "  ignore:\n");
        for (size_t i = 0; i < c->sync.ignore_count; i++) {
            sb_puts(out, "    - ");
            yaml_scalar(out, c->sync.ignore[i]);
            sb_putc(out, '\n');
        }
    }
    sb_puts(out, "daemon:\n");
    field(out, 2, "log_level", c->daemon.log_level);
    field(out, 2, "log_format", c->daemon.log_format);
    field(out, 2, "listen", c->daemon.listen);
    arena_free(&a);
}

/* ---- promote ---- */

typedef struct {
    const char *name;
    YmlNode *role;
} StoreRoleNode;

Error config_promote(const char *path, const char *store, Err *err) {
    StrBuf raw = {0};
    Error e = read_file(path, &raw, err);
    if (e != ERR_OK) return e;
    Arena a;
    arena_init(&a, 16 * 1024);
    Err inner;
    YmlNode *root;
    e = yml_load(&a, raw.data ? raw.data : "", raw.len, &root, &inner);
    if (e != ERR_OK) {
        e = err_set(err, e, "%s: %s", path, inner.msg);
        goto out;
    }
    YmlNode *stores = yml_get(root, "stores");
    if (!stores || !yml_get(stores, store)) {
        e = err_set(err, ERR_NOT_FOUND, "%s: no store named \"%s\"", path, store);
        goto out;
    }
    /* Edits are applied from the end of the file so earlier offsets stay valid. */
    StrBuf out = {0};
    sb_append(&out, raw.data, raw.len);
    for (size_t i = stores->count; i-- > 0;) {
        YmlNode *role = yml_get(stores->items[i], "role");
        if (!role || role->kind != YML_SCALAR || role->end <= role->start) continue;
        const char *replacement = NULL;
        if (strcmp(stores->keys[i], store) == 0) replacement = role_name(ROLE_PRIMARY);
        else if (strcmp(role->value, "primary") == 0) replacement = role_name(ROLE_MIRROR);
        if (!replacement) continue;
        StrBuf edited = {0};
        sb_append(&edited, out.data, role->start);
        sb_puts(&edited, replacement);
        sb_append(&edited, out.data + role->end, out.len - role->end);
        sb_free(&out);
        out = edited;
    }
    FileStat info;
    e = file_info_follow(path, &info, err);
    if (e == ERR_OK && !info.exists) e = err_set(err, ERR_NOT_FOUND, "%s: no such file", path);
    if (e == ERR_OK) e = write_file_atomic(path, out.data, out.len, info.mode, err);
    sb_free(&out);
out:
    arena_free(&a);
    sb_free(&raw);
    return e;
}
