#include "engine.h"

#include "content.h"
#include "engine_internal.h"
#include "mc/platform/platform.h"
#include "mc/platform/watch.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"

/* LISTING_BLOCK sizes the arenas that hold a whole store listing or folder walk. */
enum { LISTING_BLOCK = 256 * 1024, WATCH_POLL_MS = 200 };

static void on_debounced(void *context, String key) {
    Engine *e = context;
    pathqueue_push(&e->queue, e->cancel, key);
}

static int store_workers(const Engine *e, String name) { return config_store(e->config, name)->workers; }

Error engine_create(Arena *arena, const Config *config, Index *index, const StoreSet *stores, EngineOptions options, Engine **out, Err *err) {
    *out = nullptr;
    Engine *e = arena_push(arena, sizeof *e);
    e->config = config;
    e->root = path_clean(arena, config->sync.root);
    e->index = index;
    e->log = options.log ? options.log : (e->own_log = logger_create(LOG_INFO, LOG_TEXT, stderr));
    e->dry_run = options.dry_run;
    e->host = host_short_name(arena);
    e->ignore = ignore_create(arena, config->sync.ignore);
    e->primary_name = config_primary(config);
    e->primary = storeset_get(stores, e->primary_name);
    e->mirror_names = config_mirrors(arena, config);
    size_t mirrors = e->mirror_names.count;
    e->mirrors = arena_push(arena, arena_size_mul(mirrors + 1, sizeof *e->mirrors));
    e->mirror_pools = arena_push(arena, arena_size_mul(mirrors + 1, sizeof *e->mirror_pools));
    e->mirror_woken = arena_push(arena, arena_size_mul(mirrors + 1, sizeof *e->mirror_woken));
    for (size_t i = 0; i < mirrors; i++) e->mirrors[i] = storeset_get(stores, e->mirror_names.items[i]);
    pathqueue_init(&e->queue);
    pathlocks_init(&e->locks);
    atomic_init(&e->stopping, false);
    Error result = debounce_create(config->sync.debounce_ns, on_debounced, e, &e->debouncer, err);
    if (result == ERR_OK && e->primary) result = threadpool_create((size_t)store_workers(e, e->primary_name), &e->primary_pool, err);
    for (size_t i = 0; result == ERR_OK && i < mirrors; i++) result = threadpool_create((size_t)store_workers(e, e->mirror_names.items[i]), &e->mirror_pools[i], err);
    if (result != ERR_OK) {
        engine_destroy(e);
        return result;
    }
    *out = e;
    return ERR_OK;
}

void engine_destroy(Engine *e) {
    if (!e) return;
    if (e->debouncer) debounce_destroy(e->debouncer);
    if (e->primary_pool) threadpool_destroy(e->primary_pool);
    for (size_t i = 0; i < e->mirror_names.count; i++)
        if (e->mirror_pools[i]) threadpool_destroy(e->mirror_pools[i]);
    pathqueue_destroy(&e->queue);
    pathlocks_destroy(&e->locks);
    if (e->own_log) logger_destroy(e->own_log);
}

String engine_abs(Arena *arena, const Engine *e, String rel) { return path_join(arena, e->root, rel); }

int64_t retry_delay_ns(int attempts) { return (int64_t)(1 << attempts) * 10 * NS_PER_SECOND; }

void engine_wake_mirrors(Engine *e) {
    cancel_lock(e->cancel);
    for (size_t i = 0; i < e->mirror_names.count; i++) e->mirror_woken[i] = true;
    cancel_notify_locked(e->cancel);
    cancel_unlock(e->cancel);
}

/* ---- worker pools ---- */

typedef struct {
    Engine *e;
    void **items;
    void (*run)(Engine *, void *);
} Parallel;

static void run_chunk(void *context, size_t low, size_t high, size_t chunk) {
    Parallel *p = context;
    for (size_t i = low; i < high; i++) p->run(p->e, p->items[i]);
}

void engine_parallel(Engine *e, ThreadPool *pool, Arena *scratch, void **items, size_t count, void (*run)(Engine *, void *)) {
    if (count == 0) return;
    Parallel p = {e, items, run};
    /* One item per task, so a few large files do not hold up the rest of a chunk. */
    threadpool_run_chunks(pool, scratch, (ParallelPlan){.count = count, .chunk_size = 1, .chunks = count}, run_chunk, &p);
}

/* ---- local files ---- */

Error engine_local(Engine *e, Arena *scratch, String rel, const IndexFile *file, LocalFile *out, Err *err) {
    String abs = engine_abs(scratch, e, rel);
    FileInfo info;
    *out = (LocalFile){0};
    Error result = file_info(abs, &info, err);
    if (result != ERR_OK || !info.exists) return result;
    out->exists = true;
    out->changed = true;
    out->size = info.size;
    out->mtime_ns = info.mtime_ns;
    if (file && file->size == out->size && file->mtime_ns == out->mtime_ns) {
        out->changed = false;
    } else if (file) {
        /* Size and time decide first; content is hashed only when they differ. */
        String sum;
        result = content_hash_file(scratch, abs, &sum, err);
        if (result == ERR_OK) out->changed = !str_equal(sum, file->sha256);
    }
    return result;
}

/* ---- pushing to the primary ---- */

/*
 * upload sends rel, whose content hashed to sum when it looked like info, to
 * the primary. If the file changed meanwhile, nothing is recorded and the
 * path is queued again, so the index never describes a version other than
 * the one the primary holds.
 */
[[nodiscard]] static Error upload(Engine *e, Arena *scratch, String rel, String sum, const FileInfo *info, Err *err) {
    if (e->dry_run) {
        LogAttr attrs[] = {log_string("path", rel), log_string("store", e->primary_name)};
        log_info(e->log, "would upload", countof(attrs), attrs);
        return ERR_OK;
    }
    String abs = engine_abs(scratch, e, rel);
    int fd;
    Error result = file_open_read(abs, &fd, err);
    if (result == ERR_NOT_FOUND) return err_set(err, result, "%.*s: no such file", (int)abs.len, abs.data);
    if (result != ERR_OK) return result;
    Meta meta = {.sha256 = sum, .mtime_ns = info->mtime_ns};
    String etag;
    Err inner;
    result = store_put(e->primary, e->cancel, scratch, rel, fd, info->size, meta, &etag, &inner);
    file_close(fd);
    if (result != ERR_OK) return err_set(err, result, "upload to %.*s: %s", (int)e->primary_name.len, e->primary_name.data, inner.msg);
    FileInfo after;
    bool unchanged = file_info(abs, &after, nullptr) == ERR_OK && after.exists && after.size == info->size && after.mtime_ns == info->mtime_ns;
    if (!unchanged) {
        LogAttr attrs[] = {log_string("path", rel)};
        log_info(e->log, "changed during upload; syncing again", countof(attrs), attrs);
        pathqueue_push(&e->queue, e->cancel, rel);
        return ERR_OK;
    }
    IndexFile record = {.path = rel, .size = info->size, .mtime_ns = info->mtime_ns, .sha256 = sum};
    result = index_record_synced(e->index, &record, e->primary_name, etag, e->mirror_names, err);
    if (result != ERR_OK) return result;
    LogAttr attrs[] = {log_string("path", rel), log_string("store", e->primary_name), log_int("bytes", info->size)};
    log_info(e->log, "uploaded", countof(attrs), attrs);
    engine_wake_mirrors(e);
    return ERR_OK;
}

/* push_delete handles a path that is gone locally: a deleted file, or a directory that was removed or renamed with everything in it. */
[[nodiscard]] static Error push_delete(Engine *e, Arena *scratch, String rel, Err *err) {
    IndexFile file;
    bool found;
    Error result = index_file(e->index, scratch, rel, &file, &found, err);
    if (result != ERR_OK) return result;
    if (!found) {
        IndexFileList inside;
        result = index_clear_upload_failure(e->index, rel, err);
        if (result == ERR_OK) result = index_files_under(e->index, scratch, rel, &inside, err);
        for (size_t i = 0; result == ERR_OK && i < inside.count; i++) pathqueue_push(&e->queue, e->cancel, inside.items[i].path);
        return result;
    }
    if (file.deleted) return ERR_OK;
    LogAttr attrs[] = {log_string("path", rel), log_string("store", e->primary_name)};
    if (e->dry_run) {
        log_info(e->log, "would delete", countof(attrs), attrs);
        return ERR_OK;
    }
    if (e->config->sync.delete_remote) {
        Err inner;
        result = store_remove(e->primary, e->cancel, scratch, rel, &inner);
        if (result != ERR_OK) return err_set(err, result, "delete from %.*s: %s", (int)e->primary_name.len, e->primary_name.data, inner.msg);
        log_info(e->log, "deleted", countof(attrs), attrs);
    }
    result = index_tombstone(e->index, rel, e->primary_name, !e->config->sync.delete_remote, err);
    if (result == ERR_OK) engine_wake_mirrors(e);
    return result;
}

/* push makes the primary match the local file at rel. */
[[nodiscard]] static Error push(Engine *e, Arena *scratch, String rel, Err *err) {
    String abs = engine_abs(scratch, e, rel);
    FileInfo info;
    Error result = file_info(abs, &info, err);
    if (result != ERR_OK) return result;
    if (!info.exists) return push_delete(e, scratch, rel, err);
    if (!info.is_regular) return ERR_OK;
    IndexFile file;
    bool found;
    result = index_file(e->index, scratch, rel, &file, &found, err);
    bool primary_current = false;
    if (result == ERR_OK && found && !file.deleted) {
        Replica replica;
        bool has_replica;
        result = index_replica(e->index, scratch, rel, e->primary_name, &replica, &has_replica, err);
        primary_current = result == ERR_OK && has_replica && replica.state == REPLICA_VERIFIED;
    }
    if (result != ERR_OK || (primary_current && file.size == info.size && file.mtime_ns == info.mtime_ns)) return result;
    String sum;
    result = content_hash_file(scratch, abs, &sum, err);
    if (result != ERR_OK) return result;
    if (primary_current && str_equal(file.sha256, sum)) return index_touch(e->index, rel, info.mtime_ns, err);
    return upload(e, scratch, rel, sum, &info, err);
}

static void record_upload_failure(Engine *e, String rel, const Err *cause) {
    Arena *scratch = arena_create(SCRATCH_BLOCK);
    int attempts = 1;
    UploadFailure previous;
    bool found;
    Err err;
    if (index_upload_failure(e->index, scratch, rel, &previous, &found, &err) == ERR_OK && found) attempts = previous.attempts + 1;
    if (attempts >= MAX_ATTEMPTS) {
        LogAttr attrs[] = {log_string("path", rel), log_int("attempts", attempts), log_err(cause)};
        log_error(e->log, "sync failed; giving up until `dbox retry` or the file changes", countof(attrs), attrs);
    } else {
        LogAttr attrs[] = {log_string("path", rel), log_int("attempt", attempts), log_duration("in", retry_delay_ns(attempts)), log_err(cause)};
        log_error(e->log, "sync failed; will retry", countof(attrs), attrs);
    }
    if (index_mark_upload_failed(e->index, rel, S(cause->msg), attempts, clock_wall_ns() + retry_delay_ns(attempts), &err) != ERR_OK) {
        LogAttr attrs[] = {log_string("path", rel), log_err(&err)};
        log_error(e->log, "record failure", countof(attrs), attrs);
    }
    arena_destroy(scratch);
}

/* process syncs one path under its lock, and records a failure so that retry_due_uploads brings the path back after a backoff. */
static void process(Engine *e, Arena *scratch, String rel) {
    if (!pathlocks_acquire(&e->locks, rel)) return;
    Err err;
    Error result = push(e, scratch, rel, &err);
    if (pathlocks_release(&e->locks, rel)) pathqueue_push(&e->queue, e->cancel, rel);
    if (result != ERR_OK && !cancel_requested(e->cancel)) record_upload_failure(e, rel, &err);
}

static void process_item(Engine *e, void *item) {
    Arena *scratch = arena_create(SCRATCH_BLOCK);
    process(e, scratch, *(String *)item);
    arena_destroy(scratch);
}

static void *primary_worker(void *argument) {
    Engine *e = argument;
    Arena *scratch = arena_create(SCRATCH_BLOCK);
    String path;
    while (pathqueue_pop(&e->queue, e->cancel, &e->stopping, scratch, &path)) {
        process(e, scratch, path);
        arena_reset(scratch);
    }
    arena_destroy(scratch);
    return nullptr;
}

/* retry_due_uploads queues every failed upload whose backoff has passed. */
[[nodiscard]] static Error retry_due_uploads(Engine *e, Err *err) {
    Arena *scratch = arena_create(SCRATCH_BLOCK);
    StringList due;
    Error result = index_due_uploads(e->index, scratch, clock_wall_ns(), &due, err);
    for (size_t i = 0; result == ERR_OK && i < due.count; i++) pathqueue_push(&e->queue, e->cancel, due.items[i]);
    arena_destroy(scratch);
    return result;
}

/* walk collects the relative paths of every regular file under dir that is not ignored. */
[[nodiscard]] static Error walk(Engine *e, Arena *arena, String dir, StringList *paths, Err *err) {
    StringList names = {0};
    Error result = dir_list(arena, dir, &names, err);
    if (result == ERR_NOT_FOUND) result = err_set(err, result, "%.*s: no such directory", (int)dir.len, dir.data);
    for (size_t i = 0; result == ERR_OK && i < names.count; i++) {
        String path = path_join(arena, dir, names.items[i]);
        String rel;
        FileInfo info;
        if (!path_relative(e->root, path, &rel) || file_info(path, &info, nullptr) != ERR_OK || !info.exists || ignore_match(&e->ignore, rel, info.is_dir)) continue;
        if (info.is_dir) result = walk(e, arena, path, paths, err);
        else if (info.is_regular) strlist_push(arena, paths, rel);
    }
    return result;
}

/* push_all syncs every local file and every indexed file that is gone locally. */
[[nodiscard]] static Error push_all(Engine *e, Err *err) {
    Arena *arena = arena_create(LISTING_BLOCK);
    StringList paths = {0};
    IndexFileList files;
    Error result = walk(e, arena, e->root, &paths, err);
    if (result == ERR_OK) result = index_files(e->index, arena, &files, err);
    if (result == ERR_OK) {
        for (size_t i = 0; i < files.count; i++)
            if (!files.items[i].deleted) strlist_push(arena, &paths, files.items[i].path);
        StrMap *seen = strmap_create(arena, paths.count);
        void **unique = arena_push(arena, arena_size_mul(paths.count + 1, sizeof *unique));
        size_t count = 0;
        for (size_t i = 0; i < paths.count; i++)
            if (!strmap_put(seen, paths.items[i], &paths.items[i])) unique[count++] = &paths.items[i];
        engine_parallel(e, e->primary_pool, arena, unique, count, process_item);
    }
    arena_destroy(arena);
    return result;
}

/* ---- pulling from the primary ---- */

/*
 * download_to fetches key into a temp file and renames it to rel. With
 * record, the index is updated before the rename so the watcher event that
 * follows finds nothing to upload.
 */
[[nodiscard]] static Error download_to(Engine *e, Arena *scratch, String key, String rel, bool record, Err *err) {
    Object object;
    int body;
    Err inner;
    Error result = store_get(e->primary, e->cancel, scratch, key, &object, &body, &inner);
    if (result != ERR_OK) return err_set(err, result, "%s", inner.msg);
    String tmp_dir = path_join(scratch, config_state_dir(scratch, e->config), S("tmp"));
    String abs = engine_abs(scratch, e, rel);
    String tmp = S("");
    int out = -1;
    int64_t copied;
    Sha256 hash;
    sha256_init(&hash);
    FileInfo info;
    if ((result = dir_create_all(tmp_dir, 0755, err)) != ERR_OK) goto done;
    if ((result = file_create_unique(scratch, path_join(scratch, tmp_dir, S("download-XXXXXX")), &tmp, &out, err)) != ERR_OK) goto done;
    if ((result = content_copy(body, out, &hash, &copied, err)) != ERR_OK) goto done;
    file_close(out);
    out = -1;
    if (object.mtime_ns != 0 && (result = file_set_mtime(tmp, object.mtime_ns, err)) != ERR_OK) goto done;
    if ((result = file_info_follow(tmp, &info, err)) != ERR_OK) goto done;
    if (!info.exists) {
        result = err_set(err, ERR_IO, "%.*s: vanished", (int)tmp.len, tmp.data);
        goto done;
    }
    if (record) {
        IndexFile file = {.path = rel, .size = info.size, .mtime_ns = info.mtime_ns, .sha256 = content_digest(scratch, &hash)};
        if ((result = index_record_synced(e->index, &file, e->primary_name, object.etag, e->mirror_names, err)) != ERR_OK) goto done;
    }
    if ((result = dir_create_all(path_dir(abs), 0755, err)) != ERR_OK) goto done;
    if ((result = file_rename(tmp, abs, err)) != ERR_OK) goto done;
    LogAttr attrs[] = {log_string("path", rel), log_string("store", e->primary_name), log_int("bytes", info.size)};
    log_info(e->log, "downloaded", countof(attrs), attrs);
done:
    if (out >= 0) file_close(out);
    if (result != ERR_OK && tmp.len > 0) (void)file_remove(tmp, nullptr);
    file_close(body);
    return result;
}

[[nodiscard]] static Error download(Engine *e, Arena *scratch, String key, Err *err) {
    if (e->dry_run) {
        LogAttr attrs[] = {log_string("path", key), log_string("store", e->primary_name)};
        log_info(e->log, "would download", countof(attrs), attrs);
        return ERR_OK;
    }
    return download_to(e, scratch, key, key, true, err);
}

/* conflict_stamp is the UTC time without separators, 20240313T100000Z, so it sorts and fits in a file name. */
static String conflict_stamp(Arena *arena) {
    String rfc3339 = fmt_rfc3339(arena, clock_wall_ns(), 0, false);
    return str_replace_all(arena, str_replace_all(arena, rfc3339, S("-"), S("")), S(":"), S(""));
}

/* conflict keeps the local file and saves the remote version beside it. */
[[nodiscard]] static Error conflict(Engine *e, Arena *scratch, String rel, Err *err) {
    String ext = path_ext(rel);
    String stem = str_trim_suffix(rel, ext);
    String stamp = conflict_stamp(scratch);
    String copy = str_format(scratch, "%.*s.conflict-%.*s-%.*s%.*s", (int)stem.len, stem.data, (int)e->host.len, e->host.data, (int)stamp.len, stamp.data, (int)ext.len, ext.data);
    LogAttr attrs[] = {log_string("path", rel), log_string("copy", copy)};
    if (e->dry_run) {
        log_info(e->log, "would save conflict copy", countof(attrs), attrs);
        return ERR_OK;
    }
    Error result = download_to(e, scratch, rel, copy, false, err);
    if (result != ERR_OK) return result;
    log_warn(e->log, "conflict: kept local version, saved remote beside it", countof(attrs), attrs);
    pathqueue_push(&e->queue, e->cancel, copy);
    String abs = engine_abs(scratch, e, rel);
    FileInfo info;
    String sum;
    result = file_info(abs, &info, err);
    if (result == ERR_OK && !info.exists) result = err_set(err, ERR_NOT_FOUND, "%.*s: no such file", (int)abs.len, abs.data);
    if (result == ERR_OK) result = content_hash_file(scratch, abs, &sum, err);
    if (result == ERR_OK) result = upload(e, scratch, rel, sum, &info, err);
    return result;
}

/* adopt_or_conflict handles a file that exists both locally and remotely but was never synced from here, such as the first run on a second machine. */
[[nodiscard]] static Error adopt_or_conflict(Engine *e, Arena *scratch, const Object *object, const LocalFile *local, Err *err) {
    Object head;
    Err inner;
    Error result = store_head(e->primary, e->cancel, scratch, object->key, &head, &inner);
    if (result != ERR_OK) return err_set(err, result, "%s", inner.msg);
    String sum;
    result = content_hash_file(scratch, engine_abs(scratch, e, object->key), &sum, err);
    if (result != ERR_OK) return result;
    if (!str_equal(head.sha256, sum)) return conflict(e, scratch, object->key, err);
    if (e->dry_run) {
        LogAttr attrs[] = {log_string("path", object->key)};
        log_info(e->log, "would adopt", countof(attrs), attrs);
        return ERR_OK;
    }
    IndexFile record = {.path = object->key, .size = local->size, .mtime_ns = local->mtime_ns, .sha256 = sum};
    return index_record_synced(e->index, &record, e->primary_name, head.etag, e->mirror_names, err);
}

[[nodiscard]] static Error pull(Engine *e, Arena *scratch, const Object *object, Err *err) {
    Replica replica;
    bool has_replica;
    Error result = index_replica(e->index, scratch, object->key, e->primary_name, &replica, &has_replica, err);
    if (result != ERR_OK) return result;
    if (has_replica && replica.state == REPLICA_VERIFIED && str_equal(replica.etag, object->etag)) return ERR_OK;
    IndexFile file;
    bool found;
    result = index_file(e->index, scratch, object->key, &file, &found, err);
    if (result != ERR_OK) return result;
    if (found && file.deleted) found = false;
    LocalFile local;
    result = engine_local(e, scratch, object->key, found ? &file : nullptr, &local, err);
    if (result != ERR_OK) return result;
    if (!local.exists) return download(e, scratch, object->key, err);
    if (!found) return adopt_or_conflict(e, scratch, object, &local, err);
    if (!local.changed) return download(e, scratch, object->key, err);
    return conflict(e, scratch, object->key, err);
}

/*
 * remote_gone handles an indexed file that is missing from a listing of the
 * primary. The listing may predate an upload that finished while the poll
 * was still pulling, so the miss is confirmed before anything is deleted.
 */
[[nodiscard]] static Error remote_gone(Engine *e, Arena *scratch, const IndexFile *file, Err *err) {
    Object head;
    Err inner;
    Error result = store_head(e->primary, e->cancel, scratch, file->path, &head, &inner);
    if (result == ERR_OK) return ERR_OK;
    if (result != ERR_NOT_FOUND) return err_set(err, result, "%s", inner.msg);
    Replica replica;
    bool has_replica;
    result = index_replica(e->index, scratch, file->path, e->primary_name, &replica, &has_replica, err);
    if (result != ERR_OK) return result;
    if (!has_replica || replica.state != REPLICA_VERIFIED) {
        /* Never reached this primary, for example right after a promotion. */
        pathqueue_push(&e->queue, e->cancel, file->path);
        return ERR_OK;
    }
    LocalFile local;
    result = engine_local(e, scratch, file->path, file, &local, err);
    if (result != ERR_OK) return result;
    if (!local.exists) return index_tombstone(e->index, file->path, e->primary_name, false, err);
    if (local.changed) {
        pathqueue_push(&e->queue, e->cancel, file->path);
        return ERR_OK;
    }
    if (!e->config->sync.delete_local) {
        result = index_drop_replica(e->index, file->path, e->primary_name, err);
        if (result == ERR_OK) pathqueue_push(&e->queue, e->cancel, file->path);
        return result;
    }
    LogAttr attrs[] = {log_string("path", file->path)};
    if (e->dry_run) {
        log_info(e->log, "would delete local", countof(attrs), attrs);
        return ERR_OK;
    }
    result = file_remove(engine_abs(scratch, e, file->path), err);
    if (result != ERR_OK && result != ERR_NOT_FOUND) return result;
    log_info(e->log, "deleted locally, gone from primary", countof(attrs), attrs);
    result = index_tombstone(e->index, file->path, e->primary_name, false, err);
    if (result == ERR_OK) engine_wake_mirrors(e);
    return result;
}

typedef struct {
    String rel;
    Error (*run)(Engine *, Arena *, const void *, Err *);
    const void *argument;
} Locked;

static void with_lock(Engine *e, void *item) {
    Locked *job = item;
    if (!pathlocks_acquire(&e->locks, job->rel)) return;
    Arena *scratch = arena_create(SCRATCH_BLOCK);
    Err err;
    Error result = job->run(e, scratch, job->argument, &err);
    if (pathlocks_release(&e->locks, job->rel)) pathqueue_push(&e->queue, e->cancel, job->rel);
    if (result != ERR_OK) {
        LogAttr attrs[] = {log_string("path", job->rel), log_err(&err)};
        log_error(e->log, "pull", countof(attrs), attrs);
    }
    arena_destroy(scratch);
}

[[nodiscard]] static Error pull_object(Engine *e, Arena *scratch, const void *object, Err *err) { return pull(e, scratch, object, err); }
[[nodiscard]] static Error gone_file(Engine *e, Arena *scratch, const void *file, Err *err) { return remote_gone(e, scratch, file, err); }

/* poll_primary pulls changes from the primary. See the sync table in docs/design.md. */
[[nodiscard]] static Error poll_primary(Engine *e, Err *err) {
    Arena *arena = arena_create(LISTING_BLOCK);
    ObjectList objects;
    Err inner;
    Error result = store_list(e->primary, e->cancel, arena, &objects, &inner);
    if (result != ERR_OK) {
        result = err_set(err, result, "%s", inner.msg);
        arena_destroy(arena);
        return result;
    }
    StrMap *remote = strmap_create(arena, objects.count);
    Locked *jobs = arena_push(arena, arena_size_mul(objects.count + 1, sizeof *jobs));
    void **items = arena_push(arena, arena_size_mul(objects.count + 1, sizeof *items));
    size_t count = 0;
    for (size_t i = 0; i < objects.count; i++) {
        const Object *object = &objects.items[i];
        if (ignore_match(&e->ignore, object->key, false)) continue;
        strmap_put(remote, object->key, nullptr);
        jobs[count] = (Locked){object->key, pull_object, object};
        items[count] = &jobs[count];
        count++;
    }
    engine_parallel(e, e->primary_pool, arena, items, count, with_lock);
    IndexFileList files;
    result = index_files(e->index, arena, &files, err);
    for (size_t i = 0; result == ERR_OK && i < files.count; i++) {
        const IndexFile *file = &files.items[i];
        if (file->deleted || strmap_has(remote, file->path) || ignore_match(&e->ignore, file->path, false)) continue;
        Locked job = {file->path, gone_file, file};
        with_lock(e, &job);
    }
    arena_destroy(arena);
    return result;
}

[[nodiscard]] static Error backfill(Engine *e, Err *err) {
    for (size_t i = 0; i < e->mirror_names.count; i++) {
        int64_t added;
        Error result = index_backfill(e->index, e->mirror_names.items[i], &added, err);
        if (result != ERR_OK) return result;
        if (added > 0) {
            LogAttr attrs[] = {log_string("store", e->mirror_names.items[i]), log_int("files", added)};
            log_info(e->log, "backfill", countof(attrs), attrs);
        }
    }
    engine_wake_mirrors(e);
    return ERR_OK;
}

/* ---- the public entry points ---- */

Error engine_reconcile(Engine *e, Cancel *cancel, Err *err) {
    e->cancel = cancel;
    Error result = dir_create_all(e->root, 0755, err);
    if (result != ERR_OK) return result;
    Err inner;
    result = poll_primary(e, &inner);
    if (result != ERR_OK) return err_set(err, result, "pull from %.*s: %s", (int)e->primary_name.len, e->primary_name.data, inner.msg);
    result = push_all(e, err);
    if (result != ERR_OK) return result;
    for (size_t i = 0; i < e->mirror_names.count; i++) {
        if (mirror_reconcile(e, i, &inner) == ERR_OK) continue;
        LogAttr attrs[] = {log_string("store", e->mirror_names.items[i]), log_err(&inner)};
        log_error(e->log, "reconcile mirror", countof(attrs), attrs);
    }
    return backfill(e, err);
}

Error engine_once(Engine *e, Cancel *cancel, Err *err) {
    Error result = engine_reconcile(e, cancel, err);
    if (result != ERR_OK || e->dry_run) return result;
    for (size_t i = 0; i < e->mirror_names.count; i++) {
        Err inner;
        Error drained = mirror_drain(e, i, &inner);
        if (drained != ERR_OK && result == ERR_OK) result = err_set(err, drained, "%s", inner.msg);
    }
    return result;
}

Error engine_drain_mirror(Engine *e, Cancel *cancel, String name, Err *err) {
    e->cancel = cancel;
    for (size_t i = 0; i < e->mirror_names.count; i++)
        if (str_equal(e->mirror_names.items[i], name)) return mirror_drain(e, i, err);
    return err_set(err, ERR_NOT_FOUND, "%.*s is not a mirror", (int)name.len, name.data);
}

size_t engine_backlog(Engine *e) { return debounce_pending(e->debouncer) + pathqueue_count(&e->queue, e->cancel) + pathlocks_in_flight(&e->locks); }

/*
 * reconcile_until_done keeps retrying the startup reconcile, one pull
 * interval apart, until it succeeds or the run is cancelled. The daemon is
 * usually started by launchd or systemd before the network is up, and giving
 * up would only make the supervisor restart it into the same situation.
 */
static void reconcile_until_done(Engine *e) {
    for (;;) {
        Err err;
        if (engine_reconcile(e, e->cancel, &err) == ERR_OK || cancel_requested(e->cancel)) return;
        LogAttr attrs[] = {log_err(&err), log_duration("in", e->config->sync.pull_interval_ns)};
        log_error(e->log, "reconcile; retrying", countof(attrs), attrs);
        if (!cancel_sleep(e->cancel, e->config->sync.pull_interval_ns / NS_PER_MILLISECOND)) return;
    }
}

typedef struct {
    Engine *e;
    int64_t interval_ns;
    const char *name;
    Error (*run)(Engine *, Err *);
} Periodic;

static void *every(void *argument) {
    Periodic *p = argument;
    while (cancel_sleep(p->e->cancel, p->interval_ns / NS_PER_MILLISECOND) && !engine_done(p->e)) {
        Err err;
        if (p->run(p->e, &err) == ERR_OK || engine_done(p->e)) continue;
        LogAttr attrs[] = {log_err(&err)};
        log_error(p->e->log, p->name, countof(attrs), attrs);
    }
    return nullptr;
}

typedef struct {
    Engine *e;
    size_t mirror;
} MirrorArgument;

static void *mirror_main(void *argument) {
    MirrorArgument *m = argument;
    mirror_loop(m->e, m->mirror);
    return nullptr;
}

/* ---- watching the folder ---- */

static bool skip_ignored(void *context, String directory) {
    Engine *e = context;
    String rel;
    return path_relative(e->root, directory, &rel) && ignore_match(&e->ignore, rel, true);
}

/* report_tree triggers every file below dir, whose changes may have gone unreported. */
static void report_tree(Engine *e, Arena *arena, String dir) {
    StringList paths = {0};
    Err err;
    if (walk(e, arena, dir, &paths, &err) != ERR_OK) {
        LogAttr attrs[] = {log_string("path", dir), log_err(&err)};
        log_error(e->log, "rescan", countof(attrs), attrs);
    }
    for (size_t i = 0; i < paths.count; i++) debounce_trigger(e->debouncer, paths.items[i]);
}

/* changed handles one path the watcher names: a file to sync, or a directory whose contents may need a fresh look. */
static void changed(Engine *e, Arena *arena, WatchEvent event) {
    String rel;
    if (!path_relative(e->root, event.path, &rel)) {
        if (event.rescan) report_tree(e, arena, e->root);
        return;
    }
    FileInfo info;
    bool is_dir = file_info(event.path, &info, nullptr) == ERR_OK && info.exists && info.is_dir;
    if (ignore_match(&e->ignore, rel, is_dir)) return;
    if (!is_dir) debounce_trigger(e->debouncer, rel);
    else if (event.rescan) report_tree(e, arena, event.path);
}

typedef struct {
    Engine *e;
    Watch *watch;
} WatchArgument;

static void *watch_main(void *argument) {
    WatchArgument *w = argument;
    Engine *e = w->e;
    Arena *arena = arena_create(LISTING_BLOCK);
    while (!engine_done(e)) {
        WatchEventList events = {0};
        Err err;
        if (watch_read(w->watch, arena, WATCH_POLL_MS, 0, &events, &err) != ERR_OK) {
            LogAttr attrs[] = {log_err(&err)};
            log_error(e->log, "watch", countof(attrs), attrs);
            cancel_sleep(e->cancel, WATCH_POLL_MS);
        }
        for (size_t i = 0; i < events.count; i++) changed(e, arena, events.items[i]);
        arena_reset(arena);
    }
    arena_destroy(arena);
    return nullptr;
}

typedef struct {
    Arena *arena;
    Thread *items;
    size_t count, capacity;
} Threads;

[[nodiscard]] static Error start(Threads *threads, void *(*run)(void *), void *argument, Err *err) {
    threads->items = arena_grow(threads->arena, threads->items, &threads->capacity, threads->count, sizeof *threads->items);
    Error result = thread_start(&threads->items[threads->count], run, argument, err);
    if (result == ERR_OK) threads->count++;
    return result;
}

static void join_all(Threads *threads) {
    for (size_t i = 0; i < threads->count; i++) thread_join(&threads->items[i]);
}

static void log_watching(Engine *e, Arena *arena) {
    String names = str_join(arena, e->mirror_names, S(" "));
    String mirrors = str_format(arena, "[%.*s]", (int)names.len, names.data);
    LogAttr attrs[] = {log_string("root", e->root), log_string("primary", e->primary_name), log_string("mirrors", mirrors)};
    log_info(e->log, "watching", countof(attrs), attrs);
}

Error engine_run(Engine *e, Cancel *cancel, Err *err) {
    e->cancel = cancel;
    atomic_store(&e->stopping, false);
    Error result = dir_create_all(e->root, 0755, err);
    if (result != ERR_OK) return result;
    reconcile_until_done(e);
    if (cancel_requested(cancel)) return ERR_OK;

    Arena *arena = arena_create(SCRATCH_BLOCK);
    StringList roots = {0};
    strlist_push(arena, &roots, e->root);
    Watch *watch;
    result = watch_create(roots, skip_ignored, e, &watch, err);
    if (result != ERR_OK) {
        arena_destroy(arena);
        return result;
    }
    Threads threads = {.arena = arena};
    WatchArgument watch_argument = {e, watch};
    MirrorArgument *mirror_arguments = arena_push(arena, arena_size_mul(e->mirror_names.count + 1, sizeof *mirror_arguments));
    Periodic jobs[] = {
        {e, e->config->sync.pull_interval_ns, "poll", poll_primary},
        {e, e->config->sync.pull_interval_ns, "retry", retry_due_uploads},
        {e, e->config->sync.backfill_interval_ns, "backfill", backfill},
    };
    int workers = store_workers(e, e->primary_name);
    for (int i = 0; result == ERR_OK && i < workers; i++) result = start(&threads, primary_worker, e, err);
    for (size_t i = 0; result == ERR_OK && i < e->mirror_names.count; i++) {
        mirror_arguments[i] = (MirrorArgument){e, i};
        result = start(&threads, mirror_main, &mirror_arguments[i], err);
    }
    if (result == ERR_OK) result = start(&threads, watch_main, &watch_argument, err);
    for (size_t i = 0; result == ERR_OK && i < countof(jobs); i++) result = start(&threads, every, &jobs[i], err);

    if (result == ERR_OK) {
        log_watching(e, arena);
        cancel_lock(cancel);
        while (cancel_wait(cancel, 0)) {
        }
        cancel_unlock(cancel);
    } else {
        atomic_store(&e->stopping, true);
        cancel_notify(cancel);
    }
    join_all(&threads);
    watch_destroy(watch);
    arena_destroy(arena);
    return result;
}
