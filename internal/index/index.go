// Package index is the local SQLite record of every synced file and of which
// stores hold a verified copy of it.
package index

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"

	_ "modernc.org/sqlite"
)

type State string

const (
	Pending  State = "pending"
	Verified State = "verified"
	Failed   State = "failed"
)

// MaxAttempts is how often a failed copy is retried before it waits for
// `dbox retry` or the next backfill.
const MaxAttempts = 5

const schema = `
CREATE TABLE IF NOT EXISTS files (
  path      TEXT PRIMARY KEY,
  size      INTEGER NOT NULL,
  mtime_ns  INTEGER NOT NULL,
  sha256    TEXT    NOT NULL,
  synced_at INTEGER NOT NULL,
  deleted   INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS replicas (
  path            TEXT    NOT NULL REFERENCES files(path) ON DELETE CASCADE,
  store           TEXT    NOT NULL,
  state           TEXT    NOT NULL,
  etag            TEXT,
  verified_at     INTEGER,
  last_error      TEXT,
  attempts        INTEGER NOT NULL DEFAULT 0,
  next_attempt_at INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (path, store)
);
CREATE INDEX IF NOT EXISTS replicas_by_store_state ON replicas (store, state);
`

// File is a path the daemon has synced. Deleted marks a tombstone: the file is
// gone locally and some store may still have to delete its copy.
type File struct {
	Path    string
	Size    int64
	MTimeNs int64
	SHA256  string
	Deleted bool
}

type Replica struct {
	Path      string
	Store     string
	State     State
	ETag      string
	Attempts  int
	LastError string
}

type Index struct {
	db *sql.DB
}

func Open(path string) (*Index, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return nil, err
	}
	dsn := "file:" + path + "?_pragma=busy_timeout(10000)&_pragma=journal_mode(WAL)&_pragma=foreign_keys(1)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return &Index{db: db}, nil
}

func (x *Index) Close() error { return x.db.Close() }

func (x *Index) File(ctx context.Context, path string) (*File, error) {
	var f File
	err := x.db.QueryRowContext(ctx, `SELECT path, size, mtime_ns, sha256, deleted FROM files WHERE path = ?`, path).
		Scan(&f.Path, &f.Size, &f.MTimeNs, &f.SHA256, &f.Deleted)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	return &f, err
}

// Files returns every row, tombstones included.
func (x *Index) Files(ctx context.Context) ([]File, error) {
	return x.files(ctx, `SELECT path, size, mtime_ns, sha256, deleted FROM files ORDER BY path`)
}

// FilesUnder returns live files inside dir, for when a whole directory vanishes.
func (x *Index) FilesUnder(ctx context.Context, dir string) ([]File, error) {
	return x.files(ctx, `SELECT path, size, mtime_ns, sha256, deleted FROM files WHERE deleted = 0 AND substr(path, 1, ?) = ?`, len(dir)+1, dir+"/")
}

func (x *Index) files(ctx context.Context, query string, args ...any) ([]File, error) {
	rows, err := x.db.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var files []File
	for rows.Next() {
		var f File
		if err := rows.Scan(&f.Path, &f.Size, &f.MTimeNs, &f.SHA256, &f.Deleted); err != nil {
			return nil, err
		}
		files = append(files, f)
	}
	return files, rows.Err()
}

// RecordSynced stores f as live, the primary copy as verified with etag, and
// every mirror as pending.
func (x *Index) RecordSynced(ctx context.Context, f File, primary, etag string, mirrors []string) error {
	return x.tx(ctx, func(tx *sql.Tx) error {
		now := time.Now().UnixNano()
		if _, err := tx.Exec(`INSERT INTO files (path, size, mtime_ns, sha256, synced_at, deleted) VALUES (?, ?, ?, ?, ?, 0)
			ON CONFLICT (path) DO UPDATE SET size = excluded.size, mtime_ns = excluded.mtime_ns, sha256 = excluded.sha256, synced_at = excluded.synced_at, deleted = 0`,
			f.Path, f.Size, f.MTimeNs, f.SHA256, now); err != nil {
			return err
		}
		if err := setVerified(tx, f.Path, primary, etag, now); err != nil {
			return err
		}
		for _, m := range mirrors {
			if err := setPending(tx, f.Path, m); err != nil {
				return err
			}
		}
		return nil
	})
}

// Touch records a new modification time for content that did not change.
func (x *Index) Touch(ctx context.Context, path string, mtimeNs int64) error {
	_, err := x.db.ExecContext(ctx, `UPDATE files SET mtime_ns = ? WHERE path = ?`, mtimeNs, path)
	return err
}

// Tombstone marks path deleted and every replica except the primary's pending,
// so mirrors delete their copies. keepPrimary leaves the primary row as it is,
// for when the primary copy was deliberately not deleted.
func (x *Index) Tombstone(ctx context.Context, path, primary string, keepPrimary bool) error {
	return x.tx(ctx, func(tx *sql.Tx) error {
		if _, err := tx.Exec(`UPDATE files SET deleted = 1, synced_at = ? WHERE path = ?`, time.Now().UnixNano(), path); err != nil {
			return err
		}
		if !keepPrimary {
			if _, err := tx.Exec(`DELETE FROM replicas WHERE path = ? AND store = ?`, path, primary); err != nil {
				return err
			}
		}
		if _, err := tx.Exec(`UPDATE replicas SET state = 'pending', attempts = 0, next_attempt_at = 0, last_error = NULL
			WHERE path = ? AND store <> ?`, path, primary); err != nil {
			return err
		}
		return pruneTombstone(tx, path)
	})
}

func (x *Index) Replica(ctx context.Context, path, store string) (*Replica, error) {
	var r Replica
	var etag, lastError sql.NullString
	err := x.db.QueryRowContext(ctx, `SELECT path, store, state, etag, attempts, last_error FROM replicas WHERE path = ? AND store = ?`, path, store).
		Scan(&r.Path, &r.Store, &r.State, &etag, &r.Attempts, &lastError)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	r.ETag, r.LastError = etag.String, lastError.String
	return &r, err
}

// Due returns replicas on store that are pending, or failed and due a retry.
func (x *Index) Due(ctx context.Context, store string, limit int) ([]Replica, error) {
	return x.replicas(ctx, `SELECT path, store, state, etag, attempts, last_error FROM replicas
		WHERE store = ? AND (state = 'pending' OR (state = 'failed' AND attempts < ? AND next_attempt_at <= ?))
		ORDER BY path LIMIT ?`, store, MaxAttempts, time.Now().UnixNano(), limit)
}

// Failed returns replicas on store that are failed.
func (x *Index) Failed(ctx context.Context, store string, limit int) ([]Replica, error) {
	return x.replicas(ctx, `SELECT path, store, state, etag, attempts, last_error FROM replicas
		WHERE store = ? AND state = 'failed' ORDER BY path LIMIT ?`, store, limit)
}

// ReplicasOn returns every replica row for store.
func (x *Index) ReplicasOn(ctx context.Context, store string) ([]Replica, error) {
	return x.replicas(ctx, `SELECT path, store, state, etag, attempts, last_error FROM replicas WHERE store = ? ORDER BY path`, store)
}

func (x *Index) replicas(ctx context.Context, query string, args ...any) ([]Replica, error) {
	rows, err := x.db.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Replica
	for rows.Next() {
		var r Replica
		var etag, lastError sql.NullString
		if err := rows.Scan(&r.Path, &r.Store, &r.State, &etag, &r.Attempts, &lastError); err != nil {
			return nil, err
		}
		r.ETag, r.LastError = etag.String, lastError.String
		out = append(out, r)
	}
	return out, rows.Err()
}

// MarkVerified records a verified copy on store, but only while the file still
// has the content that was copied; a newer version keeps the row pending.
func (x *Index) MarkVerified(ctx context.Context, path, store, etag, sha string) (bool, error) {
	res, err := x.db.ExecContext(ctx, `UPDATE replicas SET state = 'verified', etag = ?, verified_at = ?, attempts = 0, last_error = NULL
		WHERE path = ? AND store = ? AND EXISTS (SELECT 1 FROM files WHERE path = ? AND sha256 = ? AND deleted = 0)`,
		etag, time.Now().UnixNano(), path, store, path, sha)
	if err != nil {
		return false, err
	}
	n, err := res.RowsAffected()
	return n == 1, err
}

// MarkFailed records a failed attempt and when to try again.
func (x *Index) MarkFailed(ctx context.Context, path, store string, cause error, attempts int, retryAt time.Time) error {
	_, err := x.db.ExecContext(ctx, `UPDATE replicas SET state = 'failed', last_error = ?, attempts = ?, next_attempt_at = ? WHERE path = ? AND store = ?`,
		cause.Error(), attempts, retryAt.UnixNano(), path, store)
	return err
}

// MarkPending sends a replica back for copying.
func (x *Index) MarkPending(ctx context.Context, path, store string) error {
	return x.tx(ctx, func(tx *sql.Tx) error { return setPending(tx, path, store) })
}

// DropReplica forgets store's copy and removes the file row once a tombstone
// has no copies left anywhere.
func (x *Index) DropReplica(ctx context.Context, path, store string) error {
	return x.tx(ctx, func(tx *sql.Tx) error {
		if _, err := tx.Exec(`DELETE FROM replicas WHERE path = ? AND store = ?`, path, store); err != nil {
			return err
		}
		return pruneTombstone(tx, path)
	})
}

// Backfill adds a pending replica on store for every live file that has none,
// and returns how many it added.
func (x *Index) Backfill(ctx context.Context, store string) (int64, error) {
	res, err := x.db.ExecContext(ctx, `INSERT INTO replicas (path, store, state)
		SELECT path, ?, 'pending' FROM files WHERE deleted = 0
		AND NOT EXISTS (SELECT 1 FROM replicas r WHERE r.path = files.path AND r.store = ?)`, store, store)
	if err != nil {
		return 0, err
	}
	return res.RowsAffected()
}

// RetryFailed makes every failed replica on store pending again.
func (x *Index) RetryFailed(ctx context.Context, store string) (int64, error) {
	res, err := x.db.ExecContext(ctx, `UPDATE replicas SET state = 'pending', attempts = 0, next_attempt_at = 0, last_error = NULL
		WHERE store = ? AND state = 'failed'`, store)
	if err != nil {
		return 0, err
	}
	return res.RowsAffected()
}

// Stats summarises one store's copies of live files.
type Stats struct {
	Files         int64
	Bytes         int64
	Verified      int64
	VerifiedBytes int64
	Pending       int64
	Failed        int64
}

// Unverified is how many live files the store does not hold a verified copy of.
func (s Stats) Unverified() int64 { return s.Files - s.Verified }

func (x *Index) Stats(ctx context.Context, store string) (Stats, error) {
	var s Stats
	err := x.db.QueryRowContext(ctx, `SELECT
		COUNT(*), COALESCE(SUM(f.size), 0),
		COALESCE(SUM(r.state = 'verified'), 0), COALESCE(SUM(CASE WHEN r.state = 'verified' THEN f.size END), 0),
		COALESCE(SUM(r.state = 'pending'), 0), COALESCE(SUM(r.state = 'failed'), 0)
		FROM files f LEFT JOIN replicas r ON r.path = f.path AND r.store = ?
		WHERE f.deleted = 0`, store).
		Scan(&s.Files, &s.Bytes, &s.Verified, &s.VerifiedBytes, &s.Pending, &s.Failed)
	return s, err
}

func (x *Index) tx(ctx context.Context, fn func(*sql.Tx) error) error {
	tx, err := x.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	if err := fn(tx); err != nil {
		tx.Rollback()
		return err
	}
	return tx.Commit()
}

func setVerified(tx *sql.Tx, path, store, etag string, now int64) error {
	_, err := tx.Exec(`INSERT INTO replicas (path, store, state, etag, verified_at) VALUES (?, ?, 'verified', ?, ?)
		ON CONFLICT (path, store) DO UPDATE SET state = 'verified', etag = excluded.etag, verified_at = excluded.verified_at,
		attempts = 0, next_attempt_at = 0, last_error = NULL`, path, store, etag, now)
	return err
}

func setPending(tx *sql.Tx, path, store string) error {
	_, err := tx.Exec(`INSERT INTO replicas (path, store, state) VALUES (?, ?, 'pending')
		ON CONFLICT (path, store) DO UPDATE SET state = 'pending', attempts = 0, next_attempt_at = 0, last_error = NULL`, path, store)
	return err
}

func pruneTombstone(tx *sql.Tx, path string) error {
	_, err := tx.Exec(`DELETE FROM files WHERE path = ? AND deleted = 1 AND NOT EXISTS (SELECT 1 FROM replicas WHERE path = ?)`, path, path)
	return err
}
