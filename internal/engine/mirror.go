package engine

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"sync"
	"time"

	"dbox/internal/index"
	"dbox/internal/store"
)

// errSourceMissing is recorded when neither the primary nor the local folder
// has the content a mirror needs. Such copies are not retried.
var errSourceMissing = errors.New("source missing on primary and locally")

const mirrorBatch = 256

func (e *Engine) mirrorLoop(ctx context.Context, name string) {
	idle := time.NewTicker(5 * time.Second)
	defer idle.Stop()
	for {
		n, err := e.mirrorBatch(ctx, name)
		if err != nil && ctx.Err() == nil {
			e.log.Error("mirror", "store", name, "err", err)
		}
		if n > 0 {
			continue
		}
		select {
		case <-ctx.Done():
			return
		case <-e.wake[name]:
		case <-idle.C:
		}
	}
}

// drainMirror copies until nothing is due, for --once.
func (e *Engine) drainMirror(ctx context.Context, name string) error {
	for {
		n, err := e.mirrorBatch(ctx, name)
		if err != nil || n == 0 {
			return err
		}
	}
}

// mirrorBatch handles up to one batch of due replicas with the store's
// workers and returns how many it handled.
func (e *Engine) mirrorBatch(ctx context.Context, name string) (int, error) {
	due, err := e.idx.Due(ctx, name, mirrorBatch)
	if err != nil || len(due) == 0 {
		return 0, err
	}
	work := make(chan index.Replica)
	var wg sync.WaitGroup
	for range e.cfg.Stores[name].Workers {
		wg.Go(func() {
			for r := range work {
				if err := e.replicate(ctx, name, r); err != nil && ctx.Err() == nil {
					e.recordFailure(ctx, r, err)
				}
			}
		})
	}
	for _, r := range due {
		work <- r
	}
	close(work)
	wg.Wait()
	return len(due), ctx.Err()
}

func (e *Engine) recordFailure(ctx context.Context, r index.Replica, cause error) {
	attempts := r.Attempts + 1
	if errors.Is(cause, errSourceMissing) {
		attempts = index.MaxAttempts
	}
	retryAt := e.now().Add(retryDelay(attempts))
	e.log.Warn("mirror copy failed", "store", r.Store, "path", r.Path, "attempt", attempts, "err", cause)
	if err := e.idx.MarkFailed(ctx, r.Path, r.Store, cause, attempts, retryAt); err != nil {
		e.log.Error("record failure", "err", err)
	}
}

// replicate makes mirror name match the index for one path: delete a
// tombstoned file, or copy the current content from the primary and verify it.
func (e *Engine) replicate(ctx context.Context, name string, r index.Replica) error {
	mirror := e.mirrors[name]
	f, err := e.idx.File(ctx, r.Path)
	if err != nil {
		return err
	}
	if f == nil {
		return e.idx.DropReplica(ctx, r.Path, name)
	}
	if f.Deleted {
		if e.dryRun {
			e.log.Info("would delete", "path", r.Path, "store", name)
			return nil
		}
		if e.cfg.Sync.DeleteRemote {
			if err := mirror.Delete(ctx, r.Path); err != nil {
				return err
			}
			e.log.Info("deleted", "path", r.Path, "store", name)
		}
		return e.idx.DropReplica(ctx, r.Path, name)
	}

	if head, err := mirror.Head(ctx, r.Path); err == nil && head.Size == f.Size && head.SHA256 == f.SHA256 {
		_, err := e.idx.MarkVerified(ctx, r.Path, name, head.ETag, f.SHA256)
		return err
	}
	if e.dryRun {
		e.log.Info("would copy", "path", r.Path, "from", e.primaryName, "to", name)
		return nil
	}

	body, meta, err := e.source(ctx, *f)
	if err != nil {
		return err
	}
	defer body.Close()
	if _, err := mirror.Put(ctx, r.Path, body, f.Size, meta); err != nil {
		return err
	}
	head, err := mirror.Head(ctx, r.Path)
	if err != nil {
		return fmt.Errorf("verify: %w", err)
	}
	if head.SHA256 != meta.SHA256 {
		return fmt.Errorf("verify: stored sha256 %s, expected %s", head.SHA256, meta.SHA256)
	}
	verified, err := e.idx.MarkVerified(ctx, r.Path, name, head.ETag, meta.SHA256)
	if err == nil && verified {
		e.log.Info("copied", "path", r.Path, "from", e.primaryName, "to", name, "bytes", head.Size)
	}
	return err
}

// source opens the content a mirror should receive: the primary's copy, or
// the local file when the primary lacks it but the file is unchanged.
func (e *Engine) source(ctx context.Context, f index.File) (io.ReadCloser, store.Meta, error) {
	body, obj, err := e.primary.Get(ctx, f.Path)
	if err == nil {
		sum := obj.SHA256
		if sum == "" {
			sum = f.SHA256
		}
		return body, store.Meta{SHA256: sum, ModTime: time.Unix(0, f.MTimeNs)}, nil
	}
	if !errors.Is(err, store.ErrNotFound) {
		return nil, store.Meta{}, err
	}
	local, lerr := e.local(f.Path, &f)
	if lerr != nil || !local.exists || local.changed {
		return nil, store.Meta{}, errSourceMissing
	}
	file, err := os.Open(e.abs(f.Path))
	if err != nil {
		return nil, store.Meta{}, err
	}
	return file, store.Meta{SHA256: f.SHA256, ModTime: time.Unix(0, f.MTimeNs)}, nil
}

// reconcileMirror lists a mirror and corrects replica rows: copies already
// there, for example from rclone or bucket replication, become verified, and
// verified copies that disappeared go back to pending.
func (e *Engine) reconcileMirror(ctx context.Context, name string) error {
	mirror := e.mirrors[name]
	objects, err := mirror.List(ctx)
	if err != nil {
		return err
	}
	present := map[string]store.Object{}
	for _, o := range objects {
		present[o.Key] = o
	}
	rows, err := e.idx.ReplicasOn(ctx, name)
	if err != nil {
		return err
	}
	for _, r := range rows {
		if _, ok := present[r.Path]; r.State == index.Verified && !ok {
			if err := e.idx.MarkPending(ctx, r.Path, name); err != nil {
				return err
			}
		}
	}
	if _, err := e.idx.Backfill(ctx, name); err != nil {
		return err
	}
	rows, err = e.idx.Due(ctx, name, 1<<30)
	if err != nil {
		return err
	}
	adopted := 0
	for _, r := range rows {
		obj, ok := present[r.Path]
		if !ok {
			continue
		}
		f, err := e.idx.File(ctx, r.Path)
		if err != nil {
			return err
		}
		if f == nil || f.Deleted || f.Size != obj.Size {
			continue
		}
		head, err := mirror.Head(ctx, r.Path)
		if err != nil || head.SHA256 != f.SHA256 {
			continue
		}
		if ok, err := e.idx.MarkVerified(ctx, r.Path, name, head.ETag, f.SHA256); err != nil {
			return err
		} else if ok {
			adopted++
		}
	}
	if adopted > 0 {
		e.log.Info("adopted existing copies", "store", name, "files", adopted)
	}
	return nil
}
