package daemon

import (
	"context"
	"net"
	"testing"
	"time"
)

func TestAskReturnsWhatServeReports(t *testing.T) {
	probe, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := probe.Addr().String()
	probe.Close()

	if _, ok := Ask(addr); ok {
		t.Fatal("Ask answered with nothing listening")
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go Serve(ctx, addr, func(context.Context) (string, error) { return "", nil }, func() Status { return Status{Backlog: 7} })

	for deadline := time.Now().Add(2 * time.Second); time.Now().Before(deadline); time.Sleep(10 * time.Millisecond) {
		if s, ok := Ask(addr); ok {
			if s.Backlog != 7 {
				t.Fatalf("backlog = %d, want 7", s.Backlog)
			}
			return
		}
	}
	t.Fatal("daemon did not answer within 2s")
}
