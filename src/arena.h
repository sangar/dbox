#ifndef DBOX_ARENA_H
#define DBOX_ARENA_H

#include <stddef.h>
#include <stdint.h>

/*
 * An Arena hands out memory for objects that die together: a parsed config,
 * one store listing, the rows of one index query. arena_free discards
 * everything at once.
 */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t capacity;
    size_t offset;
    uint8_t base[];
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
    size_t block_size;
} Arena;

void arena_init(Arena *a, size_t block_size);
void *arena_alloc(Arena *a, size_t size);
void *arena_calloc(Arena *a, size_t count, size_t size);
char *arena_strndup(Arena *a, const char *s, size_t n);
char *arena_strdup(Arena *a, const char *s);
char *arena_printf(Arena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void arena_free(Arena *a);

#endif
