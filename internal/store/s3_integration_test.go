//go:build integration

package store

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
	"testing"
	"time"

	"dbox/internal/config"
)

// minio returns a store on the docker-compose MinIO under a fresh prefix.
func minio(t *testing.T, endpoint string) *S3 {
	t.Helper()
	cfg := config.Store{
		Kind: config.KindS3, Bucket: "dbox", Region: "us-east-1", Endpoint: endpoint, PathStyle: true,
		AccessKey: "minioadmin", SecretKey: "minioadmin", Prefix: fmt.Sprintf("test-%d/", time.Now().UnixNano()),
	}
	s, err := NewS3(context.Background(), cfg, 5<<20)
	if err != nil {
		t.Fatal(err)
	}
	if err := s.CreateBucket(context.Background()); err != nil {
		t.Fatalf("%s: %v (run `make minio`)", endpoint, err)
	}
	return s
}

func TestS3RoundTripKeepsMetadata(t *testing.T) {
	ctx := context.Background()
	s := minio(t, "http://localhost:9200")
	if err := Check(ctx, s); err != nil {
		t.Fatal(err)
	}
	mtime := time.Unix(1_700_000_000, 123)
	etag, err := s.Put(ctx, "dir/a.txt", strings.NewReader("hello"), 5, Meta{SHA256: "abc", ModTime: mtime})
	if err != nil {
		t.Fatal(err)
	}
	head, err := s.Head(ctx, "dir/a.txt")
	if err != nil {
		t.Fatal(err)
	}
	if head.ETag != etag || head.SHA256 != "abc" || !head.ModTime.Equal(mtime) || head.Size != 5 {
		t.Errorf("head = %+v, put etag %s", head, etag)
	}
	list, err := s.List(ctx)
	if err != nil || len(list) != 1 || list[0].Key != "dir/a.txt" || list[0].ETag != etag {
		t.Errorf("list = %+v, %v", list, err)
	}
	if err := s.Delete(ctx, "dir/a.txt"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Head(ctx, "dir/a.txt"); !errors.Is(err, ErrNotFound) {
		t.Errorf("head after delete: %v", err)
	}
	if _, _, err := s.Get(ctx, "dir/a.txt"); !errors.Is(err, ErrNotFound) {
		t.Errorf("get after delete: %v", err)
	}
}

func TestS3MultipartFromUnseekableStream(t *testing.T) {
	ctx := context.Background()
	a, b := minio(t, "http://localhost:9200"), minio(t, "http://localhost:9300")
	const size = 12 << 20
	f, err := os.CreateTemp(t.TempDir(), "big")
	if err != nil {
		t.Fatal(err)
	}
	f.Write([]byte(strings.Repeat("0123456789abcdef", size/16)))
	f.Seek(0, io.SeekStart)
	if _, err := a.Put(ctx, "big.bin", f, size, Meta{SHA256: "big"}); err != nil {
		t.Fatal(err)
	}
	body, _, err := a.Get(ctx, "big.bin")
	if err != nil {
		t.Fatal(err)
	}
	defer body.Close()
	// A mirror copy streams one store's Get body straight into another's Put.
	if _, err := b.Put(ctx, "big.bin", body, size, Meta{SHA256: "big"}); err != nil {
		t.Fatal(err)
	}
	head, err := b.Head(ctx, "big.bin")
	if err != nil || head.Size != size || head.SHA256 != "big" {
		t.Errorf("head = %+v, %v", head, err)
	}
}
