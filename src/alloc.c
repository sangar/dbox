#include "alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void out_of_memory(void) {
    fputs("dbox: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void xfree(void *ptr) { free(ptr); }
