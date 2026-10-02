// Package watch reports changed paths under a root, recursively, using
// inotify on Linux and kqueue on macOS through fsnotify.
package watch

import (
	"context"
	"errors"
	"fmt"
	"io/fs"
	"log/slog"
	"os"
	"path/filepath"
	"runtime"
	"syscall"

	"github.com/fsnotify/fsnotify"
)

// Watcher calls changed with the slash-separated relative path of every file
// or directory that was created, written, removed or renamed. Ignored paths
// are filtered out, and ignored directories are not watched.
type Watcher struct {
	root    string
	ignored func(rel string, isDir bool) bool
	changed func(rel string)
	log     *slog.Logger
	fs      *fsnotify.Watcher
}

func New(root string, ignored func(string, bool) bool, changed func(string), log *slog.Logger) (*Watcher, error) {
	fsw, err := fsnotify.NewWatcher()
	if err != nil {
		return nil, err
	}
	w := &Watcher{root: root, ignored: ignored, changed: changed, log: log, fs: fsw}
	if err := w.addTree(root, false); err != nil {
		fsw.Close()
		return nil, err
	}
	return w, nil
}

// Run delivers events until ctx is done.
func (w *Watcher) Run(ctx context.Context) error {
	defer w.fs.Close()
	for {
		select {
		case <-ctx.Done():
			return nil
		case err := <-w.fs.Errors:
			w.log.Warn("watch error", "err", err)
		case ev := <-w.fs.Events:
			w.handle(ev)
		}
	}
}

func (w *Watcher) handle(ev fsnotify.Event) {
	rel, ok := w.rel(ev.Name)
	if !ok {
		return
	}
	info, err := os.Lstat(ev.Name)
	isDir := err == nil && info.IsDir()
	if w.ignored(rel, isDir) {
		return
	}
	if isDir && ev.Has(fsnotify.Create) {
		// Files can land in a new directory before its watch is added, so
		// everything already inside is reported too.
		if err := w.addTree(ev.Name, true); err != nil {
			w.log.Error("watch new directory", "path", rel, "err", err)
		}
		return
	}
	if isDir {
		return
	}
	w.changed(rel)
}

func (w *Watcher) addTree(dir string, report bool) error {
	return filepath.WalkDir(dir, func(p string, entry fs.DirEntry, err error) error {
		if err != nil {
			if errors.Is(err, fs.ErrNotExist) {
				return nil
			}
			return err
		}
		rel, ok := w.rel(p)
		if ok && w.ignored(rel, entry.IsDir()) {
			if entry.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if !entry.IsDir() {
			if report && ok {
				w.changed(rel)
			}
			return nil
		}
		if err := w.fs.Add(p); err != nil {
			return watchLimitHint(p, err)
		}
		return nil
	})
}

func (w *Watcher) rel(p string) (string, bool) {
	rel, err := filepath.Rel(w.root, p)
	if err != nil || rel == "." {
		return "", false
	}
	return filepath.ToSlash(rel), true
}

func watchLimitHint(p string, err error) error {
	switch {
	case runtime.GOOS == "linux" && errors.Is(err, syscall.ENOSPC):
		return fmt.Errorf("watch %s: inotify watch limit reached; raise it with `sudo sysctl fs.inotify.max_user_watches=1048576`: %w", p, err)
	case errors.Is(err, syscall.EMFILE):
		return fmt.Errorf("watch %s: out of file descriptors (one per directory with kqueue); raise `ulimit -n`: %w", p, err)
	}
	return fmt.Errorf("watch %s: %w", p, err)
}
