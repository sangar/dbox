#ifdef __linux__

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "watch.h"

#define WATCH_MASK \
    (IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW)

/* inotify needs one watch per directory; dirs maps each watch descriptor back to its path. */
struct WatchBackend {
    Watcher *watcher;
    int fd;
    char **dirs;
    size_t dir_cap;
};

Error backend_open(Watcher *w, const char *root, WatchBackend **out, Err *err) {
    *out = NULL;
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return err_sys(err, "inotify");
    WatchBackend *b = xcalloc(1, sizeof *b);
    b->watcher = w;
    b->fd = fd;
    *out = b;
    return ERR_OK;
}

void backend_close(WatchBackend *b) {
    close(b->fd);
    for (size_t i = 0; i < b->dir_cap; i++) xfree(b->dirs[i]);
    xfree(b->dirs);
    xfree(b);
}

Error backend_add_dir(WatchBackend *b, const char *path, Err *err) {
    int wd = inotify_add_watch(b->fd, path, WATCH_MASK);
    if (wd < 0) return errno == ENOENT ? ERR_OK : ERR_PLATFORM;
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
                watcher_event(b->watcher, b->dirs[e->wd], true);
                continue;
            }
            if (!e->len || !e->name[0]) continue;
            char *path = path_join(b->dirs[e->wd], e->name);
            watcher_event(b->watcher, path, false);
            xfree(path);
        }
    }
}

void backend_run(WatchBackend *b, Ctx *ctx) {
    while (!ctx_done(ctx)) {
        struct pollfd pfd = {.fd = b->fd, .events = POLLIN};
        int rc = poll(&pfd, 1, 200);
        if (rc > 0) read_events(b);
    }
}

#endif
