// dbox keeps a local folder in sync with S3-compatible stores.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/signal"
	"path/filepath"
	"sort"
	"strings"
	"syscall"

	"dbox/internal/config"
	"dbox/internal/daemon"
	"dbox/internal/engine"
	"dbox/internal/index"
	"dbox/internal/service"
	"dbox/internal/store"
)

var version = "dev"

const usageText = `dbox - keep a folder in sync with S3-compatible stores

Usage:
  dbox run [--once] [--dry-run]     the daemon; --once reconciles, copies to mirrors and exits
  dbox status                       daemon and service state, per-store copies, failed files
  dbox retry STORE                  make failed copies on STORE pending again
  dbox check STORE                  write, read back and delete a probe object on STORE
  dbox promote STORE [--force]      make STORE the primary; the old primary becomes a mirror
  dbox service enable|disable       run dbox run at login (launchd agent or systemd user unit)
  dbox version

Every command takes --config FILE (default %s, env DBOX_CONFIG).
`

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "dbox:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) == 0 {
		printUsage(os.Stdout)
		return nil
	}
	cmd, rest := args[0], args[1:]
	switch cmd {
	case "run":
		return cmdRun(rest)
	case "status":
		return cmdStatus(rest)
	case "retry":
		return cmdRetry(rest)
	case "check":
		return cmdCheck(rest)
	case "promote":
		return cmdPromote(rest)
	case "service":
		return cmdService(rest)
	case "version", "--version":
		fmt.Println("dbox", version)
		return nil
	case "help", "-h", "--help":
		printUsage(os.Stdout)
		return nil
	}
	printUsage(os.Stderr)
	return fmt.Errorf("unknown command %q", cmd)
}

func printUsage(w io.Writer) { fmt.Fprintf(w, usageText, config.DefaultPath()) }

// parse reads the flags for one command, allowing them after positional
// arguments too, and returns the positional ones.
func parse(fs *flag.FlagSet, args []string) ([]string, error) {
	var positional []string
	for {
		if err := fs.Parse(args); err != nil {
			return nil, err
		}
		if fs.NArg() == 0 {
			return positional, nil
		}
		positional = append(positional, fs.Arg(0))
		args = fs.Args()[1:]
	}
}

func newFlags(name string) (*flag.FlagSet, *string) {
	fs := flag.NewFlagSet(name, flag.ContinueOnError)
	configPath := fs.String("config", config.DefaultPath(), "config file")
	return fs, configPath
}

func pidPath(cfg *config.Config) string   { return filepath.Join(cfg.StateDir(), "daemon.pid") }
func indexPath(cfg *config.Config) string { return filepath.Join(cfg.StateDir(), "index.db") }

func cmdRun(args []string) error {
	fs, configPath := newFlags("run")
	once := fs.Bool("once", false, "reconcile and exit")
	dryRun := fs.Bool("dry-run", false, "log what would change without changing anything; implies --once")
	if _, err := parse(fs, args); err != nil {
		return err
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if *once || *dryRun {
		cfg, err := config.Load(*configPath)
		if err != nil {
			return err
		}
		if pid, ok := daemon.Running(pidPath(cfg)); ok && !*dryRun {
			return fmt.Errorf("the daemon is already syncing this folder (pid %d); stop it before running --once", pid)
		}
		return withEngine(ctx, cfg, *dryRun, func(e *engine.Engine, _ *index.Index) error { return e.Once(ctx) })
	}

	if err := daemon.RaiseFileLimit(); err != nil {
		slog.Warn("raise open file limit", "err", err)
	}
	reload := make(chan os.Signal, 1)
	signal.Notify(reload, syscall.SIGHUP)
	for {
		cfg, err := config.Load(*configPath)
		if err != nil {
			return err
		}
		runCtx, cancel := context.WithCancel(ctx)
		done := make(chan error, 1)
		go func() { done <- serve(runCtx, cfg) }()
		select {
		case err := <-done:
			cancel()
			return err
		case <-reload:
			slog.Info("reloading config", "path", cfg.Path)
			cancel()
			if err := <-done; err != nil {
				return err
			}
		}
	}
}

// serve runs the daemon for one config until ctx is done.
func serve(ctx context.Context, cfg *config.Config) error {
	slog.SetDefault(newLogger(cfg.Daemon))
	if err := os.MkdirAll(cfg.StateDir(), 0o755); err != nil {
		return err
	}
	removePID, err := daemon.WritePID(pidPath(cfg))
	if err != nil {
		return err
	}
	defer removePID()

	if cfg.Primary() == "" {
		slog.Warn("no stores configured; add one to the config and send SIGHUP", "config", cfg.Path)
		<-ctx.Done()
		return nil
	}
	return withEngine(ctx, cfg, false, func(e *engine.Engine, idx *index.Index) error {
		go func() {
			if err := daemon.Serve(ctx, cfg.Daemon.Listen, func(ctx context.Context) (string, error) { return metrics(ctx, cfg, idx) }); err != nil {
				slog.Error("health listener", "addr", cfg.Daemon.Listen, "err", err)
			}
		}()
		return e.Run(ctx)
	})
}

func withEngine(ctx context.Context, cfg *config.Config, dryRun bool, fn func(*engine.Engine, *index.Index) error) error {
	if cfg.Primary() == "" {
		return fmt.Errorf("%s: no stores configured", cfg.Path)
	}
	idx, err := index.Open(indexPath(cfg))
	if err != nil {
		return err
	}
	defer idx.Close()
	stores, err := store.OpenAll(ctx, cfg)
	if err != nil {
		return err
	}
	return fn(engine.New(cfg, idx, stores, engine.Options{DryRun: dryRun, Log: newLogger(cfg.Daemon)}), idx)
}

func newLogger(d config.Daemon) *slog.Logger {
	var level slog.Level
	if err := level.UnmarshalText([]byte(d.LogLevel)); err != nil {
		level = slog.LevelInfo
	}
	opts := &slog.HandlerOptions{Level: level}
	if d.LogFormat == "json" {
		return slog.New(slog.NewJSONHandler(os.Stderr, opts))
	}
	return slog.New(slog.NewTextHandler(os.Stderr, opts))
}

func metrics(ctx context.Context, cfg *config.Config, idx *index.Index) (string, error) {
	var b strings.Builder
	for _, name := range storeNames(cfg) {
		s, err := idx.Stats(ctx, name)
		if err != nil {
			return "", err
		}
		role := cfg.Stores[name].Role
		fmt.Fprintf(&b, "dbox_files{store=%q,role=%q} %d\n", name, role, s.Files)
		fmt.Fprintf(&b, "dbox_replicas{store=%q,role=%q,state=\"verified\"} %d\n", name, role, s.Verified)
		fmt.Fprintf(&b, "dbox_replicas{store=%q,role=%q,state=\"pending\"} %d\n", name, role, s.Pending)
		fmt.Fprintf(&b, "dbox_replicas{store=%q,role=%q,state=\"failed\"} %d\n", name, role, s.Failed)
		fmt.Fprintf(&b, "dbox_verified_bytes{store=%q,role=%q} %d\n", name, role, s.VerifiedBytes)
	}
	return b.String(), nil
}

func cmdStatus(args []string) error {
	fs, configPath := newFlags("status")
	if _, err := parse(fs, args); err != nil {
		return err
	}
	cfg, err := config.Load(*configPath)
	if err != nil {
		return err
	}
	fmt.Printf("config:  %s\nroot:    %s\n", cfg.Path, cfg.Sync.Root)
	if pid, ok := daemon.Running(pidPath(cfg)); ok {
		fmt.Printf("daemon:  running, pid %d\n", pid)
	} else {
		fmt.Println("daemon:  not running")
	}
	if path, ok := service.Installed(); ok {
		fmt.Printf("service: enabled, %s\n", path)
	} else {
		fmt.Println("service: not enabled (run `dbox service enable` to start dbox run at login)")
	}
	if len(cfg.Stores) == 0 {
		fmt.Println("stores:  none configured")
		return nil
	}

	ctx := context.Background()
	idx, err := index.Open(indexPath(cfg))
	if err != nil {
		return err
	}
	defer idx.Close()
	fmt.Println()
	fmt.Printf("%-12s %-9s %8s %8s %8s %8s %10s\n", "STORE", "ROLE", "FILES", "VERIFIED", "PENDING", "FAILED", "COPIED")
	var failed []index.Replica
	for _, name := range storeNames(cfg) {
		s, err := idx.Stats(ctx, name)
		if err != nil {
			return err
		}
		fmt.Printf("%-12s %-9s %8d %8d %8d %8d %9.0f%%\n", name, cfg.Stores[name].Role, s.Files, s.Verified, s.Pending, s.Failed, percent(s))
		f, err := idx.Failed(ctx, name, 10)
		if err != nil {
			return err
		}
		failed = append(failed, f...)
	}
	if len(failed) > 0 {
		fmt.Println("\nfailed copies (`dbox retry STORE` to try again):")
		for _, r := range failed {
			fmt.Printf("  %s  %s  %s\n", r.Store, r.Path, r.LastError)
		}
	}
	return nil
}

func percent(s index.Stats) float64 {
	if s.Bytes == 0 {
		if s.Files == s.Verified {
			return 100
		}
		return 0
	}
	return 100 * float64(s.VerifiedBytes) / float64(s.Bytes)
}

func storeNames(cfg *config.Config) []string {
	names := make([]string, 0, len(cfg.Stores))
	for name := range cfg.Stores {
		names = append(names, name)
	}
	rank := map[config.Role]int{config.RolePrimary: 0, config.RoleMirror: 1, config.RoleDetached: 2}
	sort.Slice(names, func(i, j int) bool {
		a, b := cfg.Stores[names[i]].Role, cfg.Stores[names[j]].Role
		if a != b {
			return rank[a] < rank[b]
		}
		return names[i] < names[j]
	})
	return names
}

// storeArg loads the config and returns the one named store argument.
func storeArg(name string, args []string, extra func(*flag.FlagSet)) (*config.Config, string, error) {
	fs, configPath := newFlags(name)
	if extra != nil {
		extra(fs)
	}
	positional, err := parse(fs, args)
	if err != nil {
		return nil, "", err
	}
	if len(positional) != 1 {
		return nil, "", fmt.Errorf("usage: dbox %s STORE", name)
	}
	cfg, err := config.Load(*configPath)
	if err != nil {
		return nil, "", err
	}
	if _, ok := cfg.Stores[positional[0]]; !ok {
		return nil, "", fmt.Errorf("no store named %q in %s", positional[0], cfg.Path)
	}
	return cfg, positional[0], nil
}

func cmdRetry(args []string) error {
	cfg, name, err := storeArg("retry", args, nil)
	if err != nil {
		return err
	}
	idx, err := index.Open(indexPath(cfg))
	if err != nil {
		return err
	}
	defer idx.Close()
	n, err := idx.RetryFailed(context.Background(), name)
	if err != nil {
		return err
	}
	fmt.Printf("%d failed copies on %s are pending again\n", n, name)
	return nil
}

func cmdCheck(args []string) error {
	cfg, name, err := storeArg("check", args, nil)
	if err != nil {
		return err
	}
	ctx := context.Background()
	s, err := store.Open(ctx, cfg.Stores[name], int64(cfg.Sync.PartSize))
	if err != nil {
		return err
	}
	if err := store.Check(ctx, s); err != nil {
		return fmt.Errorf("%s: %w", name, err)
	}
	fmt.Printf("%s: ok, wrote, read back and deleted a probe object\n", name)
	return nil
}

func cmdPromote(args []string) error {
	var force bool
	cfg, name, err := storeArg("promote", args, func(fs *flag.FlagSet) {
		fs.BoolVar(&force, "force", false, "promote even when the store lacks verified copies")
	})
	if err != nil {
		return err
	}
	if cfg.Stores[name].Role == config.RolePrimary {
		fmt.Printf("%s is already the primary\n", name)
		return nil
	}
	idx, err := index.Open(indexPath(cfg))
	if err != nil {
		return err
	}
	stats, err := idx.Stats(context.Background(), name)
	idx.Close()
	if err != nil {
		return err
	}
	if n := stats.Unverified(); n > 0 && !force {
		return fmt.Errorf("%s lacks a verified copy of %d of %d files; wait for backfill (see `dbox status`) or pass --force", name, n, stats.Files)
	}
	if err := config.Promote(cfg.Path, name); err != nil {
		return err
	}
	fmt.Printf("%s is now the primary, %s is a mirror (%s)\n", name, cfg.Primary(), cfg.Path)
	reloaded, err := daemon.Reload(pidPath(cfg))
	switch {
	case err != nil:
		return fmt.Errorf("signal daemon: %w", err)
	case reloaded:
		fmt.Println("the running daemon is reloading its config")
	}
	return nil
}

func cmdService(args []string) error {
	if len(args) != 1 || (args[0] != "enable" && args[0] != "disable") {
		return errors.New("usage: dbox service enable|disable")
	}
	if args[0] == "disable" {
		path, err := service.Disable()
		if err != nil {
			return err
		}
		fmt.Printf("stopped and removed %s\n", path)
		return nil
	}
	exe, err := service.ExecutablePath()
	if err != nil {
		return err
	}
	path, err := service.Enable(exe)
	if err != nil {
		return err
	}
	fmt.Printf("dbox run starts now and at every login (%s)\n", path)
	return nil
}
