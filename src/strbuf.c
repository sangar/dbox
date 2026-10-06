#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_grow(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < sb->len + extra + 1) cap *= 2;
    sb->data = xrealloc(sb->data, cap);
    sb->cap = cap;
}

void sb_append(StrBuf *sb, const char *s, size_t n) {
    sb_grow(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s) { sb_append(sb, s, strlen(s)); }

void sb_putc(StrBuf *sb, char c) { sb_append(sb, &c, 1); }

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n > 0) {
        sb_grow(sb, (size_t)n);
        vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
        sb->len += (size_t)n;
    }
    va_end(ap);
}

void sb_clear(StrBuf *sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

const char *sb_cstr(StrBuf *sb) {
    sb_grow(sb, 0);
    sb->data[sb->len] = '\0';
    return sb->data;
}

void sb_free(StrBuf *sb) {
    xfree(sb->data);
    *sb = (StrBuf){0};
}

void strlist_push_owned(StrList *l, char *s) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = xrealloc(l->items, l->cap * sizeof *l->items);
    }
    l->items[l->len++] = s;
}

void strlist_push(StrList *l, const char *s) { strlist_push_owned(l, xstrdup(s)); }

void strlist_clear(StrList *l) {
    for (size_t i = 0; i < l->len; i++) xfree(l->items[i]);
    l->len = 0;
}

void strlist_free(StrList *l) {
    strlist_clear(l);
    xfree(l->items);
    *l = (StrList){0};
}

bool strlist_contains(const StrList *l, const char *s) {
    for (size_t i = 0; i < l->len; i++)
        if (strcmp(l->items[i], s) == 0) return true;
    return false;
}

int compare_strings(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

void strlist_sort(StrList *l) { qsort(l->items, l->len, sizeof *l->items, compare_strings); }
