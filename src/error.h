#ifndef DBOX_ERROR_H
#define DBOX_ERROR_H

/*
 * Error is the one result type for everything that can fail. A fallible
 * function returns it and hands results back through out-parameters; an
 * Err, when the caller passes one, receives the human readable detail.
 */
typedef enum {
    ERR_OK = 0,
    ERR_INVALID_ARGUMENT, /* a config, flag or value the caller gave is malformed */
    ERR_NOT_FOUND,
    ERR_IO,               /* the local file system or the index database */
    ERR_REMOTE,           /* a store or HTTP request failed */
    ERR_PLATFORM,         /* processes, sockets, threads and service managers */
    ERR_CANCELLED,
} Error;

typedef struct {
    char msg[512];
} Err;

const char *error_name(Error e);

/* err_set records the message for err, when given, and returns code so a caller can `return err_set(...)`. */
[[nodiscard]] Error err_set(Err *err, Error code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* err_sys appends ": strerror(errno)" to the message and maps errno: ENOENT becomes ERR_NOT_FOUND, anything else ERR_IO. */
[[nodiscard]] Error err_sys(Err *err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
