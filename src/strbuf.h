#ifndef DBOX_STRBUF_H
#define DBOX_STRBUF_H

#include <stdbool.h>
#include <stddef.h>

#include "alloc.h"

typedef struct {
    char *data;
    size_t len, cap;
} StrBuf;

void sb_grow(StrBuf *sb, size_t extra);
void sb_append(StrBuf *sb, const char *s, size_t n);
void sb_puts(StrBuf *sb, const char *s);
void sb_putc(StrBuf *sb, char c);
void sb_printf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_clear(StrBuf *sb);
const char *sb_cstr(StrBuf *sb);
void sb_free(StrBuf *sb);

/* StrList owns its strings. */
typedef struct {
    char **items;
    size_t len, cap;
} StrList;

void strlist_push(StrList *l, const char *s);
void strlist_push_owned(StrList *l, char *s);
void strlist_clear(StrList *l);
void strlist_free(StrList *l);
bool strlist_contains(const StrList *l, const char *s);
void strlist_sort(StrList *l);

#endif
