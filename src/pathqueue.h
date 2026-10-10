#pragma once

#include <stdatomic.h>

#include "mc/concurrency/cancel.h"
#include "mc/container/strmap.h"

/*
 * PathQueue is an unbounded, de-duplicating FIFO of paths. Pushing a path
 * that is already waiting is a no-op, so a burst of events costs one sync.
 * It is guarded by the Cancel lock, so a pop waits on the same condition
 * that cancellation signals. Its paths live in an arena of its own that is
 * reset whenever the queue runs empty.
 */
typedef struct PathQueue {
    Arena *arena;
    String *items;
    size_t head, count, capacity;
    StrMap *waiting;
} PathQueue;

void pathqueue_init(PathQueue *queue);
void pathqueue_destroy(PathQueue *queue);
void pathqueue_push(PathQueue *queue, Cancel *cancel, String path);
size_t pathqueue_count(PathQueue *queue, Cancel *cancel);
/* pathqueue_pop copies the next path into the arena; it is false once cancel is requested or *stop is set. */
bool pathqueue_pop(PathQueue *queue, Cancel *cancel, atomic_bool *stop, Arena *arena, String *path);

/*
 * PathLocks lets one thread at a time act on a path. A path that is busy is
 * marked dirty, and the holder is told to run it again when it releases.
 */
typedef struct PathLocks {
    Mutex mutex;
    Arena *arena;
    StrMap *busy, *dirty;
} PathLocks;

void pathlocks_init(PathLocks *locks);
void pathlocks_destroy(PathLocks *locks);
bool pathlocks_acquire(PathLocks *locks, String path);
/* pathlocks_release reports whether path changed while it was held. */
bool pathlocks_release(PathLocks *locks, String path);
size_t pathlocks_in_flight(PathLocks *locks);
