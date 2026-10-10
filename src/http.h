#pragma once

#include "mc/concurrency/cancel.h"

/*
 * The HTTP client, and the only module that talks to libcurl. A request
 * sends a memory or file body and either keeps the response body in memory
 * or, for a 2xx response, writes it to a descriptor.
 */

/* HttpHeaderFn receives each response header, trimmed; an empty name says a new status line began. */
typedef void (*HttpHeaderFn)(void *context, String name, String value);

typedef struct HttpRequest {
    String method;
    String url;
    StringList headers; /* "Name: value" lines */
    int body_fd;        /* -1 sends body from memory */
    int64_t body_offset, body_length;
    String body;
    int sink_fd; /* where a 2xx body goes; -1 keeps it in memory */
    HttpHeaderFn on_header;
    void *context;
    long timeout_ms; /* 0 means a 15s connect timeout and giving up below 1 byte/s for 60s */
} HttpRequest;

typedef struct HttpResponse {
    long status;
    String body; /* the body unless it went to sink_fd */
} HttpResponse;

/* http_global_init readies the client; main calls it once before any thread starts. */
void http_global_init(void);
/*
 * http_perform sends one request and fills response, with its body in the
 * arena. The status is not an error here: a transport failure is
 * ERR_NETWORK, a cancelled run ERR_CANCELLED and a failed write to sink_fd
 * ERR_IO. cancel may be nullptr.
 */
[[nodiscard]] Error http_perform(Cancel *cancel, Arena *arena, const HttpRequest *request, HttpResponse *response, Err *err);
