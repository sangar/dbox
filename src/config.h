#ifndef DBOX_CONFIG_H
#define DBOX_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "util.h"

/*
 * Config is ~/.config/dbox/config.yml: the stores, their roles and the sync
 * settings. See docs/configuration.md in the Go version for the format.
 */
typedef enum { KIND_NONE, KIND_S3, KIND_DISK } StoreKind;
typedef enum { ROLE_NONE, ROLE_PRIMARY, ROLE_MIRROR, ROLE_DETACHED } StoreRole;

typedef struct {
    const char *name;
    StoreKind kind;
    StoreRole role;
    const char *bucket, *prefix, *region, *endpoint;
    bool path_style;
    const char *access_key, *secret_key, *storage_class, *root;
    int workers;
} StoreConfig;

typedef struct {
    const char *root;
    int64_t pull_interval_ns, backfill_interval_ns, debounce_ns;
    int64_t part_size;
    bool delete_remote, delete_local;
    const char **ignore;
    size_t ignore_count;
} SyncConfig;

typedef struct {
    const char *log_level, *log_format, *listen;
} DaemonConfig;

typedef struct {
    Arena arena; /* owns every string below */
    StoreConfig *stores;
    size_t store_count, store_cap;
    SyncConfig sync;
    DaemonConfig daemon;
    const char *path;
} Config;

/* config_init sets the defaults; config_add_store appends a store, for tests and loading. */
void config_init(Config *c);
void config_free(Config *c);
StoreConfig *config_add_store(Config *c, const char *name);
StoreConfig *config_store(const Config *c, const char *name);
const char *config_primary(const Config *c);
/* config_mirrors returns the mirror names sorted, allocated in a. */
const char **config_mirrors(const Config *c, Arena *a, size_t *count);
/* config_store_names returns every name, primary first, then mirrors, then detached, each group sorted. */
const char **config_store_names(const Config *c, Arena *a, size_t *count);
/* config_state_dir is <root>/.dbox, where the index, temp files and pid file live. */
char *config_state_dir(const Config *c);
const char *role_name(StoreRole r);
const char *kind_name(StoreKind k);

/* config_load reads the file, expands ${VAR}, applies DBOX_ overrides and defaults, and validates. */
bool config_load(const char *path, Config *c, Err *err);
bool config_parse(const char *path, const char *text, size_t len, char *const *environ, Config *c, Err *err);
/* config_render writes the effective config as YAML with secrets masked. */
void config_render(const Config *c, StrBuf *out);
/* config_default_path is $DBOX_CONFIG, or config.yml under $XDG_CONFIG_HOME/dbox; an existing config.yaml wins over a missing config.yml. */
char *config_default_path(void);
/* config_write_starter creates a commented config unless the file exists; *created says which. */
bool config_write_starter(const char *path, bool *created, Err *err);
/* config_promote rewrites the file so store is the primary and the old primary a mirror, keeping comments. */
bool config_promote(const char *path, const char *store, Err *err);

bool parse_size(const char *s, int64_t *out, Err *err);
const char *format_size(int64_t n, char buf[32]);

#endif
