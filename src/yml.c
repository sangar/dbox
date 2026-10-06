#include "yml.h"

#include <string.h>
#include <yaml.h>

typedef struct {
    yaml_parser_t parser;
    Arena *arena;
    Err *err;
    const char *source;
} Loader;

static YmlNode *new_node(Arena *a, YmlKind kind) {
    YmlNode *n = arena_calloc(a, 1, sizeof *n);
    n->kind = kind;
    return n;
}

YmlNode *yml_new_map(Arena *a) { return new_node(a, YML_MAP); }

YmlNode *yml_new_scalar(Arena *a, const char *value, bool quoted) {
    YmlNode *n = new_node(a, YML_SCALAR);
    n->value = arena_strdup(a, value);
    n->quoted = quoted;
    return n;
}

static void push_item(Arena *a, YmlNode *n, char *key, YmlNode *item) {
    if (n->count == n->cap) {
        size_t cap = n->cap ? n->cap * 2 : 8;
        YmlNode **items = arena_alloc(a, cap * sizeof *items);
        char **keys = arena_alloc(a, cap * sizeof *keys);
        if (n->count) {
            memcpy(items, n->items, n->count * sizeof *items);
            memcpy(keys, n->keys, n->count * sizeof *keys);
        }
        n->items = items;
        n->keys = keys;
        n->cap = cap;
    }
    n->keys[n->count] = key;
    n->items[n->count++] = item;
}

YmlNode *yml_get(const YmlNode *map, const char *key) {
    if (!map || map->kind != YML_MAP) return NULL;
    for (size_t i = 0; i < map->count; i++)
        if (strcmp(map->keys[i], key) == 0) return map->items[i];
    return NULL;
}

void yml_set(Arena *a, YmlNode *map, const char *key, YmlNode *value) {
    for (size_t i = 0; i < map->count; i++) {
        if (strcmp(map->keys[i], key) == 0) {
            map->items[i] = value;
            return;
        }
    }
    push_item(a, map, arena_strdup(a, key), value);
}

static bool plain_null(const char *v) {
    return strcmp(v, "") == 0 || strcmp(v, "~") == 0 || strcmp(v, "null") == 0 || strcmp(v, "Null") == 0 || strcmp(v, "NULL") == 0;
}

bool yml_is_null(const YmlNode *n) { return !n || n->kind == YML_NULL || (n->kind == YML_SCALAR && !n->quoted && plain_null(n->value)); }

static bool next_event(Loader *l, yaml_event_t *ev) {
    if (yaml_parser_parse(&l->parser, ev)) return true;
    err_set(l->err, "line %d: %s", (int)l->parser.problem_mark.line + 1, l->parser.problem ? l->parser.problem : "invalid yaml");
    return false;
}

static YmlNode *build(Loader *l, yaml_event_t *ev);

static YmlNode *build_collection(Loader *l, YmlKind kind, int line) {
    YmlNode *n = new_node(l->arena, kind);
    n->line = line;
    for (;;) {
        yaml_event_t ev;
        if (!next_event(l, &ev)) return NULL;
        if (ev.type == YAML_MAPPING_END_EVENT || ev.type == YAML_SEQUENCE_END_EVENT) {
            yaml_event_delete(&ev);
            return n;
        }
        if (kind == YML_SEQ) {
            YmlNode *item = build(l, &ev);
            if (!item) return NULL;
            push_item(l->arena, n, NULL, item);
            continue;
        }
        if (ev.type != YAML_SCALAR_EVENT) {
            err_set(l->err, "line %d: mapping keys must be scalars", (int)ev.start_mark.line + 1);
            yaml_event_delete(&ev);
            return NULL;
        }
        char *key = arena_strndup(l->arena, (const char *)ev.data.scalar.value, ev.data.scalar.length);
        yaml_event_delete(&ev);
        if (!next_event(l, &ev)) return NULL;
        YmlNode *value = build(l, &ev);
        if (!value) return NULL;
        push_item(l->arena, n, key, value);
    }
}

/* build turns the event ev, which the caller fetched, and its children into a node. */
static YmlNode *build(Loader *l, yaml_event_t *ev) {
    YmlNode *n = NULL;
    int line = (int)ev->start_mark.line + 1;
    switch (ev->type) {
    case YAML_SCALAR_EVENT:
        n = new_node(l->arena, YML_SCALAR);
        n->line = line;
        n->start = ev->start_mark.index;
        n->end = ev->end_mark.index;
        n->value = arena_strndup(l->arena, (const char *)ev->data.scalar.value, ev->data.scalar.length);
        n->quoted = ev->data.scalar.style != YAML_PLAIN_SCALAR_STYLE;
        if (yml_is_null(n)) n->kind = YML_NULL;
        break;
    case YAML_MAPPING_START_EVENT:
        yaml_event_delete(ev);
        return build_collection(l, YML_MAP, line);
    case YAML_SEQUENCE_START_EVENT:
        yaml_event_delete(ev);
        return build_collection(l, YML_SEQ, line);
    case YAML_ALIAS_EVENT: err_set(l->err, "line %d: aliases are not supported", line); break;
    default: err_set(l->err, "line %d: unexpected yaml structure", line); break;
    }
    yaml_event_delete(ev);
    return n;
}

YmlNode *yml_load(Arena *a, const char *text, size_t len, Err *err) {
    Loader l = {.arena = a, .err = err, .source = text};
    yaml_parser_initialize(&l.parser);
    yaml_parser_set_input_string(&l.parser, (const unsigned char *)text, len);
    YmlNode *root = NULL;
    bool ok = false;
    yaml_event_t ev;
    if (!next_event(&l, &ev)) goto out;
    yaml_event_delete(&ev);
    if (!next_event(&l, &ev)) goto out;
    if (ev.type == YAML_STREAM_END_EVENT) {
        yaml_event_delete(&ev);
        root = new_node(a, YML_NULL);
        ok = true;
        goto out;
    }
    yaml_event_delete(&ev);
    if (!next_event(&l, &ev)) goto out;
    root = build(&l, &ev);
    if (!root) goto out;
    if (!next_event(&l, &ev)) goto out;
    yaml_event_delete(&ev);
    ok = true;
out:
    yaml_parser_delete(&l.parser);
    return ok ? root : NULL;
}
