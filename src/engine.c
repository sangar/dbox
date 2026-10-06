#include "engine.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "engine_internal.h"
#include "watch.h"

static void on_debounced(void *ctx, const char *key) {
    Engine *e = ctx;
    queue_push(&e->queue, e->ctx, key);
}

Engine *engine_new(const Config *cfg, Index *idx, const StoreSet *stores, EngineOptions opts) {
    Engine *e = xcalloc(1, sizeof *e);
    arena_init(&e->arena, 4096);
    e->cfg = cfg;
    e->root = cfg->sync.root;
    e->idx = idx;
    if (opts.log) {
        e->log = opts.log;
    } else {
        logger_init(&e->own_log, "info", "text", stderr);
        e->log = &e->own_log;
    }
    e->dry_run = opts.dry_run;
    short_hostname(e->host);
    ignore_init(&e->ignore, cfg->sync.ignore, cfg->sync.ignore_count);
    e->primary_name = config_primary(cfg);
    e->primary = e->primary_name ? storeset_get(stores, e->primary_name) : NULL;
    e->mirror_names = config_mirrors(cfg, &e->arena, &e->mirror_count);
    e->mirrors = arena_calloc(&e->arena, e->mirror_count + 1, sizeof *e->mirrors);
    e->mirror_woken = arena_calloc(&e->arena, e->mirror_count + 1, sizeof *e->mirror_woken);
    for (size_t i = 0; i < e->mirror_count; i++) e->mirrors[i] = storeset_get(stores, e->mirror_names[i]);
    queue_init(&e->queue);
    locks_init(&e->locks);
    e->debouncer = debounce_new(cfg->sync.debounce_ns, on_debounced, e);
    atomic_init(&e->stopping, false);
    return e;
}

void engine_free(Engine *e) {
    if (!e) return;
    debounce_stop(e->debouncer);
    queue_free(&e->queue);
    locks_free(&e->locks);
    ignore_free(&e->ignore);
    if (e->log == &e->own_log) logger_destroy(&e->own_log);
    arena_free(&e->arena);
    xfree(e);
}

char *engine_abs(const Engine *e, const char *rel) { return path_join(e->root, rel); }

int64_t retry_delay_ns(int attempts) { return (int64_t)(1 << attempts) * 10 * NS_PER_SEC; }

void engine_wake_mirrors(Engine *e) {
    ctx_lock(e->ctx);
    for (size_t i = 0; i < e->mirror_count; i++) e->mirror_woken[i] = true;
    ctx_notify_locked(e->ctx);
    ctx_unlock(e->ctx);
}

/* ---- worker threads ---- */

typedef struct {
    Engine *e;
    void **items;
    size_t count;
    atomic_size_t next;
    void (*fn)(Engine *, void *);
} Work;

static void *work_main(void *arg) {
    Work *w = arg;
    for (;;) {
        size_t i = atomic_fetch_add(&w->next, 1);
        if (i >= w->count) return NULL;
        w->fn(w->e, w->items[i]);
    }
}

void run_workers(Engine *e, int workers, void **items, size_t count, void (*fn)(Engine *, void *)) {
    Work w = {.e = e, .items = items, .count = count, .fn = fn};
    atomic_init(&w.next, 0);
    size_t n = min_size((size_t)(workers > 1 ? workers : 1), count);
    pthread_t *threads = xcalloc(n + 1, sizeof *threads);
    for (size_t i = 0; i < n; i++) pthread_create(&threads[i], NULL, work_main, &w);
    for (size_t i = 0; i < n; i++) pthread_join(threads[i], NULL);
    xfree(threads);
}

static int primary_workers(const Engine *e) { return config_store(e->cfg, e->primary_name)->workers; }

/* ---- local files ---- */

bool engine_local(Engine *e, const char *rel, const IndexFile *f, LocalFile *out, Err *err) {
    char *abs = engine_abs(e, rel);
    struct stat st;
    memset(out, 0, sizeof *out);
    bool ok = true;
    if (lstat(abs, &st) != 0) {
        if (errno != ENOENT && errno != ENOTDIR) {
            err_sys(err, "%s", abs);
            ok = false;
        }
        xfree(abs);
        return ok;
    }
    out->exists = true;
    out->changed = true;
    out->size = st.st_size;
    out->mtime_ns = stat_mtime_ns(&st);
    if (f && f->size == out->size && f->mtime_ns == out->mtime_ns) {
        out->changed = false;
    } else if (f) {
        /* Size and time decide first; content is hashed only when they differ. */
        char sum[SHA256_HEX_LEN];
        ok = sha256_file(abs, sum, err);
        if (ok) out->changed = strcmp(sum, f->sha256) != 0;
    }
    xfree(abs);
    return ok;
}

/* ---- pushing to the primary ---- */

static bool upload(Engine *e, const char *rel, const char *sum, const struct stat *info, Err *err);

/*
 * upload sends rel, whose content hashed to sum when it looked like info, to
 * the primary. If the file changed meanwhile, nothing is recorded and the
 * path is queued again, so the index never describes a version other than
 * the one the primary holds.
 */
static bool upload(Engine *e, const char *rel, const char *sum, const struct stat *info, Err *err) {
    if (e->dry_run) {
        log_info(e->log, "would upload", log_str("path", rel), log_str("store", e->primary_name), log_end());
        return true;
    }
    char *abs = engine_abs(e, rel);
    int fd = open(abs, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err_sys(err, "%s", abs);
        xfree(abs);
        return false;
    }
    Meta meta = {.mtime_ns = stat_mtime_ns(info)};
    snprintf(meta.sha256, sizeof meta.sha256, "%s", sum);
    char etag[ETAG_MAX];
    Err inner;
    bool ok = store_put(e->primary, e->ctx, rel, fd, info->st_size, &meta, etag, &inner) == STORE_OK;
    close(fd);
    if (!ok) {
        err_set(err, "upload to %s: %s", e->primary_name, inner.msg);
        xfree(abs);
        return false;
    }
    struct stat after;
    bool unchanged = lstat(abs, &after) == 0 && after.st_size == info->st_size && stat_mtime_ns(&after) == stat_mtime_ns(info);
    xfree(abs);
    if (!unchanged) {
        log_info(e->log, "changed during upload; syncing again", log_str("path", rel), log_end());
        queue_push(&e->queue, e->ctx, rel);
        return true;
    }
    IndexFile record = {.path = rel, .size = info->st_size, .mtime_ns = stat_mtime_ns(info), .sha256 = sum};
    if (!index_record_synced(e->idx, &record, e->primary_name, etag, e->mirror_names, e->mirror_count, err)) return false;
    log_info(e->log, "uploaded", log_str("path", rel), log_str("store", e->primary_name), log_int("bytes", info->st_size), log_end());
    engine_wake_mirrors(e);
    return true;
}

/* push_delete handles a path that is gone locally: a deleted file, or a directory that was removed or renamed with everything in it. */
static bool push_delete(Engine *e, const char *rel, Err *err) {
    Arena a;
    arena_init(&a, 4096);
    IndexFile f;
    bool found;
    bool ok = index_file(e->idx, &a, rel, &f, &found, err);
    if (!ok) goto out;
    if (!found) {
        IndexFile *inside;
        size_t n;
        ok = index_clear_upload_failure(e->idx, rel, err) && index_files_under(e->idx, &a, rel, &inside, &n, err);
        if (ok)
            for (size_t i = 0; i < n; i++) queue_push(&e->queue, e->ctx, inside[i].path);
        goto out;
    }
    if (f.deleted) goto out;
    if (e->dry_run) {
        log_info(e->log, "would delete", log_str("path", rel), log_str("store", e->primary_name), log_end());
        goto out;
    }
    if (e->cfg->sync.delete_remote) {
        Err inner;
        if (store_delete(e->primary, e->ctx, rel, &inner) != STORE_OK) {
            err_set(err, "delete from %s: %s", e->primary_name, inner.msg);
            ok = false;
            goto out;
        }
        log_info(e->log, "deleted", log_str("path", rel), log_str("store", e->primary_name), log_end());
    }
    ok = index_tombstone(e->idx, rel, e->primary_name, !e->cfg->sync.delete_remote, err);
    if (ok) engine_wake_mirrors(e);
out:
    arena_free(&a);
    return ok;
}

/* push makes the primary match the local file at rel. */
static bool push(Engine *e, const char *rel, Err *err) {
    char *abs = engine_abs(e, rel);
    struct stat st;
    if (lstat(abs, &st) != 0) {
        bool missing = errno == ENOENT || errno == ENOTDIR;
        if (!missing) err_sys(err, "%s", abs);
        xfree(abs);
        return missing ? push_delete(e, rel, err) : false;
    }
    if (!S_ISREG(st.st_mode)) {
        xfree(abs);
        return true;
    }
    Arena a;
    arena_init(&a, 4096);
    IndexFile f;
    bool found, ok = index_file(e->idx, &a, rel, &f, &found, err);
    bool primary_current = false;
    if (ok && found && !f.deleted) {
        Replica r;
        bool has_replica;
        ok = index_replica(e->idx, &a, rel, e->primary_name, &r, &has_replica, err);
        primary_current = ok && has_replica && r.state == REPLICA_VERIFIED;
    }
    if (ok && !(primary_current && f.size == st.st_size && f.mtime_ns == stat_mtime_ns(&st))) {
        char sum[SHA256_HEX_LEN];
        ok = sha256_file(abs, sum, err);
        if (ok && primary_current && strcmp(f.sha256, sum) == 0) ok = index_touch(e->idx, rel, stat_mtime_ns(&st), err);
        else if (ok) ok = upload(e, rel, sum, &st, err);
    }
    arena_free(&a);
    xfree(abs);
    return ok;
}

static void record_upload_failure(Engine *e, const char *rel, const Err *cause) {
    Arena a;
    arena_init(&a, 4096);
    int attempts = 1;
    UploadFailure prev;
    bool found;
    Err err;
    if (index_upload_failure(e->idx, &a, rel, &prev, &found, &err) && found) attempts = prev.attempts + 1;
    if (attempts >= MAX_ATTEMPTS) {
        log_error(e->log, "sync failed; giving up until `dbox retry` or the file changes", log_str("path", rel), log_int("attempts", attempts), log_err(cause), log_end());
    } else {
        log_error(e->log, "sync failed; will retry", log_str("path", rel), log_int("attempt", attempts), log_dur("in", retry_delay_ns(attempts)), log_err(cause), log_end());
    }
    if (!index_mark_upload_failed(e->idx, rel, cause->msg, attempts, wall_ns() + retry_delay_ns(attempts), &err))
        log_error(e->log, "record failure", log_str("path", rel), log_err(&err), log_end());
    arena_free(&a);
}

/* process syncs one path under its lock, and records a failure so that retry_due_uploads brings the path back after a backoff. */
static void process(Engine *e, const char *rel) {
    if (!locks_acquire(&e->locks, rel)) return;
    Err err;
    bool ok = push(e, rel, &err);
    if (locks_release(&e->locks, rel)) queue_push(&e->queue, e->ctx, rel);
    if (!ok && !ctx_done(e->ctx)) record_upload_failure(e, rel, &err);
}

static void process_item(Engine *e, void *item) { process(e, item); }

static void *primary_worker(void *arg) {
    Engine *e = arg;
    char *p;
    while ((p = queue_pop(&e->queue, e->ctx, &e->stopping))) {
        process(e, p);
        xfree(p);
    }
    return NULL;
}

/* retry_due_uploads queues every failed upload whose backoff has passed. */
static bool retry_due_uploads(Engine *e, Err *err) {
    Arena a;
    arena_init(&a, 4096);
    const char **due;
    size_t n;
    bool ok = index_due_uploads(e->idx, &a, wall_ns(), &due, &n, err);
    for (size_t i = 0; ok && i < n; i++) queue_push(&e->queue, e->ctx, due[i]);
    arena_free(&a);
    return ok;
}

/* walk collects the relative paths of every regular file under dir that is not ignored. */
static bool walk(Engine *e, const char *dir, StrList *paths, Err *err) {
    DIR *d = opendir(dir);
    if (!d) {
        err_sys(err, "%s", dir);
        return false;
    }
    StrList names = {0};
    struct dirent *entry;
    while ((entry = readdir(d)))
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) strlist_push(&names, entry->d_name);
    closedir(d);
    strlist_sort(&names);
    bool ok = true;
    for (size_t i = 0; ok && i < names.len; i++) {
        char *p = path_join(dir, names.items[i]);
        char *rel = rel_path(e->root, p);
        struct stat st;
        if (rel && lstat(p, &st) == 0 && !ignore_match(&e->ignore, rel, S_ISDIR(st.st_mode))) {
            if (S_ISDIR(st.st_mode)) ok = walk(e, p, paths, err);
            else if (S_ISREG(st.st_mode)) strlist_push(paths, rel);
        }
        xfree(rel);
        xfree(p);
    }
    strlist_free(&names);
    return ok;
}

/* push_all syncs every local file and every indexed file that is gone locally. */
static bool push_all(Engine *e, Err *err) {
    StrList paths = {0};
    if (!walk(e, e->root, &paths, err)) {
        strlist_free(&paths);
        return false;
    }
    Arena a;
    arena_init(&a, 64 * 1024);
    IndexFile *files;
    size_t n;
    bool ok = index_files(e->idx, &a, &files, &n, err);
    if (ok) {
        for (size_t i = 0; i < n; i++)
            if (!files[i].deleted) strlist_push(&paths, files[i].path);
        StrMap seen;
        strmap_init(&seen);
        void **unique = xcalloc(paths.len + 1, sizeof *unique);
        size_t m = 0;
        for (size_t i = 0; i < paths.len; i++) {
            if (strmap_has(&seen, paths.items[i])) continue;
            strmap_put(&seen, paths.items[i], NULL);
            unique[m++] = paths.items[i];
        }
        run_workers(e, primary_workers(e), unique, m, process_item);
        xfree(unique);
        strmap_free(&seen);
    }
    arena_free(&a);
    strlist_free(&paths);
    return ok;
}

/* ---- pulling from the primary ---- */

static bool download_to(Engine *e, const char *key, const char *rel, bool record, Err *err);

static bool download(Engine *e, const char *key, const char *rel, Err *err) {
    if (e->dry_run) {
        log_info(e->log, "would download", log_str("path", rel), log_str("store", e->primary_name), log_end());
        return true;
    }
    return download_to(e, key, rel, true, err);
}

/*
 * download_to fetches key into a temp file and renames it to rel. With
 * record, the index is updated before the rename so the watcher event that
 * follows finds nothing to upload.
 */
static bool download_to(Engine *e, const char *key, const char *rel, bool record, Err *err) {
    Object obj;
    int body;
    Err inner;
    if (store_get(e->primary, e->ctx, key, &obj, &body, &inner) != STORE_OK) {
        err_set(err, "%s", inner.msg);
        return false;
    }
    char *state = config_state_dir(e->cfg);
    char *tmp_dir = path_join(state, "tmp");
    char *tmp = path_join(tmp_dir, "download-XXXXXX");
    char *abs = engine_abs(e, rel);
    char *parent = path_dir(abs);
    bool ok = false;
    int out = -1;
    if (!mkdir_p(tmp_dir, 0755, err)) goto done;
    out = mkstemp(tmp);
    if (out < 0) {
        err_sys(err, "%s", tmp);
        goto done;
    }
    Sha256 h;
    sha256_init(&h);
    if (copy_fd(body, out, &h, err) < 0) goto done;
    close(out);
    out = -1;
    if (obj.mtime_ns != 0 && !set_mtime_ns(tmp, obj.mtime_ns)) {
        err_sys(err, "set mtime %s", tmp);
        goto done;
    }
    struct stat st;
    if (stat(tmp, &st) != 0) {
        err_sys(err, "%s", tmp);
        goto done;
    }
    if (record) {
        char sum[SHA256_HEX_LEN];
        sha256_hex(&h, sum);
        IndexFile f = {.path = rel, .size = st.st_size, .mtime_ns = stat_mtime_ns(&st), .sha256 = sum};
        if (!index_record_synced(e->idx, &f, e->primary_name, obj.etag, e->mirror_names, e->mirror_count, err)) goto done;
    }
    if (!mkdir_p(parent, 0755, err)) goto done;
    if (rename(tmp, abs) != 0) {
        err_sys(err, "rename %s", abs);
        goto done;
    }
    log_info(e->log, "downloaded", log_str("path", rel), log_str("store", e->primary_name), log_int("bytes", st.st_size), log_end());
    ok = true;
done:
    if (out >= 0) close(out);
    if (!ok) unlink(tmp);
    close(body);
    xfree(parent);
    xfree(abs);
    xfree(tmp);
    xfree(tmp_dir);
    xfree(state);
    return ok;
}

/* conflict keeps the local file and saves the remote version beside it. */
static bool conflict(Engine *e, const char *rel, Err *err) {
    const char *ext = path_ext(rel);
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tm);
    StrBuf copy = {0};
    sb_printf(&copy, "%.*s.conflict-%s-%s%s", (int)(strlen(rel) - strlen(ext)), rel, e->host, stamp, ext);
    const char *copy_name = sb_cstr(&copy);
    bool ok = true;
    if (e->dry_run) {
        log_info(e->log, "would save conflict copy", log_str("path", rel), log_str("copy", copy_name), log_end());
        goto out;
    }
    ok = download_to(e, rel, copy_name, false, err);
    if (!ok) goto out;
    log_warn(e->log, "conflict: kept local version, saved remote beside it", log_str("path", rel), log_str("copy", copy_name), log_end());
    queue_push(&e->queue, e->ctx, copy_name);
    char *abs = engine_abs(e, rel);
    struct stat st;
    char sum[SHA256_HEX_LEN];
    if (lstat(abs, &st) != 0) {
        err_sys(err, "%s", abs);
        ok = false;
    } else {
        ok = sha256_file(abs, sum, err) && upload(e, rel, sum, &st, err);
    }
    xfree(abs);
out:
    sb_free(&copy);
    return ok;
}

/* adopt_or_conflict handles a file that exists both locally and remotely but was never synced from here, such as the first run on a second machine. */
static bool adopt_or_conflict(Engine *e, const Object *obj, const LocalFile *local, Err *err) {
    Object head;
    Err inner;
    if (store_head(e->primary, e->ctx, obj->key, &head, &inner) != STORE_OK) {
        err_set(err, "%s", inner.msg);
        return false;
    }
    char *abs = engine_abs(e, obj->key);
    char sum[SHA256_HEX_LEN];
    bool ok = sha256_file(abs, sum, err);
    xfree(abs);
    if (!ok) return false;
    if (strcmp(head.sha256, sum) != 0) return conflict(e, obj->key, err);
    if (e->dry_run) {
        log_info(e->log, "would adopt", log_str("path", obj->key), log_end());
        return true;
    }
    IndexFile record = {.path = obj->key, .size = local->size, .mtime_ns = local->mtime_ns, .sha256 = sum};
    return index_record_synced(e->idx, &record, e->primary_name, head.etag, e->mirror_names, e->mirror_count, err);
}

static bool pull(Engine *e, const Object *obj, Err *err) {
    Arena a;
    arena_init(&a, 4096);
    Replica r;
    bool has_replica;
    bool ok = index_replica(e->idx, &a, obj->key, e->primary_name, &r, &has_replica, err);
    if (!ok) goto out;
    if (has_replica && r.state == REPLICA_VERIFIED && strcmp(r.etag, obj->etag) == 0) goto out;
    IndexFile f;
    bool found;
    ok = index_file(e->idx, &a, obj->key, &f, &found, err);
    if (!ok) goto out;
    if (found && f.deleted) found = false;
    LocalFile local;
    ok = engine_local(e, obj->key, found ? &f : NULL, &local, err);
    if (!ok) goto out;
    if (!local.exists) ok = download(e, obj->key, obj->key, err);
    else if (!found) ok = adopt_or_conflict(e, obj, &local, err);
    else if (!local.changed) ok = download(e, obj->key, obj->key, err);
    else ok = conflict(e, obj->key, err);
out:
    arena_free(&a);
    return ok;
}

/*
 * remote_gone handles an indexed file that is missing from a listing of the
 * primary. The listing may predate an upload that finished while the poll
 * was still pulling, so the miss is confirmed before anything is deleted.
 */
static bool remote_gone(Engine *e, const IndexFile *f, Err *err) {
    Object head;
    Err inner;
    StoreStatus status = store_head(e->primary, e->ctx, f->path, &head, &inner);
    if (status == STORE_OK) return true;
    if (status == STORE_ERROR) {
        err_set(err, "%s", inner.msg);
        return false;
    }
    Arena a;
    arena_init(&a, 4096);
    Replica r;
    bool has_replica;
    bool ok = index_replica(e->idx, &a, f->path, e->primary_name, &r, &has_replica, err);
    if (!ok) goto out;
    if (!has_replica || r.state != REPLICA_VERIFIED) {
        /* Never reached this primary, for example right after a promotion. */
        queue_push(&e->queue, e->ctx, f->path);
        goto out;
    }
    LocalFile local;
    ok = engine_local(e, f->path, f, &local, err);
    if (!ok) goto out;
    if (!local.exists) {
        ok = index_tombstone(e->idx, f->path, e->primary_name, false, err);
        goto out;
    }
    if (local.changed) {
        queue_push(&e->queue, e->ctx, f->path);
        goto out;
    }
    if (!e->cfg->sync.delete_local) {
        ok = index_drop_replica(e->idx, f->path, e->primary_name, err);
        if (ok) queue_push(&e->queue, e->ctx, f->path);
        goto out;
    }
    if (e->dry_run) {
        log_info(e->log, "would delete local", log_str("path", f->path), log_end());
        goto out;
    }
    char *abs = engine_abs(e, f->path);
    if (unlink(abs) != 0 && errno != ENOENT) {
        err_sys(err, "%s", abs);
        ok = false;
    }
    xfree(abs);
    if (!ok) goto out;
    log_info(e->log, "deleted locally, gone from primary", log_str("path", f->path), log_end());
    ok = index_tombstone(e->idx, f->path, e->primary_name, false, err);
    if (ok) engine_wake_mirrors(e);
out:
    arena_free(&a);
    return ok;
}

typedef struct {
    const char *rel;
    bool (*fn)(Engine *, const void *, Err *);
    const void *arg;
} Locked;

static void with_lock(Engine *e, void *item) {
    Locked *l = item;
    if (!locks_acquire(&e->locks, l->rel)) return;
    Err err;
    bool ok = l->fn(e, l->arg, &err);
    if (locks_release(&e->locks, l->rel)) queue_push(&e->queue, e->ctx, l->rel);
    if (!ok) log_error(e->log, "pull", log_str("path", l->rel), log_err(&err), log_end());
}

static bool pull_object(Engine *e, const void *arg, Err *err) { return pull(e, arg, err); }
static bool gone_file(Engine *e, const void *arg, Err *err) { return remote_gone(e, arg, err); }

/* poll pulls changes from the primary. See the sync table in docs/design.md. */
static bool poll_primary(Engine *e, Err *err) {
    Arena a;
    arena_init(&a, 64 * 1024);
    Object *objects;
    size_t n;
    Err inner;
    bool ok = store_list(e->primary, e->ctx, &a, &objects, &n, &inner) == STORE_OK;
    if (!ok) {
        err_set(err, "%s", inner.msg);
        goto out;
    }
    StrMap remote;
    strmap_init(&remote);
    Locked *jobs = arena_calloc(&a, n + 1, sizeof *jobs);
    void **items = arena_calloc(&a, n + 1, sizeof *items);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (ignore_match(&e->ignore, objects[i].key, false)) continue;
        strmap_put(&remote, objects[i].key, NULL);
        jobs[m] = (Locked){objects[i].key, pull_object, &objects[i]};
        items[m] = &jobs[m];
        m++;
    }
    run_workers(e, primary_workers(e), items, m, with_lock);
    IndexFile *files;
    ok = index_files(e->idx, &a, &files, &n, err);
    for (size_t i = 0; ok && i < n; i++) {
        const IndexFile *f = &files[i];
        if (f->deleted || strmap_has(&remote, f->path) || ignore_match(&e->ignore, f->path, false)) continue;
        Locked job = {f->path, gone_file, f};
        with_lock(e, &job);
    }
    strmap_free(&remote);
out:
    arena_free(&a);
    return ok;
}

static bool backfill(Engine *e, Err *err) {
    for (size_t i = 0; i < e->mirror_count; i++) {
        int64_t n;
        if (!index_backfill(e->idx, e->mirror_names[i], &n, err)) return false;
        if (n > 0) log_info(e->log, "backfill", log_str("store", e->mirror_names[i]), log_int("files", n), log_end());
    }
    engine_wake_mirrors(e);
    return true;
}

/* ---- the public entry points ---- */

bool engine_reconcile(Engine *e, Ctx *ctx, Err *err) {
    e->ctx = ctx;
    if (!mkdir_p(e->root, 0755, err)) return false;
    Err inner;
    if (!poll_primary(e, &inner)) {
        err_set(err, "pull from %s: %s", e->primary_name, inner.msg);
        return false;
    }
    if (!push_all(e, err)) return false;
    for (size_t i = 0; i < e->mirror_count; i++)
        if (!mirror_reconcile(e, i, &inner)) log_error(e->log, "reconcile mirror", log_str("store", e->mirror_names[i]), log_err(&inner), log_end());
    return backfill(e, err);
}

bool engine_once(Engine *e, Ctx *ctx, Err *err) {
    e->ctx = ctx;
    if (!engine_reconcile(e, ctx, err)) return false;
    if (e->dry_run) return true;
    bool ok = true;
    for (size_t i = 0; i < e->mirror_count; i++) {
        Err inner;
        if (!mirror_drain(e, i, &inner)) {
            if (ok) err_set(err, "%s", inner.msg);
            ok = false;
        }
    }
    return ok;
}

bool engine_drain_mirror(Engine *e, Ctx *ctx, const char *name, Err *err) {
    e->ctx = ctx;
    for (size_t i = 0; i < e->mirror_count; i++)
        if (strcmp(e->mirror_names[i], name) == 0) return mirror_drain(e, i, err);
    err_set(err, "%s is not a mirror", name);
    return false;
}

size_t engine_backlog(Engine *e) { return debounce_pending(e->debouncer) + queue_len(&e->queue, e->ctx) + locks_in_flight(&e->locks); }

/*
 * reconcile_until_done keeps retrying the startup reconcile, one pull
 * interval apart, until it succeeds or ctx is done. The daemon is usually
 * started by launchd or systemd before the network is up, and giving up
 * would only make the supervisor restart it into the same situation.
 */
static void reconcile_until_done(Engine *e) {
    for (;;) {
        Err err;
        if (engine_reconcile(e, e->ctx, &err) || ctx_done(e->ctx)) return;
        log_error(e->log, "reconcile; retrying", log_err(&err), log_dur("in", e->cfg->sync.pull_interval_ns), log_end());
        if (!ctx_sleep(e->ctx, e->cfg->sync.pull_interval_ns / NS_PER_MS)) return;
    }
}

typedef struct {
    Engine *e;
    int64_t interval_ns;
    const char *name;
    bool (*fn)(Engine *, Err *);
} Periodic;

static void *every(void *arg) {
    Periodic *p = arg;
    while (ctx_sleep(p->e->ctx, p->interval_ns / NS_PER_MS) && !engine_done(p->e)) {
        Err err;
        if (!p->fn(p->e, &err) && !engine_done(p->e)) log_error(p->e->log, p->name, log_err(&err), log_end());
    }
    return NULL;
}

typedef struct {
    Engine *e;
    size_t mirror;
} MirrorArg;

static void *mirror_main(void *arg) {
    MirrorArg *m = arg;
    mirror_loop(m->e, m->mirror);
    return NULL;
}

typedef struct {
    Watcher *w;
    Ctx *ctx;
} WatchArg;

static void *watch_main(void *arg) {
    WatchArg *w = arg;
    watcher_run(w->w, w->ctx);
    return NULL;
}

static bool ignore_cb(void *ctx, const char *rel, bool is_dir) { return ignore_match(&((Engine *)ctx)->ignore, rel, is_dir); }
static void changed_cb(void *ctx, const char *rel) { debounce_trigger(((Engine *)ctx)->debouncer, rel); }

typedef struct {
    pthread_t *threads;
    size_t count, cap;
} Threads;

static void start(Threads *t, void *(*fn)(void *), void *arg) {
    if (t->count == t->cap) t->threads = xrealloc(t->threads, (t->cap = t->cap ? t->cap * 2 : 8) * sizeof *t->threads);
    pthread_create(&t->threads[t->count++], NULL, fn, arg);
}

static void join_all(Threads *t) {
    for (size_t i = 0; i < t->count; i++) pthread_join(t->threads[i], NULL);
    xfree(t->threads);
    memset(t, 0, sizeof *t);
}

bool engine_run(Engine *e, Ctx *ctx, Err *err) {
    e->ctx = ctx;
    atomic_store(&e->stopping, false);
    if (!mkdir_p(e->root, 0755, err)) return false;
    reconcile_until_done(e);
    if (ctx_done(ctx)) return true;

    Threads threads = {0};
    int workers = primary_workers(e);
    for (int i = 0; i < workers; i++) start(&threads, primary_worker, e);
    MirrorArg *mirror_args = xcalloc(e->mirror_count + 1, sizeof *mirror_args);
    for (size_t i = 0; i < e->mirror_count; i++) {
        mirror_args[i] = (MirrorArg){e, i};
        start(&threads, mirror_main, &mirror_args[i]);
    }

    Watcher *watcher = watcher_new(e->root, ignore_cb, changed_cb, e, e->log, err);
    if (!watcher) {
        atomic_store(&e->stopping, true);
        ctx_notify(ctx);
        join_all(&threads);
        xfree(mirror_args);
        return false;
    }
    WatchArg watch_arg = {watcher, ctx};
    start(&threads, watch_main, &watch_arg);
    Periodic poll_job = {e, e->cfg->sync.pull_interval_ns, "poll", poll_primary};
    Periodic retry_job = {e, e->cfg->sync.pull_interval_ns, "retry", retry_due_uploads};
    Periodic backfill_job = {e, e->cfg->sync.backfill_interval_ns, "backfill", backfill};
    start(&threads, every, &poll_job);
    start(&threads, every, &retry_job);
    start(&threads, every, &backfill_job);

    StrBuf mirrors = {0};
    sb_putc(&mirrors, '[');
    for (size_t i = 0; i < e->mirror_count; i++) sb_printf(&mirrors, "%s%s", i ? " " : "", e->mirror_names[i]);
    sb_putc(&mirrors, ']');
    log_info(e->log, "watching", log_str("root", e->root), log_str("primary", e->primary_name), log_str("mirrors", sb_cstr(&mirrors)), log_end());
    sb_free(&mirrors);

    ctx_lock(ctx);
    while (ctx_wait(ctx, 0)) {
    }
    ctx_unlock(ctx);
    join_all(&threads);
    watcher_free(watcher);
    xfree(mirror_args);
    return true;
}
