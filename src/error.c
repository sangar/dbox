#include "error.h"

#include <stdarg.h>
#include <stdio.h>

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
