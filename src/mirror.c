#include "engine_internal.h"
#include "mc/platform/platform.h"

enum { MIRROR_BATCH = 256, EVERYTHING_DUE = 1 << 30 };

static const int64_t idle_ns = 5LL * NS_PER_SECOND;

typedef struct {
    size_t mirror;
    const Replica *replica;
} Job;

[[nodiscard]] static Error replicate(Engine *e, Arena *scratch, size_t mirror, const Replica *replica, bool *source_missing, Err *err);

static void record_failure(Engine *e, const Replica *replica, bool source_missing, const Err *cause) {
    int attempts = replica->attempts + 1;
    /* When neither the primary nor the local folder has the content, retrying cannot help. */
    if (source_missing) attempts = MAX_ATTEMPTS;
    int64_t retry_at = clock_wall_ns() + retry_delay_ns(attempts);
    LogAttr attrs[] = {log_string("store", replica->store), log_string("path", replica->path), log_int("attempt", attempts), log_err(cause)};
    log_warn(e->log, "mirror copy failed", countof(attrs), attrs);
    Err err;
    if (index_mark_failed(e->index, replica->path, replica->store, S(cause->msg), attempts, retry_at, &err) != ERR_OK) {
        LogAttr failed[] = {log_err(&err)};
        log_error(e->log, "record failure", countof(failed), failed);
    }
}

static void replicate_job(Engine *e, void *item) {
    Job *job = item;
    Arena *scratch = arena_create(SCRATCH_BLOCK);
    Err err;
    bool source_missing = false;
    if (replicate(e, scratch, job->mirror, job->replica, &source_missing, &err) != ERR_OK && !cancel_requested(e->cancel)) record_failure(e, job->replica, source_missing, &err);
    arena_destroy(scratch);
}

/* mirror_batch handles up to one batch of due replicas with the store's workers and stores how many it handled in *handled. */
[[nodiscard]] static Error mirror_batch(Engine *e, size_t mirror, size_t *handled, Err *err) {
    Arena *arena = arena_create(SCRATCH_BLOCK);
    ReplicaList due;
    *handled = 0;
    Error result = index_due(e->index, arena, e->mirror_names.items[mirror], MIRROR_BATCH, &due, err);
    if (result == ERR_OK) {
        Job *jobs = arena_push(arena, arena_size_mul(due.count + 1, sizeof *jobs));
        void **items = arena_push(arena, arena_size_mul(due.count + 1, sizeof *items));
        for (size_t i = 0; i < due.count; i++) {
            jobs[i] = (Job){mirror, &due.items[i]};
            items[i] = &jobs[i];
        }
        engine_parallel(e, e->mirror_pools[mirror], arena, items, due.count, replicate_job);
        *handled = due.count;
    }
    arena_destroy(arena);
    return result;
}

void mirror_loop(Engine *e, size_t mirror) {
    while (!engine_done(e)) {
        Err err;
        size_t handled;
        if (mirror_batch(e, mirror, &handled, &err) != ERR_OK && !engine_done(e)) {
            LogAttr attrs[] = {log_string("store", e->mirror_names.items[mirror]), log_err(&err)};
            log_error(e->log, "mirror", countof(attrs), attrs);
        }
        if (handled > 0) continue;
        cancel_lock(e->cancel);
        int64_t deadline = clock_monotonic_ns() + idle_ns;
        while (!engine_done(e) && !e->mirror_woken[mirror] && clock_monotonic_ns() < deadline) cancel_wait(e->cancel, deadline);
        e->mirror_woken[mirror] = false;
        cancel_unlock(e->cancel);
    }
}

Error mirror_drain(Engine *e, size_t mirror, Err *err) {
    for (;;) {
        size_t handled;
        Error result = mirror_batch(e, mirror, &handled, err);
        if (result != ERR_OK || handled == 0) return result;
    }
}

/* source opens the content a mirror should receive: the primary's copy, or the local file when the primary lacks it but the file is unchanged. */
[[nodiscard]] static Error source(Engine *e, Arena *scratch, const IndexFile *file, Meta *meta, int *fd, bool *source_missing, Err *err) {
    Object object;
    Err inner;
    Error result = store_get(e->primary, e->cancel, scratch, file->path, &object, fd, &inner);
    *meta = (Meta){.sha256 = file->sha256, .mtime_ns = file->mtime_ns};
    if (result == ERR_OK) {
        if (object.sha256.len > 0) meta->sha256 = object.sha256;
        return ERR_OK;
    }
    if (result != ERR_NOT_FOUND) return err_set(err, result, "%s", inner.msg);
    LocalFile local;
    if (engine_local(e, scratch, file->path, file, &local, &inner) != ERR_OK || !local.exists || local.changed) {
        *source_missing = true;
        return err_set(err, ERR_NOT_FOUND, "source missing on primary and locally");
    }
    String abs = engine_abs(scratch, e, file->path);
    result = file_open_read(abs, fd, err);
    if (result == ERR_NOT_FOUND) result = err_set(err, result, "%.*s: no such file", (int)abs.len, abs.data);
    return result;
}

/* remove_copy deletes a tombstoned file from the mirror, when deletes travel, and forgets the replica. */
[[nodiscard]] static Error remove_copy(Engine *e, Arena *scratch, size_t mirror, String path, Err *err) {
    String name = e->mirror_names.items[mirror];
    LogAttr attrs[] = {log_string("path", path), log_string("store", name)};
    if (e->dry_run) {
        log_info(e->log, "would delete", countof(attrs), attrs);
        return ERR_OK;
    }
    if (e->config->sync.delete_remote) {
        Err inner;
        Error result = store_remove(e->mirrors[mirror], e->cancel, scratch, path, &inner);
        if (result != ERR_OK) return err_set(err, result, "%s", inner.msg);
        log_info(e->log, "deleted", countof(attrs), attrs);
    }
    return index_drop_replica(e->index, path, name, err);
}

/* replicate makes a mirror match the index for one path: delete a tombstoned file, or copy the current content from the primary and verify it. */
[[nodiscard]] static Error replicate(Engine *e, Arena *scratch, size_t mirror, const Replica *replica, bool *source_missing, Err *err) {
    String name = e->mirror_names.items[mirror];
    Store *store = e->mirrors[mirror];
    IndexFile file;
    bool found;
    Error result = index_file(e->index, scratch, replica->path, &file, &found, err);
    if (result != ERR_OK) return result;
    if (!found) return index_drop_replica(e->index, replica->path, name, err);
    if (file.deleted) return remove_copy(e, scratch, mirror, replica->path, err);

    Object head;
    Err inner;
    bool verified;
    if (store_head(store, e->cancel, scratch, replica->path, &head, &inner) == ERR_OK && head.size == file.size && str_equal(head.sha256, file.sha256))
        return index_mark_verified(e->index, replica->path, name, head.etag, file.sha256, &verified, err);
    LogAttr attrs[] = {log_string("path", replica->path), log_string("from", e->primary_name), log_string("to", name)};
    if (e->dry_run) {
        log_info(e->log, "would copy", countof(attrs), attrs);
        return ERR_OK;
    }

    Meta meta;
    int body;
    result = source(e, scratch, &file, &meta, &body, source_missing, err);
    if (result != ERR_OK) return result;
    String etag;
    result = store_put(store, e->cancel, scratch, replica->path, body, file.size, meta, &etag, &inner);
    file_close(body);
    if (result != ERR_OK) return err_set(err, result, "%s", inner.msg);
    result = store_head(store, e->cancel, scratch, replica->path, &head, &inner);
    if (result != ERR_OK) return err_set(err, result, "verify: %s", inner.msg);
    if (!str_equal(head.sha256, meta.sha256))
        return err_set(err, ERR_NETWORK, "verify: stored sha256 %.*s, expected %.*s", (int)head.sha256.len, head.sha256.data, (int)meta.sha256.len, meta.sha256.data);
    result = index_mark_verified(e->index, replica->path, name, head.etag, meta.sha256, &verified, err);
    if (result == ERR_OK && verified) {
        LogAttr copied[] = {log_string("path", replica->path), log_string("from", e->primary_name), log_string("to", name), log_int("bytes", head.size)};
        log_info(e->log, "copied", countof(copied), copied);
    }
    return result;
}

/* adopt marks copies already on the mirror verified when they hold the indexed content, and counts them in *adopted. */
[[nodiscard]] static Error adopt(Engine *e, Arena *arena, size_t mirror, const StrMap *present, int64_t *adopted, Err *err) {
    String name = e->mirror_names.items[mirror];
    ReplicaList due;
    Error result = index_due(e->index, arena, name, EVERYTHING_DUE, &due, err);
    for (size_t i = 0; result == ERR_OK && i < due.count; i++) {
        String path = due.items[i].path;
        const Object *object = strmap_get(present, path);
        if (!object) continue;
        IndexFile file;
        bool found;
        result = index_file(e->index, arena, path, &file, &found, err);
        if (result != ERR_OK || !found || file.deleted || file.size != object->size) continue;
        Object head;
        if (store_head(e->mirrors[mirror], e->cancel, arena, path, &head, nullptr) != ERR_OK || !str_equal(head.sha256, file.sha256)) continue;
        bool verified;
        result = index_mark_verified(e->index, path, name, head.etag, file.sha256, &verified, err);
        if (result == ERR_OK && verified) (*adopted)++;
    }
    return result;
}

/*
 * mirror_reconcile lists a mirror and corrects replica rows: copies already
 * there, for example from rclone or bucket replication, become verified,
 * and verified copies that disappeared go back to pending.
 */
Error mirror_reconcile(Engine *e, size_t mirror, Err *err) {
    String name = e->mirror_names.items[mirror];
    Arena *arena = arena_create(SCRATCH_BLOCK);
    ObjectList objects;
    Err inner;
    Error result = store_list(e->mirrors[mirror], e->cancel, arena, &objects, &inner);
    if (result != ERR_OK) {
        result = err_set(err, result, "%s", inner.msg);
        arena_destroy(arena);
        return result;
    }
    StrMap *present = strmap_create(arena, objects.count);
    for (size_t i = 0; i < objects.count; i++) strmap_put(present, objects.items[i].key, &objects.items[i]);
    ReplicaList rows;
    result = index_replicas_on(e->index, arena, name, &rows, err);
    for (size_t i = 0; result == ERR_OK && i < rows.count; i++)
        if (rows.items[i].state == REPLICA_VERIFIED && !strmap_has(present, rows.items[i].path)) result = index_mark_pending(e->index, rows.items[i].path, name, err);
    int64_t added, adopted = 0;
    if (result == ERR_OK) result = index_backfill(e->index, name, &added, err);
    if (result == ERR_OK) result = adopt(e, arena, mirror, present, &adopted, err);
    if (adopted > 0) {
        LogAttr attrs[] = {log_string("store", name), log_int("files", adopted)};
        log_info(e->log, "adopted existing copies", countof(attrs), attrs);
    }
    arena_destroy(arena);
    return result;
}
