#include "ctx.h"

#include "util.h"

void ctx_init(Ctx *c) {
    atomic_init(&c->done, false);
    mutex_init(&c->mu);
    cond_init(&c->cv);
}

void ctx_destroy(Ctx *c) {
    mutex_destroy(&c->mu);
    cond_destroy(&c->cv);
}

bool ctx_done(const Ctx *c) { return atomic_load(&c->done); }

void ctx_cancel(Ctx *c) {
    mutex_lock(&c->mu);
    atomic_store(&c->done, true);
    cond_broadcast(&c->cv);
    mutex_unlock(&c->mu);
}

void ctx_lock(Ctx *c) { mutex_lock(&c->mu); }

void ctx_unlock(Ctx *c) { mutex_unlock(&c->mu); }

bool ctx_wait(Ctx *c, int64_t deadline_ns) {
    if (ctx_done(c)) return false;
    if (deadline_ns == 0) cond_wait(&c->cv, &c->mu);
    else cond_wait_until(&c->cv, &c->mu, deadline_ns);
    return !ctx_done(c);
}

void ctx_notify(Ctx *c) {
    mutex_lock(&c->mu);
    cond_broadcast(&c->cv);
    mutex_unlock(&c->mu);
}

void ctx_notify_locked(Ctx *c) { cond_broadcast(&c->cv); }

bool ctx_sleep(Ctx *c, int64_t ms) {
    int64_t deadline = monotonic_ns() + ms * NS_PER_MS;
    ctx_lock(c);
    while (!ctx_done(c) && monotonic_ns() < deadline) ctx_wait(c, deadline);
    bool live = !ctx_done(c);
    ctx_unlock(c);
    return live;
}
