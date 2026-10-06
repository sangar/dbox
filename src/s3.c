/*
 * S3 stores objects in a bucket under a prefix, over libcurl with AWS
 * Signature Version 4. Content hash and modification time ride along as
 * object metadata. Uploads above part_size go as multipart uploads, and
 * downloads are staged in a temp directory because callers want a readable
 * descriptor.
 */
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "store.h"

#define META_SHA256 "x-amz-meta-sha256"
#define META_MTIME "x-amz-meta-mtime"
#define EMPTY_SHA256 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define MAX_TRIES 3

typedef struct {
    Store base;
    char *bucket, *prefix, *region, *storage_class;
    char *access_key, *secret_key, *session_token;
    char *scheme, *host; /* host as it appears in the Host header, bucket included when virtual-hosted */
    bool path_style;
    int64_t part_size;
    char *tmp_dir;
} S3;

typedef struct {
    const char *key, *value;
} KV;

typedef struct {
    const char *method;
    const char *key; /* NULL for the bucket itself */
    const KV *query;
    size_t query_count;
    const KV *headers; /* extra x-amz-* headers, lowercase names; they are signed */
    size_t header_count;
    int body_fd; /* -1 for a memory body or none */
    int64_t body_off, body_len;
    const char *body;
    size_t body_size;
    int sink_fd; /* where a successful body goes; -1 keeps it in memory */
} Request;

typedef struct {
    long status;
    StrBuf body; /* the body unless it went to sink_fd */
    char etag[ETAG_MAX];
    int64_t content_length;
    int64_t last_modified_ns;
    char meta_sha256[SHA256_HEX_LEN];
    int64_t meta_mtime_ns;
} Response;

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static void curl_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }
void dbox_curl_init(void) { pthread_once(&curl_once, curl_init); }

/* ---- encoding ---- */

static void uri_encode(StrBuf *out, const char *s, bool keep_slash) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (c == '/' && keep_slash)) {
            sb_putc(out, (char)c);
        } else {
            sb_putc(out, '%');
            sb_putc(out, hex[c >> 4]);
            sb_putc(out, hex[c & 15]);
        }
    }
}

static int compare_kv(const void *a, const void *b) {
    const KV *x = a, *y = b;
    int c = strcmp(x->key, y->key);
    return c ? c : strcmp(x->value, y->value);
}

static void strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static void xml_unescape(StrBuf *out, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') {
            sb_putc(out, s[i]);
            continue;
        }
        const char *semi = memchr(s + i, ';', n - i);
        if (!semi) {
            sb_putc(out, s[i]);
            continue;
        }
        size_t len = (size_t)(semi - (s + i)) - 1;
        const char *ent = s + i + 1;
        if (len == 4 && strncmp(ent, "quot", 4) == 0) sb_putc(out, '"');
        else if (len == 3 && strncmp(ent, "amp", 3) == 0) sb_putc(out, '&');
        else if (len == 2 && strncmp(ent, "lt", 2) == 0) sb_putc(out, '<');
        else if (len == 2 && strncmp(ent, "gt", 2) == 0) sb_putc(out, '>');
        else if (len == 4 && strncmp(ent, "apos", 4) == 0) sb_putc(out, '\'');
        else if (len > 1 && ent[0] == '#') {
            long code = ent[1] == 'x' ? strtol(ent + 2, NULL, 16) : strtol(ent + 1, NULL, 10);
            if (code < 0x80) {
                sb_putc(out, (char)code);
            } else if (code < 0x800) {
                sb_putc(out, (char)(0xC0 | code >> 6));
                sb_putc(out, (char)(0x80 | (code & 0x3F)));
            } else if (code < 0x10000) {
                sb_putc(out, (char)(0xE0 | code >> 12));
                sb_putc(out, (char)(0x80 | ((code >> 6) & 0x3F)));
                sb_putc(out, (char)(0x80 | (code & 0x3F)));
            } else {
                sb_putc(out, (char)(0xF0 | code >> 18));
                sb_putc(out, (char)(0x80 | ((code >> 12) & 0x3F)));
                sb_putc(out, (char)(0x80 | ((code >> 6) & 0x3F)));
                sb_putc(out, (char)(0x80 | (code & 0x3F)));
            }
        } else {
            sb_append(out, s + i, len + 2);
        }
        i += len + 1;
    }
}

/* xml_element finds the first <tag> inside [s, end) and returns its content range, or NULL. */
static const char *xml_element(const char *s, const char *end, const char *tag, const char **content_end) {
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    size_t open_len = strlen(open), close_len = strlen(close);
    for (const char *p = s; p + open_len <= end; p++) {
        if (memcmp(p, open, open_len) != 0) continue;
        const char *start = p + open_len;
        for (const char *q = start; q + close_len <= end; q++) {
            if (memcmp(q, close, close_len) == 0) {
                *content_end = q;
                return start;
            }
        }
        return NULL;
    }
    return NULL;
}

/* xml_text returns the unescaped text of the first <tag> in [s, end), or "" when absent. */
static char *xml_text(Arena *a, const char *s, const char *end, const char *tag) {
    const char *content_end;
    const char *start = xml_element(s, end, tag, &content_end);
    if (!start) return arena_strdup(a, "");
    StrBuf sb = {0};
    xml_unescape(&sb, start, (size_t)(content_end - start));
    char *out = arena_strdup(a, sb_cstr(&sb));
    sb_free(&sb);
    return out;
}

/* ---- time ---- */

static int64_t parse_iso8601(const char *s) {
    struct tm tm = {0};
    int year, mon, day, hour, min, sec;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &mon, &day, &hour, &min, &sec) != 6) return 0;
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    int64_t ns = (int64_t)timegm(&tm) * NS_PER_SEC;
    const char *dot = strchr(s, '.');
    if (dot) {
        int64_t frac = 0, scale = NS_PER_SEC;
        for (const char *p = dot + 1; isdigit((unsigned char)*p) && scale > 1; p++) {
            scale /= 10;
            frac += (*p - '0') * scale;
        }
        ns += frac;
    }
    return ns;
}

static int64_t parse_rfc1123(const char *s) {
    struct tm tm = {0};
    if (!strptime(s, "%a, %d %b %Y %H:%M:%S", &tm)) return 0;
    return (int64_t)timegm(&tm) * NS_PER_SEC;
}

/* ---- the HTTP layer ---- */

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
        size_t n = MIN(want, src->mem_len - (size_t)src->pos);
        memcpy(buf, src->mem + src->pos, n);
        src->pos += (int64_t)n;
        return n;
    }
    int64_t remaining = src->len - src->pos;
    if (remaining <= 0) return 0;
    ssize_t n = pread(src->fd, buf, MIN(want, (size_t)remaining), src->off + src->pos);
    if (n < 0) return CURL_READFUNC_ABORT;
    src->pos += n;
    return (size_t)n;
}

static int seek_body(void *userdata, curl_off_t offset, int origin) {
    BodySource *src = userdata;
    if (origin != SEEK_SET) return CURL_SEEKFUNC_CANTSEEK;
    src->pos = offset;
    return CURL_SEEKFUNC_OK;
}

typedef struct {
    CURL *curl;
    Response *resp;
    int sink_fd;
    bool failed;
} Sink;

static size_t write_body(char *data, size_t size, size_t nmemb, void *userdata) {
    Sink *sink = userdata;
    size_t n = size * nmemb;
    long status = 0;
    curl_easy_getinfo(sink->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 200 && status < 300 && sink->sink_fd >= 0) {
        if (!write_all(sink->sink_fd, data, n)) {
            sink->failed = true;
            return 0;
        }
        return n;
    }
    sb_append(&sink->resp->body, data, n);
    return n;
}

static size_t read_header(char *line, size_t size, size_t nitems, void *userdata) {
    Response *r = userdata;
    size_t n = size * nitems;
    if (n >= 5 && strncmp(line, "HTTP/", 5) == 0) {
        r->etag[0] = r->meta_sha256[0] = '\0';
        r->content_length = -1;
        r->last_modified_ns = r->meta_mtime_ns = 0;
        return n;
    }
    const char *colon = memchr(line, ':', n);
    if (!colon) return n;
    size_t name_len = (size_t)(colon - line);
    const char *v = colon + 1;
    const char *end = line + n;
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    while (end > v && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) end--;
    char value[1024];
    size_t vlen = MIN((size_t)(end - v), sizeof value - 1);
    memcpy(value, v, vlen);
    value[vlen] = '\0';
    if (name_len == 4 && strncasecmp(line, "etag", 4) == 0) {
        snprintf(r->etag, sizeof r->etag, "%.*s", ETAG_MAX - 1, value);
        strip_quotes(r->etag);
    } else if (name_len == 14 && strncasecmp(line, "content-length", 14) == 0) {
        r->content_length = strtoll(value, NULL, 10);
    } else if (name_len == 13 && strncasecmp(line, "last-modified", 13) == 0) {
        r->last_modified_ns = parse_rfc1123(value);
    } else if (name_len == strlen(META_SHA256) && strncasecmp(line, META_SHA256, name_len) == 0) {
        snprintf(r->meta_sha256, sizeof r->meta_sha256, "%.*s", SHA256_HEX_LEN - 1, value);
    } else if (name_len == strlen(META_MTIME) && strncasecmp(line, META_MTIME, name_len) == 0) {
        r->meta_mtime_ns = strtoll(value, NULL, 10);
    }
    return n;
}

static int check_cancel(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    return ctx_done((Ctx *)userdata) ? 1 : 0;
}

static void canonical_uri(const S3 *s, const Request *req, StrBuf *out) {
    sb_putc(out, '/');
    if (s->path_style) {
        sb_puts(out, s->bucket);
        if (req->key) sb_putc(out, '/');
    }
    if (req->key) {
        StrBuf full = {0};
        sb_printf(&full, "%s%s", s->prefix, req->key);
        uri_encode(out, sb_cstr(&full), true);
        sb_free(&full);
    }
}

static void canonical_query(const Request *req, StrBuf *out) {
    KV *sorted = xmalloc((req->query_count + 1) * sizeof *sorted);
    memcpy(sorted, req->query, req->query_count * sizeof *sorted);
    qsort(sorted, req->query_count, sizeof *sorted, compare_kv);
    for (size_t i = 0; i < req->query_count; i++) {
        if (i) sb_putc(out, '&');
        uri_encode(out, sorted[i].key, false);
        sb_putc(out, '=');
        uri_encode(out, sorted[i].value, false);
    }
    free(sorted);
}

static void hmac_hex_chain(const S3 *s, const char *date, const char *string_to_sign, char signature[SHA256_HEX_LEN]) {
    uint8_t k_date[SHA256_LEN], k_region[SHA256_LEN], k_service[SHA256_LEN], k_signing[SHA256_LEN], sig[SHA256_LEN];
    StrBuf secret = {0};
    sb_printf(&secret, "AWS4%s", s->secret_key);
    hmac_sha256(secret.data, secret.len, date, strlen(date), k_date);
    sb_free(&secret);
    hmac_sha256(k_date, SHA256_LEN, s->region, strlen(s->region), k_region);
    hmac_sha256(k_region, SHA256_LEN, "s3", 2, k_service);
    hmac_sha256(k_service, SHA256_LEN, "aws4_request", 12, k_signing);
    hmac_sha256(k_signing, SHA256_LEN, string_to_sign, strlen(string_to_sign), sig);
    hex_encode(sig, SHA256_LEN, signature);
}

/* sign builds the signed header list for req; uri and query are the canonical forms also used in the URL. */
static struct curl_slist *sign(const S3 *s, const Request *req, const char *uri, const char *query, const char *payload_hash) {
    char amz_date[20], date[9];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(amz_date, sizeof amz_date, "%Y%m%dT%H%M%SZ", &tm);
    strftime(date, sizeof date, "%Y%m%d", &tm);

    size_t n = 0;
    KV headers[32];
    headers[n++] = (KV){"host", s->host};
    headers[n++] = (KV){"x-amz-content-sha256", payload_hash};
    headers[n++] = (KV){"x-amz-date", amz_date};
    if (*s->session_token) headers[n++] = (KV){"x-amz-security-token", s->session_token};
    for (size_t i = 0; i < req->header_count && n < ARRAY_LEN(headers); i++) headers[n++] = req->headers[i];
    qsort(headers, n, sizeof *headers, compare_kv);

    StrBuf canonical = {0}, signed_names = {0};
    sb_printf(&canonical, "%s\n%s\n%s\n", req->method, uri, query);
    for (size_t i = 0; i < n; i++) {
        sb_printf(&canonical, "%s:%s\n", headers[i].key, headers[i].value);
        sb_printf(&signed_names, "%s%s", i ? ";" : "", headers[i].key);
    }
    sb_printf(&canonical, "\n%s\n%s", sb_cstr(&signed_names), payload_hash);
    char request_hash[SHA256_HEX_LEN];
    sha256_of(canonical.data, canonical.len, request_hash);

    StrBuf scope = {0}, to_sign = {0};
    sb_printf(&scope, "%s/%s/s3/aws4_request", date, s->region);
    sb_printf(&to_sign, "AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, sb_cstr(&scope), request_hash);
    char signature[SHA256_HEX_LEN];
    hmac_hex_chain(s, date, sb_cstr(&to_sign), signature);

    struct curl_slist *list = NULL;
    StrBuf line = {0};
    for (size_t i = 0; i < n; i++) {
        if (strcmp(headers[i].key, "host") == 0) continue;
        sb_clear(&line);
        sb_printf(&line, "%s: %s", headers[i].key, headers[i].value);
        list = curl_slist_append(list, sb_cstr(&line));
    }
    sb_clear(&line);
    sb_printf(&line, "Authorization: AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s", s->access_key, sb_cstr(&scope),
              sb_cstr(&signed_names), signature);
    list = curl_slist_append(list, sb_cstr(&line));
    list = curl_slist_append(list, "Expect:");
    sb_free(&line);
    sb_free(&canonical);
    sb_free(&signed_names);
    sb_free(&scope);
    sb_free(&to_sign);
    return list;
}

static bool retryable(CURLcode code, long status) {
    if (code != CURLE_OK) return code != CURLE_ABORTED_BY_CALLBACK;
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

/* perform sends req once and fills resp; it reports transport failures as false. */
static bool perform(S3 *s, Ctx *ctx, const Request *req, Response *resp, CURLcode *code, Err *err) {
    StrBuf uri = {0}, query = {0}, url = {0};
    canonical_uri(s, req, &uri);
    canonical_query(req, &query);
    sb_printf(&url, "%s://%s%s", s->scheme, s->host, sb_cstr(&uri));
    if (query.len) sb_printf(&url, "?%s", query.data);

    char payload_hash[SHA256_HEX_LEN];
    if (req->body_fd >= 0) snprintf(payload_hash, sizeof payload_hash, "UNSIGNED-PAYLOAD");
    else if (req->body) sha256_of(req->body, req->body_size, payload_hash);
    else snprintf(payload_hash, sizeof payload_hash, EMPTY_SHA256);
    struct curl_slist *headers = sign(s, req, sb_cstr(&uri), sb_cstr(&query), payload_hash);

    CURL *curl = curl_easy_init();
    BodySource src = {.fd = req->body_fd, .off = req->body_off, .len = req->body_len, .mem = req->body, .mem_len = req->body_size};
    Sink sink = {.curl = curl, .resp = resp, .sink_fd = req->sink_fd};
    sb_clear(&resp->body);
    resp->status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, sb_cstr(&url));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, read_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, check_cancel);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
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
    *code = curl_easy_perform(curl);
    bool ok = *code == CURLE_OK && !sink.failed;
    if (ok) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status);
    else if (sink.failed) err_sys(err, "write download");
    else if (*code == CURLE_ABORTED_BY_CALLBACK) err_set(err, "cancelled");
    else err_set(err, "%s %s: %s", req->method, sb_cstr(&url), curl_easy_strerror(*code));
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    sb_free(&uri);
    sb_free(&query);
    sb_free(&url);
    return ok;
}

static void response_init(Response *r) {
    memset(r, 0, sizeof *r);
    r->content_length = -1;
}

static void response_free(Response *r) { sb_free(&r->body); }

/* request sends req, retrying transient failures, and leaves the status in resp. */
static bool request(S3 *s, Ctx *ctx, const Request *req, Response *resp, Err *err) {
    static const int64_t backoff_ms[MAX_TRIES] = {0, 200, 1000};
    for (int attempt = 0;; attempt++) {
        if (attempt > 0) {
            if (!ctx_sleep(ctx, backoff_ms[attempt])) {
                err_set(err, "cancelled");
                return false;
            }
            if (req->sink_fd >= 0) {
                lseek(req->sink_fd, 0, SEEK_SET);
                if (ftruncate(req->sink_fd, 0) != 0) return false;
            }
        }
        CURLcode code;
        bool ok = perform(s, ctx, req, resp, &code, err);
        if (!retryable(code, ok ? resp->status : 0) || attempt + 1 == MAX_TRIES) return ok;
    }
}

/* fail_status describes a non-2xx response. */
static StoreStatus fail_status(const Request *req, Response *resp, Err *err) {
    if (resp->status == 404) return STORE_NOT_FOUND;
    Arena a;
    arena_init(&a, 4096);
    const char *end = resp->body.data ? resp->body.data + resp->body.len : NULL;
    const char *code = end ? xml_text(&a, resp->body.data, end, "Code") : "";
    const char *message = end ? xml_text(&a, resp->body.data, end, "Message") : "";
    err_set(err, "%s %s: HTTP %ld%s%s%s%s", req->method, req->key ? req->key : "/", resp->status, *code ? " " : "", code, *message ? ": " : "",
            message);
    arena_free(&a);
    return STORE_ERROR;
}

static bool ok_status(long status) { return status >= 200 && status < 300; }

/* ---- the store operations ---- */

static size_t meta_headers(const S3 *s, const Meta *meta, KV out[4], char mtime[32]) {
    size_t n = 0;
    out[n++] = (KV){META_SHA256, meta->sha256};
    if (meta->mtime_ns != 0) {
        snprintf(mtime, 32, "%lld", (long long)meta->mtime_ns);
        out[n++] = (KV){META_MTIME, mtime};
    }
    if (*s->storage_class) out[n++] = (KV){"x-amz-storage-class", s->storage_class};
    return n;
}

static StoreStatus put_single(S3 *s, Ctx *ctx, const char *key, int fd, int64_t size, const Meta *meta, char etag[ETAG_MAX], Err *err) {
    KV headers[4];
    char mtime[32];
    Request req = {.method = "PUT", .key = key, .headers = headers, .header_count = meta_headers(s, meta, headers, mtime), .body_fd = fd, .body_len = size, .sink_fd = -1};
    Response resp;
    response_init(&resp);
    StoreStatus status = STORE_ERROR;
    if (request(s, ctx, &req, &resp, err)) {
        if (ok_status(resp.status)) {
            snprintf(etag, ETAG_MAX, "%s", resp.etag);
            status = STORE_OK;
        } else {
            status = fail_status(&req, &resp, err);
        }
    }
    response_free(&resp);
    return status;
}

static void abort_upload(S3 *s, Ctx *ctx, const char *key, const char *upload_id) {
    KV query[] = {{"uploadId", upload_id}};
    Request req = {.method = "DELETE", .key = key, .query = query, .query_count = 1, .body_fd = -1, .sink_fd = -1};
    Response resp;
    response_init(&resp);
    Err ignored;
    request(s, ctx, &req, &resp, &ignored);
    response_free(&resp);
}

static StoreStatus put_multipart(S3 *s, Ctx *ctx, const char *key, int fd, int64_t size, const Meta *meta, char etag[ETAG_MAX], Err *err) {
    Arena a;
    arena_init(&a, 16 * 1024);
    StoreStatus status = STORE_ERROR;
    Response resp;
    response_init(&resp);
    KV headers[4];
    char mtime[32];
    KV start_query[] = {{"uploads", ""}};
    Request start = {.method = "POST", .key = key, .query = start_query, .query_count = 1, .headers = headers, .header_count = meta_headers(s, meta, headers, mtime), .body_fd = -1, .sink_fd = -1};
    if (!request(s, ctx, &start, &resp, err)) goto out;
    if (!ok_status(resp.status)) {
        status = fail_status(&start, &resp, err);
        goto out;
    }
    char *upload_id = xml_text(&a, resp.body.data, resp.body.data + resp.body.len, "UploadId");
    if (!*upload_id) {
        err_set(err, "POST %s: no UploadId in response", key);
        goto out;
    }
    StrBuf parts = {0};
    sb_puts(&parts, "<CompleteMultipartUpload>");
    bool ok = true;
    for (int64_t off = 0, number = 1; ok && off < size; off += s->part_size, number++) {
        char num[24];
        snprintf(num, sizeof num, "%lld", (long long)number);
        KV query[] = {{"partNumber", num}, {"uploadId", upload_id}};
        Request part = {.method = "PUT", .key = key, .query = query, .query_count = 2, .body_fd = fd, .body_off = off, .body_len = MIN(s->part_size, size - off), .sink_fd = -1};
        ok = request(s, ctx, &part, &resp, err);
        if (ok && !ok_status(resp.status)) {
            fail_status(&part, &resp, err);
            ok = false;
        }
        if (ok) sb_printf(&parts, "<Part><PartNumber>%s</PartNumber><ETag>\"%s\"</ETag></Part>", num, resp.etag);
    }
    sb_puts(&parts, "</CompleteMultipartUpload>");
    if (ok) {
        KV query[] = {{"uploadId", upload_id}};
        Request complete = {.method = "POST", .key = key, .query = query, .query_count = 1, .body_fd = -1, .body = parts.data, .body_size = parts.len, .sink_fd = -1};
        ok = request(s, ctx, &complete, &resp, err);
        if (ok && !ok_status(resp.status)) {
            fail_status(&complete, &resp, err);
            ok = false;
        }
        if (ok) {
            const char *end = resp.body.data + resp.body.len;
            char *code = xml_text(&a, resp.body.data, end, "Code");
            if (*code) {
                err_set(err, "POST %s: %s: %s", key, code, xml_text(&a, resp.body.data, end, "Message"));
                ok = false;
            } else {
                char *tag = xml_text(&a, resp.body.data, end, "ETag");
                strip_quotes(tag);
                snprintf(etag, ETAG_MAX, "%s", *tag ? tag : resp.etag);
                status = STORE_OK;
            }
        }
    }
    sb_free(&parts);
    if (!ok) abort_upload(s, ctx, key, upload_id);
out:
    response_free(&resp);
    arena_free(&a);
    return status;
}

static StoreStatus s3_put(Store *base, Ctx *ctx, const char *key, int fd, int64_t size, const Meta *meta, char etag[ETAG_MAX], Err *err) {
    S3 *s = (S3 *)base;
    if (size < s->part_size) return put_single(s, ctx, key, fd, size, meta, etag, err);
    return put_multipart(s, ctx, key, fd, size, meta, etag, err);
}

static void describe(Object *obj, const char *key, const Response *resp) {
    memset(obj, 0, sizeof *obj);
    obj->key = key;
    obj->size = resp->content_length < 0 ? 0 : resp->content_length;
    snprintf(obj->etag, sizeof obj->etag, "%s", resp->etag);
    snprintf(obj->sha256, sizeof obj->sha256, "%s", resp->meta_sha256);
    obj->mtime_ns = resp->meta_mtime_ns ? resp->meta_mtime_ns : resp->last_modified_ns;
}

static StoreStatus s3_get(Store *base, Ctx *ctx, const char *key, Object *obj, int *fd, Err *err) {
    S3 *s = (S3 *)base;
    int tmp = temp_file(s->tmp_dir, err);
    if (tmp < 0) return STORE_ERROR;
    Request req = {.method = "GET", .key = key, .body_fd = -1, .sink_fd = tmp};
    Response resp;
    response_init(&resp);
    StoreStatus status = STORE_ERROR;
    if (request(s, ctx, &req, &resp, err)) {
        if (ok_status(resp.status)) {
            describe(obj, key, &resp);
            if (resp.content_length < 0) obj->size = lseek(tmp, 0, SEEK_CUR);
            lseek(tmp, 0, SEEK_SET);
            *fd = tmp;
            status = STORE_OK;
        } else {
            status = fail_status(&req, &resp, err);
        }
    }
    if (status != STORE_OK) close(tmp);
    response_free(&resp);
    return status;
}

static StoreStatus s3_head(Store *base, Ctx *ctx, const char *key, Object *obj, Err *err) {
    S3 *s = (S3 *)base;
    Request req = {.method = "HEAD", .key = key, .body_fd = -1, .sink_fd = -1};
    Response resp;
    response_init(&resp);
    StoreStatus status = STORE_ERROR;
    if (request(s, ctx, &req, &resp, err)) {
        if (ok_status(resp.status)) {
            describe(obj, key, &resp);
            status = STORE_OK;
        } else if (resp.status == 404) {
            status = STORE_NOT_FOUND;
        } else {
            err_set(err, "HEAD %s: HTTP %ld", key, resp.status);
        }
    }
    response_free(&resp);
    return status;
}

static StoreStatus s3_delete(Store *base, Ctx *ctx, const char *key, Err *err) {
    S3 *s = (S3 *)base;
    Request req = {.method = "DELETE", .key = key, .body_fd = -1, .sink_fd = -1};
    Response resp;
    response_init(&resp);
    StoreStatus status = STORE_ERROR;
    if (request(s, ctx, &req, &resp, err)) {
        status = ok_status(resp.status) || resp.status == 404 ? STORE_OK : fail_status(&req, &resp, err);
    }
    response_free(&resp);
    return status;
}

static StoreStatus s3_list(Store *base, Ctx *ctx, Arena *a, Object **objects, size_t *count, Err *err) {
    S3 *s = (S3 *)base;
    size_t n = 0, cap = 256;
    Object *out = xmalloc(cap * sizeof *out);
    char *token = NULL;
    StoreStatus status = STORE_ERROR;
    Response resp;
    response_init(&resp);
    Arena scratch;
    arena_init(&scratch, 64 * 1024);
    for (;;) {
        KV query[3] = {{"list-type", "2"}, {"prefix", s->prefix}};
        size_t qn = 2;
        if (token) query[qn++] = (KV){"continuation-token", token};
        Request req = {.method = "GET", .query = query, .query_count = qn, .body_fd = -1, .sink_fd = -1};
        if (!request(s, ctx, &req, &resp, err)) goto out;
        if (!ok_status(resp.status)) {
            status = fail_status(&req, &resp, err);
            goto out;
        }
        const char *p = resp.body.data, *end = p + resp.body.len;
        for (;;) {
            const char *item_end;
            const char *item = xml_element(p, end, "Contents", &item_end);
            if (!item) break;
            char *full = xml_text(&scratch, item, item_end, "Key");
            const char *key = has_prefix(full, s->prefix) ? full + strlen(s->prefix) : full;
            if (*key && !has_suffix(key, "/")) {
                if (n == cap) out = xrealloc(out, (cap *= 2) * sizeof *out);
                Object *o = &out[n++];
                memset(o, 0, sizeof *o);
                o->key = arena_strdup(a, key);
                o->size = strtoll(xml_text(&scratch, item, item_end, "Size"), NULL, 10);
                char *etag = xml_text(&scratch, item, item_end, "ETag");
                strip_quotes(etag);
                snprintf(o->etag, sizeof o->etag, "%s", etag);
                o->mtime_ns = parse_iso8601(xml_text(&scratch, item, item_end, "LastModified"));
            }
            p = item_end;
        }
        bool truncated = strcmp(xml_text(&scratch, resp.body.data, end, "IsTruncated"), "true") == 0;
        token = truncated ? xml_text(&scratch, resp.body.data, end, "NextContinuationToken") : NULL;
        if (!token || !*token) break;
    }
    status = STORE_OK;
out:
    *objects = arena_alloc(a, (n + 1) * sizeof **objects);
    memcpy(*objects, out, n * sizeof **objects);
    free(out);
    *count = n;
    response_free(&resp);
    arena_free(&scratch);
    return status;
}

bool s3_create_bucket(Store *base, Ctx *ctx, Err *err) {
    S3 *s = (S3 *)base;
    Request req = {.method = "PUT", .body_fd = -1, .sink_fd = -1};
    Response resp;
    response_init(&resp);
    bool ok = request(s, ctx, &req, &resp, err);
    if (ok && !ok_status(resp.status)) {
        Arena a;
        arena_init(&a, 4096);
        const char *code = xml_text(&a, resp.body.data, resp.body.data + resp.body.len, "Code");
        ok = strcmp(code, "BucketAlreadyOwnedByYou") == 0;
        if (!ok) fail_status(&req, &resp, err);
        arena_free(&a);
    }
    response_free(&resp);
    return ok;
}

static void s3_close(Store *base) {
    S3 *s = (S3 *)base;
    free(s->bucket);
    free(s->prefix);
    free(s->region);
    free(s->storage_class);
    free(s->access_key);
    free(s->secret_key);
    free(s->session_token);
    free(s->scheme);
    free(s->host);
    free(s->tmp_dir);
    free(s);
}

static const StoreOps s3_ops = {s3_put, s3_get, s3_head, s3_delete, s3_list, s3_close};

/* ---- credentials ---- */

/* credentials_file reads ~/.aws/credentials for the profile and returns whether it had keys. */
static bool credentials_file(const char *profile, char **access, char **secret, char **token) {
    const char *custom = getenv("AWS_SHARED_CREDENTIALS_FILE");
    char *path = custom && *custom ? xstrdup(custom) : path_join(home_dir(), ".aws/credentials");
    StrBuf raw = {0};
    Err ignored;
    bool ok = read_file(path, &raw, &ignored);
    free(path);
    if (!ok) return false;
    bool in_profile = false;
    char *save = NULL;
    for (char *line = strtok_r(raw.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\r' || line[n - 1] == '\t')) line[--n] = '\0';
        if (*line == '[') {
            in_profile = n > 1 && strncmp(line + 1, profile, n - 2) == 0 && strlen(profile) == n - 2;
            continue;
        }
        if (!in_profile || *line == '#' || *line == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *value = eq + 1;
        while (*value == ' ' || *value == '\t') value++;
        size_t k = strlen(key);
        while (k > 0 && (key[k - 1] == ' ' || key[k - 1] == '\t')) key[--k] = '\0';
        char **dst = strcmp(key, "aws_access_key_id") == 0 ? access : strcmp(key, "aws_secret_access_key") == 0 ? secret : strcmp(key, "aws_session_token") == 0 ? token : NULL;
        if (dst) {
            free(*dst);
            *dst = xstrdup(value);
        }
    }
    sb_free(&raw);
    return *access && *secret;
}

static bool resolve_credentials(const StoreConfig *cfg, S3 *s, Err *err) {
    s->session_token = xstrdup("");
    if (*cfg->access_key) {
        s->access_key = xstrdup(cfg->access_key);
        s->secret_key = xstrdup(cfg->secret_key);
        return true;
    }
    const char *access = getenv("AWS_ACCESS_KEY_ID"), *secret = getenv("AWS_SECRET_ACCESS_KEY"), *token = getenv("AWS_SESSION_TOKEN");
    if (access && *access && secret && *secret) {
        s->access_key = xstrdup(access);
        s->secret_key = xstrdup(secret);
        if (token && *token) {
            free(s->session_token);
            s->session_token = xstrdup(token);
        }
        return true;
    }
    const char *profile = getenv("AWS_PROFILE");
    char *a = NULL, *k = NULL, *t = NULL;
    if (credentials_file(profile && *profile ? profile : "default", &a, &k, &t)) {
        s->access_key = a;
        s->secret_key = k;
        if (t) {
            free(s->session_token);
            s->session_token = t;
        }
        return true;
    }
    free(a);
    free(k);
    free(t);
    err_set(err, "no credentials: set access_key and secret_key, AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, or ~/.aws/credentials");
    return false;
}

/* parse_endpoint splits scheme://host[:port][/...] into scheme and host, as curl sends it in the Host header. */
static bool parse_endpoint(const char *endpoint, char **scheme, char **host, Err *err) {
    const char *sep = strstr(endpoint, "://");
    if (!sep) {
        err_set(err, "endpoint %s: want scheme://host[:port]", endpoint);
        return false;
    }
    *scheme = xstrndup(endpoint, (size_t)(sep - endpoint));
    const char *h = sep + 3;
    const char *end = strchr(h, '/');
    char *hp = end ? xstrndup(h, (size_t)(end - h)) : xstrdup(h);
    bool https = strcmp(*scheme, "https") == 0;
    char *colon = strrchr(hp, ':');
    if (colon && ((https && strcmp(colon, ":443") == 0) || (!https && strcmp(colon, ":80") == 0))) *colon = '\0';
    *host = hp;
    if (!*hp || (strcmp(*scheme, "http") != 0 && !https)) {
        err_set(err, "endpoint %s: want http://host or https://host", endpoint);
        return false;
    }
    return true;
}

Store *s3_open(Ctx *ctx, const StoreConfig *cfg, int64_t part_size, const char *tmp_dir, Err *err) {
    dbox_curl_init();
    S3 *s = xcalloc(1, sizeof *s);
    s->base.ops = &s3_ops;
    s->bucket = xstrdup(cfg->bucket);
    s->prefix = xstrdup(cfg->prefix);
    s->region = xstrdup(cfg->region);
    s->storage_class = xstrdup(cfg->storage_class);
    s->path_style = cfg->path_style;
    s->part_size = part_size;
    s->tmp_dir = xstrdup(tmp_dir);
    char *endpoint = *cfg->endpoint ? xstrdup(cfg->endpoint) : NULL;
    if (!endpoint) {
        StrBuf sb = {0};
        sb_printf(&sb, "https://s3.%s.amazonaws.com", cfg->region);
        endpoint = sb.data;
    }
    bool ok = parse_endpoint(endpoint, &s->scheme, &s->host, err) && resolve_credentials(cfg, s, err);
    free(endpoint);
    if (!ok) {
        s3_close(&s->base);
        return NULL;
    }
    if (!s->path_style) {
        char *virtual_host = s->host;
        s->host = path_join(s->bucket, virtual_host);
        s->host[strlen(s->bucket)] = '.';
        free(virtual_host);
    }
    return &s->base;
}
