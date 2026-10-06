#ifndef DBOX_ALLOC_H
#define DBOX_ALLOC_H

#include <stddef.h>

/*
 * The heap allocator for objects with their own lifetime: stores, the index,
 * the engine, and owned strings such as paths. Each allocation has one owner
 * that calls xfree; objects that die together use an Arena instead. Running
 * out of memory aborts, since nothing in dbox can continue without it.
 */
void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void xfree(void *ptr);

#endif
