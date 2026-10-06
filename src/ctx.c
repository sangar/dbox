#include "ctx.h"

#include <errno.h>
#include <time.h>

#include "util.h"

void ctx_init(Ctx *c) {
    atomic_init(&c->done, false);
    pthread_mutex_init(&c->mu, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#ifndef __APPLE__
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&c->cv, &attr);
    pthread_condattr_destroy(&attr);
}

void ctx_destroy(Ctx *c) {
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
}

bool ctx_done(const Ctx *c) { return atomic_load(&c->done); }

void ctx_cancel(Ctx *c) {
    pthread_mutex_lock(&c->mu);
    atomic_store(&c->done, true);
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

void ctx_lock(Ctx *c) { pthread_mutex_lock(&c->mu); }

void ctx_unlock(Ctx *c) { pthread_mutex_unlock(&c->mu); }

bool ctx_wait(Ctx *c, int64_t deadline_ns) {
    if (ctx_done(c)) return false;
    if (deadline_ns == 0) {
        pthread_cond_wait(&c->cv, &c->mu);
    } else {
        int64_t remaining = deadline_ns - monotonic_ns();
        if (remaining <= 0) return !ctx_done(c);
#ifdef __APPLE__
        struct timespec rel = {.tv_sec = remaining / NS_PER_SEC, .tv_nsec = remaining % NS_PER_SEC};
        pthread_cond_timedwait_relative_np(&c->cv, &c->mu, &rel);
#else
        struct timespec abs = {.tv_sec = deadline_ns / NS_PER_SEC, .tv_nsec = deadline_ns % NS_PER_SEC};
        pthread_cond_timedwait(&c->cv, &c->mu, &abs);
#endif
    }
    return !ctx_done(c);
}

void ctx_notify(Ctx *c) {
    pthread_mutex_lock(&c->mu);
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

void ctx_notify_locked(Ctx *c) { pthread_cond_broadcast(&c->cv); }

bool ctx_sleep(Ctx *c, int64_t ms) {
    int64_t deadline = monotonic_ns() + ms * NS_PER_MS;
    ctx_lock(c);
    while (!ctx_done(c) && monotonic_ns() < deadline) ctx_wait(c, deadline);
    bool live = !ctx_done(c);
    ctx_unlock(c);
    return live;
}
