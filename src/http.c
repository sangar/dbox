#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

void http_global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

typedef struct {
    int fd;
    int64_t offset, length, position;
    String memory;
} BodySource;

static size_t read_body(char *buffer, size_t size, size_t count, void *context) {
    BodySource *source = context;
    size_t want = size * count;
    if (source->fd < 0) {
        size_t n = min_size(want, source->memory.len - (size_t)source->position);
        if (n == 0) return 0;
        memcpy(buffer, source->memory.data + source->position, n);
        source->position += (int64_t)n;
        return n;
    }
    int64_t remaining = source->length - source->position;
    if (remaining <= 0) return 0;
    size_t n;
    if (file_pread(source->fd, buffer, min_size(want, (size_t)remaining), source->offset + source->position, &n, nullptr) != ERR_OK)
        return CURL_READFUNC_ABORT;
    source->position += (int64_t)n;
    return n;
}

static int seek_body(void *context, curl_off_t offset, int origin) {
    BodySource *source = context;
    if (origin != SEEK_SET) return CURL_SEEKFUNC_CANTSEEK;
    source->position = offset;
    return CURL_SEEKFUNC_OK;
}

typedef struct {
    CURL *curl;
    StringBuilder body;
    int sink_fd;
    bool failed;
} Sink;

static size_t write_body(char *data, size_t size, size_t count, void *context) {
    Sink *sink = context;
    size_t n = size * count;
    long status = 0;
    curl_easy_getinfo(sink->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 200 && status < 300 && sink->sink_fd >= 0) {
        if (file_write(sink->sink_fd, (String){data, n}, nullptr) != ERR_OK) {
            sink->failed = true;
            return 0;
        }
        return n;
    }
    str_builder_append(&sink->body, (String){data, n});
    return n;
}

typedef struct {
    HttpHeaderFn on_header;
    void *context;
} HeaderSink;

static size_t read_header(char *data, size_t size, size_t count, void *context) {
    HeaderSink *sink = context;
    size_t n = size * count;
    if (!sink->on_header) return n;
    String line = {data, n};
    if (str_starts_with(line, S("HTTP/"))) {
        sink->on_header(sink->context, S(""), S(""));
        return n;
    }
    String name, value;
    if (str_cut(line, ':', &name, &value)) sink->on_header(sink->context, name, str_trim(value));
    return n;
}

static int check_cancel(void *context, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total, curl_off_t uploaded) {
    return cancel_requested(context) ? 1 : 0;
}

Error http_perform(Cancel *cancel, Arena *arena, const HttpRequest *request, HttpResponse *response, Err *err) {
    const char *method = str_cstr(arena, request->method);
    const char *url = str_cstr(arena, request->url);
    CURL *curl = curl_easy_init();
    if (!curl) return err_set(err, ERR_NETWORK, "%s %s: cannot create an HTTP handle", method, url);
    struct curl_slist *headers = nullptr;
    for (size_t i = 0; i < request->headers.count; i++) headers = curl_slist_append(headers, str_cstr(arena, request->headers.items[i]));
    BodySource source = {.fd = request->body_fd, .offset = request->body_offset, .length = request->body_length, .memory = request->body};
    Sink sink = {.curl = curl, .body = str_builder_create(arena, 1024), .sink_fd = request->sink_fd};
    HeaderSink header_sink = {.on_header = request->on_header, .context = request->context};
    response->status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, read_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_sink);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, check_cancel);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
    if (request->timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, request->timeout_ms);
    } else {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    }
    if (str_equal(request->method, S("HEAD"))) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (str_equal(request->method, S("PUT"))) {
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_body);
        curl_easy_setopt(curl, CURLOPT_READDATA, &source);
        curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, seek_body);
        curl_easy_setopt(curl, CURLOPT_SEEKDATA, &source);
        curl_off_t size = request->body_fd >= 0 ? request->body_length : (curl_off_t)request->body.len;
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, size);
    } else if (str_equal(request->method, S("POST"))) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, str_cstr(arena, request->body));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request->body.len);
    } else if (str_equal(request->method, S("DELETE"))) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    CURLcode code = curl_easy_perform(curl);
    Error e = ERR_OK;
    if (code == CURLE_OK && !sink.failed) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
    else if (sink.failed) e = err_set(err, ERR_IO, "%s %s: write download failed", method, url);
    else if (code == CURLE_ABORTED_BY_CALLBACK) e = err_set(err, ERR_CANCELLED, "cancelled");
    else e = err_set(err, ERR_NETWORK, "%s %s: %s", method, url, curl_easy_strerror(code));
    response->body = str_builder_finish(&sink.body);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return e;
}
