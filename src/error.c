#include "error.h"

#include <stdarg.h>
#include <stdio.h>

Error err_set(Err *err, Error code, const char *fmt, ...) {
    if (!err) return code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
    return code;
}
