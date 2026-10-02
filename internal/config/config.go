// Package config loads ~/.config/dbox/config.yaml: the stores, their roles and
// the sync settings. See POC.md for the format.
package config

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"gopkg.in/yaml.v3"
)

type Kind string

const (
	KindS3   Kind = "s3"
	KindDisk Kind = "disk"
)

type Role string

const (
	RolePrimary  Role = "primary"
	RoleMirror   Role = "mirror"
	RoleDetached Role = "detached"
)

type Config struct {
	Stores map[string]Store `yaml:"stores"`
	Sync   Sync             `yaml:"sync"`
	Daemon Daemon           `yaml:"daemon"`

	Path string `yaml:"-"`
}

type Store struct {
	Kind         Kind   `yaml:"kind"`
	Role         Role   `yaml:"role"`
	Bucket       string `yaml:"bucket"`
	Prefix       string `yaml:"prefix"`
	Region       string `yaml:"region"`
	Endpoint     string `yaml:"endpoint"`
	PathStyle    bool   `yaml:"path_style"`
	AccessKey    string `yaml:"access_key"`
	SecretKey    string `yaml:"secret_key"`
	StorageClass string `yaml:"storage_class"`
	Root         string `yaml:"root"`
	Workers      int    `yaml:"workers"`
}

type Sync struct {
	Root             string   `yaml:"root"`
	PullInterval     Duration `yaml:"pull_interval"`
	BackfillInterval Duration `yaml:"backfill_interval"`
	Debounce         Duration `yaml:"debounce"`
	PartSize         Size     `yaml:"part_size"`
	DeleteRemote     bool     `yaml:"delete_remote"`
	DeleteLocal      bool     `yaml:"delete_local"`
	Ignore           []string `yaml:"ignore"`
}

type Daemon struct {
	LogLevel  string `yaml:"log_level"`
	LogFormat string `yaml:"log_format"`
	Listen    string `yaml:"listen"`
}

// Duration reads "30s", "10m" and the like.
type Duration time.Duration

func (d *Duration) UnmarshalYAML(n *yaml.Node) error {
	parsed, err := time.ParseDuration(n.Value)
	if err != nil {
		return fmt.Errorf("line %d: %w", n.Line, err)
	}
	*d = Duration(parsed)
	return nil
}

func (d Duration) D() time.Duration { return time.Duration(d) }

// Size reads byte counts with an optional KiB, MiB or GiB suffix.
type Size int64

func (s *Size) UnmarshalYAML(n *yaml.Node) error {
	parsed, err := ParseSize(n.Value)
	if err != nil {
		return fmt.Errorf("line %d: %w", n.Line, err)
	}
	*s = Size(parsed)
	return nil
}

func ParseSize(v string) (int64, error) {
	units := []struct {
		suffix string
		factor int64
	}{{"GiB", 1 << 30}, {"MiB", 1 << 20}, {"KiB", 1 << 10}, {"B", 1}}
	v = strings.TrimSpace(v)
	factor := int64(1)
	for _, u := range units {
		if strings.HasSuffix(v, u.suffix) {
			v, factor = strings.TrimSuffix(v, u.suffix), u.factor
			break
		}
	}
	n, err := strconv.ParseInt(strings.TrimSpace(v), 10, 64)
	if err != nil {
		return 0, fmt.Errorf("size %q: want a number with an optional KiB, MiB or GiB suffix", v)
	}
	return n * factor, nil
}

func defaults() Config {
	return Config{
		Sync: Sync{
			Root:             "~/dbox",
			PullInterval:     Duration(30 * time.Second),
			BackfillInterval: Duration(10 * time.Minute),
			Debounce:         Duration(750 * time.Millisecond),
			PartSize:         Size(8 << 20),
			DeleteRemote:     true,
			DeleteLocal:      true,
			Ignore:           []string{".git/", ".DS_Store", "*.swp", "*.tmp", "~$*"},
		},
		Daemon: Daemon{LogLevel: "info", LogFormat: "text", Listen: "127.0.0.1:7878"},
	}
}

// DefaultPath is $XDG_CONFIG_HOME/dbox/config.yaml, falling back to ~/.config.
func DefaultPath() string {
	if p := os.Getenv("DBOX_CONFIG"); p != "" {
		return p
	}
	dir := os.Getenv("XDG_CONFIG_HOME")
	if dir == "" {
		home, _ := os.UserHomeDir()
		dir = filepath.Join(home, ".config")
	}
	return filepath.Join(dir, "dbox", "config.yaml")
}

// Load reads the file at path, expands ${VAR} references, applies DBOX_
// environment overrides and defaults, and validates the result.
func Load(path string) (*Config, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return parse(path, raw, os.Environ())
}

var envReference = regexp.MustCompile(`\$\{([A-Za-z_][A-Za-z0-9_]*)\}`)

func parse(path string, raw []byte, environ []string) (*Config, error) {
	env := map[string]string{}
	for _, kv := range environ {
		k, v, _ := strings.Cut(kv, "=")
		env[k] = v
	}
	var missing []string
	expanded := envReference.ReplaceAllFunc(raw, func(ref []byte) []byte {
		name := string(envReference.FindSubmatch(ref)[1])
		v, ok := env[name]
		if !ok {
			missing = append(missing, name)
		}
		return []byte(v)
	})
	if len(missing) > 0 {
		return nil, fmt.Errorf("%s: environment variables not set: %s", path, strings.Join(missing, ", "))
	}

	var tree map[string]any
	if err := yaml.Unmarshal(expanded, &tree); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if tree == nil {
		tree = map[string]any{}
	}
	if err := applyOverrides(tree, env); err != nil {
		return nil, err
	}
	merged, err := yaml.Marshal(tree)
	if err != nil {
		return nil, err
	}

	cfg := defaults()
	dec := yaml.NewDecoder(bytes.NewReader(merged))
	dec.KnownFields(true)
	if err := dec.Decode(&cfg); err != nil && !errors.Is(err, io.EOF) {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	cfg.Path = path
	if err := cfg.finish(); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return &cfg, nil
}

// applyOverrides sets DBOX_SECTION__KEY=value as tree[section][key].
func applyOverrides(tree map[string]any, env map[string]string) error {
	keys := make([]string, 0, len(env))
	for k := range env {
		if strings.HasPrefix(k, "DBOX_") && strings.Contains(k, "__") {
			keys = append(keys, k)
		}
	}
	sort.Strings(keys)
	for _, k := range keys {
		parts := strings.Split(strings.ToLower(strings.TrimPrefix(k, "DBOX_")), "__")
		var value any
		if err := yaml.Unmarshal([]byte(env[k]), &value); err != nil {
			return fmt.Errorf("%s: %w", k, err)
		}
		node := tree
		for _, p := range parts[:len(parts)-1] {
			next, ok := node[p].(map[string]any)
			if !ok {
				next = map[string]any{}
				node[p] = next
			}
			node = next
		}
		node[parts[len(parts)-1]] = value
	}
	return nil
}

var storeName = regexp.MustCompile(`^[a-z0-9_-]+$`)

func (c *Config) finish() error {
	c.Sync.Root = expandHome(c.Sync.Root)
	if c.Sync.Root == "" {
		return errors.New("sync.root is required")
	}
	primaries := 0
	for name, s := range c.Stores {
		if !storeName.MatchString(name) {
			return fmt.Errorf("store %q: names may only contain a-z, 0-9, _ and -", name)
		}
		switch s.Role {
		case RolePrimary:
			primaries++
		case RoleMirror, RoleDetached:
		case "":
			return fmt.Errorf("store %q: role is required (primary, mirror or detached)", name)
		default:
			return fmt.Errorf("store %q: unknown role %q", name, s.Role)
		}
		switch s.Kind {
		case KindS3:
			if s.Bucket == "" || s.Region == "" {
				return fmt.Errorf("store %q: s3 stores need bucket and region", name)
			}
		case KindDisk:
			s.Root = expandHome(s.Root)
			if info, err := os.Stat(s.Root); err != nil || !info.IsDir() {
				return fmt.Errorf("store %q: root %q is not an existing directory", name, s.Root)
			}
		default:
			return fmt.Errorf("store %q: kind must be s3 or disk", name)
		}
		if s.Prefix != "" && !strings.HasSuffix(s.Prefix, "/") {
			s.Prefix += "/"
		}
		if s.Workers <= 0 {
			s.Workers = 2
			if s.Role == RolePrimary {
				s.Workers = 4
			}
		}
		c.Stores[name] = s
	}
	if len(c.Stores) > 0 && primaries != 1 {
		return fmt.Errorf("exactly one store must be primary, found %d", primaries)
	}
	if c.Sync.PartSize < 5<<20 {
		return errors.New("sync.part_size must be at least 5MiB, the S3 minimum")
	}
	return nil
}

// Primary returns the primary store's name, or "" when no stores are configured.
func (c *Config) Primary() string {
	for name, s := range c.Stores {
		if s.Role == RolePrimary {
			return name
		}
	}
	return ""
}

// Mirrors returns the names of the mirror stores, sorted.
func (c *Config) Mirrors() []string {
	var names []string
	for name, s := range c.Stores {
		if s.Role == RoleMirror {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	return names
}

// StateDir is where the index, temp files and pid file live.
func (c *Config) StateDir() string { return filepath.Join(c.Sync.Root, ".dbox") }

func expandHome(p string) string {
	if p == "~" || strings.HasPrefix(p, "~/") {
		home, _ := os.UserHomeDir()
		return filepath.Join(home, strings.TrimPrefix(p, "~"))
	}
	return p
}
