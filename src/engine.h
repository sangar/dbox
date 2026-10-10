#pragma once

#include "config.h"
#include "index.h"
#include "mc/concurrency/cancel.h"
#include "mc/log/log.h"
#include "store.h"

/*
 * Engine keeps a local folder, the primary store and every mirror in sync.
 * See docs/design.md for the decision tables it implements. Create it with
 * engine_create, then call engine_run for the daemon or engine_once for a
 * single reconcile.
 */
typedef struct Engine Engine;

typedef struct EngineOptions {
    bool dry_run;
    Logger *log; /* nullptr logs to stderr at info level */
} EngineOptions;

/* engine_create builds the engine in the arena and starts its worker pools; engine_destroy stops them. */
[[nodiscard]] Error engine_create(Arena *arena, const Config *config, Index *index, const StoreSet *stores, EngineOptions options, Engine **engine, Err *err);
void engine_destroy(Engine *engine);

/* engine_once reconciles the folder with every store, drains the mirror queues and returns. It is `dbox run --once`. */
[[nodiscard]] Error engine_once(Engine *engine, Cancel *cancel, Err *err);
/* engine_run reconciles, then watches the folder, polls the primary and copies to mirrors until cancel is requested. */
[[nodiscard]] Error engine_run(Engine *engine, Cancel *cancel, Err *err);
/*
 * engine_reconcile brings the folder and the primary in line, adopts copies
 * already on mirrors, and queues backfill for what mirrors lack.
 */
[[nodiscard]] Error engine_reconcile(Engine *engine, Cancel *cancel, Err *err);
/* engine_backlog is how many changed paths have not reached the primary yet. */
size_t engine_backlog(Engine *engine);
/* engine_drain_mirror copies to one mirror until nothing is due, for tests. */
[[nodiscard]] Error engine_drain_mirror(Engine *engine, Cancel *cancel, String name, Err *err);
