#pragma once

#include <stdatomic.h>

#include "engine.h"
#include "ignore.h"
#include "mc/concurrency/debounce.h"
#include "mc/concurrency/threadpool.h"
#include "pathqueue.h"

/* Shared between engine.c, which handles the folder and the primary, and mirror.c. */

struct Engine {
    const Config *config;
    String root;
    Index *index;
    Logger *log;
    Logger *own_log;
    bool dry_run;
    String host;
    Ignore ignore;
    String primary_name;
    Store *primary;
    ThreadPool *primary_pool;
    StringList mirror_names;
    Store **mirrors;
    ThreadPool **mirror_pools;
    bool *mirror_woken; /* guarded by the cancel lock */
    PathQueue queue;
    Debouncer *debouncer;
    PathLocks locks;
    Cancel *cancel;       /* the current run's */
    atomic_bool stopping; /* set when the run must end before cancel is requested */
};

typedef struct LocalFile {
    bool exists, changed;
    int64_t size, mtime_ns;
} LocalFile;

/* SCRATCH_BLOCK sizes the arenas that live for one path's work. */
enum { SCRATCH_BLOCK = 16 * 1024 };

static inline bool engine_done(Engine *e) { return cancel_requested(e->cancel) || atomic_load(&e->stopping); }

String engine_abs(Arena *arena, const Engine *e, String rel);
/* engine_local describes the file at rel relative to its index row file, which may be nullptr. */
[[nodiscard]] Error engine_local(Engine *e, Arena *scratch, String rel, const IndexFile *file, LocalFile *local, Err *err);
void engine_wake_mirrors(Engine *e);
/* retry_delay doubles from 20 seconds: 20s, 40s, 80s, 160s, 320s. */
int64_t retry_delay_ns(int attempts);
/* engine_parallel calls run(e, items[i]) for every item on pool and waits for all of them; its bookkeeping lives in scratch. */
void engine_parallel(Engine *e, ThreadPool *pool, Arena *scratch, void **items, size_t count, void (*run)(Engine *e, void *item));

void mirror_loop(Engine *e, size_t mirror);
[[nodiscard]] Error mirror_drain(Engine *e, size_t mirror, Err *err);
[[nodiscard]] Error mirror_reconcile(Engine *e, size_t mirror, Err *err);
