#include "watch.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "strmap.h"

struct Watcher {
    char *root;
    char *real_root; /* events may use the resolved path of a root behind symlinks */
    IgnoreFn ignored;
    ChangedFn changed;
    void *ctx;
    Logger *log;
    WatchBackend *backend;
    pthread_mutex_t mu;
    StrMap dirs; /* relative paths of the directories known to be watched */
};

static char *relative(Watcher *w, const char *p) {
    char *rel = rel_path(w->root, p);
    if (!rel && w->real_root) rel = rel_path(w->real_root, p);
    return rel;
}

static bool watch_limit_hint(const char *p, Err *err) {
    if (errno == ENOSPC) err_set(err, "watch %s: inotify watch limit reached; raise it with `sudo sysctl fs.inotify.max_user_watches=1048576`", p);
    else if (errno == EMFILE) err_set(err, "watch %s: out of file descriptors; raise `ulimit -n`", p);
    else err_sys(err, "watch %s", p);
    return false;
}

static int compare_names(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* add_tree watches dir and everything below it; with report, every file found is reported as changed. */
static bool add_tree(Watcher *w, const char *dir, bool report, Err *err) {
    char *rel = relative(w, dir);
    if (rel && w->ignored(w->ctx, rel, true)) {
        free(rel);
        return true;
    }
    pthread_mutex_lock(&w->mu);
    strmap_put(&w->dirs, rel ? rel : ".", NULL);
    pthread_mutex_unlock(&w->mu);
    free(rel);
    if (!backend_add_dir(w->backend, dir, err)) return watch_limit_hint(dir, err);
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? true : watch_limit_hint(dir, err);
    StrList names = {0};
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) strlist_push(&names, e->d_name);
    closedir(d);
    qsort(names.items, names.len, sizeof *names.items, compare_names);
    bool ok = true;
    for (size_t i = 0; ok && i < names.len; i++) {
        char *p = path_join(dir, names.items[i]);
        struct stat st;
        if (lstat(p, &st) == 0) {
            char *child = relative(w, p);
            if (S_ISDIR(st.st_mode)) {
                ok = add_tree(w, p, report, err);
            } else if (child && report && !w->ignored(w->ctx, child, false)) {
                w->changed(w->ctx, child);
            }
            free(child);
        }
        free(p);
    }
    strlist_free(&names);
    return ok;
}

static void forget_dir(Watcher *w, const char *rel) {
    pthread_mutex_lock(&w->mu);
    strmap_remove(&w->dirs, rel, NULL);
    char *prefix = path_join(rel, "");
    StrList below = {0};
    StrMapIter it = {0};
    const char *key;
    while (strmap_next(&w->dirs, &it, &key, NULL))
        if (has_prefix(key, prefix)) strlist_push(&below, key);
    for (size_t i = 0; i < below.len; i++) strmap_remove(&w->dirs, below.items[i], NULL);
    strlist_free(&below);
    free(prefix);
    pthread_mutex_unlock(&w->mu);
}

void watcher_event(Watcher *w, const char *abs_path, bool rescan_subdirs) {
    char *rel = relative(w, abs_path);
    if (!rel) {
        if (rescan_subdirs) {
            Err err;
            if (!add_tree(w, w->root, true, &err)) log_error(w->log, "rescan", LERR(&err));
        }
        return;
    }
    struct stat st;
    bool exists = lstat(abs_path, &st) == 0;
    bool is_directory = exists && S_ISDIR(st.st_mode);
    if (w->ignored(w->ctx, rel, is_directory)) {
        free(rel);
        return;
    }
    if (is_directory) {
        pthread_mutex_lock(&w->mu);
        bool known = strmap_has(&w->dirs, rel);
        pthread_mutex_unlock(&w->mu);
        /* Files can land in a new directory before its watch is added, so everything already inside is reported too. */
        if (!known || rescan_subdirs) {
            Err err;
            if (!add_tree(w, abs_path, true, &err)) log_error(w->log, "watch new directory", LS("path", rel), LERR(&err));
        }
        free(rel);
        return;
    }
    if (!exists) forget_dir(w, rel);
    w->changed(w->ctx, rel);
    free(rel);
}

Watcher *watcher_new(const char *root, IgnoreFn ignored, ChangedFn changed, void *ctx, Logger *log, Err *err) {
    Watcher *w = xcalloc(1, sizeof *w);
    w->root = xstrdup(root);
    char resolved[PATH_MAX];
    if (realpath(root, resolved) && strcmp(resolved, root) != 0) w->real_root = xstrdup(resolved);
    w->ignored = ignored;
    w->changed = changed;
    w->ctx = ctx;
    w->log = log;
    pthread_mutex_init(&w->mu, NULL);
    strmap_init(&w->dirs);
    w->backend = backend_open(w, w->real_root ? w->real_root : w->root, err);
    if (!w->backend || !add_tree(w, w->root, false, err)) {
        watcher_free(w);
        return NULL;
    }
    return w;
}

void watcher_run(Watcher *w, Ctx *ctx) { backend_run(w->backend, ctx); }

void watcher_free(Watcher *w) {
    if (!w) return;
    if (w->backend) backend_close(w->backend);
    strmap_free(&w->dirs);
    pthread_mutex_destroy(&w->mu);
    free(w->root);
    free(w->real_root);
    free(w);
}
