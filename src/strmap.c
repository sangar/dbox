#include "strmap.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static char tombstone_marker;
#define TOMBSTONE (&tombstone_marker)

uint64_t hash_string(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

void strmap_init(StrMap *m) { *m = (StrMap){0}; }

void strmap_clear(StrMap *m) {
    for (size_t i = 0; i < m->cap; i++)
        if (m->slots[i].key && m->slots[i].key != TOMBSTONE) free(m->slots[i].key);
    if (m->slots) memset(m->slots, 0, m->cap * sizeof *m->slots);
    m->used = m->live = 0;
}

void strmap_free(StrMap *m) {
    strmap_clear(m);
    free(m->slots);
    *m = (StrMap){0};
}

size_t strmap_count(const StrMap *m) { return m->live; }

static StrMapEntry *find_slot(const StrMap *m, const char *key, uint64_t h) {
    if (m->cap == 0) return NULL;
    size_t mask = m->cap - 1;
    for (size_t i = h & mask;; i = (i + 1) & mask) {
        StrMapEntry *e = &m->slots[i];
        if (!e->key) return NULL;
        if (e->key != TOMBSTONE && e->hash == h && strcmp(e->key, key) == 0) return e;
    }
}

bool strmap_has(const StrMap *m, const char *key) { return find_slot(m, key, hash_string(key)) != NULL; }

void *strmap_get(const StrMap *m, const char *key) {
    StrMapEntry *e = find_slot(m, key, hash_string(key));
    return e ? e->value : NULL;
}

static void insert_fresh(StrMap *m, char *key, void *value, uint64_t h) {
    size_t mask = m->cap - 1;
    for (size_t i = h & mask;; i = (i + 1) & mask) {
        StrMapEntry *e = &m->slots[i];
        if (!e->key || e->key == TOMBSTONE) {
            if (!e->key) m->used++;
            *e = (StrMapEntry){key, value, h};
            m->live++;
            return;
        }
    }
}

static void grow(StrMap *m) {
    StrMapEntry *old = m->slots;
    size_t old_cap = m->cap;
    m->cap = m->cap ? (m->live * 2 >= m->cap ? m->cap * 2 : m->cap) : 16;
    m->slots = xcalloc(m->cap, sizeof *m->slots);
    m->used = m->live = 0;
    for (size_t i = 0; i < old_cap; i++)
        if (old[i].key && old[i].key != TOMBSTONE) insert_fresh(m, old[i].key, old[i].value, old[i].hash);
    free(old);
}

void strmap_put(StrMap *m, const char *key, void *value) {
    uint64_t h = hash_string(key);
    StrMapEntry *e = find_slot(m, key, h);
    if (e) {
        e->value = value;
        return;
    }
    if ((m->used + 1) * 10 > m->cap * 7) grow(m);
    insert_fresh(m, xstrdup(key), value, h);
}

bool strmap_remove(StrMap *m, const char *key, void **old) {
    StrMapEntry *e = find_slot(m, key, hash_string(key));
    if (!e) return false;
    if (old) *old = e->value;
    free(e->key);
    e->key = TOMBSTONE;
    e->value = NULL;
    m->live--;
    return true;
}

bool strmap_next(const StrMap *m, StrMapIter *it, const char **key, void **value) {
    for (; it->i < m->cap; it->i++) {
        StrMapEntry *e = &m->slots[it->i];
        if (e->key && e->key != TOMBSTONE) {
            *key = e->key;
            if (value) *value = e->value;
            it->i++;
            return true;
        }
    }
    return false;
}
