package debounce

import (
	"sync"
	"testing"
	"time"
)

func TestBurstIsCoalescedPerKey(t *testing.T) {
	var mu sync.Mutex
	calls := map[string]int{}
	d := New(30*time.Millisecond, func(key string) {
		mu.Lock()
		calls[key]++
		mu.Unlock()
	})
	for range 5 {
		d.Trigger("a")
		time.Sleep(5 * time.Millisecond)
	}
	d.Trigger("b")
	time.Sleep(100 * time.Millisecond)
	mu.Lock()
	defer mu.Unlock()
	if calls["a"] != 1 || calls["b"] != 1 {
		t.Errorf("calls = %v", calls)
	}
}
