#ifndef DBOX_ENGINE_H
#define DBOX_ENGINE_H

#include <stdbool.h>

#include "config.h"
#include "ctx.h"
#include "index.h"
#include "log.h"
#include "store.h"

/*
 * Engine keeps a local folder, the primary store and every mirror in sync.
 * See docs/design.md for the decision tables it
 * implements. Build it with engine_new, then call engine_run for the daemon
 * or engine_once for a single reconcile.
 */
typedef struct Engine Engine;

typedef struct {
    bool dry_run;
    Logger *log; /* NULL logs to stderr at info level */
} EngineOptions;

Engine *engine_new(const Config *cfg, Index *idx, const StoreSet *stores, EngineOptions opts);
void engine_free(Engine *e);

/* engine_once reconciles the folder with every store, drains the mirror queues and returns. It is `dbox run --once`. */
[[nodiscard]] Error engine_once(Engine *e, Ctx *ctx, Err *err);
/* engine_run reconciles, then watches the folder, polls the primary and copies to mirrors until ctx is done. */
[[nodiscard]] Error engine_run(Engine *e, Ctx *ctx, Err *err);
/*
 * engine_reconcile brings the folder and the primary in line, adopts copies
 * already on mirrors, and queues backfill for what mirrors lack.
 */
[[nodiscard]] Error engine_reconcile(Engine *e, Ctx *ctx, Err *err);
/* engine_backlog is how many changed paths have not reached the primary yet. */
size_t engine_backlog(Engine *e);
/* engine_drain_mirror copies to one mirror until nothing is due, for tests. */
[[nodiscard]] Error engine_drain_mirror(Engine *e, Ctx *ctx, const char *name, Err *err);

#endif
