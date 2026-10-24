#include "config.h"

#include <ctype.h>
#include <stdio.h>

#include "mc/encoding/yaml.h"
#include "mc/platform/platform.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"

/* TREE_BLOCK is the arena block for a config file's parsed tree. */
enum { TREE_BLOCK = 16 * 1024 };

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

static const char *const default_ignore[] = {".git/", ".DS_Store", "*.swp", "*.tmp", "~$*"};

void config_init(Arena *arena, Config *config) {
    *config = (Config){
        .sync =
            {
                .root = S("~/dbox"),
                .pull_interval_ns = 30LL * NS_PER_SECOND,
                .backfill_interval_ns = 10LL * 60 * NS_PER_SECOND,
                .debounce_ns = 750LL * NS_PER_MILLISECOND,
                .part_size = 8 << 20,
                .delete_remote = true,
                .delete_local = true,
                .ignore = strlist_of(arena, countof(default_ignore), default_ignore),
            },
        .daemon = {.log_level = S("info"), .log_format = S("text"), .listen = S("127.0.0.1:7878")},
        .path = S(""),
    };
}

StoreConfig *config_add_store(Arena *arena, Config *config, String name) {
    config->stores = arena_grow(arena, config->stores, &config->store_capacity, config->store_count, sizeof *config->stores);
    StoreConfig *store = &config->stores[config->store_count++];
    *store = (StoreConfig){.name = str_copy(arena, name)};
    return store;
}

StoreConfig *config_store(const Config *config, String name) {
    for (size_t i = 0; i < config->store_count; i++)
        if (str_equal(config->stores[i].name, name)) return &config->stores[i];
    return nullptr;
}

String config_primary(const Config *config) {
    for (size_t i = 0; i < config->store_count; i++)
        if (config->stores[i].role == ROLE_PRIMARY) return config->stores[i].name;
    return S("");
}

static StringList names_with_role(Arena *arena, const Config *config, StoreRole role) {
    StringList names = {0};
    for (size_t i = 0; i < config->store_count; i++)
        if (config->stores[i].role == role) strlist_push(arena, &names, config->stores[i].name);
    strlist_sort(&names);
    return names;
}

StringList config_mirrors(Arena *arena, const Config *config) { return names_with_role(arena, config, ROLE_MIRROR); }

StringList config_store_names(Arena *arena, const Config *config) {
    StringList names = {0};
    static const StoreRole order[] = {ROLE_PRIMARY, ROLE_MIRROR, ROLE_DETACHED};
    for (size_t i = 0; i < countof(order); i++) strlist_push_all(arena, &names, names_with_role(arena, config, order[i]));
    return names;
}

String config_state_dir(Arena *arena, const Config *config) { return path_join(arena, config->sync.root, S(".dbox")); }

const char *role_name(StoreRole role) {
    switch (role) {
    case ROLE_PRIMARY: return "primary";
    case ROLE_MIRROR: return "mirror";
    case ROLE_DETACHED: return "detached";
    default: return "";
    }
}

const char *kind_name(StoreKind kind) {
    switch (kind) {
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

bool parse_size(String text, int64_t *bytes) {
    String digits = str_trim(text);
    int64_t factor = 1;
    for (size_t i = 0; i < countof(size_units); i++) {
        if (str_ends_with(digits, S(size_units[i].suffix))) {
            factor = size_units[i].factor;
            digits = str_trim(str_trim_suffix(digits, S(size_units[i].suffix)));
            break;
        }
    }
    int64_t count;
    if (!str_parse_i64(digits, &count)) return false;
    *bytes = count * factor;
    return true;
}

String format_size(Arena *arena, int64_t bytes) {
    for (size_t i = 0; i < countof(size_units); i++) {
        int64_t factor = size_units[i].factor;
        if (factor > 1 && bytes >= factor && bytes % factor == 0) return str_format(arena, "%lld%s", (long long)(bytes / factor), size_units[i].suffix);
    }
    return str_format(arena, "%lld", (long long)bytes);
}

/* ---- loading ---- */

static bool exists(String path) {
    FileInfo info;
    return file_info_follow(path, &info, nullptr) == ERR_OK && info.exists;
}

String config_default_path(Arena *arena) {
    String value;
    if (env_get(arena, S("DBOX_CONFIG"), &value) && value.len > 0) return value;
    String dir;
    if (!env_get(arena, S("XDG_CONFIG_HOME"), &dir) || dir.len == 0) dir = path_join(arena, env_home(arena), S(".config"));
    String yml = path_join(arena, dir, S("dbox/config.yml"));
    String yaml = path_join(arena, dir, S("dbox/config.yaml"));
    return !exists(yml) && exists(yaml) ? yaml : yml;
}

Error config_write_starter(String path, bool *created, Err *err) {
    *created = false;
    if (exists(path)) return ERR_OK;
    Error e = dir_create_all(path_dir(path), 0755, err);
    if (e != ERR_OK) return e;
    /* Private to the user because it may come to hold store secrets. */
    e = file_write_all(path, (String){starter_yml, sizeof starter_yml - 1}, 0600, err);
    if (e != ERR_OK) return e;
    *created = true;
    return ERR_OK;
}

typedef struct {
    Arena *arena;
    StringList environment;
    StringList missing;
} Expander;

static bool env_value(StringList environment, String name, String *value) {
    for (size_t i = 0; i < environment.count; i++) {
        String key;
        if (str_cut(environment.items[i], '=', &key, value) && str_equal(key, name)) return true;
    }
    return false;
}

static bool name_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool name_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* expand_string replaces ${VAR} references, noting unset names, and reports whether anything changed. */
static bool expand_string(Expander *x, String s, String *out) {
    StringBuilder builder = str_builder_create(x->arena, s.len + 16);
    bool changed = false;
    for (size_t i = 0; i < s.len;) {
        if (i + 2 < s.len && s.data[i] == '$' && s.data[i + 1] == '{' && name_start(s.data[i + 2])) {
            size_t end = i + 2;
            while (end < s.len && name_char(s.data[end])) end++;
            if (end < s.len && s.data[end] == '}') {
                String name = str_slice(s, i + 2, end);
                String value;
                if (env_value(x->environment, name, &value)) str_builder_append(&builder, value);
                else if (!strlist_contains(x->missing, name)) strlist_push(x->arena, &x->missing, name);
                changed = true;
                i = end + 1;
                continue;
            }
        }
        str_builder_append_char(&builder, s.data[i++]);
    }
    if (changed) *out = str_builder_finish(&builder);
    return changed;
}

static bool is_plain_null(String text) {
    return text.len == 0 || str_equal(text, S("~")) || str_equal(text, S("null")) || str_equal(text, S("Null")) || str_equal(text, S("NULL"));
}

/* expand_references rewrites every scalar in the tree; keys and comments are left alone. */
static void expand_references(Expander *x, Node *node) {
    switch (node->kind) {
    case NODE_SEQUENCE:
        for (size_t i = 0; i < node->count; i++) expand_references(x, node->items[i]);
        break;
    case NODE_MAPPING:
        for (size_t i = 0; i < node->entry_count; i++) expand_references(x, node->entries[i].value);
        break;
    default: {
        String expanded;
        if (!expand_string(x, node->text, &expanded)) break;
        node->text = expanded;
        /* An expanded value is read as an unquoted one would be, so a reference to an empty variable is null. */
        node->kind = is_plain_null(expanded) ? NODE_NULL : NODE_STRING;
        break;
    }
    }
}

static Node *child(Node *mapping, String key) {
    for (size_t i = 0; i < mapping->entry_count; i++)
        if (str_equal(mapping->entries[i].key, key)) return mapping->entries[i].value;
    return nullptr;
}

/* apply_overrides sets DBOX_SECTION__KEY=value as tree[section][key]. */
static void apply_overrides(Arena *arena, Node *root, StringList environment) {
    StringList keys = {0};
    for (size_t i = 0; i < environment.count; i++) {
        String key, value;
        if (str_cut(environment.items[i], '=', &key, &value) && str_starts_with(key, S("DBOX_")) && str_contains(key, S("__"))) strlist_push(arena, &keys, key);
    }
    strlist_sort(&keys);
    for (size_t i = 0; i < keys.count; i++) {
        String value = S("");
        env_value(environment, keys.items[i], &value);
        String path = str_lower_ascii(arena, str_trim_prefix(keys.items[i], S("DBOX_")));
        Node *node = root;
        size_t separator;
        while (str_find(path, S("__"), &separator)) {
            String part = str_slice(path, 0, separator);
            Node *next = child(node, part);
            if (!next || next->kind != NODE_MAPPING) {
                next = node_create(arena, NODE_MAPPING);
                node_put(arena, node, part, next);
            }
            node = next;
            path = str_slice(path, separator + 2, path.len);
        }
        node_put(arena, node, path, node_scalar(arena, NODE_STRING, value));
    }
}

/* ---- decoding ---- */

typedef struct {
    Arena *arena; /* the config's */
    Config *config;
    Err *err;
    bool failed;
} Decoder;

[[gnu::format(printf, 3, 4)]] static void decode_fail(Decoder *d, size_t line, const char *format, ...) {
    if (d->failed) return;
    d->failed = true;
    char message[400];
    va_list args;
    va_start(args, format);
    /* clang-analyzer does not model C23's one-argument va_start; 22 names the check security.VAList. */
    vsnprintf(message, sizeof message, format, args); // NOLINT(clang-analyzer-valist.Uninitialized,clang-analyzer-security.VAList)
    va_end(args);
    if (line > 0) (void)err_set(d->err, ERR_INVALID_ARGUMENT, "line %zu: %s", line, message);
    else (void)err_set(d->err, ERR_INVALID_ARGUMENT, "%s", message);
}

static bool is_null(const Node *node) { return !node || node->kind == NODE_NULL; }

static String scalar(Decoder *d, const Node *node, String key) {
    if (is_null(node)) return S("");
    if (node->kind == NODE_SEQUENCE || node->kind == NODE_MAPPING) {
        decode_fail(d, node->line, "%.*s: expected a single value", (int)key.len, key.data);
        return S("");
    }
    return node->text;
}

static String string_field(Decoder *d, const Node *node, String key) { return str_copy(d->arena, scalar(d, node, key)); }

static bool bool_field(Decoder *d, const Node *node, String key, bool current) {
    String value = scalar(d, node, key);
    if (str_equal(value, S("true"))) return true;
    if (str_equal(value, S("false"))) return false;
    decode_fail(d, node->line, "%.*s: cannot read %.*s as a bool", (int)key.len, key.data, (int)value.len, value.data);
    return current;
}

static int64_t int_field(Decoder *d, const Node *node, String key, int64_t current) {
    String value = scalar(d, node, key);
    int64_t number;
    if (str_parse_i64(str_trim(value), &number)) return number;
    decode_fail(d, node->line, "%.*s: cannot read %.*s as a number", (int)key.len, key.data, (int)value.len, value.data);
    return current;
}

static int64_t duration_field(Decoder *d, const Node *node, String key, int64_t current) {
    String value = scalar(d, node, key);
    int64_t ns;
    if (fmt_parse_duration(value, &ns)) return ns;
    decode_fail(d, node->line, "%.*s: invalid duration %.*s", (int)key.len, key.data, (int)value.len, value.data);
    return current;
}

static int64_t size_field(Decoder *d, const Node *node, String key, int64_t current) {
    String value = scalar(d, node, key);
    int64_t bytes;
    if (parse_size(value, &bytes)) return bytes;
    decode_fail(d, node->line, "%.*s: size %.*s: want a number with an optional KiB, MiB or GiB suffix", (int)key.len, key.data, (int)value.len,
                value.data);
    return current;
}

static StringList list_field(Decoder *d, const Node *node, String key, StringList current) {
    if (is_null(node)) return (StringList){0};
    if (node->kind != NODE_SEQUENCE) {
        decode_fail(d, node->line, "%.*s: expected a list", (int)key.len, key.data);
        return current;
    }
    StringList items = {0};
    for (size_t i = 0; i < node->count; i++) strlist_push(d->arena, &items, string_field(d, node->items[i], key));
    return items;
}

static bool expect_map(Decoder *d, const Node *node, String key) {
    if (node->kind == NODE_MAPPING) return true;
    decode_fail(d, node->line, "%.*s: expected a mapping", (int)key.len, key.data);
    return false;
}

static StoreKind parse_kind(String kind) {
    if (str_equal(kind, S("s3"))) return KIND_S3;
    if (str_equal(kind, S("disk"))) return KIND_DISK;
    return KIND_NONE;
}

static StoreRole parse_role(String role) {
    if (str_equal(role, S("primary"))) return ROLE_PRIMARY;
    if (str_equal(role, S("mirror"))) return ROLE_MIRROR;
    if (str_equal(role, S("detached"))) return ROLE_DETACHED;
    return ROLE_NONE;
}

static void decode_store(Decoder *d, String name, const Node *node, size_t line) {
    StoreConfig *s = config_add_store(d->arena, d->config, name);
    if (is_null(node) || !expect_map(d, node, name)) return;
    for (size_t i = 0; i < node->entry_count && !d->failed; i++) {
        String k = node->entries[i].key;
        const Node *v = node->entries[i].value;
        if (str_equal(k, S("kind"))) {
            s->kind = parse_kind(scalar(d, v, k));
            if (s->kind == KIND_NONE) decode_fail(d, 0, "store \"%.*s\": kind must be s3 or disk", (int)name.len, name.data);
        } else if (str_equal(k, S("role"))) {
            String role = scalar(d, v, k);
            s->role = parse_role(role);
            if (s->role == ROLE_NONE && role.len > 0) decode_fail(d, 0, "store \"%.*s\": unknown role \"%.*s\"", (int)name.len, name.data, (int)role.len, role.data);
        } else if (str_equal(k, S("bucket"))) s->bucket = string_field(d, v, k);
        else if (str_equal(k, S("prefix"))) s->prefix = string_field(d, v, k);
        else if (str_equal(k, S("region"))) s->region = string_field(d, v, k);
        else if (str_equal(k, S("endpoint"))) s->endpoint = string_field(d, v, k);
        else if (str_equal(k, S("path_style"))) s->path_style = bool_field(d, v, k, false);
        else if (str_equal(k, S("access_key"))) s->access_key = string_field(d, v, k);
        else if (str_equal(k, S("secret_key"))) s->secret_key = string_field(d, v, k);
        else if (str_equal(k, S("storage_class"))) s->storage_class = string_field(d, v, k);
        else if (str_equal(k, S("root"))) s->root = string_field(d, v, k);
        else if (str_equal(k, S("workers"))) s->workers = (int)int_field(d, v, k, 0);
        else decode_fail(d, line, "field %.*s not found in store %.*s", (int)k.len, k.data, (int)name.len, name.data);
    }
}

static void decode_sync(Decoder *d, const Node *node) {
    SyncConfig *s = &d->config->sync;
    if (is_null(node) || !expect_map(d, node, S("sync"))) return;
    for (size_t i = 0; i < node->entry_count && !d->failed; i++) {
        String k = node->entries[i].key;
        const Node *v = node->entries[i].value;
        if (str_equal(k, S("root"))) s->root = string_field(d, v, k);
        else if (str_equal(k, S("pull_interval"))) s->pull_interval_ns = duration_field(d, v, k, s->pull_interval_ns);
        else if (str_equal(k, S("backfill_interval"))) s->backfill_interval_ns = duration_field(d, v, k, s->backfill_interval_ns);
        else if (str_equal(k, S("debounce"))) s->debounce_ns = duration_field(d, v, k, s->debounce_ns);
        else if (str_equal(k, S("part_size"))) s->part_size = size_field(d, v, k, s->part_size);
        else if (str_equal(k, S("delete_remote"))) s->delete_remote = bool_field(d, v, k, s->delete_remote);
        else if (str_equal(k, S("delete_local"))) s->delete_local = bool_field(d, v, k, s->delete_local);
        else if (str_equal(k, S("ignore"))) s->ignore = list_field(d, v, k, s->ignore);
        else decode_fail(d, node->line, "field %.*s not found in sync", (int)k.len, k.data);
    }
}

static void decode_daemon(Decoder *d, const Node *node) {
    DaemonConfig *daemon = &d->config->daemon;
    if (is_null(node) || !expect_map(d, node, S("daemon"))) return;
    for (size_t i = 0; i < node->entry_count && !d->failed; i++) {
        String k = node->entries[i].key;
        const Node *v = node->entries[i].value;
        if (str_equal(k, S("log_level"))) daemon->log_level = string_field(d, v, k);
        else if (str_equal(k, S("log_format"))) daemon->log_format = string_field(d, v, k);
        else if (str_equal(k, S("listen"))) daemon->listen = string_field(d, v, k);
        else decode_fail(d, node->line, "field %.*s not found in daemon", (int)k.len, k.data);
    }
}

static void decode(Decoder *d, const Node *root) {
    if (!expect_map(d, root, S("config"))) return;
    for (size_t i = 0; i < root->entry_count && !d->failed; i++) {
        String k = root->entries[i].key;
        const Node *v = root->entries[i].value;
        if (str_equal(k, S("stores"))) {
            if (is_null(v)) continue;
            if (!expect_map(d, v, k)) return;
            for (size_t j = 0; j < v->entry_count && !d->failed; j++) decode_store(d, v->entries[j].key, v->entries[j].value, v->line);
        } else if (str_equal(k, S("sync"))) decode_sync(d, v);
        else if (str_equal(k, S("daemon"))) decode_daemon(d, v);
        else decode_fail(d, root->line, "field %.*s not found in config", (int)k.len, k.data);
    }
}

/* ---- validation ---- */

static bool valid_store_name(String name) {
    if (name.len == 0) return false;
    for (size_t i = 0; i < name.len; i++) {
        char c = name.data[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '_' || c == '-')) return false;
    }
    return true;
}

static bool is_directory(String path) {
    FileInfo info;
    return file_info_follow(path, &info, nullptr) == ERR_OK && info.is_dir;
}

[[nodiscard]] static Error finish(Arena *arena, Config *c, Err *err) {
    String home = env_home(arena);
    c->sync.root = path_expand_home(arena, c->sync.root, home);
    if (c->sync.root.len == 0) return err_set(err, ERR_INVALID_ARGUMENT, "sync.root is required");
    int primaries = 0;
    for (size_t i = 0; i < c->store_count; i++) {
        StoreConfig *s = &c->stores[i];
        int n = (int)s->name.len;
        const char *name = s->name.data;
        if (!valid_store_name(s->name)) return err_set(err, ERR_INVALID_ARGUMENT, "store \"%.*s\": names may only contain a-z, 0-9, _ and -", n, name);
        if (s->role == ROLE_PRIMARY) primaries++;
        if (s->role == ROLE_NONE) return err_set(err, ERR_INVALID_ARGUMENT, "store \"%.*s\": role is required (primary, mirror or detached)", n, name);
        switch (s->kind) {
        case KIND_S3:
            if (s->bucket.len == 0 || s->region.len == 0) return err_set(err, ERR_INVALID_ARGUMENT, "store \"%.*s\": s3 stores need bucket and region", n, name);
            break;
        case KIND_DISK:
            s->root = path_expand_home(arena, s->root, home);
            if (!is_directory(s->root))
                return err_set(err, ERR_INVALID_ARGUMENT, "store \"%.*s\": root \"%.*s\" is not an existing directory", n, name, (int)s->root.len, s->root.data);
            break;
        default: return err_set(err, ERR_INVALID_ARGUMENT, "store \"%.*s\": kind must be s3 or disk", n, name);
        }
        if (s->prefix.len > 0 && !str_ends_with(s->prefix, S("/"))) s->prefix = str_concat(arena, s->prefix, S("/"));
        if (s->workers <= 0) s->workers = s->role == ROLE_PRIMARY ? 4 : 2;
    }
    if (c->store_count > 0 && primaries != 1) return err_set(err, ERR_INVALID_ARGUMENT, "exactly one store must be primary, found %d", primaries);
    if (c->sync.part_size < 5 << 20) return err_set(err, ERR_INVALID_ARGUMENT, "sync.part_size must be at least 5MiB, the S3 minimum");
    return ERR_OK;
}

static String names_joined(Arena *arena, StringList names) {
    strlist_sort(&names);
    return str_join(arena, names, S(", "));
}

Error config_parse(Arena *arena, String path, String text, StringList environment, Config *config, Err *err) {
    config_init(arena, config);
    config->path = str_copy(arena, path);
    int n = (int)path.len;
    Arena *tree = arena_create(TREE_BLOCK);
    Err inner;
    Node *root;
    Error e = yaml_parse(tree, text, &root, &inner);
    if (e != ERR_OK) {
        e = err_set(err, ERR_INVALID_ARGUMENT, "%.*s: %s", n, path.data, inner.msg);
        goto out;
    }
    if (root->kind == NODE_NULL) root = node_create(tree, NODE_MAPPING);
    Expander x = {.arena = tree, .environment = environment};
    expand_references(&x, root);
    if (x.missing.count > 0) {
        String names = names_joined(tree, x.missing);
        e = err_set(err, ERR_INVALID_ARGUMENT, "%.*s: environment variables not set: %.*s", n, path.data, (int)names.len, names.data);
        goto out;
    }
    apply_overrides(tree, root, environment);
    Decoder d = {.arena = arena, .config = config, .err = &inner};
    decode(&d, root);
    e = d.failed ? ERR_INVALID_ARGUMENT : finish(arena, config, &inner);
    if (e != ERR_OK) e = err_set(err, e, "%.*s: %s", n, path.data, inner.msg);
out:
    arena_destroy(tree);
    return e;
}

Error config_load(Arena *arena, String path, Config *config, Err *err) {
    String raw;
    Error e = file_read_all(arena, path, &raw, err);
    if (e == ERR_NOT_FOUND) return err_set(err, e, "%.*s: no such file", (int)path.len, path.data);
    if (e != ERR_OK) return e;
    return config_parse(arena, path, raw, env_all(arena), config, err);
}

/* ---- rendering ---- */

static bool looks_special(String s) {
    if (s.len == 0) return true;
    if (str_contains_any(str_slice(s, 0, 1), S("*&!|>'\"%@`#-?:[]{},"))) return true;
    if (s.data[0] == ' ' || s.data[s.len - 1] == ' ') return true;
    if (str_contains(s, S(": ")) || str_contains(s, S(" #")) || str_ends_with(s, S(":"))) return true;
    static const char *const words[] = {"true", "false", "null", "~", "yes", "no", "on", "off"};
    for (size_t i = 0; i < countof(words); i++)
        if (str_equal(s, S(words[i]))) return true;
    double number;
    return float_parse(s, &number);
}

static void yaml_scalar(StringBuilder *out, String s) {
    if (!looks_special(s)) {
        str_builder_append(out, s);
        return;
    }
    str_builder_append_char(out, '\'');
    for (size_t i = 0; i < s.len; i++) {
        if (s.data[i] == '\'') str_builder_append_char(out, '\'');
        str_builder_append_char(out, s.data[i]);
    }
    str_builder_append_char(out, '\'');
}

static void field(StringBuilder *out, int indent, const char *key, String value) {
    str_builder_append_format(out, "%*s%s: ", indent, "", key);
    yaml_scalar(out, value);
    str_builder_append_char(out, '\n');
}

static String masked(String secret) { return secret.len > 0 ? S("***") : S(""); }

String config_render(Arena *arena, const Config *c) {
    StringBuilder out = str_builder_create(arena, 1024);
    StringList names = {0};
    for (size_t i = 0; i < c->store_count; i++) strlist_push(arena, &names, c->stores[i].name);
    strlist_sort(&names);
    str_builder_append(&out, c->store_count ? S("stores:\n") : S("stores: {}\n"));
    for (size_t i = 0; i < names.count; i++) {
        const StoreConfig *s = config_store(c, names.items[i]);
        str_builder_append_format(&out, "  %.*s:\n", (int)s->name.len, s->name.data);
        field(&out, 4, "kind", S(kind_name(s->kind)));
        field(&out, 4, "role", S(role_name(s->role)));
        field(&out, 4, "bucket", s->bucket);
        field(&out, 4, "prefix", s->prefix);
        field(&out, 4, "region", s->region);
        field(&out, 4, "endpoint", s->endpoint);
        str_builder_append_format(&out, "    path_style: %s\n", s->path_style ? "true" : "false");
        field(&out, 4, "access_key", masked(s->access_key));
        field(&out, 4, "secret_key", masked(s->secret_key));
        field(&out, 4, "storage_class", s->storage_class);
        field(&out, 4, "root", s->root);
        str_builder_append_format(&out, "    workers: %d\n", s->workers);
    }
    str_builder_append(&out, S("sync:\n"));
    field(&out, 2, "root", c->sync.root);
    field(&out, 2, "pull_interval", fmt_duration(arena, c->sync.pull_interval_ns));
    field(&out, 2, "backfill_interval", fmt_duration(arena, c->sync.backfill_interval_ns));
    field(&out, 2, "debounce", fmt_duration(arena, c->sync.debounce_ns));
    field(&out, 2, "part_size", format_size(arena, c->sync.part_size));
    str_builder_append_format(&out, "  delete_remote: %s\n", c->sync.delete_remote ? "true" : "false");
    str_builder_append_format(&out, "  delete_local: %s\n", c->sync.delete_local ? "true" : "false");
    if (c->sync.ignore.count == 0) {
        str_builder_append(&out, S("  ignore: []\n"));
    } else {
        str_builder_append(&out, S("  ignore:\n"));
        for (size_t i = 0; i < c->sync.ignore.count; i++) {
            str_builder_append(&out, S("    - "));
            yaml_scalar(&out, c->sync.ignore.items[i]);
            str_builder_append_char(&out, '\n');
        }
    }
    str_builder_append(&out, S("daemon:\n"));
    field(&out, 2, "log_level", c->daemon.log_level);
    field(&out, 2, "log_format", c->daemon.log_format);
    field(&out, 2, "listen", c->daemon.listen);
    return str_builder_finish(&out);
}

/* ---- promote ---- */

/* replaced is text with length bytes at offset replaced by replacement. */
static String replaced(Arena *arena, String text, size_t offset, size_t length, String replacement) {
    StringBuilder out = str_builder_create(arena, text.len + replacement.len);
    str_builder_append(&out, str_slice(text, 0, offset));
    str_builder_append(&out, replacement);
    str_builder_append(&out, str_slice(text, offset + length, text.len));
    return str_builder_finish(&out);
}

Error config_promote(Arena *scratch, String path, String store, Err *err) {
    int n = (int)path.len;
    String raw;
    Error e = file_read_all(scratch, path, &raw, err);
    if (e == ERR_NOT_FOUND) return err_set(err, e, "%.*s: no such file", n, path.data);
    if (e != ERR_OK) return e;
    Err inner;
    Node *root;
    if (yaml_parse(scratch, raw, &root, &inner) != ERR_OK) return err_set(err, ERR_INVALID_ARGUMENT, "%.*s: %s", n, path.data, inner.msg);
    const Node *stores = node_get(root, S("stores"));
    if (!node_get(stores, store)) return err_set(err, ERR_NOT_FOUND, "%.*s: no store named \"%.*s\"", n, path.data, (int)store.len, store.data);
    /* Edits are applied from the end of the file so earlier offsets stay valid. */
    String out = raw;
    for (size_t i = stores->entry_count; i-- > 0;) {
        const Node *role = node_get(stores->entries[i].value, S("role"));
        if (!role || role->length == 0 || role->kind == NODE_SEQUENCE || role->kind == NODE_MAPPING) continue;
        String replacement = S("");
        if (str_equal(stores->entries[i].key, store)) replacement = S(role_name(ROLE_PRIMARY));
        else if (str_equal(role->text, S("primary"))) replacement = S(role_name(ROLE_MIRROR));
        if (replacement.len > 0) out = replaced(scratch, out, role->offset, role->length, replacement);
    }
    FileInfo info;
    if ((e = file_info_follow(path, &info, err)) != ERR_OK) return e;
    if (!info.exists) return err_set(err, ERR_NOT_FOUND, "%.*s: no such file", n, path.data);
    return file_write_atomic(path, out, info.mode, err);
}
