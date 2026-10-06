#include "daemon.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

Error daemon_write_pid(const char *path, Err *err) {
    pid_t pid;
    if (daemon_running(path, &pid)) return err_set(err, ERR_PLATFORM, "dbox is already running as pid %d", (int)pid);
    char line[32];
    snprintf(line, sizeof line, "%d\n", (int)getpid());
    return write_file(path, line, strlen(line), 0644, err);
}

void daemon_remove_pid(const char *path) { unlink(path); }

bool daemon_running(const char *path, pid_t *pid) {
    StrBuf raw = {0};
    Err ignored;
    if (read_file(path, &raw, &ignored) != ERR_OK) return false;
    char *end;
    long long n = strtoll(raw.data, &end, 10);
    bool ok = end != raw.data && n > 0;
    sb_free(&raw);
    if (!ok) return false;
    if (kill((pid_t)n, 0) != 0 && errno != EPERM) return false;
    *pid = (pid_t)n;
    return true;
}

Error daemon_reload(const char *path, bool *reloaded, Err *err) {
    pid_t pid;
    *reloaded = daemon_running(path, &pid);
    if (!*reloaded) return ERR_OK;
    if (kill(pid, SIGHUP) != 0) return err_set(err, ERR_PLATFORM, "signal pid %d: %s", (int)pid, strerror(errno));
    return ERR_OK;
}

/* ---- the listener ---- */

[[nodiscard]] static Error listen_on(const char *addr, int *out, Err *err) {
    *out = -1;
    const char *colon = strrchr(addr, ':');
    if (!colon) return err_set(err, ERR_INVALID_ARGUMENT, "listen %s: want host:port", addr);
    char *host = xstrndup(addr, (size_t)(colon - addr));
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE | AI_NUMERICSERV};
    struct addrinfo *res;
    int rc = getaddrinfo(*host ? host : NULL, colon + 1, &hints, &res);
    xfree(host);
    if (rc != 0) return err_set(err, ERR_PLATFORM, "listen %s: %s", addr, gai_strerror(rc));
    int fd = -1;
    Error e = ERR_OK;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || listen(fd, 16) != 0) {
            e = err_set(err, ERR_PLATFORM, "listen %s: %s", addr, strerror(errno));
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) return e != ERR_OK ? e : err_set(err, ERR_PLATFORM, "listen %s: no usable address", addr);
    *out = fd;
    return ERR_OK;
}

static void respond(int client, int status, const char *reason, const char *content_type, const char *body, size_t len) {
    StrBuf head = {0};
    sb_printf(&head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, reason, content_type, len);
    write_all(client, head.data, head.len);
    write_all(client, body, len);
    sb_free(&head);
}

static void handle(int client, MetricsFn metrics, BacklogFn backlog, void *arg) {
    struct timeval timeout = {.tv_sec = 5};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    char req[8192];
    size_t n = 0;
    while (n < sizeof req - 1) {
        ssize_t r = read(client, req + n, sizeof req - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
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
    close(client);
}

Error daemon_serve(Ctx *ctx, const char *addr, MetricsFn metrics, BacklogFn backlog, void *arg, Err *err) {
    if (!*addr) return ERR_OK;
    int fd;
    Error e = listen_on(addr, &fd, err);
    if (e != ERR_OK) return e;
    while (!ctx_done(ctx)) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 200) <= 0) continue;
        int client = accept(fd, NULL, NULL);
        if (client >= 0) handle(client, metrics, backlog, arg);
    }
    close(fd);
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

Error daemon_raise_file_limit(Err *err) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return err_set(err, ERR_PLATFORM, "getrlimit: %s", strerror(errno));
    if (limit.rlim_cur >= limit.rlim_max) return ERR_OK;
    limit.rlim_cur = limit.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &limit) == 0) return ERR_OK;
    /* macOS rejects RLIM_INFINITY for the soft limit; OPEN_MAX-sized values work. */
    limit.rlim_cur = 1 << 20;
    if (setrlimit(RLIMIT_NOFILE, &limit) == 0) return ERR_OK;
    return err_set(err, ERR_PLATFORM, "setrlimit: %s", strerror(errno));
}
