#ifndef DBOX_INDEX_H
#define DBOX_INDEX_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "util.h"

/*
 * Index is the local SQLite record of every synced file and of which stores
 * hold a verified copy of it. Strings in the rows it returns live in the
 * Arena the caller passes.
 */
typedef struct Index Index;

typedef enum { REPLICA_PENDING, REPLICA_VERIFIED, REPLICA_FAILED } ReplicaState;

/* MAX_ATTEMPTS is how often a failed copy is retried before it waits for `dbox retry` or the next backfill. */
#define MAX_ATTEMPTS 5

/* IndexFile is a path the daemon has synced. deleted marks a tombstone: gone locally, and some store may still hold a copy. */
typedef struct {
    const char *path;
    int64_t size;
    int64_t mtime_ns;
    const char *sha256;
    bool deleted;
} IndexFile;

typedef struct {
    const char *path;
    const char *store;
    ReplicaState state;
    const char *etag;
    int attempts;
    const char *last_error;
} Replica;

/* UploadFailure is a local file the primary has refused so far. */
typedef struct {
    const char *path;
    int attempts;
    const char *last_error;
} UploadFailure;

/* Stats summarises one store's copies of live files. */
typedef struct {
    int64_t files, bytes, verified, verified_bytes, pending, failed;
} Stats;

static inline int64_t stats_unverified(const Stats *s) { return s->files - s->verified; }

Index *index_open(const char *path, Err *err);
void index_close(Index *x);

/* index_file looks up one row; *found says whether it exists. */
bool index_file(Index *x, Arena *a, const char *path, IndexFile *out, bool *found, Err *err);
/* index_files returns every row, tombstones included, ordered by path. */
bool index_files(Index *x, Arena *a, IndexFile **out, size_t *count, Err *err);
/* index_files_under returns live files inside dir, for when a whole directory vanishes. */
bool index_files_under(Index *x, Arena *a, const char *dir, IndexFile **out, size_t *count, Err *err);
/* index_record_synced stores f as live, the primary copy as verified with etag, and every mirror as pending. */
bool index_record_synced(Index *x, const IndexFile *f, const char *primary, const char *etag, const char **mirrors, size_t mirror_count, Err *err);
/* index_touch records a new modification time for content that did not change. */
bool index_touch(Index *x, const char *path, int64_t mtime_ns, Err *err);
/*
 * index_tombstone marks path deleted and every replica except the primary's
 * pending, so mirrors delete their copies. keep_primary leaves the primary
 * row as it is, for when the primary copy was deliberately not deleted.
 */
bool index_tombstone(Index *x, const char *path, const char *primary, bool keep_primary, Err *err);

bool index_replica(Index *x, Arena *a, const char *path, const char *store, Replica *out, bool *found, Err *err);
/* index_due returns replicas on store that are pending, or failed and due a retry. */
bool index_due(Index *x, Arena *a, const char *store, int limit, Replica **out, size_t *count, Err *err);
bool index_failed(Index *x, Arena *a, const char *store, int limit, Replica **out, size_t *count, Err *err);
bool index_replicas_on(Index *x, Arena *a, const char *store, Replica **out, size_t *count, Err *err);
/* index_mark_verified records a verified copy, but only while the file still has the content that was copied. */
bool index_mark_verified(Index *x, const char *path, const char *store, const char *etag, const char *sha, bool *verified, Err *err);
bool index_mark_failed(Index *x, const char *path, const char *store, const char *cause, int attempts, int64_t retry_at_ns, Err *err);
bool index_mark_pending(Index *x, const char *path, const char *store, Err *err);
/* index_drop_replica forgets store's copy and removes the file row once a tombstone has no copies left anywhere. */
bool index_drop_replica(Index *x, const char *path, const char *store, Err *err);
/* index_backfill adds a pending replica on store for every live file that has none; *added says how many. */
bool index_backfill(Index *x, const char *store, int64_t *added, Err *err);
bool index_retry_failed(Index *x, const char *store, int64_t *count, Err *err);

bool index_mark_upload_failed(Index *x, const char *path, const char *cause, int attempts, int64_t retry_at_ns, Err *err);
bool index_upload_failure(Index *x, Arena *a, const char *path, UploadFailure *out, bool *found, Err *err);
bool index_clear_upload_failure(Index *x, const char *path, Err *err);
/* index_due_uploads returns the paths whose upload is worth retrying at now. */
bool index_due_uploads(Index *x, Arena *a, int64_t now_ns, const char ***paths, size_t *count, Err *err);
bool index_failed_uploads(Index *x, Arena *a, UploadFailure **out, size_t *count, Err *err);
bool index_retry_failed_uploads(Index *x, int64_t *count, Err *err);

bool index_stats(Index *x, const char *store, Stats *out, Err *err);

const char *replica_state_name(ReplicaState s);

#endif
