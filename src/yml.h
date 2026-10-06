#ifndef DBOX_YML_H
#define DBOX_YML_H

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"
#include "util.h"

/*
 * YmlNode is a YAML document read into a tree with libyaml. Scalars keep
 * their text, whether they were quoted, and their byte range in the source,
 * so a value can be rewritten in place without disturbing comments.
 */
typedef enum { YML_NULL, YML_SCALAR, YML_MAP, YML_SEQ } YmlKind;

typedef struct YmlNode {
    YmlKind kind;
    int line;          /* 1-based */
    size_t start, end; /* byte offsets of a scalar in the source */
    char *value;       /* scalars */
    bool quoted;
    size_t count, cap;
    char **keys; /* maps */
    struct YmlNode **items;
} YmlNode;

/* yml_load parses text into *root, allocated in a. */
[[nodiscard]] Error yml_load(Arena *a, const char *text, size_t len, YmlNode **root, Err *err);
YmlNode *yml_get(const YmlNode *map, const char *key);
YmlNode *yml_new_map(Arena *a);
YmlNode *yml_new_scalar(Arena *a, const char *value, bool quoted);
/* yml_set stores value under key in map, replacing an existing entry. */
void yml_set(Arena *a, YmlNode *map, const char *key, YmlNode *value);
bool yml_is_null(const YmlNode *n);

#endif
