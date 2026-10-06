#ifndef DBOX_ENGINE_INTERNAL_H
#define DBOX_ENGINE_INTERNAL_H

#include <stdatomic.h>

#include "debounce.h"
#include "engine.h"
#include "ignore.h"
#include "queue.h"

/* Shared between engine.c, which handles the folder and the primary, and mirror.c. */

struct Engine {
    const Config *cfg;
    const char *root;
    Index *idx;
    Logger *log;
    Logger own_log;
    bool dry_run;
    char host[256];
    Ignore ignore;
    const char *primary_name;
    Store *primary;
    size_t mirror_count;
    const char **mirror_names;
    Store **mirrors;
    bool *mirror_woken; /* guarded by the ctx lock */
    Arena arena;
    PathQueue queue;
    Debouncer *debouncer;
    PathLocks locks;
    Ctx *ctx;             /* the current run's context */
    atomic_bool stopping; /* set when the run must end before ctx is cancelled */
};

typedef struct {
    bool exists, changed;
    int64_t size, mtime_ns;
} LocalFile;

static inline bool engine_done(const Engine *e) { return ctx_done(e->ctx) || atomic_load(&e->stopping); }

char *engine_abs(const Engine *e, const char *rel);
/* engine_local describes the file at rel relative to its index row f, which may be NULL. */
[[nodiscard]] Error engine_local(Engine *e, const char *rel, const IndexFile *f, LocalFile *out, Err *err);
void engine_wake_mirrors(Engine *e);
/* retry_delay doubles from 20 seconds: 20s, 40s, 80s, 160s, 320s. */
int64_t retry_delay_ns(int attempts);
/* run_workers calls fn(e, items[i]) for every item from up to `workers` threads and waits for all of them. */
void run_workers(Engine *e, int workers, void **items, size_t count, void (*fn)(Engine *e, void *item));

void mirror_loop(Engine *e, size_t mirror);
[[nodiscard]] Error mirror_drain(Engine *e, size_t mirror, Err *err);
[[nodiscard]] Error mirror_reconcile(Engine *e, size_t mirror, Err *err);

#endif
