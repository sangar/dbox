// Package store is the backend a folder is synced to: an S3-compatible bucket
// or a directory. Keys are paths relative to the synced root with forward
// slashes; each store applies its own prefix.
package store

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"
	"time"

	"dbox/internal/config"
)

var ErrNotFound = errors.New("object not found")

// Object describes a stored file. SHA256 is empty in List results from S3,
// which does not return metadata there; Head fills it in.
type Object struct {
	Key     string
	Size    int64
	ETag    string
	SHA256  string
	ModTime time.Time
}

// Meta travels with every Put so other machines and mirrors can compare
// content without downloading it.
type Meta struct {
	SHA256  string
	ModTime time.Time
}

type Store interface {
	Put(ctx context.Context, key string, body io.Reader, size int64, meta Meta) (etag string, err error)
	Get(ctx context.Context, key string) (io.ReadCloser, Object, error)
	Head(ctx context.Context, key string) (Object, error)
	Delete(ctx context.Context, key string) error
	List(ctx context.Context) ([]Object, error)
}

// Open builds the store described by cfg.
func Open(ctx context.Context, cfg config.Store, partSize int64) (Store, error) {
	switch cfg.Kind {
	case config.KindS3:
		return NewS3(ctx, cfg, partSize)
	case config.KindDisk:
		return NewDisk(cfg.Root, cfg.Prefix), nil
	}
	return nil, fmt.Errorf("unknown store kind %q", cfg.Kind)
}

// OpenAll builds every configured store, keyed by name.
func OpenAll(ctx context.Context, cfg *config.Config) (map[string]Store, error) {
	stores := map[string]Store{}
	names := make([]string, 0, len(cfg.Stores))
	for name := range cfg.Stores {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		s, err := Open(ctx, cfg.Stores[name], int64(cfg.Sync.PartSize))
		if err != nil {
			return nil, fmt.Errorf("store %s: %w", name, err)
		}
		stores[name] = s
	}
	return stores, nil
}

// Check writes, reads back and deletes a probe object.
func Check(ctx context.Context, s Store) error {
	const content = "dbox probe"
	key := fmt.Sprintf(".dbox-probe-%d", time.Now().UnixNano())
	sum := sha256.Sum256([]byte(content))
	meta := Meta{SHA256: hex.EncodeToString(sum[:]), ModTime: time.Now()}
	if _, err := s.Put(ctx, key, strings.NewReader(content), int64(len(content)), meta); err != nil {
		return fmt.Errorf("write: %w", err)
	}
	body, _, err := s.Get(ctx, key)
	if err != nil {
		return fmt.Errorf("read back: %w", err)
	}
	got, err := io.ReadAll(body)
	body.Close()
	if err != nil {
		return fmt.Errorf("read back: %w", err)
	}
	if string(got) != content {
		return errors.New("read back different content than was written")
	}
	if err := s.Delete(ctx, key); err != nil {
		return fmt.Errorf("delete: %w", err)
	}
	return nil
}
