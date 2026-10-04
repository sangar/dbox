package store

import (
	"context"
	"errors"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/aws/aws-sdk-go-v2/aws"
	awshttp "github.com/aws/aws-sdk-go-v2/aws/transport/http"
	awsconfig "github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/feature/s3/transfermanager"
	tmtypes "github.com/aws/aws-sdk-go-v2/feature/s3/transfermanager/types"
	"github.com/aws/aws-sdk-go-v2/service/s3"
	"github.com/aws/aws-sdk-go-v2/service/s3/types"

	"dbox/internal/config"
)

const (
	metaSHA256 = "sha256"
	metaMTime  = "mtime"
)

// S3 stores objects in a bucket under a prefix. Content hash and modification
// time ride along as object metadata.
type S3 struct {
	client       *s3.Client
	uploader     *transfermanager.Client
	bucket       string
	prefix       string
	storageClass types.StorageClass
}

func NewS3(ctx context.Context, cfg config.Store, partSize int64) (*S3, error) {
	opts := []func(*awsconfig.LoadOptions) error{awsconfig.WithRegion(cfg.Region)}
	if cfg.AccessKey != "" {
		opts = append(opts, awsconfig.WithCredentialsProvider(credentials.NewStaticCredentialsProvider(cfg.AccessKey, cfg.SecretKey, "")))
	}
	awsCfg, err := awsconfig.LoadDefaultConfig(ctx, opts...)
	if err != nil {
		return nil, err
	}
	client := s3.NewFromConfig(awsCfg, func(o *s3.Options) {
		if cfg.Endpoint != "" {
			o.BaseEndpoint = aws.String(cfg.Endpoint)
		}
		o.UsePathStyle = cfg.PathStyle
		// R2, B2 and older MinIO reject the checksum headers the SDK adds by default.
		o.RequestChecksumCalculation = aws.RequestChecksumCalculationWhenRequired
		o.ResponseChecksumValidation = aws.ResponseChecksumValidationWhenRequired
	})
	return &S3{
		client: client,
		uploader: transfermanager.New(client, func(o *transfermanager.Options) {
			o.PartSizeBytes = partSize
			o.MultipartUploadThreshold = partSize
			// Overrides the client setting above unless set here too.
			o.RequestChecksumCalculation = aws.RequestChecksumCalculationWhenRequired
		}),
		bucket:       cfg.Bucket,
		prefix:       cfg.Prefix,
		storageClass: types.StorageClass(cfg.StorageClass),
	}, nil
}

// CreateBucket creates the bucket if it does not exist yet.
func (s *S3) CreateBucket(ctx context.Context) error {
	_, err := s.client.CreateBucket(ctx, &s3.CreateBucketInput{Bucket: &s.bucket})
	var owned *types.BucketAlreadyOwnedByYou
	if errors.As(err, &owned) {
		return nil
	}
	return err
}

func (s *S3) Put(ctx context.Context, key string, body io.Reader, _ int64, meta Meta) (string, error) {
	out, err := s.uploader.UploadObject(ctx, &transfermanager.UploadObjectInput{
		Bucket:       &s.bucket,
		Key:          aws.String(s.prefix + key),
		Body:         body,
		Metadata:     metadata(meta),
		StorageClass: tmtypes.StorageClass(s.storageClass),
	})
	if err != nil {
		return "", err
	}
	return etag(out.ETag), nil
}

func (s *S3) Get(ctx context.Context, key string) (io.ReadCloser, Object, error) {
	out, err := s.client.GetObject(ctx, &s3.GetObjectInput{Bucket: &s.bucket, Key: aws.String(s.prefix + key)})
	if err != nil {
		return nil, Object{}, notFound(err)
	}
	obj := Object{Key: key, Size: aws.ToInt64(out.ContentLength), ETag: etag(out.ETag), ModTime: aws.ToTime(out.LastModified)}
	readMetadata(&obj, out.Metadata)
	return out.Body, obj, nil
}

func (s *S3) Head(ctx context.Context, key string) (Object, error) {
	out, err := s.client.HeadObject(ctx, &s3.HeadObjectInput{Bucket: &s.bucket, Key: aws.String(s.prefix + key)})
	if err != nil {
		return Object{}, notFound(err)
	}
	obj := Object{Key: key, Size: aws.ToInt64(out.ContentLength), ETag: etag(out.ETag), ModTime: aws.ToTime(out.LastModified)}
	readMetadata(&obj, out.Metadata)
	return obj, nil
}

func (s *S3) Delete(ctx context.Context, key string) error {
	_, err := s.client.DeleteObject(ctx, &s3.DeleteObjectInput{Bucket: &s.bucket, Key: aws.String(s.prefix + key)})
	if errors.Is(notFound(err), ErrNotFound) {
		return nil
	}
	return err
}

func (s *S3) List(ctx context.Context) ([]Object, error) {
	var objects []Object
	pages := s3.NewListObjectsV2Paginator(s.client, &s3.ListObjectsV2Input{Bucket: &s.bucket, Prefix: aws.String(s.prefix)})
	for pages.HasMorePages() {
		page, err := pages.NextPage(ctx)
		if err != nil {
			return nil, err
		}
		for _, o := range page.Contents {
			key := strings.TrimPrefix(aws.ToString(o.Key), s.prefix)
			if key == "" || strings.HasSuffix(key, "/") {
				continue
			}
			objects = append(objects, Object{Key: key, Size: aws.ToInt64(o.Size), ETag: etag(o.ETag), ModTime: aws.ToTime(o.LastModified)})
		}
	}
	return objects, nil
}

func metadata(meta Meta) map[string]string {
	m := map[string]string{metaSHA256: meta.SHA256}
	if !meta.ModTime.IsZero() {
		m[metaMTime] = strconv.FormatInt(meta.ModTime.UnixNano(), 10)
	}
	return m
}

func readMetadata(obj *Object, m map[string]string) {
	obj.SHA256 = m[metaSHA256]
	if ns, err := strconv.ParseInt(m[metaMTime], 10, 64); err == nil {
		obj.ModTime = time.Unix(0, ns)
	}
}

func etag(v *string) string { return strings.Trim(aws.ToString(v), `"`) }

func notFound(err error) error {
	var resp *awshttp.ResponseError
	if errors.As(err, &resp) && resp.HTTPStatusCode() == http.StatusNotFound {
		return ErrNotFound
	}
	return err
}

var _ Store = (*S3)(nil)
