package engine

import "sync"

// queue is an unbounded, de-duplicating FIFO of paths. Pushing a path that is
// already waiting is a no-op, so a burst of events costs one sync.
type queue struct {
	mu      sync.Mutex
	items   []string
	waiting map[string]bool
	ready   chan struct{}
}

func newQueue() *queue {
	return &queue{waiting: map[string]bool{}, ready: make(chan struct{}, 1)}
}

func (q *queue) push(p string) {
	q.mu.Lock()
	if !q.waiting[p] {
		q.waiting[p] = true
		q.items = append(q.items, p)
	}
	q.mu.Unlock()
	select {
	case q.ready <- struct{}{}:
	default:
	}
}

func (q *queue) len() int {
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.items)
}

// pop returns the next path, or false when done is closed first.
func (q *queue) pop(done <-chan struct{}) (string, bool) {
	for {
		q.mu.Lock()
		if len(q.items) > 0 {
			p := q.items[0]
			q.items = q.items[1:]
			delete(q.waiting, p)
			more := len(q.items) > 0
			q.mu.Unlock()
			if more {
				select {
				case q.ready <- struct{}{}:
				default:
				}
			}
			return p, true
		}
		q.mu.Unlock()
		select {
		case <-done:
			return "", false
		case <-q.ready:
		}
	}
}

// pathLocks lets one goroutine at a time act on a path. A path that is busy
// is marked dirty, and the holder is told to run it again when it releases.
type pathLocks struct {
	mu    sync.Mutex
	busy  map[string]bool
	dirty map[string]bool
}

func newPathLocks() *pathLocks {
	return &pathLocks{busy: map[string]bool{}, dirty: map[string]bool{}}
}

func (l *pathLocks) inFlight() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.busy)
}

func (l *pathLocks) acquire(p string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.busy[p] {
		l.dirty[p] = true
		return false
	}
	l.busy[p] = true
	return true
}

// release reports whether p changed while it was held.
func (l *pathLocks) release(p string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	delete(l.busy, p)
	again := l.dirty[p]
	delete(l.dirty, p)
	return again
}
