#include "arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define ARENA_ALIGN 16

static ArenaBlock *new_block(size_t capacity) {
    ArenaBlock *b = xmalloc(sizeof *b + capacity);
    b->next = NULL;
    b->capacity = capacity;
    b->offset = 0;
    return b;
}

void arena_init(Arena *a, size_t block_size) {
    a->head = NULL;
    a->block_size = block_size ? block_size : 64 * 1024;
}

void *arena_alloc(Arena *a, size_t size) {
    size = (size + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    ArenaBlock *b = a->head;
    if (!b || b->offset + size > b->capacity) {
        b = new_block(max_size(a->block_size, size));
        b->next = a->head;
        a->head = b;
    }
    void *p = b->base + b->offset;
    b->offset += size;
    return p;
}

void *arena_calloc(Arena *a, size_t count, size_t size) {
    void *p = arena_alloc(a, count * size);
    memset(p, 0, count * size);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *arena_strdup(Arena *a, const char *s) { return arena_strndup(a, s, strlen(s)); }

char *arena_printf(Arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *p = arena_alloc(a, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}
