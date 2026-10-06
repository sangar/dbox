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

/* The platform backends implement these for watch.c. */
typedef struct WatchBackend WatchBackend;
[[nodiscard]] Error backend_open(Watcher *w, const char *root, WatchBackend **out, Err *err);
void backend_close(WatchBackend *b);
/* backend_add_dir starts watching one directory; it is a no-op where the backend watches whole trees. */
[[nodiscard]] Error backend_add_dir(WatchBackend *b, const char *path, Err *err);
void backend_run(WatchBackend *b, Ctx *ctx);
/* watcher_event is what a backend calls with every absolute path it hears about. */
void watcher_event(Watcher *w, const char *abs_path, bool rescan_subdirs);

#endif
