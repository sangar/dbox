#include "index.h"

#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "sqlite3.h"

struct Index {
    sqlite3 *db;
    Mutex mu; /* one operation or transaction at a time */
};

static const char schema[] =
    "CREATE TABLE IF NOT EXISTS files ("
    "  path      TEXT PRIMARY KEY,"
    "  size      INTEGER NOT NULL,"
    "  mtime_ns  INTEGER NOT NULL,"
    "  sha256    TEXT    NOT NULL,"
    "  synced_at INTEGER NOT NULL,"
    "  deleted   INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS replicas ("
    "  path            TEXT    NOT NULL REFERENCES files(path) ON DELETE CASCADE,"
    "  store           TEXT    NOT NULL,"
    "  state           TEXT    NOT NULL,"
    "  etag            TEXT,"
    "  verified_at     INTEGER,"
    "  last_error      TEXT,"
    "  attempts        INTEGER NOT NULL DEFAULT 0,"
    "  next_attempt_at INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (path, store)"
    ");"
    "CREATE INDEX IF NOT EXISTS replicas_by_store_state ON replicas (store, state);"
    "CREATE TABLE IF NOT EXISTS upload_failures ("
    "  path            TEXT PRIMARY KEY,"
    "  attempts        INTEGER NOT NULL,"
    "  next_attempt_at INTEGER NOT NULL,"
    "  last_error      TEXT    NOT NULL"
    ");";

static ReplicaState parse_state(String state) {
    if (str_equal(state, S("verified"))) return REPLICA_VERIFIED;
    if (str_equal(state, S("failed"))) return REPLICA_FAILED;
    return REPLICA_PENDING;
}

[[nodiscard]] static Error db_fail(Index *x, Err *err, const char *what) { return err_set(err, ERR_IO, "index: %s: %s", what, sqlite3_errmsg(x->db)); }

Error index_open(Arena *arena, String path, Index **out, Err *err) {
    *out = nullptr;
    Error e = dir_create_all(path_dir(path), 0755, err);
    if (e != ERR_OK) return e;
    Index *x = arena_push(arena, sizeof *x);
    const char *file = str_cstr(arena, path);
    if (sqlite3_open_v2(file, &x->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        e = err_set(err, ERR_IO, "%s: %s", file, x->db ? sqlite3_errmsg(x->db) : "cannot open");
        sqlite3_close(x->db);
        return e;
    }
    sqlite3_busy_timeout(x->db, 10000);
    const char *setup = "PRAGMA journal_mode = WAL; PRAGMA foreign_keys = ON;";
    char *msg = nullptr;
    if (sqlite3_exec(x->db, setup, nullptr, nullptr, &msg) != SQLITE_OK || sqlite3_exec(x->db, schema, nullptr, nullptr, &msg) != SQLITE_OK) {
        e = err_set(err, ERR_IO, "%s: %s", file, msg ? msg : sqlite3_errmsg(x->db));
        sqlite3_free(msg);
        sqlite3_close(x->db);
        return e;
    }
    mutex_init(&x->mu);
    *out = x;
    return ERR_OK;
}

void index_close(Index *x) {
    if (!x) return;
    sqlite3_close(x->db);
    mutex_destroy(&x->mu);
}

/* ---- statement helpers ---- */

/*
 * A Stmt binds parameters by position in the order given: each bind_*
 * call takes the next slot. Strings are bound transiently, so they only
 * need to live until the statement runs.
 */
typedef struct {
    Index *x;
    sqlite3_stmt *stmt;
    int next;
} Stmt;

[[nodiscard]] static Error prepare(Index *x, Stmt *s, const char *sql, Err *err) {
    s->x = x;
    s->next = 1;
    if (sqlite3_prepare_v2(x->db, sql, -1, &s->stmt, nullptr) != SQLITE_OK) {
        s->stmt = nullptr;
        return db_fail(x, err, "prepare");
    }
    return ERR_OK;
}

static void bind_text(Stmt *s, String v) { sqlite3_bind_text(s->stmt, s->next++, v.data ? v.data : "", (int)v.len, SQLITE_TRANSIENT); }
static void bind_int(Stmt *s, int64_t v) { sqlite3_bind_int64(s->stmt, s->next++, v); }

/* step_done runs a statement to completion and reports how many rows changed. */
[[nodiscard]] static Error step_done(Stmt *s, int64_t *changed, Err *err) {
    int rc = sqlite3_step(s->stmt);
    Error e = rc == SQLITE_DONE || rc == SQLITE_ROW ? ERR_OK : db_fail(s->x, err, "exec");
    if (e == ERR_OK && changed) *changed = sqlite3_changes(s->x->db);
    sqlite3_finalize(s->stmt);
    return e;
}

[[nodiscard]] static Error exec_sql(Index *x, const char *sql, Err *err) {
    char *msg = nullptr;
    if (sqlite3_exec(x->db, sql, nullptr, nullptr, &msg) == SQLITE_OK) return ERR_OK;
    Error e = err_set(err, ERR_IO, "index: %s", msg ? msg : sqlite3_errmsg(x->db));
    sqlite3_free(msg);
    return e;
}

static String column_text(Arena *a, sqlite3_stmt *stmt, int col) {
    const unsigned char *v = sqlite3_column_text(stmt, col);
    return str_copy(a, v ? (String){(const char *)v, (size_t)sqlite3_column_bytes(stmt, col)} : S(""));
}

static void lock(Index *x) { mutex_lock(&x->mu); }
static void unlock(Index *x) { mutex_unlock(&x->mu); }

/* ---- files ---- */

#define FILE_COLUMNS "path, size, mtime_ns, sha256, deleted"

static void read_file_row(Arena *a, sqlite3_stmt *st, IndexFile *f) {
    f->path = column_text(a, st, 0);
    f->size = sqlite3_column_int64(st, 1);
    f->mtime_ns = sqlite3_column_int64(st, 2);
    f->sha256 = column_text(a, st, 3);
    f->deleted = sqlite3_column_int(st, 4) != 0;
}

Error index_file(Index *x, Arena *a, String path, IndexFile *out, bool *found, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s, "SELECT " FILE_COLUMNS " FROM files WHERE path = ?", err);
    if (e == ERR_OK) {
        bind_text(&s, path);
        int rc = sqlite3_step(s.stmt);
        *found = rc == SQLITE_ROW;
        if (*found) read_file_row(a, s.stmt, out);
        else if (rc != SQLITE_DONE) e = db_fail(x, err, "query");
        sqlite3_finalize(s.stmt);
    }
    unlock(x);
    return e;
}

[[nodiscard]] static Error collect_files(Index *x, Arena *a, Stmt *s, IndexFileList *files, Err *err) {
    *files = (IndexFileList){0};
    int rc;
    while ((rc = sqlite3_step(s->stmt)) == SQLITE_ROW) {
        files->items = arena_grow(a, files->items, &files->capacity, files->count, sizeof *files->items);
        read_file_row(a, s->stmt, &files->items[files->count++]);
    }
    Error e = rc == SQLITE_DONE ? ERR_OK : db_fail(x, err, "query");
    sqlite3_finalize(s->stmt);
    return e;
}

Error index_files(Index *x, Arena *a, IndexFileList *files, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s, "SELECT " FILE_COLUMNS " FROM files ORDER BY path", err);
    if (e == ERR_OK) e = collect_files(x, a, &s, files, err);
    unlock(x);
    return e;
}

Error index_files_under(Index *x, Arena *a, String dir, IndexFileList *files, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s, "SELECT " FILE_COLUMNS " FROM files WHERE deleted = 0 AND substr(path, 1, length(?) + 1) = ? || '/'", err);
    if (e == ERR_OK) {
        bind_text(&s, dir);
        bind_text(&s, dir);
        e = collect_files(x, a, &s, files, err);
    }
    unlock(x);
    return e;
}

/* ---- transactions and the pieces they are built from ---- */

[[nodiscard]] static Error begin(Index *x, Err *err) { return exec_sql(x, "BEGIN IMMEDIATE", err); }

/* commit_or_rollback commits when e is ERR_OK and otherwise rolls back and returns e. */
[[nodiscard]] static Error commit_or_rollback(Index *x, Error e, Err *err) {
    if (e == ERR_OK) return exec_sql(x, "COMMIT", err);
    Err ignored;
    (void)exec_sql(x, "ROLLBACK", &ignored);
    return e;
}

[[nodiscard]] static Error clear_upload_failure(Index *x, String path, Err *err) {
    Stmt s;
    Error e = prepare(x, &s, "DELETE FROM upload_failures WHERE path = ?", err);
    if (e != ERR_OK) return e;
    bind_text(&s, path);
    return step_done(&s, nullptr, err);
}

[[nodiscard]] static Error set_verified(Index *x, String path, String store, String etag, int64_t now, Err *err) {
    Stmt s;
    Error e = prepare(x, &s,
                 "INSERT INTO replicas (path, store, state, etag, verified_at) VALUES (?, ?, 'verified', ?, ?)"
                 " ON CONFLICT (path, store) DO UPDATE SET state = 'verified', etag = excluded.etag, verified_at = excluded.verified_at,"
                 " attempts = 0, next_attempt_at = 0, last_error = NULL",
                 err);
    if (e != ERR_OK) return e;
    bind_text(&s, path);
    bind_text(&s, store);
    bind_text(&s, etag);
    bind_int(&s, now);
    return step_done(&s, nullptr, err);
}

[[nodiscard]] static Error set_pending(Index *x, String path, String store, Err *err) {
    Stmt s;
    Error e = prepare(x, &s,
                 "INSERT INTO replicas (path, store, state) VALUES (?, ?, 'pending')"
                 " ON CONFLICT (path, store) DO UPDATE SET state = 'pending', attempts = 0, next_attempt_at = 0, last_error = NULL",
                 err);
    if (e != ERR_OK) return e;
    bind_text(&s, path);
    bind_text(&s, store);
    return step_done(&s, nullptr, err);
}

[[nodiscard]] static Error prune_tombstone(Index *x, String path, Err *err) {
    Stmt s;
    Error e = prepare(x, &s, "DELETE FROM files WHERE path = ? AND deleted = 1 AND NOT EXISTS (SELECT 1 FROM replicas WHERE path = ?)", err);
    if (e != ERR_OK) return e;
    bind_text(&s, path);
    bind_text(&s, path);
    return step_done(&s, nullptr, err);
}

[[nodiscard]] static Error delete_replica(Index *x, String path, String store, Err *err) {
    Stmt s;
    Error e = prepare(x, &s, "DELETE FROM replicas WHERE path = ? AND store = ?", err);
    if (e != ERR_OK) return e;
    bind_text(&s, path);
    bind_text(&s, store);
    return step_done(&s, nullptr, err);
}

Error index_record_synced(Index *x, const IndexFile *f, String primary, String etag, StringList mirrors, Err *err) {
    lock(x);
    int64_t now = clock_wall_ns();
    Error e = begin(x, err);
    if (e == ERR_OK) {
        Stmt s;
        e = prepare(x, &s,
                     "INSERT INTO files (path, size, mtime_ns, sha256, synced_at, deleted) VALUES (?, ?, ?, ?, ?, 0)"
                     " ON CONFLICT (path) DO UPDATE SET size = excluded.size, mtime_ns = excluded.mtime_ns, sha256 = excluded.sha256,"
                     " synced_at = excluded.synced_at, deleted = 0",
                     err);
        if (e == ERR_OK) {
            bind_text(&s, f->path);
            bind_int(&s, f->size);
            bind_int(&s, f->mtime_ns);
            bind_text(&s, f->sha256);
            bind_int(&s, now);
            e = step_done(&s, nullptr, err);
        }
        if (e == ERR_OK) e = clear_upload_failure(x, f->path, err);
        if (e == ERR_OK) e = set_verified(x, f->path, primary, etag, now, err);
        for (size_t i = 0; e == ERR_OK && i < mirrors.count; i++) e = set_pending(x, f->path, mirrors.items[i], err);
        e = commit_or_rollback(x, e, err);
    }
    unlock(x);
    return e;
}

Error index_touch(Index *x, String path, int64_t mtime_ns, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s, "UPDATE files SET mtime_ns = ? WHERE path = ?", err);
    if (e == ERR_OK) {
        bind_int(&s, mtime_ns);
        bind_text(&s, path);
        e = step_done(&s, nullptr, err);
    }
    unlock(x);
    return e;
}

Error index_tombstone(Index *x, String path, String primary, bool keep_primary, Err *err) {
    lock(x);
    Error e = begin(x, err);
    if (e == ERR_OK) {
        Stmt s;
        e = prepare(x, &s, "UPDATE files SET deleted = 1, synced_at = ? WHERE path = ?", err);
        if (e == ERR_OK) {
            bind_int(&s, clock_wall_ns());
            bind_text(&s, path);
            e = step_done(&s, nullptr, err);
        }
        if (e == ERR_OK) e = clear_upload_failure(x, path, err);
        if (e == ERR_OK && !keep_primary) e = delete_replica(x, path, primary, err);
        if (e == ERR_OK) {
            e = prepare(x, &s,
                         "UPDATE replicas SET state = 'pending', attempts = 0, next_attempt_at = 0, last_error = NULL"
                         " WHERE path = ? AND store <> ?",
                         err);
            if (e == ERR_OK) {
                bind_text(&s, path);
                bind_text(&s, primary);
                e = step_done(&s, nullptr, err);
            }
        }
        if (e == ERR_OK) e = prune_tombstone(x, path, err);
        e = commit_or_rollback(x, e, err);
    }
    unlock(x);
    return e;
}

/* ---- replicas ---- */

#define REPLICA_COLUMNS "path, store, state, etag, attempts, last_error"

static void read_replica_row(Arena *a, sqlite3_stmt *st, Replica *r) {
    r->path = column_text(a, st, 0);
    r->store = column_text(a, st, 1);
    r->state = parse_state(column_text(a, st, 2));
    r->etag = column_text(a, st, 3);
    r->attempts = sqlite3_column_int(st, 4);
    r->last_error = column_text(a, st, 5);
}

Error index_replica(Index *x, Arena *a, String path, String store, Replica *out, bool *found, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s, "SELECT " REPLICA_COLUMNS " FROM replicas WHERE path = ? AND store = ?", err);
    if (e == ERR_OK) {
        bind_text(&s, path);
        bind_text(&s, store);
        int rc = sqlite3_step(s.stmt);
        *found = rc == SQLITE_ROW;
        if (*found) read_replica_row(a, s.stmt, out);
        else if (rc != SQLITE_DONE) e = db_fail(x, err, "query");
        sqlite3_finalize(s.stmt);
    }
    unlock(x);
    return e;
}

[[nodiscard]] static Error collect_replicas(Index *x, Arena *a, Stmt *s, ReplicaList *rows, Err *err) {
    *rows = (ReplicaList){0};
    int rc;
    while ((rc = sqlite3_step(s->stmt)) == SQLITE_ROW) {
        rows->items = arena_grow(a, rows->items, &rows->capacity, rows->count, sizeof *rows->items);
        read_replica_row(a, s->stmt, &rows->items[rows->count++]);
    }
    Error e = rc == SQLITE_DONE ? ERR_OK : db_fail(x, err, "query");
    sqlite3_finalize(s->stmt);
    return e;
}

Error index_due(Index *x, Arena *a, String store, int limit, ReplicaList *out, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s,
                      "SELECT " REPLICA_COLUMNS " FROM replicas"
                      " WHERE store = ? AND (state = 'pending' OR (state = 'failed' AND attempts < ? AND next_attempt_at <= ?))"
                      " ORDER BY path LIMIT ?",
                      err);
    if (e == ERR_OK) {
        bind_text(&s, store);
        bind_int(&s, MAX_ATTEMPTS);
        bind_int(&s, clock_wall_ns());
        bind_int(&s, limit);
        e = collect_replicas(x, a, &s, out, err);
    }
    unlock(x);
    return e;
}

Error index_failed(Index *x, Arena *a, String store, int limit, ReplicaList *out, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s, "SELECT " REPLICA_COLUMNS " FROM replicas WHERE store = ? AND state = 'failed' ORDER BY path LIMIT ?", err);
    if (e == ERR_OK) {
        bind_text(&s, store);
        bind_int(&s, limit);
        e = collect_replicas(x, a, &s, out, err);
    }
    unlock(x);
    return e;
}

Error index_replicas_on(Index *x, Arena *a, String store, ReplicaList *out, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s, "SELECT " REPLICA_COLUMNS " FROM replicas WHERE store = ? ORDER BY path", err);
    if (e == ERR_OK) {
        bind_text(&s, store);
        e = collect_replicas(x, a, &s, out, err);
    }
    unlock(x);
    return e;
}

Error index_mark_verified(Index *x, String path, String store, String etag, String sha, bool *verified, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s,
                      "UPDATE replicas SET state = 'verified', etag = ?, verified_at = ?, attempts = 0, last_error = NULL"
                      " WHERE path = ? AND store = ? AND EXISTS (SELECT 1 FROM files WHERE path = ? AND sha256 = ? AND deleted = 0)",
                      err);
    if (e == ERR_OK) {
        bind_text(&s, etag);
        bind_int(&s, clock_wall_ns());
        bind_text(&s, path);
        bind_text(&s, store);
        bind_text(&s, path);
        bind_text(&s, sha);
        int64_t changed = 0;
        e = step_done(&s, &changed, err);
        *verified = e == ERR_OK && changed == 1;
    }
    unlock(x);
    return e;
}

Error index_mark_failed(Index *x, String path, String store, String cause, int attempts, int64_t retry_at_ns, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s, "UPDATE replicas SET state = 'failed', last_error = ?, attempts = ?, next_attempt_at = ? WHERE path = ? AND store = ?", err);
    if (e == ERR_OK) {
        bind_text(&s, cause);
        bind_int(&s, attempts);
        bind_int(&s, retry_at_ns);
        bind_text(&s, path);
        bind_text(&s, store);
        e = step_done(&s, nullptr, err);
    }
    unlock(x);
    return e;
}

Error index_mark_pending(Index *x, String path, String store, Err *err) {
    lock(x);
    Error e = set_pending(x, path, store, err);
    unlock(x);
    return e;
}

Error index_drop_replica(Index *x, String path, String store, Err *err) {
    lock(x);
    Error e = begin(x, err);
    if (e == ERR_OK) {
        e = delete_replica(x, path, store, err);
        if (e == ERR_OK) e = prune_tombstone(x, path, err);
        e = commit_or_rollback(x, e, err);
    }
    unlock(x);
    return e;
}

Error index_backfill(Index *x, String store, int64_t *added, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s,
                      "INSERT INTO replicas (path, store, state)"
                      " SELECT path, ?, 'pending' FROM files WHERE deleted = 0"
                      " AND NOT EXISTS (SELECT 1 FROM replicas r WHERE r.path = files.path AND r.store = ?)",
                      err);
    if (e == ERR_OK) {
        bind_text(&s, store);
        bind_text(&s, store);
        e = step_done(&s, added, err);
    }
    unlock(x);
    return e;
}

Error index_retry_failed(Index *x, String store, int64_t *count, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s,
                      "UPDATE replicas SET state = 'pending', attempts = 0, next_attempt_at = 0, last_error = NULL"
                      " WHERE store = ? AND state = 'failed'",
                      err);
    if (e == ERR_OK) {
        bind_text(&s, store);
        e = step_done(&s, count, err);
    }
    unlock(x);
    return e;
}

/* ---- upload failures ---- */

Error index_mark_upload_failed(Index *x, String path, String cause, int attempts, int64_t retry_at_ns, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s,
                      "INSERT INTO upload_failures (path, attempts, next_attempt_at, last_error) VALUES (?, ?, ?, ?)"
                      " ON CONFLICT (path) DO UPDATE SET attempts = excluded.attempts, next_attempt_at = excluded.next_attempt_at,"
                      " last_error = excluded.last_error",
                      err);
    if (e == ERR_OK) {
        bind_text(&s, path);
        bind_int(&s, attempts);
        bind_int(&s, retry_at_ns);
        bind_text(&s, cause);
        e = step_done(&s, nullptr, err);
    }
    unlock(x);
    return e;
}

static void read_failure_row(Arena *a, sqlite3_stmt *st, UploadFailure *f) {
    f->path = column_text(a, st, 0);
    f->attempts = sqlite3_column_int(st, 1);
    f->last_error = column_text(a, st, 2);
}

Error index_upload_failure(Index *x, Arena *a, String path, UploadFailure *out, bool *found, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s, "SELECT path, attempts, last_error FROM upload_failures WHERE path = ?", err);
    if (e == ERR_OK) {
        bind_text(&s, path);
        int rc = sqlite3_step(s.stmt);
        *found = rc == SQLITE_ROW;
        if (*found) read_failure_row(a, s.stmt, out);
        else if (rc != SQLITE_DONE) e = db_fail(x, err, "query");
        sqlite3_finalize(s.stmt);
    }
    unlock(x);
    return e;
}

Error index_clear_upload_failure(Index *x, String path, Err *err) {
    lock(x);
    Error e = clear_upload_failure(x, path, err);
    unlock(x);
    return e;
}

Error index_due_uploads(Index *x, Arena *a, int64_t now_ns, StringList *paths, Err *err) {
    lock(x);
    Stmt s;
    *paths = (StringList){0};
    Error e = prepare(x, &s, "SELECT path FROM upload_failures WHERE attempts < ? AND next_attempt_at <= ? ORDER BY path", err);
    if (e == ERR_OK) {
        bind_int(&s, MAX_ATTEMPTS);
        bind_int(&s, now_ns);
        int rc;
        while ((rc = sqlite3_step(s.stmt)) == SQLITE_ROW) strlist_push(a, paths, column_text(a, s.stmt, 0));
        e = rc == SQLITE_DONE ? ERR_OK : db_fail(x, err, "query");
        sqlite3_finalize(s.stmt);
    }
    unlock(x);
    return e;
}

Error index_failed_uploads(Index *x, Arena *a, UploadFailureList *rows, Err *err) {
    lock(x);
    Stmt s;
    *rows = (UploadFailureList){0};
    Error e = prepare(x, &s, "SELECT path, attempts, last_error FROM upload_failures ORDER BY path", err);
    if (e == ERR_OK) {
        int rc;
        while ((rc = sqlite3_step(s.stmt)) == SQLITE_ROW) {
            rows->items = arena_grow(a, rows->items, &rows->capacity, rows->count, sizeof *rows->items);
            read_failure_row(a, s.stmt, &rows->items[rows->count++]);
        }
        e = rc == SQLITE_DONE ? ERR_OK : db_fail(x, err, "query");
        sqlite3_finalize(s.stmt);
    }
    unlock(x);
    return e;
}

Error index_retry_failed_uploads(Index *x, int64_t *count, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s, "UPDATE upload_failures SET attempts = 0, next_attempt_at = 0", err);
    if (e == ERR_OK) e = step_done(&s, count, err);
    unlock(x);
    return e;
}

/* ---- stats ---- */

Error index_stats(Index *x, String store, Stats *out, Err *err) {
    lock(x);
    Stmt s;
    Error e = prepare(x, &s,
                      "SELECT COUNT(*), COALESCE(SUM(f.size), 0),"
                      " COALESCE(SUM(r.state = 'verified'), 0), COALESCE(SUM(CASE WHEN r.state = 'verified' THEN f.size END), 0),"
                      " COALESCE(SUM(r.state = 'pending'), 0), COALESCE(SUM(r.state = 'failed'), 0)"
                      " FROM files f LEFT JOIN replicas r ON r.path = f.path AND r.store = ?"
                      " WHERE f.deleted = 0",
                      err);
    if (e == ERR_OK) {
        bind_text(&s, store);
        e = sqlite3_step(s.stmt) == SQLITE_ROW ? ERR_OK : db_fail(x, err, "stats");
        if (e == ERR_OK) {
            out->files = sqlite3_column_int64(s.stmt, 0);
            out->bytes = sqlite3_column_int64(s.stmt, 1);
            out->verified = sqlite3_column_int64(s.stmt, 2);
            out->verified_bytes = sqlite3_column_int64(s.stmt, 3);
            out->pending = sqlite3_column_int64(s.stmt, 4);
            out->failed = sqlite3_column_int64(s.stmt, 5);
        }
        sqlite3_finalize(s.stmt);
    }
    unlock(x);
    return e;
}
