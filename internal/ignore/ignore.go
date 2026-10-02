// Package ignore decides which relative paths are never synced.
package ignore

import (
	"path"
	"strings"
)

// StateDir is the daemon's own directory under the synced root.
const StateDir = ".dbox"

// Matcher holds patterns from the config. A pattern ending in "/" matches a
// directory of that name anywhere in the tree. A pattern containing "/"
// matches the whole relative path. Any other pattern matches the base name
// of the path or of any directory above it.
type Matcher struct {
	dirs  []string
	paths []string
	names []string
}

func New(patterns []string) *Matcher {
	m := &Matcher{dirs: []string{StateDir}}
	for _, p := range patterns {
		switch {
		case strings.HasSuffix(p, "/"):
			m.dirs = append(m.dirs, strings.TrimSuffix(p, "/"))
		case strings.Contains(p, "/"):
			m.paths = append(m.paths, p)
		default:
			m.names = append(m.names, p)
		}
	}
	return m
}

// Match reports whether rel, a slash-separated path relative to the root, is
// ignored. isDir says whether rel itself is a directory.
func (m *Matcher) Match(rel string, isDir bool) bool {
	for _, p := range m.paths {
		if ok, _ := path.Match(p, rel); ok {
			return true
		}
	}
	segments := strings.Split(rel, "/")
	for i, segment := range segments {
		segmentIsDir := isDir || i < len(segments)-1
		if segmentIsDir && matchAny(m.dirs, segment) {
			return true
		}
		if matchAny(m.names, segment) {
			return true
		}
	}
	return false
}

func matchAny(patterns []string, name string) bool {
	for _, p := range patterns {
		if ok, _ := path.Match(p, name); ok {
			return true
		}
	}
	return false
}
