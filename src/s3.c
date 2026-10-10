/*
 * S3 stores objects in a bucket under a prefix, over HTTP with AWS
 * Signature Version 4. Content hash and modification time ride along as
 * object metadata. Uploads above part_size go as multipart uploads, and
 * downloads are staged in a temp directory because callers want a readable
 * descriptor.
 */
#include <stdio.h>
#include <string.h>

#include "content.h"
#include "http.h"
#include "mc/container/sort.h"
#include "mc/crypto/sigv4.h"
#include "mc/platform/platform.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"
#include "mc/text/utf8.h"
#include "store.h"

#define META_SHA256 "x-amz-meta-sha256"
#define META_MTIME "x-amz-meta-mtime"

enum { MAX_TRIES = 3, LIST_PAGE_BLOCK = 64 * 1024 };

typedef struct {
    Store base;
    String bucket, prefix, region, storage_class;
    String access_key, secret_key, session_token;
    String scheme, host; /* host as it appears in the Host header, bucket included when virtual-hosted */
    bool path_style;
    int64_t part_size;
    String tmp_dir;
} S3;

typedef struct {
    String name, value;
} Pair;

typedef struct {
    String method;
    String key; /* empty for the bucket itself */
    const Pair *query;
    size_t query_count;
    const Pair *headers; /* extra x-amz-* headers, lowercase names; they are signed */
    size_t header_count;
    int body_fd; /* -1 for a memory body or none */
    int64_t body_offset, body_length;
    String body;
    int sink_fd; /* where a successful body goes; -1 keeps it in memory */
} Request;

typedef struct {
    Arena *arena;
    HttpResponse http;
    String etag;
    int64_t content_length;
    int64_t last_modified_ns;
    String meta_sha256;
    int64_t meta_mtime_ns;
} Response;

/* ---- XML ---- */

static String strip_quotes(String s) {
    if (s.len >= 2 && s.data[0] == '"' && s.data[s.len - 1] == '"') return str_slice(s, 1, s.len - 1);
    return s;
}

static uint64_t parse_number(String digits, unsigned base) {
    uint64_t value = 0;
    for (size_t i = 0; i < digits.len; i++) {
        char c = digits.data[i];
        unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : base;
        if (digit >= base) break;
        value = value * base + digit;
    }
    return value;
}

static void append_entity(StringBuilder *out, String entity) {
    static const struct {
        const char *name;
        char c;
    } named[] = {{"quot", '"'}, {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"apos", '\''}};
    for (size_t i = 0; i < countof(named); i++) {
        if (str_equal(entity, S(named[i].name))) {
            str_builder_append_char(out, named[i].c);
            return;
        }
    }
    if (entity.len > 1 && entity.data[0] == '#') {
        bool hex = entity.data[1] == 'x';
        uint64_t code = parse_number(str_slice(entity, hex ? 2 : 1, entity.len), hex ? 16 : 10);
        char encoded[UTF8_MAX_BYTES];
        str_builder_append(out, (String){encoded, utf8_encode((uint32_t)min_size(code, UINT32_MAX), encoded)});
        return;
    }
    str_builder_append_format(out, "&%.*s;", (int)entity.len, entity.data);
}

static String xml_unescape(Arena *arena, String s) {
    StringBuilder out = str_builder_create(arena, s.len);
    for (size_t i = 0; i < s.len; i++) {
        size_t semicolon;
        if (s.data[i] != '&' || !str_find_char(str_slice(s, i, s.len), ';', &semicolon)) {
            str_builder_append_char(&out, s.data[i]);
            continue;
        }
        append_entity(&out, str_slice(s, i + 1, i + semicolon));
        i += semicolon;
    }
    return str_builder_finish(&out);
}

/* xml_element finds the first <tag> in s; content is what it encloses and after is what follows its closing tag. */
static bool xml_element(String s, const char *tag, String *content, String *after) {
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    size_t start, end;
    if (!str_find(s, S(open), &start)) return false;
    String rest = str_slice(s, start + strlen(open), s.len);
    if (!str_find(rest, S(close), &end)) return false;
    *content = str_slice(rest, 0, end);
    if (after) *after = str_slice(rest, end + strlen(close), rest.len);
    return true;
}

/* xml_text returns the unescaped text of the first <tag> in s, or "" when absent. */
static String xml_text(Arena *arena, String s, const char *tag) {
    String content;
    return xml_element(s, tag, &content, nullptr) ? xml_unescape(arena, content) : S("");
}

/* ---- time ---- */

static int64_t field(String s, size_t start, size_t end) {
    uint64_t value = 0;
    return str_parse_u64(str_slice(s, start, end), &value) ? (int64_t)value : 0;
}

static int64_t unix_ns(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second) {
    int64_t days = civil_to_days((CivilDate){.year = year, .month = (unsigned)month, .day = (unsigned)day});
    return ((days * 24 + hour) * 60 + minute) * 60 * NS_PER_SECOND + second * NS_PER_SECOND;
}

/* parse_iso8601 reads 2024-03-13T10:00:00.123Z, as ListObjectsV2 writes LastModified. */
static int64_t parse_iso8601(String s) {
    if (s.len < 19 || s.data[4] != '-' || s.data[10] != 'T') return 0;
    int64_t ns = unix_ns(field(s, 0, 4), field(s, 5, 7), field(s, 8, 10), field(s, 11, 13), field(s, 14, 16), field(s, 17, 19));
    if (s.len > 20 && s.data[19] == '.') {
        int64_t scale = NS_PER_SECOND;
        for (size_t i = 20; i < s.len && s.data[i] >= '0' && s.data[i] <= '9' && scale > 1; i++) {
            scale /= 10;
            ns += (s.data[i] - '0') * scale;
        }
    }
    return ns;
}

/* parse_rfc1123 reads Wed, 13 Mar 2024 10:00:00 GMT, as the Last-Modified header carries it. */
static int64_t parse_rfc1123(Arena *arena, String s) {
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    StringList fields = str_fields(arena, s);
    if (fields.count < 5 || fields.items[4].len != 8) return 0;
    int64_t month = 0;
    for (size_t i = 0; i < countof(months); i++)
        if (str_equal(fields.items[2], S(months[i]))) month = (int64_t)i + 1;
    if (month == 0) return 0;
    String clock = fields.items[4];
    return unix_ns(field(fields.items[3], 0, 4), month, field(fields.items[1], 0, 2), field(clock, 0, 2), field(clock, 3, 5), field(clock, 6, 8));
}

/* ---- the HTTP layer ---- */

/* on_header picks the object metadata out of the response headers; a new status line resets it. */
static void on_header(void *context, String name, String value) {
    Response *r = context;
    if (name.len == 0) {
        r->etag = r->meta_sha256 = S("");
        r->content_length = -1;
        r->last_modified_ns = r->meta_mtime_ns = 0;
        return;
    }
    int64_t number;
    if (str_equal_ignore_case(name, S("etag"))) r->etag = str_copy(r->arena, strip_quotes(value));
    else if (str_equal_ignore_case(name, S("content-length"))) r->content_length = str_parse_i64(value, &number) ? number : -1;
    else if (str_equal_ignore_case(name, S("last-modified"))) r->last_modified_ns = parse_rfc1123(r->arena, value);
    else if (str_equal_ignore_case(name, S(META_SHA256))) r->meta_sha256 = str_copy(r->arena, value);
    else if (str_equal_ignore_case(name, S(META_MTIME))) r->meta_mtime_ns = str_parse_i64(value, &number) ? number : 0;
}

static String canonical_uri(Arena *arena, const S3 *s, const Request *req) {
    StringBuilder out = str_builder_create(arena, 128);
    str_builder_append_char(&out, '/');
    if (s->path_style) {
        str_builder_append(&out, s->bucket);
        if (req->key.len > 0) str_builder_append_char(&out, '/');
    }
    if (req->key.len > 0) str_builder_append(&out, str_percent_encode(arena, str_concat(arena, s->prefix, req->key), true));
    return str_builder_finish(&out);
}

static int compare_pairs(const void *context, const void *a, const void *b) {
    const Pair *x = a, *y = b;
    int c = str_compare(x->name, y->name);
    return c ? c : str_compare(x->value, y->value);
}

static String canonical_query(Arena *arena, const Request *req) {
    Pair *sorted = arena_push(arena, arena_size_mul(req->query_count + 1, sizeof *sorted));
    if (req->query_count > 0) memcpy(sorted, req->query, req->query_count * sizeof *sorted);
    sort_stable(arena, sorted, req->query_count, sizeof *sorted, compare_pairs, nullptr);
    StringBuilder out = str_builder_create(arena, 128);
    for (size_t i = 0; i < req->query_count; i++) {
        if (i) str_builder_append_char(&out, '&');
        str_builder_append(&out, str_percent_encode(arena, sorted[i].name, false));
        str_builder_append_char(&out, '=');
        str_builder_append(&out, str_percent_encode(arena, sorted[i].value, false));
    }
    return str_builder_finish(&out);
}

static String header_line(Arena *arena, String name, String value) {
    return str_format(arena, "%.*s: %.*s", (int)name.len, name.data, (int)value.len, value.data);
}

/* signed_headers returns the header lines for req, the Authorization header last; uri and query are the canonical forms also used in the URL. */
static StringList signed_headers(Arena *arena, const S3 *s, const Request *req, String uri, String query, String payload_hash) {
    StringList amz = {0};
    strlist_push(arena, &amz, header_line(arena, S("x-amz-content-sha256"), payload_hash));
    if (s->session_token.len > 0) strlist_push(arena, &amz, header_line(arena, S("x-amz-security-token"), s->session_token));
    for (size_t i = 0; i < req->header_count; i++) strlist_push(arena, &amz, header_line(arena, req->headers[i].name, req->headers[i].value));
    SigV4Request signing = {
        .method = req->method,
        .path = uri,
        .query = query,
        .host = s->host,
        .amz_date = sigv4_date(arena, clock_wall_ns() / NS_PER_SECOND),
        .headers = amz,
        .payload_hash = payload_hash,
    };
    SigV4Credentials credentials = {.access_key = s->access_key, .secret_key = s->secret_key, .region = s->region, .service = S("s3")};
    StringList lines = strlist_copy(arena, amz);
    strlist_push(arena, &lines, header_line(arena, S("x-amz-date"), signing.amz_date));
    strlist_push(arena, &lines, header_line(arena, S("Authorization"), sigv4_authorization(arena, signing, credentials)));
    strlist_push(arena, &lines, S("Expect:"));
    return lines;
}

/* retryable says whether a failed attempt is worth another: any transport failure except cancellation, or a throttling or server error status. */
static bool retryable(Error e, long status) {
    if (e != ERR_OK) return e != ERR_CANCELLED;
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

/* perform sends req once and fills resp; transport failures are the error, HTTP statuses are left in resp. */
[[nodiscard]] static Error perform(S3 *s, Cancel *cancel, const Request *req, Response *resp, Err *err) {
    Arena *arena = resp->arena;
    String uri = canonical_uri(arena, s, req);
    String query = canonical_query(arena, req);
    String url = str_format(arena, "%.*s://%.*s%.*s%s%.*s", (int)s->scheme.len, s->scheme.data, (int)s->host.len, s->host.data, (int)uri.len, uri.data,
                            query.len ? "?" : "", (int)query.len, query.data);
    String payload_hash = req->body_fd >= 0 ? S("UNSIGNED-PAYLOAD") : sigv4_payload_hash(arena, req->body);
    HttpRequest http = {
        .method = req->method,
        .url = url,
        .headers = signed_headers(arena, s, req, uri, query, payload_hash),
        .body_fd = req->body_fd,
        .body_offset = req->body_offset,
        .body_length = req->body_length,
        .body = req->body,
        .sink_fd = req->sink_fd,
        .on_header = on_header,
        .context = resp,
    };
    return http_perform(cancel, arena, &http, &resp->http, err);
}

static Response response_create(Arena *arena) { return (Response){.arena = arena, .etag = S(""), .meta_sha256 = S(""), .content_length = -1}; }

/* request sends req, retrying transient failures, and leaves the status in resp. */
[[nodiscard]] static Error request(S3 *s, Cancel *cancel, const Request *req, Response *resp, Err *err) {
    static const int64_t backoff_ms[MAX_TRIES] = {0, 200, 1000};
    for (int attempt = 0;; attempt++) {
        if (attempt > 0) {
            if (!cancel_sleep(cancel, backoff_ms[attempt])) return err_set(err, ERR_CANCELLED, "cancelled");
            if (req->sink_fd >= 0) {
                Error e = file_truncate(req->sink_fd, 0, err);
                if (e == ERR_OK) e = file_seek(req->sink_fd, 0, err);
                if (e != ERR_OK) return e;
            }
        }
        Error e = perform(s, cancel, req, resp, err);
        if (!retryable(e, e == ERR_OK ? resp->http.status : 0) || attempt + 1 == MAX_TRIES) return e;
    }
}

/* fail_status describes a non-2xx response. */
[[nodiscard]] static Error fail_status(const Request *req, const Response *resp, Err *err) {
    if (resp->http.status == 404) return ERR_NOT_FOUND;
    String code = xml_text(resp->arena, resp->http.body, "Code");
    String message = xml_text(resp->arena, resp->http.body, "Message");
    String key = req->key.len > 0 ? req->key : S("/");
    return err_set(err, ERR_NETWORK, "%.*s %.*s: HTTP %ld%s%.*s%s%.*s", (int)req->method.len, req->method.data, (int)key.len, key.data, resp->http.status,
                   code.len ? " " : "", (int)code.len, code.data, message.len ? ": " : "", (int)message.len, message.data);
}

static bool ok_status(long status) { return status >= 200 && status < 300; }

/* ---- the store operations ---- */

static size_t meta_headers(Arena *arena, const S3 *s, Meta meta, Pair out[3]) {
    size_t n = 0;
    out[n++] = (Pair){S(META_SHA256), meta.sha256};
    if (meta.mtime_ns != 0) out[n++] = (Pair){S(META_MTIME), str_format(arena, "%lld", (long long)meta.mtime_ns)};
    if (s->storage_class.len > 0) out[n++] = (Pair){S("x-amz-storage-class"), s->storage_class};
    return n;
}

[[nodiscard]] static Error put_single(S3 *s, Cancel *cancel, Arena *arena, String key, int fd, int64_t size, Meta meta, String *etag, Err *err) {
    Pair headers[3];
    Request req = {.method = S("PUT"), .key = key, .headers = headers, .header_count = meta_headers(arena, s, meta, headers), .body_fd = fd, .body_length = size, .sink_fd = -1};
    Response resp = response_create(arena);
    Error e = request(s, cancel, &req, &resp, err);
    if (e != ERR_OK) return e;
    if (!ok_status(resp.http.status)) return fail_status(&req, &resp, err);
    *etag = resp.etag;
    return ERR_OK;
}

static void abort_upload(S3 *s, Cancel *cancel, Arena *arena, String key, String upload_id) {
    Pair query[] = {{S("uploadId"), upload_id}};
    Request req = {.method = S("DELETE"), .key = key, .query = query, .query_count = 1, .body_fd = -1, .sink_fd = -1};
    Response resp = response_create(arena);
    (void)request(s, cancel, &req, &resp, nullptr);
}

[[nodiscard]] static Error put_multipart(S3 *s, Cancel *cancel, Arena *arena, String key, int fd, int64_t size, Meta meta, String *etag, Err *err) {
    Response resp = response_create(arena);
    Pair headers[3];
    Pair start_query[] = {{S("uploads"), S("")}};
    Request start = {.method = S("POST"), .key = key, .query = start_query, .query_count = 1, .headers = headers, .header_count = meta_headers(arena, s, meta, headers), .body_fd = -1, .sink_fd = -1};
    Error e = request(s, cancel, &start, &resp, err);
    if (e != ERR_OK) return e;
    if (!ok_status(resp.http.status)) return fail_status(&start, &resp, err);
    String upload_id = xml_text(arena, resp.http.body, "UploadId");
    if (upload_id.len == 0) return err_set(err, ERR_NETWORK, "POST %.*s: no UploadId in response", (int)key.len, key.data);
    StringBuilder parts = str_builder_create(arena, 1024);
    str_builder_append(&parts, S("<CompleteMultipartUpload>"));
    for (int64_t offset = 0, number = 1; e == ERR_OK && offset < size; offset += s->part_size, number++) {
        String part_number = str_format(arena, "%lld", (long long)number);
        Pair query[] = {{S("partNumber"), part_number}, {S("uploadId"), upload_id}};
        int64_t length = s->part_size < size - offset ? s->part_size : size - offset;
        Request part = {.method = S("PUT"), .key = key, .query = query, .query_count = 2, .body_fd = fd, .body_offset = offset, .body_length = length, .sink_fd = -1};
        e = request(s, cancel, &part, &resp, err);
        if (e == ERR_OK && !ok_status(resp.http.status)) e = fail_status(&part, &resp, err);
        if (e == ERR_OK)
            str_builder_append_format(&parts, "<Part><PartNumber>%.*s</PartNumber><ETag>\"%.*s\"</ETag></Part>", (int)part_number.len, part_number.data, (int)resp.etag.len,
                                      resp.etag.data);
    }
    str_builder_append(&parts, S("</CompleteMultipartUpload>"));
    if (e == ERR_OK) {
        Pair query[] = {{S("uploadId"), upload_id}};
        Request complete = {.method = S("POST"), .key = key, .query = query, .query_count = 1, .body_fd = -1, .body = str_builder_finish(&parts), .sink_fd = -1};
        e = request(s, cancel, &complete, &resp, err);
        if (e == ERR_OK && !ok_status(resp.http.status)) e = fail_status(&complete, &resp, err);
        if (e == ERR_OK) {
            String code = xml_text(arena, resp.http.body, "Code");
            if (code.len > 0) {
                String message = xml_text(arena, resp.http.body, "Message");
                e = err_set(err, ERR_NETWORK, "POST %.*s: %.*s: %.*s", (int)key.len, key.data, (int)code.len, code.data, (int)message.len, message.data);
            } else {
                String tag = strip_quotes(xml_text(arena, resp.http.body, "ETag"));
                *etag = tag.len > 0 ? tag : resp.etag;
            }
        }
    }
    if (e != ERR_OK) abort_upload(s, cancel, arena, key, upload_id);
    return e;
}

[[nodiscard]] static Error s3_put(Store *store, Cancel *cancel, Arena *arena, String key, int fd, int64_t size, Meta meta, String *etag, Err *err) {
    S3 *s = (S3 *)store;
    if (size < s->part_size) return put_single(s, cancel, arena, key, fd, size, meta, etag, err);
    return put_multipart(s, cancel, arena, key, fd, size, meta, etag, err);
}

static Object describe(String key, const Response *resp) {
    return (Object){
        .key = key,
        .size = resp->content_length < 0 ? 0 : resp->content_length,
        .etag = resp->etag,
        .sha256 = resp->meta_sha256,
        .mtime_ns = resp->meta_mtime_ns ? resp->meta_mtime_ns : resp->last_modified_ns,
    };
}

[[nodiscard]] static Error s3_get(Store *store, Cancel *cancel, Arena *arena, String key, Object *object, int *fd, Err *err) {
    S3 *s = (S3 *)store;
    int tmp;
    Error e = temp_file(arena, s->tmp_dir, &tmp, err);
    if (e != ERR_OK) return e;
    Request req = {.method = S("GET"), .key = key, .body_fd = -1, .sink_fd = tmp};
    Response resp = response_create(arena);
    e = request(s, cancel, &req, &resp, err);
    if (e == ERR_OK && !ok_status(resp.http.status)) e = fail_status(&req, &resp, err);
    FileInfo info = {0};
    if (e == ERR_OK && resp.content_length < 0) e = file_info_fd(tmp, &info, err);
    if (e == ERR_OK) e = file_seek(tmp, 0, err);
    if (e != ERR_OK) {
        file_close(tmp);
        return e;
    }
    *object = describe(key, &resp);
    if (resp.content_length < 0) object->size = info.size;
    *fd = tmp;
    return ERR_OK;
}

[[nodiscard]] static Error s3_head(Store *store, Cancel *cancel, Arena *arena, String key, Object *object, Err *err) {
    S3 *s = (S3 *)store;
    Request req = {.method = S("HEAD"), .key = key, .body_fd = -1, .sink_fd = -1};
    Response resp = response_create(arena);
    Error e = request(s, cancel, &req, &resp, err);
    if (e != ERR_OK) return e;
    if (resp.http.status == 404) return ERR_NOT_FOUND;
    if (!ok_status(resp.http.status)) return err_set(err, ERR_NETWORK, "HEAD %.*s: HTTP %ld", (int)key.len, key.data, resp.http.status);
    *object = describe(key, &resp);
    return ERR_OK;
}

[[nodiscard]] static Error s3_remove(Store *store, Cancel *cancel, Arena *arena, String key, Err *err) {
    S3 *s = (S3 *)store;
    Request req = {.method = S("DELETE"), .key = key, .body_fd = -1, .sink_fd = -1};
    Response resp = response_create(arena);
    Error e = request(s, cancel, &req, &resp, err);
    if (e == ERR_OK && !ok_status(resp.http.status) && resp.http.status != 404) e = fail_status(&req, &resp, err);
    return e;
}

/* list_page appends the objects of one ListObjectsV2 response and returns its continuation token, or "" after the last page. */
static String list_page(Arena *arena, const S3 *s, String body, ObjectList *objects) {
    String rest = body, item;
    while (xml_element(rest, "Contents", &item, &rest)) {
        String key = str_trim_prefix(xml_text(arena, item, "Key"), s->prefix);
        if (key.len == 0 || str_ends_with(key, S("/"))) continue;
        int64_t size = 0;
        objectlist_push(arena, objects,
                        (Object){
                            .key = key,
                            .size = str_parse_i64(xml_text(arena, item, "Size"), &size) ? size : 0,
                            .etag = strip_quotes(xml_text(arena, item, "ETag")),
                            .sha256 = S(""),
                            .mtime_ns = parse_iso8601(xml_text(arena, item, "LastModified")),
                        });
    }
    if (!str_equal(xml_text(arena, body, "IsTruncated"), S("true"))) return S("");
    return xml_text(arena, body, "NextContinuationToken");
}

[[nodiscard]] static Error s3_list(Store *store, Cancel *cancel, Arena *arena, ObjectList *objects, Err *err) {
    S3 *s = (S3 *)store;
    *objects = (ObjectList){0};
    /* Each page's response lives only until the next is fetched; the objects go to the caller's arena. */
    Arena *page = arena_create(LIST_PAGE_BLOCK);
    String token = S("");
    Error e = ERR_OK;
    do {
        arena_reset(page);
        Pair query[3] = {{S("list-type"), S("2")}, {S("prefix"), s->prefix}};
        size_t n = 2;
        if (token.len > 0) query[n++] = (Pair){S("continuation-token"), str_copy(page, token)};
        Request req = {.method = S("GET"), .query = query, .query_count = n, .body_fd = -1, .sink_fd = -1};
        Response resp = response_create(page);
        if ((e = request(s, cancel, &req, &resp, err)) != ERR_OK) break;
        if (!ok_status(resp.http.status)) {
            e = fail_status(&req, &resp, err);
            break;
        }
        token = list_page(arena, s, resp.http.body, objects);
    } while (token.len > 0);
    arena_destroy(page);
    return e;
}

Error s3_create_bucket(Store *store, Cancel *cancel, Arena *scratch, Err *err) {
    S3 *s = (S3 *)store;
    Request req = {.method = S("PUT"), .body_fd = -1, .sink_fd = -1};
    Response resp = response_create(scratch);
    Error e = request(s, cancel, &req, &resp, err);
    if (e == ERR_OK && !ok_status(resp.http.status) && !str_equal(xml_text(scratch, resp.http.body, "Code"), S("BucketAlreadyOwnedByYou"))) e = fail_status(&req, &resp, err);
    return e;
}

static const StoreOps s3_ops = {s3_put, s3_get, s3_head, s3_remove, s3_list};

/* ---- credentials ---- */

/* credentials_file reads ~/.aws/credentials for the profile and returns whether it had keys. */
static bool credentials_file(Arena *arena, String profile, S3 *s) {
    String path;
    if (!env_get(arena, S("AWS_SHARED_CREDENTIALS_FILE"), &path) || path.len == 0) path = path_join(arena, env_home(arena), S(".aws/credentials"));
    String raw;
    if (file_read_all(arena, path, &raw, nullptr) != ERR_OK) return false;
    StringList lines = strlist_from_lines(arena, raw);
    bool in_profile = false;
    String access = S(""), secret = S("");
    for (size_t i = 0; i < lines.count; i++) {
        String line = str_trim(lines.items[i]);
        if (str_starts_with(line, S("["))) {
            in_profile = str_ends_with(line, S("]")) && str_equal(str_slice(line, 1, line.len - 1), profile);
            continue;
        }
        String key, value;
        if (!in_profile || str_starts_with(line, S("#")) || str_starts_with(line, S(";")) || !str_cut(line, '=', &key, &value)) continue;
        key = str_trim(key);
        value = str_trim(value);
        if (str_equal(key, S("aws_access_key_id"))) access = value;
        else if (str_equal(key, S("aws_secret_access_key"))) secret = value;
        else if (str_equal(key, S("aws_session_token"))) s->session_token = value;
    }
    s->access_key = access;
    s->secret_key = secret;
    return access.len > 0 && secret.len > 0;
}

[[nodiscard]] static Error resolve_credentials(Arena *arena, const StoreConfig *config, S3 *s, Err *err) {
    s->session_token = S("");
    if (config->access_key.len > 0) {
        s->access_key = str_copy(arena, config->access_key);
        s->secret_key = str_copy(arena, config->secret_key);
        return ERR_OK;
    }
    String access, secret, token;
    if (env_get(arena, S("AWS_ACCESS_KEY_ID"), &access) && access.len > 0 && env_get(arena, S("AWS_SECRET_ACCESS_KEY"), &secret) && secret.len > 0) {
        s->access_key = access;
        s->secret_key = secret;
        if (env_get(arena, S("AWS_SESSION_TOKEN"), &token)) s->session_token = token;
        return ERR_OK;
    }
    String profile;
    if (!env_get(arena, S("AWS_PROFILE"), &profile) || profile.len == 0) profile = S("default");
    if (credentials_file(arena, profile, s)) return ERR_OK;
    return err_set(err, ERR_INVALID_ARGUMENT, "no credentials: set access_key and secret_key, AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, or ~/.aws/credentials");
}

/* parse_endpoint splits scheme://host[:port][/...] into scheme and host, as it goes in the Host header. */
[[nodiscard]] static Error parse_endpoint(Arena *arena, String endpoint, S3 *s, Err *err) {
    int n = (int)endpoint.len;
    size_t separator;
    if (!str_find(endpoint, S("://"), &separator)) return err_set(err, ERR_INVALID_ARGUMENT, "endpoint %.*s: want scheme://host[:port]", n, endpoint.data);
    s->scheme = str_copy(arena, str_slice(endpoint, 0, separator));
    String host, path;
    str_cut(str_slice(endpoint, separator + 3, endpoint.len), '/', &host, &path);
    bool https = str_equal(s->scheme, S("https"));
    if ((https && str_ends_with(host, S(":443"))) || (!https && str_ends_with(host, S(":80")))) host = str_slice(host, 0, host.len - (https ? 4 : 3));
    s->host = str_copy(arena, host);
    if (host.len == 0 || (!https && !str_equal(s->scheme, S("http")))) return err_set(err, ERR_INVALID_ARGUMENT, "endpoint %.*s: want http://host or https://host", n, endpoint.data);
    return ERR_OK;
}

Error s3_open(Arena *arena, const StoreConfig *config, int64_t part_size, String tmp_dir, Store **store, Err *err) {
    *store = nullptr;
    S3 *s = arena_push(arena, sizeof *s);
    s->base.ops = &s3_ops;
    s->bucket = str_copy(arena, config->bucket);
    s->prefix = str_copy(arena, config->prefix);
    s->region = str_copy(arena, config->region);
    s->storage_class = str_copy(arena, config->storage_class);
    s->path_style = config->path_style;
    s->part_size = part_size;
    s->tmp_dir = str_copy(arena, tmp_dir);
    String endpoint = config->endpoint.len > 0 ? config->endpoint : str_format(arena, "https://s3.%.*s.amazonaws.com", (int)config->region.len, config->region.data);
    Error e = parse_endpoint(arena, endpoint, s, err);
    if (e == ERR_OK) e = resolve_credentials(arena, config, s, err);
    if (e != ERR_OK) return e;
    if (!s->path_style) s->host = str_format(arena, "%.*s.%.*s", (int)s->bucket.len, s->bucket.data, (int)s->host.len, s->host.data);
    *store = &s->base;
    return ERR_OK;
}
