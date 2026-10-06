#ifndef DBOX_WATCH_H
#define DBOX_WATCH_H

#include <stdbool.h>

#include "ctx.h"
#include "log.h"
#include "util.h"

/*
 * Watcher calls changed with the slash-separated relative path of every file
 * or directory that was created, written, removed or renamed under a root,
 * using FSEvents on macOS and inotify on Linux. Ignored paths are filtered
 * out, and ignored directories are not watched. Callbacks may arrive on any
 * thread.
 */
typedef struct Watcher Watcher;
typedef bool (*IgnoreFn)(void *ctx, const char *rel, bool is_dir);
typedef void (*ChangedFn)(void *ctx, const char *rel);

/* watcher_new starts watching root into *out. */
[[nodiscard]] Error watcher_new(const char *root, IgnoreFn ignored, ChangedFn changed, void *ctx, Logger *log, Watcher **out, Err *err);
/* watcher_run delivers events until ctx is done. */
void watcher_run(Watcher *w, Ctx *ctx);
void watcher_free(Watcher *w);

#endif
