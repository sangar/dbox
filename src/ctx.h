#ifndef DBOX_CTX_H
#define DBOX_CTX_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "platform/platform.h"

/*
 * Ctx is a cancellation token shared by every thread of one run. Cancelling
 * it wakes everything that waits on it. Work that waits for either new input
 * or cancellation takes the lock, checks its own condition and calls
 * ctx_wait, so one condition variable serves both.
 */
typedef struct {
    atomic_bool done;
    Mutex mu;
    Cond cv;
} Ctx;

void ctx_init(Ctx *c);
void ctx_destroy(Ctx *c);
bool ctx_done(const Ctx *c);
void ctx_cancel(Ctx *c);

void ctx_lock(Ctx *c);
void ctx_unlock(Ctx *c);
/*
 * ctx_wait releases the lock until ctx_notify, cancellation or the monotonic
 * deadline (0 waits without one), then holds it again. It returns false
 * once the context is cancelled.
 */
bool ctx_wait(Ctx *c, int64_t deadline_ns);
void ctx_notify(Ctx *c);
/* ctx_notify_locked is ctx_notify for a caller that already holds the lock. */
void ctx_notify_locked(Ctx *c);

/* ctx_sleep waits for ms milliseconds, or less when cancelled, and reports whether the context is still live. */
bool ctx_sleep(Ctx *c, int64_t ms);

#endif
