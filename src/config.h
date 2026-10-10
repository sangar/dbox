#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

/*
 * Config is ~/.config/dbox/config.yml: the stores, their roles and the sync
 * settings. See docs/configuration.md for the format. A Config and every
 * string in it live in the arena it was loaded into.
 */
typedef enum StoreKind { KIND_NONE, KIND_S3, KIND_DISK } StoreKind;
typedef enum StoreRole { ROLE_NONE, ROLE_PRIMARY, ROLE_MIRROR, ROLE_DETACHED } StoreRole;

typedef struct StoreConfig {
    String name;
    StoreKind kind;
    StoreRole role;
    String bucket, prefix, region, endpoint;
    bool path_style;
    String access_key, secret_key, storage_class, root;
    int workers;
} StoreConfig;

typedef struct SyncConfig {
    String root;
    int64_t pull_interval_ns, backfill_interval_ns, debounce_ns;
    int64_t part_size;
    bool delete_remote, delete_local;
    StringList ignore;
} SyncConfig;

typedef struct DaemonConfig {
    String log_level, log_format, listen;
} DaemonConfig;

typedef struct Config {
    StoreConfig *stores;
    size_t store_count, store_capacity;
    SyncConfig sync;
    DaemonConfig daemon;
    String path;
} Config;

/* config_init sets the defaults; config_add_store appends a store, for tests and loading. */
void config_init(Arena *arena, Config *config);
StoreConfig *config_add_store(Arena *arena, Config *config, String name);
StoreConfig *config_store(const Config *config, String name);
/* config_primary is the primary's name, or empty when no store is configured. */
String config_primary(const Config *config);
/* config_mirrors returns the mirror names sorted. */
StringList config_mirrors(Arena *arena, const Config *config);
/* config_store_names returns every name, primary first, then mirrors, then detached, each group sorted. */
StringList config_store_names(Arena *arena, const Config *config);
/* config_state_dir is <root>/.dbox, where the index, temp files and pid file live. */
String config_state_dir(Arena *arena, const Config *config);
const char *role_name(StoreRole role);
const char *kind_name(StoreKind kind);

/* config_load reads the file, expands ${VAR}, applies DBOX_ overrides and defaults, and validates. */
[[nodiscard]] Error config_load(Arena *arena, String path, Config *config, Err *err);
/* config_parse is config_load for text already read, with environment as NAME=value entries. */
[[nodiscard]] Error config_parse(Arena *arena, String path, String text, StringList environment, Config *config, Err *err);
/* config_render writes the effective config as YAML with secrets masked. */
String config_render(Arena *arena, const Config *config);
/* config_default_path is $DBOX_CONFIG, or config.yml under $XDG_CONFIG_HOME/dbox; an existing config.yaml wins over a missing config.yml. */
String config_default_path(Arena *arena);
/* config_write_starter creates a commented config unless the file exists; *created says which. */
[[nodiscard]] Error config_write_starter(String path, bool *created, Err *err);
/* config_promote rewrites the file so store is the primary and the old primary a mirror, keeping comments. */
[[nodiscard]] Error config_promote(Arena *scratch, String path, String store, Err *err);

/* parse_size reads a byte count with an optional KiB, MiB or GiB suffix. */
bool parse_size(String text, int64_t *bytes);
String format_size(Arena *arena, int64_t bytes);
