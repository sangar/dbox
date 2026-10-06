#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine_internal.h"
#include "platform/platform.h"
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
    host_short_name(e->host);
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
    Thread *threads = xcalloc(n + 1, sizeof *threads);
    for (size_t i = 0; i < n; i++) thread_start(&threads[i], work_main, &w);
    for (size_t i = 0; i < n; i++) thread_join(&threads[i]);
    xfree(threads);
}

static int primary_workers(const Engine *e) { return config_store(e->cfg, e->primary_name)->workers; }

/* ---- local files ---- */

Error engine_local(Engine *e, const char *rel, const IndexFile *f, LocalFile *out, Err *err) {
    char *abs = engine_abs(e, rel);
    FileStat st;
    memset(out, 0, sizeof *out);
    Error result = file_info(abs, &st, err);
    if (result != ERR_OK || !st.exists) {
        xfree(abs);
        return result;
    }
    out->exists = true;
    out->changed = true;
    out->size = st.size;
    out->mtime_ns = st.mtime_ns;
    if (f && f->size == out->size && f->mtime_ns == out->mtime_ns) {
        out->changed = false;
    } else if (f) {
        /* Size and time decide first; content is hashed only when they differ. */
        char sum[SHA256_HEX_LEN];
        result = sha256_file(abs, sum, err);
        if (result == ERR_OK) out->changed = strcmp(sum, f->sha256) != 0;
    }
    xfree(abs);
    return result;
}

/* ---- pushing to the primary ---- */

[[nodiscard]] static Error upload(Engine *e, const char *rel, const char *sum, const FileStat *info, Err *err);

/*
 * upload sends rel, whose content hashed to sum when it looked like info, to
 * the primary. If the file changed meanwhile, nothing is recorded and the
 * path is queued again, so the index never describes a version other than
 * the one the primary holds.
 */
[[nodiscard]] static Error upload(Engine *e, const char *rel, const char *sum, const FileStat *info, Err *err) {
    if (e->dry_run) {
        log_info(e->log, "would upload", log_str("path", rel), log_str("store", e->primary_name), log_end());
        return ERR_OK;
    }
    char *abs = engine_abs(e, rel);
    int fd;
    Error result = file_open_read(abs, &fd, err);
    if (result != ERR_OK) {
        if (result == ERR_NOT_FOUND) result = err_set(err, result, "%s: no such file", abs);
        xfree(abs);
        return result;
    }
    Meta meta = {.mtime_ns = info->mtime_ns};
    snprintf(meta.sha256, sizeof meta.sha256, "%s", sum);
    char etag[ETAG_MAX];
    Err inner;
    result = store_put(e->primary, e->ctx, rel, fd, info->size, &meta, etag, &inner);
    file_close(fd);
    if (result != ERR_OK) {
        xfree(abs);
        return err_set(err, result, "upload to %s: %s", e->primary_name, inner.msg);
    }
    FileStat after;
    bool unchanged = file_info(abs, &after, NULL) == ERR_OK && after.exists && after.size == info->size && after.mtime_ns == info->mtime_ns;
    xfree(abs);
    if (!unchanged) {
        log_info(e->log, "changed during upload; syncing again", log_str("path", rel), log_end());
        queue_push(&e->queue, e->ctx, rel);
        return ERR_OK;
    }
    IndexFile record = {.path = rel, .size = info->size, .mtime_ns = info->mtime_ns, .sha256 = sum};
    result = index_record_synced(e->idx, &record, e->primary_name, etag, e->mirror_names, e->mirror_count, err);
    if (result != ERR_OK) return result;
    log_info(e->log, "uploaded", log_str("path", rel), log_str("store", e->primary_name), log_int("bytes", info->size), log_end());
    engine_wake_mirrors(e);
    return ERR_OK;
}

/* push_delete handles a path that is gone locally: a deleted file, or a directory that was removed or renamed with everything in it. */
[[nodiscard]] static Error push_delete(Engine *e, const char *rel, Err *err) {
    Arena a;
    arena_init(&a, 4096);
    IndexFile f;
    bool found;
    Error result = index_file(e->idx, &a, rel, &f, &found, err);
    if (result != ERR_OK) goto out;
    if (!found) {
        IndexFile *inside;
        size_t n;
        result = index_clear_upload_failure(e->idx, rel, err);
        if (result == ERR_OK) result = index_files_under(e->idx, &a, rel, &inside, &n, err);
        if (result == ERR_OK)
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
        result = store_delete(e->primary, e->ctx, rel, &inner);
        if (result != ERR_OK) {
            result = err_set(err, result, "delete from %s: %s", e->primary_name, inner.msg);
            goto out;
        }
        log_info(e->log, "deleted", log_str("path", rel), log_str("store", e->primary_name), log_end());
    }
    result = index_tombstone(e->idx, rel, e->primary_name, !e->cfg->sync.delete_remote, err);
    if (result == ERR_OK) engine_wake_mirrors(e);
out:
    arena_free(&a);
    return result;
}

/* push makes the primary match the local file at rel. */
[[nodiscard]] static Error push(Engine *e, const char *rel, Err *err) {
    char *abs = engine_abs(e, rel);
    FileStat st;
    Error result = file_info(abs, &st, err);
    if (result != ERR_OK) {
        xfree(abs);
        return result;
    }
    if (!st.exists) {
        xfree(abs);
        return push_delete(e, rel, err);
    }
    if (!st.is_regular) {
        xfree(abs);
        return ERR_OK;
    }
    Arena a;
    arena_init(&a, 4096);
    IndexFile f;
    bool found;
    result = index_file(e->idx, &a, rel, &f, &found, err);
    bool primary_current = false;
    if (result == ERR_OK && found && !f.deleted) {
        Replica r;
        bool has_replica;
        result = index_replica(e->idx, &a, rel, e->primary_name, &r, &has_replica, err);
        primary_current = result == ERR_OK && has_replica && r.state == REPLICA_VERIFIED;
    }
    if (result == ERR_OK && !(primary_current && f.size == st.size && f.mtime_ns == st.mtime_ns)) {
        char sum[SHA256_HEX_LEN];
        result = sha256_file(abs, sum, err);
        if (result == ERR_OK && primary_current && strcmp(f.sha256, sum) == 0) result = index_touch(e->idx, rel, st.mtime_ns, err);
        else if (result == ERR_OK) result = upload(e, rel, sum, &st, err);
    }
    arena_free(&a);
    xfree(abs);
    return result;
}

static void record_upload_failure(Engine *e, const char *rel, const Err *cause) {
    Arena a;
    arena_init(&a, 4096);
    int attempts = 1;
    UploadFailure prev;
    bool found;
    Err err;
    if (index_upload_failure(e->idx, &a, rel, &prev, &found, &err) == ERR_OK && found) attempts = prev.attempts + 1;
    if (attempts >= MAX_ATTEMPTS) {
        log_error(e->log, "sync failed; giving up until `dbox retry` or the file changes", log_str("path", rel), log_int("attempts", attempts), log_err(cause), log_end());
    } else {
        log_error(e->log, "sync failed; will retry", log_str("path", rel), log_int("attempt", attempts), log_dur("in", retry_delay_ns(attempts)), log_err(cause), log_end());
    }
    if (index_mark_upload_failed(e->idx, rel, cause->msg, attempts, wall_ns() + retry_delay_ns(attempts), &err) != ERR_OK)
        log_error(e->log, "record failure", log_str("path", rel), log_err(&err), log_end());
    arena_free(&a);
}

/* process syncs one path under its lock, and records a failure so that retry_due_uploads brings the path back after a backoff. */
static void process(Engine *e, const char *rel) {
    if (!locks_acquire(&e->locks, rel)) return;
    Err err;
    Error result = push(e, rel, &err);
    if (locks_release(&e->locks, rel)) queue_push(&e->queue, e->ctx, rel);
    if (result != ERR_OK && !ctx_done(e->ctx)) record_upload_failure(e, rel, &err);
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
[[nodiscard]] static Error retry_due_uploads(Engine *e, Err *err) {
    Arena a;
    arena_init(&a, 4096);
    const char **due;
    size_t n;
    Error result = index_due_uploads(e->idx, &a, wall_ns(), &due, &n, err);
    for (size_t i = 0; result == ERR_OK && i < n; i++) queue_push(&e->queue, e->ctx, due[i]);
    arena_free(&a);
    return result;
}

/* walk collects the relative paths of every regular file under dir that is not ignored. */
[[nodiscard]] static Error walk(Engine *e, const char *dir, StrList *paths, Err *err) {
    StrList names = {0};
    Error result = dir_list(dir, &names, err);
    if (result == ERR_NOT_FOUND) result = err_set(err, result, "%s: no such directory", dir);
    for (size_t i = 0; result == ERR_OK && i < names.len; i++) {
        char *p = path_join(dir, names.items[i]);
        char *rel = rel_path(e->root, p);
        FileStat st;
        if (rel && file_info(p, &st, NULL) == ERR_OK && st.exists && !ignore_match(&e->ignore, rel, st.is_dir)) {
            if (st.is_dir) result = walk(e, p, paths, err);
            else if (st.is_regular) strlist_push(paths, rel);
        }
        xfree(rel);
        xfree(p);
    }
    strlist_free(&names);
    return result;
}

/* push_all syncs every local file and every indexed file that is gone locally. */
[[nodiscard]] static Error push_all(Engine *e, Err *err) {
    StrList paths = {0};
    Error result = walk(e, e->root, &paths, err);
    if (result != ERR_OK) {
        strlist_free(&paths);
        return result;
    }
    Arena a;
    arena_init(&a, 64 * 1024);
    IndexFile *files;
    size_t n;
    result = index_files(e->idx, &a, &files, &n, err);
    if (result == ERR_OK) {
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
    return result;
}

/* ---- pulling from the primary ---- */

[[nodiscard]] static Error download_to(Engine *e, const char *key, const char *rel, bool record, Err *err);

[[nodiscard]] static Error download(Engine *e, const char *key, const char *rel, Err *err) {
    if (e->dry_run) {
        log_info(e->log, "would download", log_str("path", rel), log_str("store", e->primary_name), log_end());
        return ERR_OK;
    }
    return download_to(e, key, rel, true, err);
}

/*
 * download_to fetches key into a temp file and renames it to rel. With
 * record, the index is updated before the rename so the watcher event that
 * follows finds nothing to upload.
 */
[[nodiscard]] static Error download_to(Engine *e, const char *key, const char *rel, bool record, Err *err) {
    Object obj;
    int body;
    Err inner;
    Error result = store_get(e->primary, e->ctx, key, &obj, &body, &inner);
    if (result != ERR_OK) return err_set(err, result, "%s", inner.msg);
    char *state = config_state_dir(e->cfg);
    char *tmp_dir = path_join(state, "tmp");
    char *tmp = path_join(tmp_dir, "download-XXXXXX");
    char *abs = engine_abs(e, rel);
    char *parent = path_dir(abs);
    int out = -1;
    int64_t copied;
    if ((result = mkdir_p(tmp_dir, 0755, err)) != ERR_OK) goto done;
    if ((result = file_mkstemp(tmp, &out, err)) != ERR_OK) goto done;
    Sha256 h;
    sha256_init(&h);
    if ((result = copy_fd(body, out, &h, &copied, err)) != ERR_OK) goto done;
    file_close(out);
    out = -1;
    if (obj.mtime_ns != 0 && (result = file_set_mtime(tmp, obj.mtime_ns, err)) != ERR_OK) goto done;
    FileStat st;
    if ((result = file_info_follow(tmp, &st, err)) != ERR_OK) goto done;
    if (!st.exists) {
        result = err_set(err, ERR_IO, "%s: vanished", tmp);
        goto done;
    }
    if (record) {
        char sum[SHA256_HEX_LEN];
        sha256_hex(&h, sum);
        IndexFile f = {.path = rel, .size = st.size, .mtime_ns = st.mtime_ns, .sha256 = sum};
        if ((result = index_record_synced(e->idx, &f, e->primary_name, obj.etag, e->mirror_names, e->mirror_count, err)) != ERR_OK) goto done;
    }
    if ((result = mkdir_p(parent, 0755, err)) != ERR_OK) goto done;
    if ((result = file_rename(tmp, abs, err)) != ERR_OK) goto done;
    log_info(e->log, "downloaded", log_str("path", rel), log_str("store", e->primary_name), log_int("bytes", st.size), log_end());
done:
    if (out >= 0) file_close(out);
    if (result != ERR_OK) (void)file_remove(tmp, NULL);
    file_close(body);
    xfree(parent);
    xfree(abs);
    xfree(tmp);
    xfree(tmp_dir);
    xfree(state);
    return result;
}

/* conflict keeps the local file and saves the remote version beside it. */
[[nodiscard]] static Error conflict(Engine *e, const char *rel, Err *err) {
    const char *ext = path_ext(rel);
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tm);
    StrBuf copy = {0};
    sb_printf(&copy, "%.*s.conflict-%s-%s%s", (int)(strlen(rel) - strlen(ext)), rel, e->host, stamp, ext);
    const char *copy_name = sb_cstr(&copy);
    Error result = ERR_OK;
    if (e->dry_run) {
        log_info(e->log, "would save conflict copy", log_str("path", rel), log_str("copy", copy_name), log_end());
        goto out;
    }
    result = download_to(e, rel, copy_name, false, err);
    if (result != ERR_OK) goto out;
    log_warn(e->log, "conflict: kept local version, saved remote beside it", log_str("path", rel), log_str("copy", copy_name), log_end());
    queue_push(&e->queue, e->ctx, copy_name);
    char *abs = engine_abs(e, rel);
    FileStat st;
    char sum[SHA256_HEX_LEN];
    result = file_info(abs, &st, err);
    if (result == ERR_OK && !st.exists) result = err_set(err, ERR_NOT_FOUND, "%s: no such file", abs);
    if (result == ERR_OK) result = sha256_file(abs, sum, err);
    if (result == ERR_OK) result = upload(e, rel, sum, &st, err);
    xfree(abs);
out:
    sb_free(&copy);
    return result;
}

/* adopt_or_conflict handles a file that exists both locally and remotely but was never synced from here, such as the first run on a second machine. */
[[nodiscard]] static Error adopt_or_conflict(Engine *e, const Object *obj, const LocalFile *local, Err *err) {
    Object head;
    Err inner;
    Error result = store_head(e->primary, e->ctx, obj->key, &head, &inner);
    if (result != ERR_OK) return err_set(err, result, "%s", inner.msg);
    char *abs = engine_abs(e, obj->key);
    char sum[SHA256_HEX_LEN];
    result = sha256_file(abs, sum, err);
    xfree(abs);
    if (result != ERR_OK) return result;
    if (strcmp(head.sha256, sum) != 0) return conflict(e, obj->key, err);
    if (e->dry_run) {
        log_info(e->log, "would adopt", log_str("path", obj->key), log_end());
        return ERR_OK;
    }
    IndexFile record = {.path = obj->key, .size = local->size, .mtime_ns = local->mtime_ns, .sha256 = sum};
    return index_record_synced(e->idx, &record, e->primary_name, head.etag, e->mirror_names, e->mirror_count, err);
}

[[nodiscard]] static Error pull(Engine *e, const Object *obj, Err *err) {
    Arena a;
    arena_init(&a, 4096);
    Replica r;
    bool has_replica;
    Error result = index_replica(e->idx, &a, obj->key, e->primary_name, &r, &has_replica, err);
    if (result != ERR_OK) goto out;
    if (has_replica && r.state == REPLICA_VERIFIED && strcmp(r.etag, obj->etag) == 0) goto out;
    IndexFile f;
    bool found;
    result = index_file(e->idx, &a, obj->key, &f, &found, err);
    if (result != ERR_OK) goto out;
    if (found && f.deleted) found = false;
    LocalFile local;
    result = engine_local(e, obj->key, found ? &f : NULL, &local, err);
    if (result != ERR_OK) goto out;
    if (!local.exists) result = download(e, obj->key, obj->key, err);
    else if (!found) result = adopt_or_conflict(e, obj, &local, err);
    else if (!local.changed) result = download(e, obj->key, obj->key, err);
    else result = conflict(e, obj->key, err);
out:
    arena_free(&a);
    return result;
}

/*
 * remote_gone handles an indexed file that is missing from a listing of the
 * primary. The listing may predate an upload that finished while the poll
 * was still pulling, so the miss is confirmed before anything is deleted.
 */
[[nodiscard]] static Error remote_gone(Engine *e, const IndexFile *f, Err *err) {
    Object head;
    Err inner;
    Error result = store_head(e->primary, e->ctx, f->path, &head, &inner);
    if (result == ERR_OK) return ERR_OK;
    if (result != ERR_NOT_FOUND) return err_set(err, result, "%s", inner.msg);
    Arena a;
    arena_init(&a, 4096);
    Replica r;
    bool has_replica;
    result = index_replica(e->idx, &a, f->path, e->primary_name, &r, &has_replica, err);
    if (result != ERR_OK) goto out;
    if (!has_replica || r.state != REPLICA_VERIFIED) {
        /* Never reached this primary, for example right after a promotion. */
        queue_push(&e->queue, e->ctx, f->path);
        goto out;
    }
    LocalFile local;
    result = engine_local(e, f->path, f, &local, err);
    if (result != ERR_OK) goto out;
    if (!local.exists) {
        result = index_tombstone(e->idx, f->path, e->primary_name, false, err);
        goto out;
    }
    if (local.changed) {
        queue_push(&e->queue, e->ctx, f->path);
        goto out;
    }
    if (!e->cfg->sync.delete_local) {
        result = index_drop_replica(e->idx, f->path, e->primary_name, err);
        if (result == ERR_OK) queue_push(&e->queue, e->ctx, f->path);
        goto out;
    }
    if (e->dry_run) {
        log_info(e->log, "would delete local", log_str("path", f->path), log_end());
        goto out;
    }
    char *abs = engine_abs(e, f->path);
    result = file_remove(abs, err);
    if (result == ERR_NOT_FOUND) result = ERR_OK;
    xfree(abs);
    if (result != ERR_OK) goto out;
    log_info(e->log, "deleted locally, gone from primary", log_str("path", f->path), log_end());
    result = index_tombstone(e->idx, f->path, e->primary_name, false, err);
    if (result == ERR_OK) engine_wake_mirrors(e);
out:
    arena_free(&a);
    return result;
}

typedef struct {
    const char *rel;
    Error (*fn)(Engine *, const void *, Err *);
    const void *arg;
} Locked;

static void with_lock(Engine *e, void *item) {
    Locked *l = item;
    if (!locks_acquire(&e->locks, l->rel)) return;
    Err err;
    Error result = l->fn(e, l->arg, &err);
    if (locks_release(&e->locks, l->rel)) queue_push(&e->queue, e->ctx, l->rel);
    if (result != ERR_OK) log_error(e->log, "pull", log_str("path", l->rel), log_err(&err), log_end());
}

[[nodiscard]] static Error pull_object(Engine *e, const void *arg, Err *err) { return pull(e, arg, err); }
[[nodiscard]] static Error gone_file(Engine *e, const void *arg, Err *err) { return remote_gone(e, arg, err); }

/* poll pulls changes from the primary. See the sync table in docs/design.md. */
[[nodiscard]] static Error poll_primary(Engine *e, Err *err) {
    Arena a;
    arena_init(&a, 64 * 1024);
    Object *objects;
    size_t n;
    Err inner;
    Error result = store_list(e->primary, e->ctx, &a, &objects, &n, &inner);
    if (result != ERR_OK) {
        result = err_set(err, result, "%s", inner.msg);
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
    result = index_files(e->idx, &a, &files, &n, err);
    for (size_t i = 0; result == ERR_OK && i < n; i++) {
        const IndexFile *f = &files[i];
        if (f->deleted || strmap_has(&remote, f->path) || ignore_match(&e->ignore, f->path, false)) continue;
        Locked job = {f->path, gone_file, f};
        with_lock(e, &job);
    }
    strmap_free(&remote);
out:
    arena_free(&a);
    return result;
}

[[nodiscard]] static Error backfill(Engine *e, Err *err) {
    for (size_t i = 0; i < e->mirror_count; i++) {
        int64_t n;
        Error result = index_backfill(e->idx, e->mirror_names[i], &n, err);
        if (result != ERR_OK) return result;
        if (n > 0) log_info(e->log, "backfill", log_str("store", e->mirror_names[i]), log_int("files", n), log_end());
    }
    engine_wake_mirrors(e);
    return ERR_OK;
}

/* ---- the public entry points ---- */

Error engine_reconcile(Engine *e, Ctx *ctx, Err *err) {
    e->ctx = ctx;
    Error result = mkdir_p(e->root, 0755, err);
    if (result != ERR_OK) return result;
    Err inner;
    result = poll_primary(e, &inner);
    if (result != ERR_OK) return err_set(err, result, "pull from %s: %s", e->primary_name, inner.msg);
    result = push_all(e, err);
    if (result != ERR_OK) return result;
    for (size_t i = 0; i < e->mirror_count; i++)
        if (mirror_reconcile(e, i, &inner) != ERR_OK) log_error(e->log, "reconcile mirror", log_str("store", e->mirror_names[i]), log_err(&inner), log_end());
    return backfill(e, err);
}

Error engine_once(Engine *e, Ctx *ctx, Err *err) {
    e->ctx = ctx;
    Error result = engine_reconcile(e, ctx, err);
    if (result != ERR_OK) return result;
    if (e->dry_run) return ERR_OK;
    for (size_t i = 0; i < e->mirror_count; i++) {
        Err inner;
        Error drained = mirror_drain(e, i, &inner);
        if (drained != ERR_OK && result == ERR_OK) result = err_set(err, drained, "%s", inner.msg);
    }
    return result;
}

Error engine_drain_mirror(Engine *e, Ctx *ctx, const char *name, Err *err) {
    e->ctx = ctx;
    for (size_t i = 0; i < e->mirror_count; i++)
        if (strcmp(e->mirror_names[i], name) == 0) return mirror_drain(e, i, err);
    return err_set(err, ERR_NOT_FOUND, "%s is not a mirror", name);
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
        if (engine_reconcile(e, e->ctx, &err) == ERR_OK || ctx_done(e->ctx)) return;
        log_error(e->log, "reconcile; retrying", log_err(&err), log_dur("in", e->cfg->sync.pull_interval_ns), log_end());
        if (!ctx_sleep(e->ctx, e->cfg->sync.pull_interval_ns / NS_PER_MS)) return;
    }
}

typedef struct {
    Engine *e;
    int64_t interval_ns;
    const char *name;
    Error (*fn)(Engine *, Err *);
} Periodic;

static void *every(void *arg) {
    Periodic *p = arg;
    while (ctx_sleep(p->e->ctx, p->interval_ns / NS_PER_MS) && !engine_done(p->e)) {
        Err err;
        if (p->fn(p->e, &err) != ERR_OK && !engine_done(p->e)) log_error(p->e->log, p->name, log_err(&err), log_end());
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
    Thread *threads;
    size_t count, cap;
} Threads;

static void start(Threads *t, void *(*fn)(void *), void *arg) {
    if (t->count == t->cap) t->threads = xrealloc(t->threads, (t->cap = t->cap ? t->cap * 2 : 8) * sizeof *t->threads);
    thread_start(&t->threads[t->count++], fn, arg);
}

static void join_all(Threads *t) {
    for (size_t i = 0; i < t->count; i++) thread_join(&t->threads[i]);
    xfree(t->threads);
    memset(t, 0, sizeof *t);
}

Error engine_run(Engine *e, Ctx *ctx, Err *err) {
    e->ctx = ctx;
    atomic_store(&e->stopping, false);
    Error result = mkdir_p(e->root, 0755, err);
    if (result != ERR_OK) return result;
    reconcile_until_done(e);
    if (ctx_done(ctx)) return ERR_OK;

    Threads threads = {0};
    int workers = primary_workers(e);
    for (int i = 0; i < workers; i++) start(&threads, primary_worker, e);
    MirrorArg *mirror_args = xcalloc(e->mirror_count + 1, sizeof *mirror_args);
    for (size_t i = 0; i < e->mirror_count; i++) {
        mirror_args[i] = (MirrorArg){e, i};
        start(&threads, mirror_main, &mirror_args[i]);
    }

    Watcher *watcher;
    result = watcher_new(e->root, ignore_cb, changed_cb, e, e->log, &watcher, err);
    if (result != ERR_OK) {
        atomic_store(&e->stopping, true);
        ctx_notify(ctx);
        join_all(&threads);
        xfree(mirror_args);
        return result;
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
    return ERR_OK;
}
