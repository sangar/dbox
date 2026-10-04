// Package debounce coalesces bursts of events per key into one call.
package debounce

import (
	"sync"
	"time"
)

// Debouncer calls fn(key) once key has been quiet for the delay.
type Debouncer struct {
	delay  time.Duration
	fn     func(string)
	mu     sync.Mutex
	timers map[string]*time.Timer
}

func New(delay time.Duration, fn func(string)) *Debouncer {
	return &Debouncer{delay: delay, fn: fn, timers: map[string]*time.Timer{}}
}

// Trigger starts or restarts key's quiet period.
func (d *Debouncer) Trigger(key string) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if t, ok := d.timers[key]; ok {
		t.Reset(d.delay)
		return
	}
	d.timers[key] = time.AfterFunc(d.delay, func() {
		d.mu.Lock()
		delete(d.timers, key)
		d.mu.Unlock()
		d.fn(key)
	})
}

// Pending is how many keys are in their quiet period.
func (d *Debouncer) Pending() int {
	d.mu.Lock()
	defer d.mu.Unlock()
	return len(d.timers)
}

// Stop cancels every pending call.
func (d *Debouncer) Stop() {
	d.mu.Lock()
	defer d.mu.Unlock()
	for key, t := range d.timers {
		t.Stop()
		delete(d.timers, key)
	}
}
