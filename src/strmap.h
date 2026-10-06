#ifndef DBOX_STRMAP_H
#define DBOX_STRMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * StrMap maps strings to pointers with open addressing. It owns copies of
 * its keys and never touches the values.
 */
typedef struct {
    char *key;
    void *value;
    uint64_t hash;
} StrMapEntry;

typedef struct {
    StrMapEntry *slots;
    size_t cap;  /* power of two, or 0 */
    size_t used; /* live entries plus tombstones */
    size_t live;
} StrMap;

void strmap_init(StrMap *m);
void strmap_free(StrMap *m);
size_t strmap_count(const StrMap *m);
bool strmap_has(const StrMap *m, const char *key);
void *strmap_get(const StrMap *m, const char *key);
/* strmap_put stores value under key, replacing an earlier value. */
void strmap_put(StrMap *m, const char *key, void *value);
/* strmap_remove drops key and reports whether it was present; *old receives its value. */
bool strmap_remove(StrMap *m, const char *key, void **old);
void strmap_clear(StrMap *m);

typedef struct {
    size_t i;
} StrMapIter;

/* strmap_next walks the live entries in no particular order. */
bool strmap_next(const StrMap *m, StrMapIter *it, const char **key, void **value);

uint64_t hash_string(const char *s);

#endif
