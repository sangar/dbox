#include "debounce.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "strmap.h"
#include "util.h"

typedef struct {
    char *key;
    int64_t deadline;
} Pending;

struct Debouncer {
    int64_t delay_ns;
    DebounceFn fn;
    void *ctx;
    Mutex mu;
    Cond cv;
    Thread thread;
    bool stopped;
    Pending *items;
    size_t count, cap;
    StrMap index; /* key -> its position in items, stored as position + 1 */
};

static void remove_at(Debouncer *d, size_t i) {
    strmap_remove(&d->index, d->items[i].key, NULL);
    d->count--;
    if (i != d->count) {
        d->items[i] = d->items[d->count];
        strmap_put(&d->index, d->items[i].key, (void *)(i + 1));
    }
}

static void *timer_main(void *arg) {
    Debouncer *d = arg;
    mutex_lock(&d->mu);
    while (!d->stopped) {
        int64_t earliest = 0;
        for (size_t i = 0; i < d->count; i++)
            if (earliest == 0 || d->items[i].deadline < earliest) earliest = d->items[i].deadline;
        if (earliest == 0) {
            cond_wait(&d->cv, &d->mu);
            continue;
        }
        int64_t now = monotonic_ns();
        if (earliest > now) {
            cond_wait_until(&d->cv, &d->mu, earliest);
            continue;
        }
        StrList due = {0};
        for (size_t i = 0; i < d->count;) {
            if (d->items[i].deadline <= now) {
                strlist_push_owned(&due, d->items[i].key);
                remove_at(d, i);
            } else {
                i++;
            }
        }
        mutex_unlock(&d->mu);
        for (size_t i = 0; i < due.len; i++) d->fn(d->ctx, due.items[i]);
        strlist_free(&due);
        mutex_lock(&d->mu);
    }
    mutex_unlock(&d->mu);
    return NULL;
}

Debouncer *debounce_new(int64_t delay_ns, DebounceFn fn, void *ctx) {
    Debouncer *d = xcalloc(1, sizeof *d);
    d->delay_ns = delay_ns;
    d->fn = fn;
    d->ctx = ctx;
    mutex_init(&d->mu);
    cond_init(&d->cv);
    strmap_init(&d->index);
    thread_start(&d->thread, timer_main, d);
    return d;
}

void debounce_trigger(Debouncer *d, const char *key) {
    mutex_lock(&d->mu);
    int64_t deadline = monotonic_ns() + d->delay_ns;
    void *slot = strmap_get(&d->index, key);
    if (slot) {
        d->items[(size_t)slot - 1].deadline = deadline;
    } else {
        if (d->count == d->cap) {
            d->cap = d->cap ? d->cap * 2 : 64;
            d->items = xrealloc(d->items, d->cap * sizeof *d->items);
        }
        d->items[d->count] = (Pending){xstrdup(key), deadline};
        strmap_put(&d->index, key, (void *)(d->count + 1));
        d->count++;
    }
    cond_signal(&d->cv);
    mutex_unlock(&d->mu);
}

size_t debounce_pending(Debouncer *d) {
    mutex_lock(&d->mu);
    size_t n = d->count;
    mutex_unlock(&d->mu);
    return n;
}

void debounce_stop(Debouncer *d) {
    mutex_lock(&d->mu);
    d->stopped = true;
    for (size_t i = 0; i < d->count; i++) xfree(d->items[i].key);
    d->count = 0;
    cond_broadcast(&d->cv);
    mutex_unlock(&d->mu);
    thread_join(&d->thread);
    strmap_free(&d->index);
    xfree(d->items);
    mutex_destroy(&d->mu);
    cond_destroy(&d->cv);
    xfree(d);
}
