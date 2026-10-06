#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine_internal.h"

#define MIRROR_BATCH 256
#define IDLE_NS (5 * NS_PER_SEC)

typedef struct {
    size_t mirror;
    Replica *replica;
} Job;

[[nodiscard]] static Error replicate(Engine *e, size_t mirror, const Replica *r, bool *source_missing, Err *err);

static void record_failure(Engine *e, const Replica *r, bool source_missing, const Err *cause) {
    int attempts = r->attempts + 1;
    /* When neither the primary nor the local folder has the content, retrying cannot help. */
    if (source_missing) attempts = MAX_ATTEMPTS;
    int64_t retry_at = wall_ns() + retry_delay_ns(attempts);
    log_warn(e->log, "mirror copy failed", log_str("store", r->store), log_str("path", r->path), log_int("attempt", attempts), log_err(cause), log_end());
    Err err;
    if (index_mark_failed(e->idx, r->path, r->store, cause->msg, attempts, retry_at, &err) != ERR_OK) log_error(e->log, "record failure", log_err(&err), log_end());
}

static void replicate_job(Engine *e, void *item) {
    Job *job = item;
    Err err;
    bool source_missing = false;
    if (replicate(e, job->mirror, job->replica, &source_missing, &err) != ERR_OK && !ctx_done(e->ctx)) record_failure(e, job->replica, source_missing, &err);
}

/* mirror_batch handles up to one batch of due replicas with the store's workers and stores how many it handled in *handled. */
[[nodiscard]] static Error mirror_batch(Engine *e, size_t mirror, size_t *handled, Err *err) {
    const char *name = e->mirror_names[mirror];
    Arena a;
    arena_init(&a, 16 * 1024);
    Replica *due;
    size_t n;
    *handled = 0;
    Error result = index_due(e->idx, &a, name, MIRROR_BATCH, &due, &n, err);
    if (result == ERR_OK) {
        Job *jobs = arena_calloc(&a, n + 1, sizeof *jobs);
        void **items = arena_calloc(&a, n + 1, sizeof *items);
        for (size_t i = 0; i < n; i++) {
            jobs[i] = (Job){mirror, &due[i]};
            items[i] = &jobs[i];
        }
        run_workers(e, config_store(e->cfg, name)->workers, items, n, replicate_job);
        *handled = n;
    }
    arena_free(&a);
    return result;
}

void mirror_loop(Engine *e, size_t mirror) {
    while (!engine_done(e)) {
        Err err;
        size_t n;
        if (mirror_batch(e, mirror, &n, &err) != ERR_OK && !engine_done(e)) log_error(e->log, "mirror", log_str("store", e->mirror_names[mirror]), log_err(&err), log_end());
        if (n > 0) continue;
        ctx_lock(e->ctx);
        int64_t deadline = monotonic_ns() + IDLE_NS;
        while (!engine_done(e) && !e->mirror_woken[mirror] && monotonic_ns() < deadline) ctx_wait(e->ctx, deadline);
        e->mirror_woken[mirror] = false;
        ctx_unlock(e->ctx);
    }
}

Error mirror_drain(Engine *e, size_t mirror, Err *err) {
    for (;;) {
        size_t n;
        Error result = mirror_batch(e, mirror, &n, err);
        if (result != ERR_OK || n == 0) return result;
    }
}

/* source opens the content a mirror should receive: the primary's copy, or the local file when the primary lacks it but the file is unchanged. */
[[nodiscard]] static Error source(Engine *e, const IndexFile *f, Meta *meta, int *fd, bool *source_missing, Err *err) {
    Object obj;
    Err inner;
    Error result = store_get(e->primary, e->ctx, f->path, &obj, fd, &inner);
    memset(meta, 0, sizeof *meta);
    meta->mtime_ns = f->mtime_ns;
    if (result == ERR_OK) {
        snprintf(meta->sha256, sizeof meta->sha256, "%s", *obj.sha256 ? obj.sha256 : f->sha256);
        return ERR_OK;
    }
    if (result != ERR_NOT_FOUND) return err_set(err, result, "%s", inner.msg);
    LocalFile local;
    if (engine_local(e, f->path, f, &local, &inner) != ERR_OK || !local.exists || local.changed) {
        *source_missing = true;
        return err_set(err, ERR_NOT_FOUND, "source missing on primary and locally");
    }
    char *abs = engine_abs(e, f->path);
    *fd = open(abs, O_RDONLY | O_CLOEXEC);
    result = *fd < 0 ? err_sys(err, "%s", abs) : ERR_OK;
    xfree(abs);
    snprintf(meta->sha256, sizeof meta->sha256, "%s", f->sha256);
    return result;
}

/* replicate makes a mirror match the index for one path: delete a tombstoned file, or copy the current content from the primary and verify it. */
[[nodiscard]] static Error replicate(Engine *e, size_t mirror, const Replica *r, bool *source_missing, Err *err) {
    const char *name = e->mirror_names[mirror];
    Store *store = e->mirrors[mirror];
    Arena a;
    arena_init(&a, 4096);
    IndexFile f;
    bool found;
    Error result = index_file(e->idx, &a, r->path, &f, &found, err);
    if (result != ERR_OK) goto out;
    if (!found) {
        result = index_drop_replica(e->idx, r->path, name, err);
        goto out;
    }
    if (f.deleted) {
        if (e->dry_run) {
            log_info(e->log, "would delete", log_str("path", r->path), log_str("store", name), log_end());
            goto out;
        }
        if (e->cfg->sync.delete_remote) {
            Err inner;
            result = store_delete(store, e->ctx, r->path, &inner);
            if (result != ERR_OK) {
                result = err_set(err, result, "%s", inner.msg);
                goto out;
            }
            log_info(e->log, "deleted", log_str("path", r->path), log_str("store", name), log_end());
        }
        result = index_drop_replica(e->idx, r->path, name, err);
        goto out;
    }

    Object head;
    Err inner;
    if (store_head(store, e->ctx, r->path, &head, &inner) == ERR_OK && head.size == f.size && strcmp(head.sha256, f.sha256) == 0) {
        bool verified;
        result = index_mark_verified(e->idx, r->path, name, head.etag, f.sha256, &verified, err);
        goto out;
    }
    if (e->dry_run) {
        log_info(e->log, "would copy", log_str("path", r->path), log_str("from", e->primary_name), log_str("to", name), log_end());
        goto out;
    }

    Meta meta;
    int body;
    result = source(e, &f, &meta, &body, source_missing, err);
    if (result != ERR_OK) goto out;
    char etag[ETAG_MAX];
    result = store_put(store, e->ctx, r->path, body, f.size, &meta, etag, &inner);
    close(body);
    if (result != ERR_OK) {
        result = err_set(err, result, "%s", inner.msg);
        goto out;
    }
    result = store_head(store, e->ctx, r->path, &head, &inner);
    if (result != ERR_OK) {
        result = err_set(err, result, "verify: %s", inner.msg);
        goto out;
    }
    if (strcmp(head.sha256, meta.sha256) != 0) {
        result = err_set(err, ERR_REMOTE, "verify: stored sha256 %s, expected %s", head.sha256, meta.sha256);
        goto out;
    }
    bool verified;
    result = index_mark_verified(e->idx, r->path, name, head.etag, meta.sha256, &verified, err);
    if (result == ERR_OK && verified) log_info(e->log, "copied", log_str("path", r->path), log_str("from", e->primary_name), log_str("to", name), log_int("bytes", head.size), log_end());
out:
    arena_free(&a);
    return result;
}

/*
 * mirror_reconcile lists a mirror and corrects replica rows: copies already
 * there, for example from rclone or bucket replication, become verified,
 * and verified copies that disappeared go back to pending.
 */
Error mirror_reconcile(Engine *e, size_t mirror, Err *err) {
    const char *name = e->mirror_names[mirror];
    Store *store = e->mirrors[mirror];
    Arena a;
    arena_init(&a, 64 * 1024);
    Object *objects;
    size_t n;
    Err inner;
    Error result = store_list(store, e->ctx, &a, &objects, &n, &inner);
    if (result != ERR_OK) {
        result = err_set(err, result, "%s", inner.msg);
        goto out;
    }
    StrMap present;
    strmap_init(&present);
    for (size_t i = 0; i < n; i++) strmap_put(&present, objects[i].key, &objects[i]);
    Replica *rows;
    size_t count;
    result = index_replicas_on(e->idx, &a, name, &rows, &count, err);
    for (size_t i = 0; result == ERR_OK && i < count; i++)
        if (rows[i].state == REPLICA_VERIFIED && !strmap_has(&present, rows[i].path)) result = index_mark_pending(e->idx, rows[i].path, name, err);
    int64_t added;
    if (result == ERR_OK) result = index_backfill(e->idx, name, &added, err);
    if (result == ERR_OK) result = index_due(e->idx, &a, name, 1 << 30, &rows, &count, err);
    long adopted = 0;
    for (size_t i = 0; result == ERR_OK && i < count; i++) {
        const Object *obj = strmap_get(&present, rows[i].path);
        if (!obj) continue;
        IndexFile f;
        bool found;
        result = index_file(e->idx, &a, rows[i].path, &f, &found, err);
        if (result != ERR_OK || !found || f.deleted || f.size != obj->size) continue;
        Object head;
        if (store_head(store, e->ctx, rows[i].path, &head, &inner) != ERR_OK || strcmp(head.sha256, f.sha256) != 0) continue;
        bool verified;
        result = index_mark_verified(e->idx, rows[i].path, name, head.etag, f.sha256, &verified, err);
        if (result == ERR_OK && verified) adopted++;
    }
    if (adopted > 0) log_info(e->log, "adopted existing copies", log_str("store", name), log_int("files", adopted), log_end());
    strmap_free(&present);
out:
    arena_free(&a);
    return result;
}
