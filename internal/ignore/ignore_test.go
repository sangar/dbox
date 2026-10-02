package ignore

import "testing"

func TestMatch(t *testing.T) {
	m := New([]string{".git/", ".DS_Store", "*.swp", "~$*", "build/out/*"})
	cases := []struct {
		path  string
		isDir bool
		want  bool
	}{
		{".dbox", true, true},
		{".dbox/index.db", false, true},
		{"src/.git", true, true},
		{"src/.git/HEAD", false, true},
		{".git", false, false},
		{"docs/.DS_Store", false, true},
		{"notes/.todo.md.swp", false, true},
		{"~$report.docx", false, true},
		{"build/out/app", false, true},
		{"other/build/out/app", false, false},
		{"notes/todo.md", false, false},
	}
	for _, c := range cases {
		if got := m.Match(c.path, c.isDir); got != c.want {
			t.Errorf("Match(%q, %v) = %v, want %v", c.path, c.isDir, got, c.want)
		}
	}
}
