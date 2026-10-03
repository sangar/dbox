package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func load(t *testing.T, yaml string, env ...string) (*Config, error) {
	t.Helper()
	return parse("config.yaml", []byte(yaml), env)
}

func TestLoadsStoresWithDefaultsAndExpandedSecrets(t *testing.T) {
	nas := t.TempDir()
	cfg, err := load(t, `
stores:
  minio: {kind: s3, role: primary, bucket: dbox, region: us-east-1, prefix: me, secret_key: "${SECRET}"}
  nas: {kind: disk, role: mirror, root: `+nas+`}
sync: {root: /tmp/box}
`, "SECRET=s3cr3t")
	if err != nil {
		t.Fatal(err)
	}
	minio := cfg.Stores["minio"]
	if minio.SecretKey != "s3cr3t" || minio.Prefix != "me/" || minio.Workers != 4 {
		t.Errorf("minio = %+v", minio)
	}
	if cfg.Stores["nas"].Workers != 2 || cfg.Primary() != "minio" || strings.Join(cfg.Mirrors(), ",") != "nas" {
		t.Errorf("roles: primary %q, mirrors %v", cfg.Primary(), cfg.Mirrors())
	}
	if cfg.Sync.PullInterval.D() != 30*time.Second || cfg.Sync.PartSize != 8<<20 || !cfg.Sync.DeleteRemote {
		t.Errorf("sync defaults = %+v", cfg.Sync)
	}
}

func TestReferencesExpandInValuesButNotComments(t *testing.T) {
	cfg, err := load(t, `
# secret_key: ${UNSET}
stores:
  a: {kind: s3, role: primary, bucket: b, region: r, workers: "${N}"}
sync: {root: /tmp/box}
`, "N=3")
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Stores["a"].Workers != 3 {
		t.Errorf("workers = %d", cfg.Stores["a"].Workers)
	}
}

func TestEnvironmentOverridesNestedKeys(t *testing.T) {
	cfg, err := load(t, "sync: {root: /a}\n", "DBOX_SYNC__ROOT=/b", "DBOX_SYNC__DELETE_LOCAL=false")
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Sync.Root != "/b" || cfg.Sync.DeleteLocal {
		t.Errorf("sync = %+v", cfg.Sync)
	}
}

func TestRejectsInvalidConfigs(t *testing.T) {
	s3 := "{kind: s3, bucket: b, region: r, role: %s}"
	store := func(role string) string { return strings.Replace(s3, "%s", role, 1) }
	cases := map[string]string{
		"two primaries":   "stores: {a: " + store("primary") + ", b: " + store("primary") + "}",
		"no primary":      "stores: {a: " + store("mirror") + "}",
		"bad name":        "stores: {Big: " + store("primary") + "}",
		"unknown role":    "stores: {a: " + store("backup") + "}",
		"s3 lacks bucket": "stores: {a: {kind: s3, region: r, role: primary}}",
		"missing disk":    "stores: {a: {kind: disk, root: /does/not/exist, role: primary}}",
		"missing env":     "stores: {a: {kind: s3, bucket: b, region: r, role: primary, secret_key: '${NOPE}'}}",
		"unknown key":     "sync: {rooot: /x}",
	}
	for name, yaml := range cases {
		if _, err := load(t, yaml); err == nil {
			t.Errorf("%s: loaded without error", name)
		}
	}
}

func TestNoStoresIsAccepted(t *testing.T) {
	cfg, err := load(t, "")
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Primary() != "" {
		t.Errorf("primary = %q", cfg.Primary())
	}
}

func TestPromoteSwapsRolesAndKeepsComments(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config.yaml")
	original := `# my stores
stores:
  minio:
    kind: s3
    role: primary # home server
    bucket: dbox
    region: us-east-1
  r2:
    kind: s3
    role: mirror
    bucket: dbox
    region: auto
  old:
    kind: s3
    role: detached
    bucket: dbox
    region: auto
`
	if err := os.WriteFile(path, []byte(original), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := Promote(path, "r2"); err != nil {
		t.Fatal(err)
	}
	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	roles := map[string]Role{}
	for name, s := range cfg.Stores {
		roles[name] = s.Role
	}
	if roles["r2"] != RolePrimary || roles["minio"] != RoleMirror || roles["old"] != RoleDetached {
		t.Errorf("roles = %v", roles)
	}
	raw, _ := os.ReadFile(path)
	if !strings.Contains(string(raw), "# my stores") || !strings.Contains(string(raw), "# home server") {
		t.Errorf("comments lost:\n%s", raw)
	}
	if err := Promote(path, "nope"); err == nil {
		t.Error("promoting an unknown store should fail")
	}
}

func TestDefaultPathHonoursEnvironment(t *testing.T) {
	t.Setenv("DBOX_CONFIG", "/elsewhere/dbox.yaml")
	if got := DefaultPath(); got != "/elsewhere/dbox.yaml" {
		t.Errorf("DefaultPath() = %q", got)
	}
}

func TestDefaultPathPrefersYmlButKeepsAnExistingYaml(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", dir)
	yml := filepath.Join(dir, "dbox", "config.yml")
	if got := DefaultPath(); got != yml {
		t.Errorf("without a file: DefaultPath() = %q", got)
	}
	yaml := filepath.Join(dir, "dbox", "config.yaml")
	os.MkdirAll(filepath.Dir(yaml), 0o755)
	os.WriteFile(yaml, nil, 0o644)
	if got := DefaultPath(); got != yaml {
		t.Errorf("with only config.yaml: DefaultPath() = %q", got)
	}
}

func TestStarterConfigLoadsAndRendersWithoutSecrets(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config.yml")
	if created, err := WriteStarter(path); !created || err != nil {
		t.Fatalf("WriteStarter: %v, %v", created, err)
	}
	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	cfg.Stores = map[string]Store{"a": {Kind: KindS3, Role: RolePrimary, Bucket: "b", Region: "r", SecretKey: "s3cr3t"}}
	out, err := Render(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(out), "s3cr3t") || !strings.Contains(string(out), "part_size: 8MiB") {
		t.Errorf("rendered:\n%s", out)
	}
	if _, err := parse(path, out, nil); err != nil {
		t.Errorf("rendered config does not load: %v", err)
	}
}
