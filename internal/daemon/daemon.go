// Package daemon holds what the long-running process needs besides syncing:
// a pid file so other commands can find it, the health and metrics listener,
// and a raised open-file limit for kqueue.
package daemon

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// WritePID records the running process in path and returns a func that
// removes it. It fails when another live daemon already owns the file.
func WritePID(path string) (func(), error) {
	if pid, ok := Running(path); ok {
		return nil, fmt.Errorf("dbox is already running as pid %d", pid)
	}
	if err := os.WriteFile(path, []byte(strconv.Itoa(os.Getpid())+"\n"), 0o644); err != nil {
		return nil, err
	}
	return func() { os.Remove(path) }, nil
}

// Running returns the pid in path when that process is alive.
func Running(path string) (int, bool) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return 0, false
	}
	pid, err := strconv.Atoi(strings.TrimSpace(string(raw)))
	if err != nil || pid <= 0 {
		return 0, false
	}
	if err := syscall.Kill(pid, 0); err != nil && !errors.Is(err, syscall.EPERM) {
		return 0, false
	}
	return pid, true
}

// Reload asks the daemon recorded in path to re-read its config. It reports
// false when no daemon is running.
func Reload(path string) (bool, error) {
	pid, ok := Running(path)
	if !ok {
		return false, nil
	}
	return true, syscall.Kill(pid, syscall.SIGHUP)
}

// Serve answers /healthz and /metrics on addr until ctx is done. An empty
// addr disables the listener.
func Serve(ctx context.Context, addr string, metrics func(context.Context) (string, error)) error {
	if addr == "" {
		return nil
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) { fmt.Fprintln(w, "ok") })
	mux.HandleFunc("GET /metrics", func(w http.ResponseWriter, r *http.Request) {
		body, err := metrics(r.Context())
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "text/plain; version=0.0.4")
		fmt.Fprint(w, body)
	})
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	server := &http.Server{Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	go func() {
		<-ctx.Done()
		server.Close()
	}()
	if err := server.Serve(listener); !errors.Is(err, http.ErrServerClosed) {
		return err
	}
	return nil
}

// RaiseFileLimit lifts the soft open-file limit to the hard limit. kqueue on
// macOS holds one descriptor per watched directory, and the default soft
// limit there is 256.
func RaiseFileLimit() error {
	var limit syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &limit); err != nil {
		return err
	}
	if limit.Cur >= limit.Max {
		return nil
	}
	limit.Cur = limit.Max
	// macOS rejects RLIM_INFINITY for the soft limit; OPEN_MAX-sized values work.
	if err := syscall.Setrlimit(syscall.RLIMIT_NOFILE, &limit); err != nil {
		limit.Cur = 1 << 20
		return syscall.Setrlimit(syscall.RLIMIT_NOFILE, &limit)
	}
	return nil
}
