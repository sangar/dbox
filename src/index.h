#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

/*
 * Index is the local SQLite record of every synced file and of which stores
 * hold a verified copy of it. Strings in the rows it returns live in the
 * arena the caller passes. One Index may be shared between threads.
 */
typedef struct Index Index;

typedef enum ReplicaState { REPLICA_PENDING, REPLICA_VERIFIED, REPLICA_FAILED } ReplicaState;

/* MAX_ATTEMPTS is how often a failed copy is retried before it waits for `dbox retry` or the next backfill. */
enum { MAX_ATTEMPTS = 5 };

/* IndexFile is a path the daemon has synced. deleted marks a tombstone: gone locally, and some store may still hold a copy. */
typedef struct IndexFile {
    String path;
    int64_t size;
    int64_t mtime_ns;
    String sha256;
    bool deleted;
} IndexFile;

typedef struct IndexFileList {
    IndexFile *items;
    size_t count, capacity;
} IndexFileList;

typedef struct Replica {
    String path;
    String store;
    ReplicaState state;
    String etag;
    int attempts;
    String last_error;
} Replica;

typedef struct ReplicaList {
    Replica *items;
    size_t count, capacity;
} ReplicaList;

/* UploadFailure is a local file the primary has refused so far. */
typedef struct UploadFailure {
    String path;
    int attempts;
    String last_error;
} UploadFailure;

typedef struct UploadFailureList {
    UploadFailure *items;
    size_t count, capacity;
} UploadFailureList;

/* Stats summarises one store's copies of live files. */
typedef struct Stats {
    int64_t files, bytes, verified, verified_bytes, pending, failed;
} Stats;

static inline int64_t stats_unverified(const Stats *stats) { return stats->files - stats->verified; }

/* index_open opens or creates the database at path; the Index lives in the arena until index_close. */
[[nodiscard]] Error index_open(Arena *arena, String path, Index **index, Err *err);
void index_close(Index *index);

/* index_file looks up one row; *found says whether it exists. */
[[nodiscard]] Error index_file(Index *index, Arena *arena, String path, IndexFile *file, bool *found, Err *err);
/* index_files returns every row, tombstones included, ordered by path. */
[[nodiscard]] Error index_files(Index *index, Arena *arena, IndexFileList *files, Err *err);
/* index_files_under returns live files inside dir, for when a whole directory vanishes. */
[[nodiscard]] Error index_files_under(Index *index, Arena *arena, String dir, IndexFileList *files, Err *err);
/* index_record_synced stores file as live, the primary copy as verified with etag, and every mirror as pending. */
[[nodiscard]] Error index_record_synced(Index *index, const IndexFile *file, String primary, String etag, StringList mirrors, Err *err);
/* index_touch records a new modification time for content that did not change. */
[[nodiscard]] Error index_touch(Index *index, String path, int64_t mtime_ns, Err *err);
/*
 * index_tombstone marks path deleted and every replica except the primary's
 * pending, so mirrors delete their copies. keep_primary leaves the primary
 * row as it is, for when the primary copy was deliberately not deleted.
 */
[[nodiscard]] Error index_tombstone(Index *index, String path, String primary, bool keep_primary, Err *err);

[[nodiscard]] Error index_replica(Index *index, Arena *arena, String path, String store, Replica *replica, bool *found, Err *err);
/* index_due returns replicas on store that are pending, or failed and due a retry. */
[[nodiscard]] Error index_due(Index *index, Arena *arena, String store, int limit, ReplicaList *replicas, Err *err);
[[nodiscard]] Error index_failed(Index *index, Arena *arena, String store, int limit, ReplicaList *replicas, Err *err);
[[nodiscard]] Error index_replicas_on(Index *index, Arena *arena, String store, ReplicaList *replicas, Err *err);
/* index_mark_verified records a verified copy, but only while the file still has the content that was copied. */
[[nodiscard]] Error index_mark_verified(Index *index, String path, String store, String etag, String sha256, bool *verified, Err *err);
[[nodiscard]] Error index_mark_failed(Index *index, String path, String store, String cause, int attempts, int64_t retry_at_ns, Err *err);
[[nodiscard]] Error index_mark_pending(Index *index, String path, String store, Err *err);
/* index_drop_replica forgets store's copy and removes the file row once a tombstone has no copies left anywhere. */
[[nodiscard]] Error index_drop_replica(Index *index, String path, String store, Err *err);
/* index_backfill adds a pending replica on store for every live file that has none; *added says how many. */
[[nodiscard]] Error index_backfill(Index *index, String store, int64_t *added, Err *err);
[[nodiscard]] Error index_retry_failed(Index *index, String store, int64_t *count, Err *err);

[[nodiscard]] Error index_mark_upload_failed(Index *index, String path, String cause, int attempts, int64_t retry_at_ns, Err *err);
[[nodiscard]] Error index_upload_failure(Index *index, Arena *arena, String path, UploadFailure *failure, bool *found, Err *err);
[[nodiscard]] Error index_clear_upload_failure(Index *index, String path, Err *err);
/* index_due_uploads returns the paths whose upload is worth retrying at now. */
[[nodiscard]] Error index_due_uploads(Index *index, Arena *arena, int64_t now_ns, StringList *paths, Err *err);
[[nodiscard]] Error index_failed_uploads(Index *index, Arena *arena, UploadFailureList *failures, Err *err);
[[nodiscard]] Error index_retry_failed_uploads(Index *index, int64_t *count, Err *err);

[[nodiscard]] Error index_stats(Index *index, String store, Stats *stats, Err *err);
