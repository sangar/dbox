#ifndef DBOX_DAEMON_H
#define DBOX_DAEMON_H

#include <stdbool.h>
#include <sys/types.h>

#include "ctx.h"
#include "util.h"

/*
 * What the long-running process needs besides syncing: a pid file so other
 * commands can find it, the health and metrics listener, and a raised
 * open-file limit.
 */

/* daemon_write_pid records the running process in path; it fails when another live daemon already owns the file. */
bool daemon_write_pid(const char *path, Err *err);
void daemon_remove_pid(const char *path);
/* daemon_running returns the pid in path when that process is alive. */
bool daemon_running(const char *path, pid_t *pid);
/* daemon_reload asks the daemon recorded in path to re-read its config; *reloaded is false when none is running. */
bool daemon_reload(const char *path, bool *reloaded, Err *err);

typedef bool (*MetricsFn)(void *arg, StrBuf *out, Err *err);
typedef long (*BacklogFn)(void *arg);

/* daemon_serve answers /healthz, /metrics and /status on addr until ctx is done. An empty addr disables the listener. */
bool daemon_serve(Ctx *ctx, const char *addr, MetricsFn metrics, BacklogFn backlog, void *arg, Err *err);
/* daemon_ask fetches the running daemon's backlog from addr; it reports false when no daemon answers there. */
bool daemon_ask(const char *addr, long *backlog);

/* daemon_raise_file_limit lifts the soft open-file limit to the hard limit. */
bool daemon_raise_file_limit(Err *err);

#endif
