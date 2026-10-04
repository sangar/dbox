package engine

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"dbox/internal/config"
	"dbox/internal/index"
	"dbox/internal/store"
)

// machine is one synced folder with its own index, sharing store directories
// with other machines in the same test.
type machine struct {
	t      *testing.T
	root   string
	cfg    *config.Config
	idx    *index.Index
	stores map[string]store.Store
}

type storeDirs map[string]string

func newStores(t *testing.T, names ...string) storeDirs {
	dirs := storeDirs{}
	for _, n := range names {
		dirs[n] = t.TempDir()
	}
	return dirs
}

func newMachine(t *testing.T, dirs storeDirs, roles map[string]config.Role) *machine {
	t.Helper()
	root := t.TempDir()
	cfg := &config.Config{
		Stores: map[string]config.Store{},
		Sync: config.Sync{
			Root:             root,
			PullInterval:     config.Duration(50 * time.Millisecond),
			BackfillInterval: config.Duration(time.Hour),
			Debounce:         config.Duration(20 * time.Millisecond),
			PartSize:         config.Size(8 << 20),
			DeleteRemote:     true,
			DeleteLocal:      true,
			Ignore:           []string{".git/"},
		},
	}
	m := &machine{t: t, root: root, cfg: cfg, stores: map[string]store.Store{}}
	m.setRoles(dirs, roles)
	idx, err := index.Open(filepath.Join(cfg.StateDir(), "index.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { idx.Close() })
	m.idx = idx
	return m
}

func (m *machine) setRoles(dirs storeDirs, roles map[string]config.Role) {
	for name, role := range roles {
		m.cfg.Stores[name] = config.Store{Kind: config.KindDisk, Role: role, Root: dirs[name], Workers: 2}
		m.stores[name] = store.NewDisk(dirs[name], "")
	}
}

func (m *machine) engine() *Engine {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	if testing.Verbose() {
		log = slog.Default()
	}
	return New(m.cfg, m.idx, m.stores, Options{Log: log})
}

func (m *machine) once() {
	m.t.Helper()
	if err := m.engine().Once(context.Background()); err != nil {
		m.t.Fatal(err)
	}
}

func (m *machine) write(rel, content string) { writeFile(m.t, filepath.Join(m.root, rel), content) }
func (m *machine) read(rel string) string    { return readFile(m.t, filepath.Join(m.root, rel)) }

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func readFile(t *testing.T, path string) string {
	t.Helper()
	raw, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return "<missing>"
	}
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

func expect(t *testing.T, what, got, want string) {
	t.Helper()
	if got != want {
		t.Errorf("%s = %q, want %q", what, got, want)
	}
}

var primaryAndMirror = map[string]config.Role{"home": config.RolePrimary, "nas": config.RoleMirror}

func TestNewFilesReachPrimaryAndMirror(t *testing.T) {
	dirs := newStores(t, "home", "nas")
	m := newMachine(t, dirs, primaryAndMirror)
	m.write("docs/a.txt", "hello")
	m.write(".git/HEAD", "ignored")
	m.once()

	for _, store := range []string{"home", "nas"} {
		expect(t, store+" docs/a.txt", readFile(t, filepath.Join(dirs[store], "docs/a.txt")), "hello")
		expect(t, store+" .git/HEAD", readFile(t, filepath.Join(dirs[store], ".git/HEAD")), "<missing>")
	}
	stats, _ := m.idx.Stats(context.Background(), "nas")
	if stats.Unverified() != 0 {
		t.Errorf("nas stats = %+v", stats)
	}
}

func TestLocalDeleteReachesEveryStore(t *testing.T) {
	dirs := newStores(t, "home", "nas")
	m := newMachine(t, dirs, primaryAndMirror)
	m.write("a.txt", "x")
	m.once()
	os.Remove(filepath.Join(m.root, "a.txt"))
	m.once()

	for _, store := range []string{"home", "nas"} {
		expect(t, store+" a.txt", readFile(t, filepath.Join(dirs[store], "a.txt")), "<missing>")
	}
	if f, _ := m.idx.File(context.Background(), "a.txt"); f != nil {
		t.Errorf("index still has %+v", f)
	}
}

func TestChangesMadeOnOneMachineArriveOnAnother(t *testing.T) {
	dirs := newStores(t, "home", "nas")
	a := newMachine(t, dirs, primaryAndMirror)
	b := newMachine(t, dirs, primaryAndMirror)

	a.write("shared.txt", "v1")
	a.once()
	b.once()
	expect(t, "b after create", b.read("shared.txt"), "v1")

	a.write("shared.txt", "version 2")
	a.once()
	b.once()
	expect(t, "b after edit", b.read("shared.txt"), "version 2")

	os.Remove(filepath.Join(a.root, "shared.txt"))
	a.once()
	b.once()
	expect(t, "b after delete", b.read("shared.txt"), "<missing>")
}

// staleListing hides one key from List, as a listing taken before that
// file's upload finished would.
type staleListing struct {
	store.Store
	hidden string
}

func (s staleListing) List(ctx context.Context) ([]store.Object, error) {
	objects, err := s.Store.List(ctx)
	if err != nil {
		return nil, err
	}
	return slices.DeleteFunc(objects, func(o store.Object) bool { return o.Key == s.hidden }), nil
}

func TestFileMissingFromStaleListingIsNotDeletedLocally(t *testing.T) {
	dirs := newStores(t, "home")
	m := newMachine(t, dirs, map[string]config.Role{"home": config.RolePrimary})
	m.write("fresh.txt", "just uploaded")
	m.once()

	m.stores["home"] = staleListing{Store: m.stores["home"], hidden: "fresh.txt"}
	m.once()

	expect(t, "local file", m.read("fresh.txt"), "just uploaded")
	f, err := m.idx.File(context.Background(), "fresh.txt")
	if err != nil || f == nil || f.Deleted {
		t.Errorf("index row = %+v, %v; want a live row", f, err)
	}
}

func TestIdenticalFileOnSecondMachineIsAdoptedNotConflicted(t *testing.T) {
	dirs := newStores(t, "home")
	a := newMachine(t, dirs, map[string]config.Role{"home": config.RolePrimary})
	b := newMachine(t, dirs, map[string]config.Role{"home": config.RolePrimary})
	a.write("same.txt", "same")
	b.write("same.txt", "same")
	a.once()
	b.once()

	entries, _ := os.ReadDir(b.root)
	for _, e := range entries {
		if strings.Contains(e.Name(), "conflict") {
			t.Errorf("unexpected conflict copy %s", e.Name())
		}
	}
}

func TestEditOnBothSidesKeepsLocalAndSavesRemoteBeside(t *testing.T) {
	dirs := newStores(t, "home")
	roles := map[string]config.Role{"home": config.RolePrimary}
	a := newMachine(t, dirs, roles)
	b := newMachine(t, dirs, roles)
	a.write("notes.md", "base")
	a.once()
	b.once()

	a.write("notes.md", "from a")
	a.once()
	b.write("notes.md", "from b, longer")
	b.once()

	expect(t, "b keeps its version", b.read("notes.md"), "from b, longer")
	expect(t, "primary has b's version", readFile(t, filepath.Join(dirs["home"], "notes.md")), "from b, longer")
	copies, _ := filepath.Glob(filepath.Join(b.root, "notes.conflict-*.md"))
	if len(copies) != 1 {
		t.Fatalf("conflict copies = %v", copies)
	}
	expect(t, "conflict copy", readFile(t, copies[0]), "from a")
}

func TestMigrationByPromotingAMirror(t *testing.T) {
	dirs := newStores(t, "minio", "r2")
	m := newMachine(t, dirs, map[string]config.Role{"minio": config.RolePrimary})
	m.write("old.txt", "before r2 existed")
	m.once()

	m.setRoles(dirs, map[string]config.Role{"minio": config.RolePrimary, "r2": config.RoleMirror})
	m.once()
	expect(t, "backfilled to r2", readFile(t, filepath.Join(dirs["r2"], "old.txt")), "before r2 existed")
	if s, _ := m.idx.Stats(context.Background(), "r2"); s.Unverified() != 0 {
		t.Fatalf("r2 not ready for promotion: %+v", s)
	}

	m.setRoles(dirs, map[string]config.Role{"r2": config.RolePrimary, "minio": config.RoleMirror})
	m.write("new.txt", "after promotion")
	m.once()
	for _, s := range []string{"r2", "minio"} {
		expect(t, s+" new.txt", readFile(t, filepath.Join(dirs[s], "new.txt")), "after promotion")
	}

	m.setRoles(dirs, map[string]config.Role{"r2": config.RolePrimary, "minio": config.RoleDetached})
	m.write("later.txt", "detached gets nothing")
	m.once()
	expect(t, "detached store", readFile(t, filepath.Join(dirs["minio"], "later.txt")), "<missing>")
	expect(t, "detached keeps old objects", readFile(t, filepath.Join(dirs["minio"], "old.txt")), "before r2 existed")
}

func TestMirrorReconcileAdoptsCopiesMadeByOtherTools(t *testing.T) {
	dirs := newStores(t, "home", "nas")
	m := newMachine(t, dirs, map[string]config.Role{"home": config.RolePrimary})
	m.write("big.bin", "pretend this is large")
	m.once()

	writeFile(t, filepath.Join(dirs["nas"], "big.bin"), "pretend this is large")
	before, _ := os.Stat(filepath.Join(dirs["nas"], "big.bin"))
	m.setRoles(dirs, primaryAndMirror)
	if err := m.engine().Reconcile(context.Background()); err != nil {
		t.Fatal(err)
	}
	if s, _ := m.idx.Stats(context.Background(), "nas"); s.Verified != 1 {
		t.Errorf("nas stats = %+v, want the existing copy adopted", s)
	}
	after, _ := os.Stat(filepath.Join(dirs["nas"], "big.bin"))
	if !after.ModTime().Equal(before.ModTime()) {
		t.Error("adopted copy was rewritten")
	}
}

func TestMirrorFallsBackToLocalFileWhenPrimaryLacksIt(t *testing.T) {
	dirs := newStores(t, "home", "nas")
	m := newMachine(t, dirs, map[string]config.Role{"home": config.RolePrimary})
	m.write("a.txt", "only local and in index")
	m.once()
	os.Remove(filepath.Join(dirs["home"], "a.txt"))
	m.setRoles(dirs, primaryAndMirror)
	m.idx.Backfill(context.Background(), "nas")
	if err := m.engine().drainMirror(context.Background(), "nas"); err != nil {
		t.Fatal(err)
	}
	expect(t, "nas copy", readFile(t, filepath.Join(dirs["nas"], "a.txt")), "only local and in index")
}

func TestDaemonPushesEditsAsTheyHappen(t *testing.T) {
	dirs := newStores(t, "home", "nas")
	m := newMachine(t, dirs, primaryAndMirror)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error)
	go func() { done <- m.engine().Run(ctx) }()
	defer func() {
		cancel()
		if err := <-done; err != nil {
			t.Error(err)
		}
	}()
	time.Sleep(100 * time.Millisecond)

	m.write("new/dir/a.txt", "created in a new directory")
	// Atomic save, the way most editors write: temp file renamed over the original.
	m.write("new/dir/.a.txt.tmp", "saved by editor")
	os.Rename(filepath.Join(m.root, "new/dir/.a.txt.tmp"), filepath.Join(m.root, "new/dir/a.txt"))
	eventually(t, func() bool {
		return readFile(t, filepath.Join(dirs["nas"], "new/dir/a.txt")) == "saved by editor"
	})

	writeFile(t, filepath.Join(dirs["home"], "from-elsewhere.txt"), "pulled")
	eventually(t, func() bool { return m.read("from-elsewhere.txt") == "pulled" })

	os.RemoveAll(filepath.Join(m.root, "new"))
	eventually(t, func() bool {
		return readFile(t, filepath.Join(dirs["home"], "new/dir/a.txt")) == "<missing>" &&
			readFile(t, filepath.Join(dirs["nas"], "new/dir/a.txt")) == "<missing>"
	})
}

// offlineListing fails List until online is closed, like a daemon that
// starts before the network is up.
type offlineListing struct {
	store.Store
	online chan struct{}
}

func (s offlineListing) List(ctx context.Context) ([]store.Object, error) {
	select {
	case <-s.online:
		return s.Store.List(ctx)
	default:
		return nil, errors.New("network is unreachable")
	}
}

func TestDaemonRetriesReconcileInsteadOfExiting(t *testing.T) {
	dirs := newStores(t, "home")
	m := newMachine(t, dirs, map[string]config.Role{"home": config.RolePrimary})
	online := make(chan struct{})
	m.stores["home"] = offlineListing{Store: m.stores["home"], online: online}
	m.write("written-while-offline.txt", "queued")

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error)
	go func() { done <- m.engine().Run(ctx) }()
	defer func() {
		cancel()
		if err := <-done; err != nil {
			t.Error(err)
		}
	}()

	time.Sleep(150 * time.Millisecond)
	expect(t, "before the network is up", readFile(t, filepath.Join(dirs["home"], "written-while-offline.txt")), "<missing>")
	close(online)
	eventually(t, func() bool {
		return readFile(t, filepath.Join(dirs["home"], "written-while-offline.txt")) == "queued"
	})
}

// concurrentGets lets a Get proceed only once `want` of them are in flight
// together, so it fails when downloads run one after another.
type concurrentGets struct {
	store.Store
	want     int
	mu       sync.Mutex
	inFlight int
	ready    chan struct{}
}

func (s *concurrentGets) Get(ctx context.Context, key string) (io.ReadCloser, store.Object, error) {
	s.mu.Lock()
	s.inFlight++
	if s.inFlight == s.want {
		close(s.ready)
	}
	s.mu.Unlock()
	select {
	case <-s.ready:
		return s.Store.Get(ctx, key)
	case <-time.After(2 * time.Second):
		return nil, store.Object{}, errors.New("downloads did not overlap")
	}
}

func TestPollDownloadsInParallel(t *testing.T) {
	dirs := newStores(t, "home")
	m := newMachine(t, dirs, map[string]config.Role{"home": config.RolePrimary})
	m.stores["home"] = &concurrentGets{Store: m.stores["home"], want: 2, ready: make(chan struct{})}
	writeFile(t, filepath.Join(dirs["home"], "one.txt"), "1")
	writeFile(t, filepath.Join(dirs["home"], "two.txt"), "2")

	m.once()

	expect(t, "one", m.read("one.txt"), "1")
	expect(t, "two", m.read("two.txt"), "2")
}

func eventually(t *testing.T, ok func() bool) {
	t.Helper()
	for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); time.Sleep(20 * time.Millisecond) {
		if ok() {
			return
		}
	}
	t.Fatal("condition not met within 5s")
}
