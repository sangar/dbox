#include "pathqueue.h"

#include <string.h>

enum { QUEUE_BLOCK = 64 * 1024, EXPECTED_PATHS = 64 };

/* clear forgets every path at once; strmap_remove leaves the key bytes behind, so the arena is reset instead. */
static void clear(PathQueue *queue) {
    arena_reset(queue->arena);
    queue->items = nullptr;
    queue->head = queue->count = queue->capacity = 0;
    queue->waiting = strmap_create(queue->arena, EXPECTED_PATHS);
}

void pathqueue_init(PathQueue *queue) {
    queue->arena = arena_create(QUEUE_BLOCK);
    clear(queue);
}

void pathqueue_destroy(PathQueue *queue) {
    arena_destroy(queue->arena);
    memset(queue, 0, sizeof *queue);
}

static void grow(PathQueue *queue) {
    size_t capacity = queue->capacity ? queue->capacity * 2 : EXPECTED_PATHS;
    String *items = arena_push(queue->arena, arena_size_mul(capacity, sizeof *items));
    for (size_t i = 0; i < queue->count; i++) items[i] = queue->items[(queue->head + i) % queue->capacity];
    queue->items = items;
    queue->capacity = capacity;
    queue->head = 0;
}

void pathqueue_push(PathQueue *queue, Cancel *cancel, String path) {
    cancel_lock(cancel);
    if (!strmap_has(queue->waiting, path)) {
        if (queue->count == queue->capacity) grow(queue);
        queue->items[(queue->head + queue->count++) % queue->capacity] = str_copy(queue->arena, path);
        strmap_put(queue->waiting, path, nullptr);
        cancel_notify_locked(cancel);
    }
    cancel_unlock(cancel);
}

size_t pathqueue_count(PathQueue *queue, Cancel *cancel) {
    cancel_lock(cancel);
    size_t count = queue->count;
    cancel_unlock(cancel);
    return count;
}

bool pathqueue_pop(PathQueue *queue, Cancel *cancel, atomic_bool *stop, Arena *arena, String *path) {
    bool popped = false;
    cancel_lock(cancel);
    while (!cancel_requested(cancel) && !atomic_load(stop)) {
        if (queue->count > 0) {
            *path = str_copy(arena, queue->items[queue->head]);
            queue->head = (queue->head + 1) % queue->capacity;
            queue->count--;
            if (queue->count == 0) clear(queue);
            else strmap_remove(queue->waiting, *path, nullptr);
            popped = true;
            break;
        }
        cancel_wait(cancel, 0);
    }
    cancel_unlock(cancel);
    return popped;
}

static void create_maps(PathLocks *locks) {
    locks->busy = strmap_create(locks->arena, EXPECTED_PATHS);
    locks->dirty = strmap_create(locks->arena, EXPECTED_PATHS);
}

/* forget_if_idle resets the arena once no path is busy or dirty, so the maps do not keep the bytes of every path ever locked. */
static void forget_if_idle(PathLocks *locks) {
    if (strmap_count(locks->busy) + strmap_count(locks->dirty) > 0) return;
    arena_reset(locks->arena);
    create_maps(locks);
}

void pathlocks_init(PathLocks *locks) {
    mutex_init(&locks->mutex);
    locks->arena = arena_create(QUEUE_BLOCK);
    create_maps(locks);
}

void pathlocks_destroy(PathLocks *locks) {
    mutex_destroy(&locks->mutex);
    arena_destroy(locks->arena);
}

bool pathlocks_acquire(PathLocks *locks, String path) {
    mutex_lock(&locks->mutex);
    bool acquired = !strmap_has(locks->busy, path);
    strmap_put(acquired ? locks->busy : locks->dirty, path, nullptr);
    mutex_unlock(&locks->mutex);
    return acquired;
}

bool pathlocks_release(PathLocks *locks, String path) {
    mutex_lock(&locks->mutex);
    strmap_remove(locks->busy, path, nullptr);
    bool again = strmap_remove(locks->dirty, path, nullptr);
    forget_if_idle(locks);
    mutex_unlock(&locks->mutex);
    return again;
}

size_t pathlocks_in_flight(PathLocks *locks) {
    mutex_lock(&locks->mutex);
    size_t count = strmap_count(locks->busy);
    mutex_unlock(&locks->mutex);
    return count;
}
