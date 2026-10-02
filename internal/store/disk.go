package store

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

const diskTempPrefix = ".dbox-upload-"

// Disk stores objects as files under root/prefix. The ETag is size and
// modification time; the content hash is computed on Head.
type Disk struct {
	dir string
}

func NewDisk(root, prefix string) *Disk {
	return &Disk{dir: filepath.Join(root, filepath.FromSlash(prefix))}
}

func (d *Disk) path(key string) string { return filepath.Join(d.dir, filepath.FromSlash(key)) }

func (d *Disk) Put(_ context.Context, key string, body io.Reader, _ int64, meta Meta) (string, error) {
	target := d.path(key)
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		return "", err
	}
	tmp, err := os.CreateTemp(filepath.Dir(target), diskTempPrefix)
	if err != nil {
		return "", err
	}
	defer os.Remove(tmp.Name())
	if _, err := io.Copy(tmp, body); err != nil {
		tmp.Close()
		return "", err
	}
	if err := tmp.Close(); err != nil {
		return "", err
	}
	if !meta.ModTime.IsZero() {
		if err := os.Chtimes(tmp.Name(), meta.ModTime, meta.ModTime); err != nil {
			return "", err
		}
	}
	if err := os.Rename(tmp.Name(), target); err != nil {
		return "", err
	}
	info, err := os.Stat(target)
	if err != nil {
		return "", err
	}
	return diskETag(info), nil
}

func (d *Disk) Get(_ context.Context, key string) (io.ReadCloser, Object, error) {
	f, err := os.Open(d.path(key))
	if errors.Is(err, fs.ErrNotExist) {
		return nil, Object{}, ErrNotFound
	}
	if err != nil {
		return nil, Object{}, err
	}
	info, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, Object{}, err
	}
	return f, diskObject(key, info), nil
}

func (d *Disk) Head(_ context.Context, key string) (Object, error) {
	f, err := os.Open(d.path(key))
	if errors.Is(err, fs.ErrNotExist) {
		return Object{}, ErrNotFound
	}
	if err != nil {
		return Object{}, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return Object{}, err
	}
	if !info.Mode().IsRegular() {
		return Object{}, ErrNotFound
	}
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return Object{}, err
	}
	obj := diskObject(key, info)
	obj.SHA256 = hex.EncodeToString(h.Sum(nil))
	return obj, nil
}

func (d *Disk) Delete(_ context.Context, key string) error {
	err := os.Remove(d.path(key))
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	return err
}

func (d *Disk) List(_ context.Context) ([]Object, error) {
	var objects []Object
	err := filepath.WalkDir(d.dir, func(p string, entry fs.DirEntry, err error) error {
		if errors.Is(err, fs.ErrNotExist) && p == d.dir {
			return filepath.SkipDir
		}
		if err != nil {
			return err
		}
		if !entry.Type().IsRegular() || strings.HasPrefix(entry.Name(), diskTempPrefix) {
			return nil
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(d.dir, p)
		if err != nil {
			return err
		}
		objects = append(objects, diskObject(filepath.ToSlash(rel), info))
		return nil
	})
	return objects, err
}

func diskObject(key string, info fs.FileInfo) Object {
	return Object{Key: key, Size: info.Size(), ETag: diskETag(info), ModTime: info.ModTime()}
}

func diskETag(info fs.FileInfo) string {
	return fmt.Sprintf("%d-%d", info.Size(), info.ModTime().UnixNano())
}

var _ Store = (*Disk)(nil)
