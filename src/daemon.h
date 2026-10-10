#pragma once

#include "mc/concurrency/cancel.h"

/*
 * What the long-running process needs besides syncing: a pid file so other
 * commands can find it, and the health and metrics listener.
 */

/* daemon_write_pid records the running process in path; it fails when another live daemon already owns the file. */
[[nodiscard]] Error daemon_write_pid(Arena *scratch, String path, Err *err);
void daemon_remove_pid(String path);
/* daemon_running returns the pid in path when that process is alive. */
bool daemon_running(Arena *scratch, String path, int *pid);
/* daemon_reload asks the daemon recorded in path to re-read its config; *reloaded is false when none is running. */
[[nodiscard]] Error daemon_reload(Arena *scratch, String path, bool *reloaded, Err *err);

/* MetricsFn appends the Prometheus metrics to out. */
typedef Error (*MetricsFn)(void *context, Arena *arena, StringBuilder *out, Err *err);
typedef int64_t (*BacklogFn)(void *context);

/* daemon_serve answers /healthz, /metrics and /status on address until cancel is requested. An empty address disables the listener. */
[[nodiscard]] Error daemon_serve(Cancel *cancel, String address, MetricsFn metrics, BacklogFn backlog, void *context, Err *err);
/* daemon_ask fetches the running daemon's backlog from address; it reports false when no daemon answers there. */
bool daemon_ask(Arena *scratch, String address, int64_t *backlog);
