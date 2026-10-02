// Package engine keeps a local folder, the primary store and every mirror in
// sync. See POC.md for the decision tables it implements.
package engine

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"dbox/internal/config"
	"dbox/internal/debounce"
	"dbox/internal/ignore"
	"dbox/internal/index"
	"dbox/internal/store"
	"dbox/internal/watch"
)

// Engine syncs one root. Build it with New, then call Run for the daemon or
// Once for a single reconcile.
type Engine struct {
	cfg         *config.Config
	root        string
	idx         *index.Index
	log         *slog.Logger
	dryRun      bool
	host        string
	ignore      *ignore.Matcher
	primaryName string
	primary     store.Store
	mirrors     map[string]store.Store
	mirrorNames []string
	queue       *queue
	locks       *pathLocks
	wake        map[string]chan struct{}
	now         func() time.Time
}

type Options struct {
	DryRun bool
	Log    *slog.Logger
}

func New(cfg *config.Config, idx *index.Index, stores map[string]store.Store, opts Options) *Engine {
	host, _ := os.Hostname()
	if i := strings.IndexByte(host, '.'); i > 0 {
		host = host[:i]
	}
	log := opts.Log
	if log == nil {
		log = slog.Default()
	}
	e := &Engine{
		cfg:         cfg,
		root:        cfg.Sync.Root,
		idx:         idx,
		log:         log,
		dryRun:      opts.DryRun,
		host:        host,
		ignore:      ignore.New(cfg.Sync.Ignore),
		primaryName: cfg.Primary(),
		primary:     stores[cfg.Primary()],
		mirrors:     map[string]store.Store{},
		mirrorNames: cfg.Mirrors(),
		queue:       newQueue(),
		locks:       newPathLocks(),
		wake:        map[string]chan struct{}{},
		now:         time.Now,
	}
	for _, name := range e.mirrorNames {
		e.mirrors[name] = stores[name]
		e.wake[name] = make(chan struct{}, 1)
	}
	return e
}

// Once reconciles the folder with every store, drains the mirror queues and
// returns. It is `dbox run --once`.
func (e *Engine) Once(ctx context.Context) error {
	if err := e.Reconcile(ctx); err != nil {
		return err
	}
	if e.dryRun {
		return nil
	}
	var errs []error
	for _, name := range e.mirrorNames {
		if err := e.drainMirror(ctx, name); err != nil {
			errs = append(errs, err)
		}
	}
	return errors.Join(errs...)
}

// Run reconciles, then watches the folder, polls the primary and copies to
// mirrors until ctx is done.
func (e *Engine) Run(ctx context.Context) error {
	if err := os.MkdirAll(e.root, 0o755); err != nil {
		return err
	}
	if err := e.Reconcile(ctx); err != nil {
		return err
	}

	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	var wg sync.WaitGroup
	start := func(fn func()) {
		wg.Add(1)
		go func() { defer wg.Done(); fn() }()
	}

	for range e.cfg.Stores[e.primaryName].Workers {
		start(func() { e.primaryWorker(ctx) })
	}
	for _, name := range e.mirrorNames {
		start(func() { e.mirrorLoop(ctx, name) })
	}

	debouncer := debounce.New(e.cfg.Sync.Debounce.D(), e.queue.push)
	defer debouncer.Stop()
	watcher, err := watch.New(e.root, e.ignore.Match, debouncer.Trigger, e.log)
	if err != nil {
		cancel()
		wg.Wait()
		return err
	}
	start(func() { watcher.Run(ctx) })
	start(func() { e.every(ctx, e.cfg.Sync.PullInterval.D(), "poll", e.poll) })
	start(func() { e.every(ctx, e.cfg.Sync.BackfillInterval.D(), "backfill", e.backfill) })

	e.log.Info("watching", "root", e.root, "primary", e.primaryName, "mirrors", e.mirrorNames)
	<-ctx.Done()
	wg.Wait()
	return nil
}

func (e *Engine) every(ctx context.Context, interval time.Duration, name string, fn func(context.Context) error) {
	t := time.NewTicker(interval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			if err := fn(ctx); err != nil && ctx.Err() == nil {
				e.log.Error(name, "err", err)
			}
		}
	}
}

// Reconcile brings the folder and the primary in line, adopts copies already
// on mirrors, and queues backfill for what mirrors lack. Remote changes are
// pulled first so a file changed on both sides is seen as a conflict rather
// than overwritten by the local walk.
func (e *Engine) Reconcile(ctx context.Context) error {
	if err := os.MkdirAll(e.root, 0o755); err != nil {
		return err
	}
	if err := e.poll(ctx); err != nil {
		return fmt.Errorf("pull from %s: %w", e.primaryName, err)
	}
	if err := e.pushAll(ctx); err != nil {
		return err
	}
	for _, name := range e.mirrorNames {
		if err := e.reconcileMirror(ctx, name); err != nil {
			e.log.Error("reconcile mirror", "store", name, "err", err)
		}
	}
	return e.backfill(ctx)
}

// pushAll syncs every local file and every indexed file that is gone locally.
func (e *Engine) pushAll(ctx context.Context) error {
	var paths []string
	err := filepath.WalkDir(e.root, func(p string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, ok := e.rel(p)
		if !ok {
			return nil
		}
		if e.ignore.Match(rel, entry.IsDir()) {
			if entry.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if entry.Type().IsRegular() {
			paths = append(paths, rel)
		}
		return nil
	})
	if err != nil {
		return err
	}
	files, err := e.idx.Files(ctx)
	if err != nil {
		return err
	}
	for _, f := range files {
		if !f.Deleted {
			paths = append(paths, f.Path)
		}
	}

	work := make(chan string)
	var wg sync.WaitGroup
	for range e.cfg.Stores[e.primaryName].Workers {
		wg.Go(func() {
			for p := range work {
				e.process(ctx, p)
			}
		})
	}
	seen := map[string]bool{}
	for _, p := range paths {
		if !seen[p] {
			seen[p] = true
			work <- p
		}
	}
	close(work)
	wg.Wait()
	return ctx.Err()
}

func (e *Engine) primaryWorker(ctx context.Context) {
	for {
		p, ok := e.queue.pop(ctx.Done())
		if !ok {
			return
		}
		e.process(ctx, p)
	}
}

// process syncs one path under its lock, and retries failures later.
func (e *Engine) process(ctx context.Context, rel string) {
	if !e.locks.acquire(rel) {
		return
	}
	err := e.push(ctx, rel)
	if e.locks.release(rel) {
		e.queue.push(rel)
	}
	if err != nil && ctx.Err() == nil {
		e.log.Error("sync", "path", rel, "err", err)
		time.AfterFunc(30*time.Second, func() { e.queue.push(rel) })
	}
}

// push makes the primary match the local file at rel.
func (e *Engine) push(ctx context.Context, rel string) error {
	info, err := os.Lstat(e.abs(rel))
	if errors.Is(err, fs.ErrNotExist) {
		return e.pushDelete(ctx, rel)
	}
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return nil
	}
	f, err := e.idx.File(ctx, rel)
	if err != nil {
		return err
	}
	primaryCurrent := false
	if f != nil && !f.Deleted {
		r, err := e.idx.Replica(ctx, rel, e.primaryName)
		if err != nil {
			return err
		}
		primaryCurrent = r != nil && r.State == index.Verified
	}
	if primaryCurrent && f.Size == info.Size() && f.MTimeNs == info.ModTime().UnixNano() {
		return nil
	}
	sum, err := hashFile(e.abs(rel))
	if err != nil {
		return err
	}
	if primaryCurrent && f.SHA256 == sum {
		return e.idx.Touch(ctx, rel, info.ModTime().UnixNano())
	}
	return e.upload(ctx, rel, sum)
}

func (e *Engine) upload(ctx context.Context, rel, sum string) error {
	if e.dryRun {
		e.log.Info("would upload", "path", rel, "store", e.primaryName)
		return nil
	}
	file, err := os.Open(e.abs(rel))
	if err != nil {
		return err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return err
	}
	etag, err := e.primary.Put(ctx, rel, file, info.Size(), store.Meta{SHA256: sum, ModTime: info.ModTime()})
	if err != nil {
		return fmt.Errorf("upload to %s: %w", e.primaryName, err)
	}
	record := index.File{Path: rel, Size: info.Size(), MTimeNs: info.ModTime().UnixNano(), SHA256: sum}
	if err := e.idx.RecordSynced(ctx, record, e.primaryName, etag, e.mirrorNames); err != nil {
		return err
	}
	e.log.Info("uploaded", "path", rel, "store", e.primaryName, "bytes", info.Size())
	e.wakeMirrors()
	return nil
}

// pushDelete handles a path that is gone locally: a deleted file, or a
// directory that was removed or renamed with everything in it.
func (e *Engine) pushDelete(ctx context.Context, rel string) error {
	f, err := e.idx.File(ctx, rel)
	if err != nil {
		return err
	}
	if f == nil {
		inside, err := e.idx.FilesUnder(ctx, rel)
		for _, child := range inside {
			e.queue.push(child.Path)
		}
		return err
	}
	if f.Deleted {
		return nil
	}
	if e.dryRun {
		e.log.Info("would delete", "path", rel, "store", e.primaryName)
		return nil
	}
	if e.cfg.Sync.DeleteRemote {
		if err := e.primary.Delete(ctx, rel); err != nil {
			return fmt.Errorf("delete from %s: %w", e.primaryName, err)
		}
		e.log.Info("deleted", "path", rel, "store", e.primaryName)
	}
	if err := e.idx.Tombstone(ctx, rel, e.primaryName, !e.cfg.Sync.DeleteRemote); err != nil {
		return err
	}
	e.wakeMirrors()
	return nil
}

// poll pulls changes from the primary. See the sync table in POC.md.
func (e *Engine) poll(ctx context.Context) error {
	objects, err := e.primary.List(ctx)
	if err != nil {
		return err
	}
	remote := map[string]bool{}
	for _, obj := range objects {
		if e.ignore.Match(obj.Key, false) {
			continue
		}
		remote[obj.Key] = true
		e.withLock(obj.Key, func() error { return e.pull(ctx, obj) })
	}
	files, err := e.idx.Files(ctx)
	if err != nil {
		return err
	}
	for _, f := range files {
		if f.Deleted || remote[f.Path] || e.ignore.Match(f.Path, false) {
			continue
		}
		e.withLock(f.Path, func() error { return e.remoteGone(ctx, f) })
	}
	return ctx.Err()
}

func (e *Engine) withLock(rel string, fn func() error) {
	if !e.locks.acquire(rel) {
		return
	}
	err := fn()
	if e.locks.release(rel) {
		e.queue.push(rel)
	}
	if err != nil {
		e.log.Error("pull", "path", rel, "err", err)
	}
}

func (e *Engine) pull(ctx context.Context, obj store.Object) error {
	r, err := e.idx.Replica(ctx, obj.Key, e.primaryName)
	if err != nil {
		return err
	}
	if r != nil && r.State == index.Verified && r.ETag == obj.ETag {
		return nil
	}
	f, err := e.idx.File(ctx, obj.Key)
	if err != nil {
		return err
	}
	if f != nil && f.Deleted {
		f = nil
	}
	local, err := e.local(obj.Key, f)
	if err != nil {
		return err
	}
	switch {
	case !local.exists:
		return e.download(ctx, obj.Key, obj.Key)
	case f == nil:
		return e.adoptOrConflict(ctx, obj, local)
	case !local.changed:
		return e.download(ctx, obj.Key, obj.Key)
	default:
		return e.conflict(ctx, obj.Key)
	}
}

// adoptOrConflict handles a file that exists both locally and remotely but was
// never synced from here, such as the first run on a second machine.
func (e *Engine) adoptOrConflict(ctx context.Context, obj store.Object, local localFile) error {
	head, err := e.primary.Head(ctx, obj.Key)
	if err != nil {
		return err
	}
	sum, err := hashFile(e.abs(obj.Key))
	if err != nil {
		return err
	}
	if head.SHA256 != sum {
		return e.conflict(ctx, obj.Key)
	}
	if e.dryRun {
		e.log.Info("would adopt", "path", obj.Key)
		return nil
	}
	record := index.File{Path: obj.Key, Size: local.size, MTimeNs: local.mtimeNs, SHA256: sum}
	return e.idx.RecordSynced(ctx, record, e.primaryName, head.ETag, e.mirrorNames)
}

// conflict keeps the local file and saves the remote version beside it.
func (e *Engine) conflict(ctx context.Context, rel string) error {
	ext := path.Ext(rel)
	copyName := fmt.Sprintf("%s.conflict-%s-%s%s", strings.TrimSuffix(rel, ext), e.host, e.now().UTC().Format("20060102T150405Z"), ext)
	if e.dryRun {
		e.log.Info("would save conflict copy", "path", rel, "copy", copyName)
		return nil
	}
	if err := e.downloadTo(ctx, rel, copyName, false); err != nil {
		return err
	}
	e.log.Warn("conflict: kept local version, saved remote beside it", "path", rel, "copy", copyName)
	e.queue.push(copyName)
	sum, err := hashFile(e.abs(rel))
	if err != nil {
		return err
	}
	return e.upload(ctx, rel, sum)
}

// remoteGone handles an indexed file that is no longer on the primary.
func (e *Engine) remoteGone(ctx context.Context, f index.File) error {
	r, err := e.idx.Replica(ctx, f.Path, e.primaryName)
	if err != nil {
		return err
	}
	if r == nil || r.State != index.Verified {
		// Never reached this primary, for example right after a promotion.
		e.queue.push(f.Path)
		return nil
	}
	local, err := e.local(f.Path, &f)
	if err != nil {
		return err
	}
	switch {
	case !local.exists:
		return e.idx.Tombstone(ctx, f.Path, e.primaryName, false)
	case local.changed:
		e.queue.push(f.Path)
		return nil
	case !e.cfg.Sync.DeleteLocal:
		if err := e.idx.DropReplica(ctx, f.Path, e.primaryName); err != nil {
			return err
		}
		e.queue.push(f.Path)
		return nil
	}
	if e.dryRun {
		e.log.Info("would delete local", "path", f.Path)
		return nil
	}
	if err := os.Remove(e.abs(f.Path)); err != nil && !errors.Is(err, fs.ErrNotExist) {
		return err
	}
	e.log.Info("deleted locally, gone from primary", "path", f.Path)
	if err := e.idx.Tombstone(ctx, f.Path, e.primaryName, false); err != nil {
		return err
	}
	e.wakeMirrors()
	return nil
}

func (e *Engine) download(ctx context.Context, key, rel string) error {
	if e.dryRun {
		e.log.Info("would download", "path", rel, "store", e.primaryName)
		return nil
	}
	return e.downloadTo(ctx, key, rel, true)
}

// downloadTo fetches key into a temp file and renames it to rel. With record,
// the index is updated before the rename so the watcher event that follows
// finds nothing to upload.
func (e *Engine) downloadTo(ctx context.Context, key, rel string, record bool) error {
	body, obj, err := e.primary.Get(ctx, key)
	if err != nil {
		return err
	}
	defer body.Close()
	tmpDir := filepath.Join(e.cfg.StateDir(), "tmp")
	if err := os.MkdirAll(tmpDir, 0o755); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(tmpDir, "download-")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	h := sha256.New()
	if _, err := io.Copy(io.MultiWriter(tmp, h), body); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if !obj.ModTime.IsZero() {
		if err := os.Chtimes(tmp.Name(), obj.ModTime, obj.ModTime); err != nil {
			return err
		}
	}
	info, err := os.Stat(tmp.Name())
	if err != nil {
		return err
	}
	if record {
		f := index.File{Path: rel, Size: info.Size(), MTimeNs: info.ModTime().UnixNano(), SHA256: hex.EncodeToString(h.Sum(nil))}
		if err := e.idx.RecordSynced(ctx, f, e.primaryName, obj.ETag, e.mirrorNames); err != nil {
			return err
		}
	}
	if err := os.MkdirAll(filepath.Dir(e.abs(rel)), 0o755); err != nil {
		return err
	}
	if err := os.Rename(tmp.Name(), e.abs(rel)); err != nil {
		return err
	}
	e.log.Info("downloaded", "path", rel, "store", e.primaryName, "bytes", info.Size())
	return nil
}

type localFile struct {
	exists  bool
	changed bool
	size    int64
	mtimeNs int64
}

// local describes the file at rel relative to its index row f, which may be
// nil. Size and time decide first; content is hashed only when they differ.
func (e *Engine) local(rel string, f *index.File) (localFile, error) {
	info, err := os.Lstat(e.abs(rel))
	if errors.Is(err, fs.ErrNotExist) {
		return localFile{}, nil
	}
	if err != nil {
		return localFile{}, err
	}
	l := localFile{exists: true, changed: true, size: info.Size(), mtimeNs: info.ModTime().UnixNano()}
	if f == nil {
		return l, nil
	}
	if f.Size == l.size && f.MTimeNs == l.mtimeNs {
		l.changed = false
		return l, nil
	}
	sum, err := hashFile(e.abs(rel))
	if err != nil {
		return l, err
	}
	l.changed = sum != f.SHA256
	return l, nil
}

func (e *Engine) backfill(ctx context.Context) error {
	for _, name := range e.mirrorNames {
		n, err := e.idx.Backfill(ctx, name)
		if err != nil {
			return err
		}
		if n > 0 {
			e.log.Info("backfill", "store", name, "files", n)
		}
	}
	e.wakeMirrors()
	return nil
}

func (e *Engine) wakeMirrors() {
	for _, ch := range e.wake {
		select {
		case ch <- struct{}{}:
		default:
		}
	}
}

func (e *Engine) abs(rel string) string { return filepath.Join(e.root, filepath.FromSlash(rel)) }

func (e *Engine) rel(p string) (string, bool) {
	rel, err := filepath.Rel(e.root, p)
	if err != nil || rel == "." {
		return "", false
	}
	return filepath.ToSlash(rel), true
}

func hashFile(p string) (string, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
