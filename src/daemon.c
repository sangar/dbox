#include "daemon.h"

#include "http.h"
#include "mc/encoding/json.h"

enum { REQUEST_LIMIT = 8192, CLIENT_TIMEOUT_MS = 5000, REQUEST_BLOCK = 64 * 1024 };

Error daemon_write_pid(Arena *scratch, String path, Err *err) {
    int pid;
    if (daemon_running(scratch, path, &pid)) return err_set(err, ERR_PLATFORM, "dbox is already running as pid %d", pid);
    return file_write_all(path, str_format(scratch, "%d\n", process_id()), 0644, err);
}

void daemon_remove_pid(String path) { (void)file_remove(path, nullptr); }

bool daemon_running(Arena *scratch, String path, int *pid) {
    String raw;
    int64_t n;
    if (file_read_all(scratch, path, &raw, nullptr) != ERR_OK || !str_parse_i64(str_trim(raw), &n) || n <= 0 || n > INT32_MAX) return false;
    if (!process_alive((int)n)) return false;
    *pid = (int)n;
    return true;
}

Error daemon_reload(Arena *scratch, String path, bool *reloaded, Err *err) {
    int pid;
    *reloaded = daemon_running(scratch, path, &pid);
    if (!*reloaded) return ERR_OK;
    return process_signal(pid, SIGNAL_RELOAD, err);
}

/* ---- the listener ---- */

static void respond(Arena *arena, int client, int status, const char *reason, const char *content_type, String body) {
    String head = str_format(arena, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, reason, content_type, body.len);
    if (file_write(client, head, nullptr) == ERR_OK) (void)file_write(client, body, nullptr);
}

/* read_request reads until the end of the request head, and returns its first line. */
static String read_request(Arena *arena, int client) {
    char *buffer = arena_push(arena, REQUEST_LIMIT);
    size_t n = 0;
    while (n < REQUEST_LIMIT) {
        size_t got;
        if (file_read(client, buffer + n, REQUEST_LIMIT - n, &got, nullptr) != ERR_OK || got == 0) break;
        n += got;
        String so_far = {buffer, n};
        if (str_contains(so_far, S("\r\n\r\n")) || str_contains(so_far, S("\n\n"))) break;
    }
    return str_first_line((String){buffer, n});
}

static void handle(Arena *arena, int client, MetricsFn metrics, BacklogFn backlog, void *context) {
    (void)net_set_timeouts(client, CLIENT_TIMEOUT_MS, nullptr);
    StringList words = str_fields(arena, read_request(arena, client));
    String method = words.count > 0 ? words.items[0] : S("");
    String path = S(""), query;
    if (words.count > 1) str_cut(words.items[1], '?', &path, &query);
    if (!str_equal(method, S("GET"))) {
        respond(arena, client, 405, "Method Not Allowed", "text/plain; charset=utf-8", S("method not allowed\n"));
    } else if (str_equal(path, S("/healthz"))) {
        respond(arena, client, 200, "OK", "text/plain; charset=utf-8", S("ok\n"));
    } else if (str_equal(path, S("/status"))) {
        respond(arena, client, 200, "OK", "application/json", str_format(arena, "{\"backlog\":%lld}\n", (long long)backlog(context)));
    } else if (str_equal(path, S("/metrics"))) {
        StringBuilder body = str_builder_create(arena, 4096);
        Err err;
        if (metrics(context, arena, &body, &err) == ERR_OK) respond(arena, client, 200, "OK", "text/plain; version=0.0.4", str_builder_finish(&body));
        else respond(arena, client, 500, "Internal Server Error", "text/plain; charset=utf-8", str_format(arena, "%s\n", err.msg));
    } else {
        respond(arena, client, 404, "Not Found", "text/plain; charset=utf-8", S("404 page not found\n"));
    }
    net_close(client);
}

Error daemon_serve(Cancel *cancel, String address, MetricsFn metrics, BacklogFn backlog, void *context, Err *err) {
    if (address.len == 0) return ERR_OK;
    int fd;
    Error e = net_listen(address, &fd, err);
    if (e != ERR_OK) return e;
    Arena *arena = arena_create(REQUEST_BLOCK);
    while (!cancel_requested(cancel)) {
        int client;
        if (!net_accept(fd, 200, &client)) continue;
        handle(arena, client, metrics, backlog, context);
        arena_reset(arena);
    }
    arena_destroy(arena);
    net_close(fd);
    return ERR_OK;
}

bool daemon_ask(Arena *scratch, String address, int64_t *backlog) {
    if (address.len == 0) return false;
    HttpRequest request = {
        .method = S("GET"),
        .url = str_format(scratch, "http://%.*s/status", (int)address.len, address.data),
        .body_fd = -1,
        .sink_fd = -1,
        .timeout_ms = 2000,
    };
    HttpResponse response;
    Node *status;
    if (http_perform(nullptr, scratch, &request, &response, nullptr) != ERR_OK || response.status != 200) return false;
    if (json_parse(scratch, response.body, &status, nullptr) != ERR_OK) return false;
    const Node *count = node_get(status, S("backlog"));
    if (!count || count->kind != NODE_INT) return false;
    *backlog = count->integer;
    return true;
}
