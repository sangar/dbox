#include "daemon.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

Error daemon_write_pid(const char *path, Err *err) {
    int pid;
    if (daemon_running(path, &pid)) return err_set(err, ERR_PLATFORM, "dbox is already running as pid %d", pid);
    char line[32];
    snprintf(line, sizeof line, "%d\n", process_id());
    return write_file(path, line, strlen(line), 0644, err);
}

void daemon_remove_pid(const char *path) { (void)file_remove(path, NULL); }

bool daemon_running(const char *path, int *pid) {
    StrBuf raw = {0};
    Err ignored;
    if (read_file(path, &raw, &ignored) != ERR_OK) return false;
    char *end;
    long long n = strtoll(raw.data, &end, 10);
    bool ok = end != raw.data && n > 0;
    sb_free(&raw);
    if (!ok) return false;
    if (!process_alive((int)n)) return false;
    *pid = (int)n;
    return true;
}

Error daemon_reload(const char *path, bool *reloaded, Err *err) {
    int pid;
    *reloaded = daemon_running(path, &pid);
    if (!*reloaded) return ERR_OK;
    return process_signal_reload(pid, err);
}

/* ---- the listener ---- */

static void respond(int client, int status, const char *reason, const char *content_type, const char *body, size_t len) {
    StrBuf head = {0};
    sb_printf(&head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, reason, content_type, len);
    file_write_all(client, head.data, head.len);
    file_write_all(client, body, len);
    sb_free(&head);
}

static void handle(int client, MetricsFn metrics, BacklogFn backlog, void *arg) {
    net_set_timeouts(client, 5);
    char req[8192];
    size_t n = 0;
    while (n < sizeof req - 1) {
        size_t r;
        if (file_read(client, req + n, sizeof req - 1 - n, &r, NULL) != ERR_OK || r == 0) break;
        n += r;
        req[n] = '\0';
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[n] = '\0';
    char method[8] = "", path[256] = "";
    sscanf(req, "%7s %255s", method, path);
    char *query = strchr(path, '?');
    if (query) *query = '\0';
    if (strcmp(method, "GET") != 0) {
        respond(client, 405, "Method Not Allowed", "text/plain; charset=utf-8", "method not allowed\n", 19);
    } else if (strcmp(path, "/healthz") == 0) {
        respond(client, 200, "OK", "text/plain; charset=utf-8", "ok\n", 3);
    } else if (strcmp(path, "/status") == 0) {
        char body[64];
        int len = snprintf(body, sizeof body, "{\"backlog\":%ld}\n", backlog(arg));
        respond(client, 200, "OK", "application/json", body, (size_t)len);
    } else if (strcmp(path, "/metrics") == 0) {
        StrBuf body = {0};
        Err err;
        if (metrics(arg, &body, &err) == ERR_OK) {
            respond(client, 200, "OK", "text/plain; version=0.0.4", sb_cstr(&body), body.len);
        } else {
            sb_clear(&body);
            sb_printf(&body, "%s\n", err.msg);
            respond(client, 500, "Internal Server Error", "text/plain; charset=utf-8", body.data, body.len);
        }
        sb_free(&body);
    } else {
        respond(client, 404, "Not Found", "text/plain; charset=utf-8", "404 page not found\n", 19);
    }
    file_close(client);
}

Error daemon_serve(Ctx *ctx, const char *addr, MetricsFn metrics, BacklogFn backlog, void *arg, Err *err) {
    if (!*addr) return ERR_OK;
    int fd;
    Error e = net_listen(addr, &fd, err);
    if (e != ERR_OK) return e;
    while (!ctx_done(ctx)) {
        int client;
        net_accept(fd, 200, &client);
        if (client >= 0) handle(client, metrics, backlog, arg);
    }
    file_close(fd);
    return ERR_OK;
}

static size_t collect(char *data, size_t size, size_t nmemb, void *userdata) {
    sb_append(userdata, data, size * nmemb);
    return size * nmemb;
}

bool daemon_ask(const char *addr, long *backlog) {
    if (!*addr) return false;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    StrBuf url = {0}, body = {0};
    sb_printf(&url, "http://%s/status", addr);
    curl_easy_setopt(curl, CURLOPT_URL, url.data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    long status = 0;
    bool ok = curl_easy_perform(curl) == CURLE_OK && curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200;
    if (ok) {
        const char *key = strstr(sb_cstr(&body), "\"backlog\"");
        const char *colon = key ? strchr(key, ':') : NULL;
        ok = colon != NULL;
        if (ok) *backlog = strtol(colon + 1, NULL, 10);
    }
    curl_easy_cleanup(curl);
    sb_free(&url);
    sb_free(&body);
    return ok;
}
