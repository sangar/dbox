#include "http.h"

#include <curl/curl.h>
#include <string.h>

#include "platform/platform.h"
#include "util.h"

void http_global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

void http_response_init(HttpResponse *r) { memset(r, 0, sizeof *r); }

void http_response_free(HttpResponse *r) { sb_free(&r->body); }

typedef struct {
    int fd;
    int64_t off, len, pos;
    const char *mem;
    size_t mem_len;
} BodySource;

static size_t read_body(char *buf, size_t size, size_t nitems, void *userdata) {
    BodySource *src = userdata;
    size_t want = size * nitems;
    if (src->mem) {
        size_t n = min_size(want, src->mem_len - (size_t)src->pos);
        memcpy(buf, src->mem + src->pos, n);
        src->pos += (int64_t)n;
        return n;
    }
    int64_t remaining = src->len - src->pos;
    if (remaining <= 0) return 0;
    size_t n;
    if (file_pread(src->fd, buf, min_size(want, (size_t)remaining), src->off + src->pos, &n, NULL) != ERR_OK) return CURL_READFUNC_ABORT;
    src->pos += (int64_t)n;
    return n;
}

static int seek_body(void *userdata, curl_off_t offset, int origin) {
    BodySource *src = userdata;
    if (origin != SEEK_SET) return CURL_SEEKFUNC_CANTSEEK;
    src->pos = offset;
    return CURL_SEEKFUNC_OK;
}

typedef struct {
    CURL *curl;
    HttpResponse *resp;
    int sink_fd;
    bool failed;
} Sink;

static size_t write_body(char *data, size_t size, size_t nmemb, void *userdata) {
    Sink *sink = userdata;
    size_t n = size * nmemb;
    long status = 0;
    curl_easy_getinfo(sink->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 200 && status < 300 && sink->sink_fd >= 0) {
        if (!file_write_all(sink->sink_fd, data, n)) {
            sink->failed = true;
            return 0;
        }
        return n;
    }
    sb_append(&sink->resp->body, data, n);
    return n;
}

typedef struct {
    HttpHeaderFn fn;
    void *user;
} HeaderSink;

static size_t read_header(char *line, size_t size, size_t nitems, void *userdata) {
    HeaderSink *h = userdata;
    size_t n = size * nitems;
    if (!h->fn) return n;
    if (n >= 5 && strncmp(line, "HTTP/", 5) == 0) {
        h->fn(h->user, NULL, 0, NULL);
        return n;
    }
    const char *colon = memchr(line, ':', n);
    if (!colon) return n;
    const char *v = colon + 1;
    const char *end = line + n;
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    while (end > v && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) end--;
    char value[1024];
    size_t vlen = min_size((size_t)(end - v), sizeof value - 1);
    memcpy(value, v, vlen);
    value[vlen] = '\0';
    h->fn(h->user, line, (size_t)(colon - line), value);
    return n;
}

static int check_cancel(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    return ctx_done((Ctx *)userdata) ? 1 : 0;
}

Error http_perform(Ctx *ctx, const HttpRequest *req, HttpResponse *resp, Err *err) {
    CURL *curl = curl_easy_init();
    if (!curl) return err_set(err, ERR_REMOTE, "%s %s: cannot create an HTTP handle", req->method, req->url);
    struct curl_slist *headers = NULL;
    for (size_t i = 0; i < req->header_count; i++) headers = curl_slist_append(headers, req->headers[i]);
    BodySource src = {.fd = req->body_fd, .off = req->body_off, .len = req->body_len, .mem = req->body, .mem_len = req->body_size};
    Sink sink = {.curl = curl, .resp = resp, .sink_fd = req->sink_fd};
    HeaderSink header_sink = {.fn = req->on_header, .user = req->user};
    sb_clear(&resp->body);
    resp->status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, read_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_sink);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (ctx) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, check_cancel);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, ctx);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
    if (req->timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req->timeout_ms);
    } else {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    }
    if (strcmp(req->method, "HEAD") == 0) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (strcmp(req->method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_body);
        curl_easy_setopt(curl, CURLOPT_READDATA, &src);
        curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, seek_body);
        curl_easy_setopt(curl, CURLOPT_SEEKDATA, &src);
        curl_off_t size = req->body_fd >= 0 ? req->body_len : (curl_off_t)req->body_size;
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, size);
    } else if (strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body ? req->body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_size);
    } else if (strcmp(req->method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    CURLcode code = curl_easy_perform(curl);
    Error e = ERR_OK;
    if (code == CURLE_OK && !sink.failed) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status);
    else if (sink.failed) e = err_set(err, ERR_IO, "%s %s: write download failed", req->method, req->url);
    else if (code == CURLE_ABORTED_BY_CALLBACK) e = err_set(err, ERR_CANCELLED, "cancelled");
    else e = err_set(err, ERR_REMOTE, "%s %s: %s", req->method, req->url, curl_easy_strerror(code));
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return e;
}
