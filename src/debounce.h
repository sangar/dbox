#ifndef DBOX_DEBOUNCE_H
#define DBOX_DEBOUNCE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Debouncer calls fn(ctx, key) once key has been quiet for the delay, so a
 * burst of events for one path costs one sync. One timer thread serves
 * every key.
 */
typedef struct Debouncer Debouncer;
typedef void (*DebounceFn)(void *ctx, const char *key);

Debouncer *debounce_new(int64_t delay_ns, DebounceFn fn, void *ctx);
/* debounce_trigger starts or restarts key's quiet period. */
void debounce_trigger(Debouncer *d, const char *key);
/* debounce_pending is how many keys are in their quiet period. */
size_t debounce_pending(Debouncer *d);
/* debounce_stop cancels every pending call, stops the timer thread and frees the debouncer. */
void debounce_stop(Debouncer *d);

#endif
