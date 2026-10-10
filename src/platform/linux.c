#ifdef __linux__

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "alloc.h"
#include "platform.h"

Os os_current(void) { return OS_LINUX; }

char *process_executable_path(void) {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
    char *resolved = path_resolve(buf);
    return resolved ? resolved : xstrdup(buf);
}

#define WATCH_MASK \
    (IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW)

/* inotify needs one watch per directory; dirs maps each watch descriptor back to its path. */
struct WatchBackend {
    WatchEventFn fn;
    void *user;
    int fd;
    char **dirs;
    size_t dir_cap;
};

Error watch_backend_open(const char *root, WatchEventFn fn, void *user, WatchBackend **out, Err *err) {
    *out = NULL;
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return err_sys(err, "inotify");
    WatchBackend *b = xcalloc(1, sizeof *b);
    b->fn = fn;
    b->user = user;
    b->fd = fd;
    *out = b;
    return ERR_OK;
}

void watch_backend_close(WatchBackend *b) {
    close(b->fd);
    for (size_t i = 0; i < b->dir_cap; i++) xfree(b->dirs[i]);
    xfree(b->dirs);
    xfree(b);
}

Error watch_backend_add_dir(WatchBackend *b, const char *path, Err *err) {
    int wd = inotify_add_watch(b->fd, path, WATCH_MASK);
    if (wd < 0) {
        if (errno == ENOENT) return ERR_OK;
        if (errno == ENOSPC) return err_set(err, ERR_PLATFORM, "watch %s: inotify watch limit reached; raise it with `sudo sysctl fs.inotify.max_user_watches=1048576`", path);
        if (errno == EMFILE) return err_set(err, ERR_PLATFORM, "watch %s: out of file descriptors; raise `ulimit -n`", path);
        return err_sys(err, "watch %s", path);
    }
    if ((size_t)wd >= b->dir_cap) {
        size_t cap = b->dir_cap ? b->dir_cap : 1024;
        while (cap <= (size_t)wd) cap *= 2;
        b->dirs = xrealloc(b->dirs, cap * sizeof *b->dirs);
        memset(b->dirs + b->dir_cap, 0, (cap - b->dir_cap) * sizeof *b->dirs);
        b->dir_cap = cap;
    }
    xfree(b->dirs[wd]);
    b->dirs[wd] = xstrdup(path);
    return ERR_OK;
}

static void read_events(WatchBackend *b) {
    char buf[64 * 1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(b->fd, buf, sizeof buf);
        if (n <= 0) return;
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *e = (struct inotify_event *)p;
            p += sizeof *e + e->len;
            if (e->wd < 0 || (size_t)e->wd >= b->dir_cap || !b->dirs[e->wd]) continue;
            if (e->mask & IN_IGNORED) {
                xfree(b->dirs[e->wd]);
                b->dirs[e->wd] = NULL;
                continue;
            }
            if (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) continue;
            if (e->mask & IN_Q_OVERFLOW) {
                b->fn(b->user, b->dirs[e->wd], true);
                continue;
            }
            if (!e->len || !e->name[0]) continue;
            size_t len = strlen(b->dirs[e->wd]) + 1 + strlen(e->name) + 1;
            char *path = xmalloc(len);
            snprintf(path, len, "%s/%s", b->dirs[e->wd], e->name);
            b->fn(b->user, path, false);
            xfree(path);
        }
    }
}

void watch_backend_poll(WatchBackend *b, int timeout_ms) {
    struct pollfd pfd = {.fd = b->fd, .events = POLLIN};
    if (poll(&pfd, 1, timeout_ms) > 0) read_events(b);
}

#endif
