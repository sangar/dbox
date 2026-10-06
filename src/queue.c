#include "queue.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

void queue_init(PathQueue *q) {
    memset(q, 0, sizeof *q);
    strmap_init(&q->waiting);
}

void queue_free(PathQueue *q) {
    for (size_t i = 0; i < q->len; i++) xfree(q->items[(q->head + i) % q->cap]);
    xfree(q->items);
    strmap_free(&q->waiting);
    memset(q, 0, sizeof *q);
}

void queue_push(PathQueue *q, Ctx *ctx, const char *path) {
    ctx_lock(ctx);
    if (!strmap_has(&q->waiting, path)) {
        if (q->len == q->cap) {
            size_t cap = q->cap ? q->cap * 2 : 64;
            char **items = xmalloc(cap * sizeof *items);
            for (size_t i = 0; i < q->len; i++) items[i] = q->items[(q->head + i) % q->cap];
            xfree(q->items);
            q->items = items;
            q->cap = cap;
            q->head = 0;
        }
        q->items[(q->head + q->len++) % q->cap] = xstrdup(path);
        strmap_put(&q->waiting, path, NULL);
        ctx_notify_locked(ctx);
    }
    ctx_unlock(ctx);
}

size_t queue_len(PathQueue *q, Ctx *ctx) {
    ctx_lock(ctx);
    size_t n = q->len;
    ctx_unlock(ctx);
    return n;
}

char *queue_pop(PathQueue *q, Ctx *ctx, atomic_bool *stop) {
    char *out = NULL;
    ctx_lock(ctx);
    while (!ctx_done(ctx) && !atomic_load(stop)) {
        if (q->len > 0) {
            out = q->items[q->head];
            q->head = (q->head + 1) % q->cap;
            q->len--;
            strmap_remove(&q->waiting, out, NULL);
            break;
        }
        ctx_wait(ctx, 0);
    }
    ctx_unlock(ctx);
    return out;
}

void locks_init(PathLocks *l) {
    mutex_init(&l->mu);
    strmap_init(&l->busy);
    strmap_init(&l->dirty);
}

void locks_free(PathLocks *l) {
    mutex_destroy(&l->mu);
    strmap_free(&l->busy);
    strmap_free(&l->dirty);
}

bool locks_acquire(PathLocks *l, const char *path) {
    mutex_lock(&l->mu);
    bool acquired = !strmap_has(&l->busy, path);
    if (acquired) strmap_put(&l->busy, path, NULL);
    else strmap_put(&l->dirty, path, NULL);
    mutex_unlock(&l->mu);
    return acquired;
}

bool locks_release(PathLocks *l, const char *path) {
    mutex_lock(&l->mu);
    strmap_remove(&l->busy, path, NULL);
    bool again = strmap_remove(&l->dirty, path, NULL);
    mutex_unlock(&l->mu);
    return again;
}

size_t locks_in_flight(PathLocks *l) {
    mutex_lock(&l->mu);
    size_t n = strmap_count(&l->busy);
    mutex_unlock(&l->mu);
    return n;
}
