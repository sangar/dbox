#ifndef DBOX_QUEUE_H
#define DBOX_QUEUE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "ctx.h"
#include "strmap.h"

/*
 * PathQueue is an unbounded, de-duplicating FIFO of paths. Pushing a path
 * that is already waiting is a no-op, so a burst of events costs one sync.
 * It is guarded by the Ctx lock, so a pop waits on the same condition that
 * cancellation signals.
 */
typedef struct {
    char **items;
    size_t head, len, cap;
    StrMap waiting;
} PathQueue;

void queue_init(PathQueue *q);
void queue_free(PathQueue *q);
void queue_push(PathQueue *q, Ctx *ctx, const char *path);
size_t queue_len(PathQueue *q, Ctx *ctx);
/* queue_pop returns the next path, malloc'd, or NULL once ctx is done or *stop is set. */
char *queue_pop(PathQueue *q, Ctx *ctx, atomic_bool *stop);

/*
 * PathLocks lets one thread at a time act on a path. A path that is busy is
 * marked dirty, and the holder is told to run it again when it releases.
 */
typedef struct {
    pthread_mutex_t mu;
    StrMap busy, dirty;
} PathLocks;

void locks_init(PathLocks *l);
void locks_free(PathLocks *l);
bool locks_acquire(PathLocks *l, const char *path);
/* locks_release reports whether path changed while it was held. */
bool locks_release(PathLocks *l, const char *path);
size_t locks_in_flight(PathLocks *l);

#endif
