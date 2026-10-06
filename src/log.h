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

static inline LogAttr log_str(const char *key, const char *value) { return (LogAttr){key, ATTR_STR, value, 0}; }
static inline LogAttr log_int(const char *key, long long value) { return (LogAttr){key, ATTR_INT, NULL, value}; }
static inline LogAttr log_dur(const char *key, int64_t ns) { return (LogAttr){key, ATTR_DURATION, NULL, ns}; }
static inline LogAttr log_err(const Err *err) { return log_str("err", err->msg); }
/* log_end closes the attribute list of every log call. */
static inline LogAttr log_end(void) { return (LogAttr){NULL, ATTR_END, NULL, 0}; }

/* logger_init takes the config's level ("debug", "info", "warn", "error") and format ("text", "json"). */
void logger_init(Logger *l, const char *level, const char *format, FILE *out);
void logger_destroy(Logger *l);

/* Each takes LogAttr values and ends with log_end(). */
void log_debug(Logger *l, const char *msg, ...);
void log_info(Logger *l, const char *msg, ...);
void log_warn(Logger *l, const char *msg, ...);
void log_error(Logger *l, const char *msg, ...);

#endif
