#ifndef DBOX_LOG_H
#define DBOX_LOG_H

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "util.h"

/*
 * Logger writes one structured line per call, as Go's slog does: a message
 * followed by key=value attributes, in text or JSON.
 */
typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

typedef struct {
    LogLevel level;
    bool json;
    FILE *out;
    pthread_mutex_t mu;
} Logger;

typedef enum { ATTR_END, ATTR_STR, ATTR_INT, ATTR_DURATION } AttrKind;

typedef struct {
    const char *key;
    AttrKind kind;
    const char *str;
    long long num;
} LogAttr;

#define LS(k, v) ((LogAttr){(k), ATTR_STR, (v), 0})
#define LI(k, v) ((LogAttr){(k), ATTR_INT, NULL, (long long)(v)})
#define LD(k, ns) ((LogAttr){(k), ATTR_DURATION, NULL, (long long)(ns)})
#define LERR(e) LS("err", (e)->msg)
#define LEND ((LogAttr){NULL, ATTR_END, NULL, 0})

/* logger_init takes the config's level ("debug", "info", "warn", "error") and format ("text", "json"). */
void logger_init(Logger *l, const char *level, const char *format, FILE *out);
void logger_destroy(Logger *l);
/* logger_discard drops everything, for tests. */
Logger *logger_discard(void);

void log_at(Logger *l, LogLevel level, const char *msg, ...);
#define log_debug(l, ...) log_at((l), LOG_DEBUG, __VA_ARGS__, LEND)
#define log_info(l, ...) log_at((l), LOG_INFO, __VA_ARGS__, LEND)
#define log_warn(l, ...) log_at((l), LOG_WARN, __VA_ARGS__, LEND)
#define log_error(l, ...) log_at((l), LOG_ERROR, __VA_ARGS__, LEND)

#endif
