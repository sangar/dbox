#include "watch.h"

#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "strmap.h"

struct Watcher {
    char *root;
    char *real_root; /* events may use the resolved path of a root behind symlinks */
    IgnoreFn ignored;
    ChangedFn changed;
    void *ctx;
    Logger *log;
    WatchBackend *backend;
    Mutex mu;
    StrMap dirs; /* relative paths of the directories known to be watched */
};

static char *relative(Watcher *w, const char *p) {
    char *rel = rel_path(w->root, p);
    if (!rel && w->real_root) rel = rel_path(w->real_root, p);
    return rel;
}

[[nodiscard]] /* add_tree watches dir and everything below it; with report, every file found is reported as changed. */
[[nodiscard]] static Error add_tree(Watcher *w, const char *dir, bool report, Err *err) {
    char *rel = relative(w, dir);
    if (rel && w->ignored(w->ctx, rel, true)) {
        xfree(rel);
        return ERR_OK;
    }
    mutex_lock(&w->mu);
    strmap_put(&w->dirs, rel ? rel : ".", NULL);
    mutex_unlock(&w->mu);
    xfree(rel);
    Error e = watch_backend_add_dir(w->backend, dir, err);
    if (e != ERR_OK) return e;
    StrList names = {0};
    e = dir_list(dir, &names, err);
    if (e != ERR_OK) {
        strlist_free(&names);
        return e == ERR_NOT_FOUND ? ERR_OK : e;
    }
    for (size_t i = 0; e == ERR_OK && i < names.len; i++) {
        char *p = path_join(dir, names.items[i]);
        FileStat st;
        if (file_info(p, &st, NULL) == ERR_OK && st.exists) {
            char *child = relative(w, p);
            if (st.is_dir) {
                e = add_tree(w, p, report, err);
            } else if (child && report && !w->ignored(w->ctx, child, false)) {
                w->changed(w->ctx, child);
            }
            xfree(child);
        }
        xfree(p);
    }
    strlist_free(&names);
    return e;
}

static void forget_dir(Watcher *w, const char *rel) {
    mutex_lock(&w->mu);
    strmap_remove(&w->dirs, rel, NULL);
    char *prefix = path_join(rel, "");
    StrList below = {0};
    StrMapIter it = {0};
    const char *key;
    while (strmap_next(&w->dirs, &it, &key, NULL))
        if (has_prefix(key, prefix)) strlist_push(&below, key);
    for (size_t i = 0; i < below.len; i++) strmap_remove(&w->dirs, below.items[i], NULL);
    strlist_free(&below);
    xfree(prefix);
    mutex_unlock(&w->mu);
}

/* watcher_event handles every absolute path the backend hears about. */
static void watcher_event(Watcher *w, const char *abs_path, bool rescan_subdirs) {
    char *rel = relative(w, abs_path);
    if (!rel) {
        if (rescan_subdirs) {
            Err err;
            if (add_tree(w, w->root, true, &err) != ERR_OK) log_error(w->log, "rescan", log_err(&err), log_end());
        }
        return;
    }
    FileStat st;
    bool exists = file_info(abs_path, &st, NULL) == ERR_OK && st.exists;
    bool is_directory = exists && st.is_dir;
    if (w->ignored(w->ctx, rel, is_directory)) {
        xfree(rel);
        return;
    }
    if (is_directory) {
        mutex_lock(&w->mu);
        bool known = strmap_has(&w->dirs, rel);
        mutex_unlock(&w->mu);
        /* Files can land in a new directory before its watch is added, so everything already inside is reported too. */
        if (!known || rescan_subdirs) {
            Err err;
            if (add_tree(w, abs_path, true, &err) != ERR_OK) log_error(w->log, "watch new directory", log_str("path", rel), log_err(&err), log_end());
        }
        xfree(rel);
        return;
    }
    if (!exists) forget_dir(w, rel);
    w->changed(w->ctx, rel);
    xfree(rel);
}

static void on_backend_event(void *user, const char *abs_path, bool rescan_subdirs) { watcher_event(user, abs_path, rescan_subdirs); }

Error watcher_new(const char *root, IgnoreFn ignored, ChangedFn changed, void *ctx, Logger *log, Watcher **out, Err *err) {
    *out = NULL;
    Watcher *w = xcalloc(1, sizeof *w);
    w->root = xstrdup(root);
    char *resolved = path_resolve(root);
    if (resolved && strcmp(resolved, root) != 0) w->real_root = resolved;
    else xfree(resolved);
    w->ignored = ignored;
    w->changed = changed;
    w->ctx = ctx;
    w->log = log;
    mutex_init(&w->mu);
    strmap_init(&w->dirs);
    Error e = watch_backend_open(w->real_root ? w->real_root : w->root, on_backend_event, w, &w->backend, err);
    if (e == ERR_OK) e = add_tree(w, w->root, false, err);
    if (e != ERR_OK) {
        watcher_free(w);
        return e;
    }
    *out = w;
    return ERR_OK;
}

void watcher_run(Watcher *w, Ctx *ctx) {
    while (!ctx_done(ctx)) watch_backend_poll(w->backend, 200);
}

void watcher_free(Watcher *w) {
    if (!w) return;
    if (w->backend) watch_backend_close(w->backend);
    strmap_free(&w->dirs);
    mutex_destroy(&w->mu);
    xfree(w->root);
    xfree(w->real_root);
    xfree(w);
}
