#include "debounce.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

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
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    bool stopped;
    Pending *items;
    size_t count, cap;
    StrMap index; /* key -> its position in items, stored as position + 1 */
};

static void wait_until(Debouncer *d, int64_t deadline) {
    int64_t remaining = deadline - monotonic_ns();
    if (remaining <= 0) return;
#ifdef __APPLE__
    struct timespec rel = {.tv_sec = remaining / NS_PER_SEC, .tv_nsec = remaining % NS_PER_SEC};
    pthread_cond_timedwait_relative_np(&d->cv, &d->mu, &rel);
#else
    struct timespec abs = {.tv_sec = deadline / NS_PER_SEC, .tv_nsec = deadline % NS_PER_SEC};
    pthread_cond_timedwait(&d->cv, &d->mu, &abs);
#endif
}

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
    pthread_mutex_lock(&d->mu);
    while (!d->stopped) {
        int64_t earliest = 0;
        for (size_t i = 0; i < d->count; i++)
            if (earliest == 0 || d->items[i].deadline < earliest) earliest = d->items[i].deadline;
        if (earliest == 0) {
            pthread_cond_wait(&d->cv, &d->mu);
            continue;
        }
        int64_t now = monotonic_ns();
        if (earliest > now) {
            wait_until(d, earliest);
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
        pthread_mutex_unlock(&d->mu);
        for (size_t i = 0; i < due.len; i++) d->fn(d->ctx, due.items[i]);
        strlist_free(&due);
        pthread_mutex_lock(&d->mu);
    }
    pthread_mutex_unlock(&d->mu);
    return NULL;
}

Debouncer *debounce_new(int64_t delay_ns, DebounceFn fn, void *ctx) {
    Debouncer *d = xcalloc(1, sizeof *d);
    d->delay_ns = delay_ns;
    d->fn = fn;
    d->ctx = ctx;
    pthread_mutex_init(&d->mu, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#ifndef __APPLE__
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&d->cv, &attr);
    pthread_condattr_destroy(&attr);
    strmap_init(&d->index);
    pthread_create(&d->thread, NULL, timer_main, d);
    return d;
}

void debounce_trigger(Debouncer *d, const char *key) {
    pthread_mutex_lock(&d->mu);
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
    pthread_cond_signal(&d->cv);
    pthread_mutex_unlock(&d->mu);
}

size_t debounce_pending(Debouncer *d) {
    pthread_mutex_lock(&d->mu);
    size_t n = d->count;
    pthread_mutex_unlock(&d->mu);
    return n;
}

void debounce_stop(Debouncer *d) {
    pthread_mutex_lock(&d->mu);
    d->stopped = true;
    for (size_t i = 0; i < d->count; i++) free(d->items[i].key);
    d->count = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
    pthread_join(d->thread, NULL);
    strmap_free(&d->index);
    free(d->items);
    pthread_mutex_destroy(&d->mu);
    pthread_cond_destroy(&d->cv);
    free(d);
}
