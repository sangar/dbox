#ifndef DBOX_HTTP_H
#define DBOX_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctx.h"
#include "error.h"
#include "strbuf.h"

/*
 * The HTTP client, and the only module that talks to libcurl. A request
 * sends a memory or file body and either stores the response body in memory
 * or, for a 2xx response, writes it to a descriptor.
 */

/* HttpHeaderFn receives each response header, trimmed; a new status line arrives as a NULL name. */
typedef void (*HttpHeaderFn)(void *user, const char *name, size_t name_len, const char *value);

typedef struct {
    const char *method;
    const char *url;
    const char *const *headers; /* "Name: value" lines */
    size_t header_count;
    int body_fd; /* -1 for a memory body or none */
    int64_t body_off, body_len;
    const char *body;
    size_t body_size;
    int sink_fd; /* where a 2xx body goes; -1 keeps it in memory */
    HttpHeaderFn on_header;
    void *user;
    long timeout_ms; /* 0 means a 15s connect timeout and giving up below 1 byte/s for 60s */
} HttpRequest;

typedef struct {
    long status;
    StrBuf body; /* the body unless it went to sink_fd */
} HttpResponse;

/* http_global_init readies the client; main calls it once before any thread starts. */
void http_global_init(void);
void http_response_init(HttpResponse *r);
void http_response_free(HttpResponse *r);
/*
 * http_perform sends one request and fills resp. The status is not an error
 * here: a transport failure is ERR_REMOTE, a cancelled ctx ERR_CANCELLED and
 * a failed write to sink_fd ERR_IO.
 */
[[nodiscard]] Error http_perform(Ctx *ctx, const HttpRequest *req, HttpResponse *resp, Err *err);

#endif
