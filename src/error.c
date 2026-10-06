#include "error.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *error_name(Error e) {
    switch (e) {
    case ERR_OK: return "ok";
    case ERR_INVALID_ARGUMENT: return "invalid argument";
    case ERR_NOT_FOUND: return "not found";
    case ERR_IO: return "io";
    case ERR_REMOTE: return "remote";
    case ERR_PLATFORM: return "platform";
    case ERR_CANCELLED: return "cancelled";
    }
    return "unknown";
}

Error err_set(Err *err, Error code, const char *fmt, ...) {
    if (!err) return code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
    return code;
}

Error err_sys(Err *err, const char *fmt, ...) {
    int saved = errno;
    Error code = saved == ENOENT ? ERR_NOT_FOUND : ERR_IO;
    if (!err) return code;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n < sizeof err->msg) snprintf(err->msg + n, sizeof err->msg - (size_t)n, ": %s", strerror(saved));
    return code;
}
