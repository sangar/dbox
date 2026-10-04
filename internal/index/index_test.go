package index

import (
	"context"
	"errors"
	"path/filepath"
	"slices"
	"testing"
	"time"
)

func open(t *testing.T) *Index {
	t.Helper()
	idx, err := Open(filepath.Join(t.TempDir(), "index.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { idx.Close() })
	return idx
}

func TestTombstoneIsRemovedOnceNoStoreHoldsACopy(t *testing.T) {
	ctx := context.Background()
	idx := open(t)
	if err := idx.RecordSynced(ctx, File{Path: "a", Size: 1, SHA256: "x"}, "p", "e1", []string{"m"}); err != nil {
		t.Fatal(err)
	}
	if err := idx.Tombstone(ctx, "a", "p", false); err != nil {
		t.Fatal(err)
	}
	if f, _ := idx.File(ctx, "a"); f == nil || !f.Deleted {
		t.Fatalf("file = %+v, want a tombstone while the mirror still holds a copy", f)
	}
	if err := idx.DropReplica(ctx, "a", "m"); err != nil {
		t.Fatal(err)
	}
	if f, _ := idx.File(ctx, "a"); f != nil {
		t.Errorf("file = %+v, want it gone", f)
	}
}

func TestMarkVerifiedIgnoresStaleContent(t *testing.T) {
	ctx := context.Background()
	idx := open(t)
	idx.RecordSynced(ctx, File{Path: "a", SHA256: "new"}, "p", "e", []string{"m"})
	if ok, err := idx.MarkVerified(ctx, "a", "m", "e", "old"); err != nil || ok {
		t.Errorf("verified a copy of old content: %v %v", ok, err)
	}
	if ok, _ := idx.MarkVerified(ctx, "a", "m", "e", "new"); !ok {
		t.Error("did not verify a copy of current content")
	}
}

func TestBackfillAndStats(t *testing.T) {
	ctx := context.Background()
	idx := open(t)
	idx.RecordSynced(ctx, File{Path: "a", Size: 10, SHA256: "x"}, "p", "e", nil)
	idx.RecordSynced(ctx, File{Path: "b", Size: 20, SHA256: "y"}, "p", "e", nil)
	if n, _ := idx.Backfill(ctx, "new"); n != 2 {
		t.Errorf("backfill added %d", n)
	}
	if n, _ := idx.Backfill(ctx, "new"); n != 0 {
		t.Errorf("second backfill added %d", n)
	}
	idx.MarkVerified(ctx, "a", "new", "e", "x")
	s, err := idx.Stats(ctx, "new")
	if err != nil {
		t.Fatal(err)
	}
	want := Stats{Files: 2, Bytes: 30, Verified: 1, VerifiedBytes: 10, Pending: 1}
	if s != want {
		t.Errorf("stats = %+v, want %+v", s, want)
	}
	if s.Unverified() != 1 {
		t.Errorf("unverified = %d", s.Unverified())
	}
}

func TestUploadFailuresAreDueAfterBackoffUntilGivenUp(t *testing.T) {
	ctx := context.Background()
	idx := open(t)
	now := time.Now()
	if err := idx.MarkUploadFailed(ctx, "a", errors.New("504"), 1, now.Add(time.Minute)); err != nil {
		t.Fatal(err)
	}
	if err := idx.MarkUploadFailed(ctx, "b", errors.New("403"), MaxAttempts, now); err != nil {
		t.Fatal(err)
	}
	if due, _ := idx.DueUploads(ctx, now); len(due) != 0 {
		t.Errorf("due before backoff = %v, want none", due)
	}
	if due, _ := idx.DueUploads(ctx, now.Add(time.Minute)); !slices.Equal(due, []string{"a"}) {
		t.Errorf("due after backoff = %v, want [a]; b has used its attempts", due)
	}
	if n, _ := idx.RetryFailedUploads(ctx); n != 2 {
		t.Errorf("retried %d, want 2", n)
	}
	if due, _ := idx.DueUploads(ctx, now); !slices.Equal(due, []string{"a", "b"}) {
		t.Errorf("due after retry = %v, want [a b]", due)
	}
	if err := idx.RecordSynced(ctx, File{Path: "a", Size: 1, SHA256: "x"}, "p", "e1", nil); err != nil {
		t.Fatal(err)
	}
	failed, _ := idx.FailedUploads(ctx)
	if len(failed) != 1 || failed[0].Path != "b" || failed[0].LastError != "403" {
		t.Errorf("failed after a synced = %+v, want only b", failed)
	}
}
