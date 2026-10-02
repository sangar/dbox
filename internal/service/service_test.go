package service

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// capture records service manager commands. launchctl print reports a
// service as gone, and bootout of a missing service fails like launchctl does.
func capture(t *testing.T) *[]string {
	t.Helper()
	var calls []string
	previous := run
	loaded := false
	run = func(name string, args ...string) error {
		calls = append(calls, name+" "+strings.Join(args, " "))
		switch {
		case name == "launchctl" && args[0] == "bootstrap":
			loaded = true
		case name == "launchctl" && args[0] == "bootout" && !loaded:
			return errors.New("Boot-out failed: 3: No such process")
		case name == "launchctl" && args[0] == "bootout":
			loaded = false
		case name == "launchctl" && args[0] == "print" && !loaded:
			return errors.New("Could not find service")
		}
		return nil
	}
	t.Cleanup(func() { run = previous })
	return &calls
}

func TestEnableOnMacWritesAgentAndBootstrapsIt(t *testing.T) {
	calls := capture(t)
	m := manager{goos: "darwin", home: t.TempDir(), uid: 501}
	if err := m.enable("/opt/homebrew/bin/dbox"); err != nil {
		t.Fatal(err)
	}
	plist, err := os.ReadFile(filepath.Join(m.home, "Library", "LaunchAgents", "dbox.plist"))
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"<string>dbox</string>", "<string>/opt/homebrew/bin/dbox</string><string>run</string>", "<key>KeepAlive</key><true/>"} {
		if !strings.Contains(string(plist), want) {
			t.Errorf("plist lacks %q:\n%s", want, plist)
		}
	}
	if got := strings.Join(*calls, "\n"); got != "launchctl bootout gui/501/dbox\nlaunchctl bootstrap gui/501 "+m.unitPath() {
		t.Errorf("commands:\n%s", got)
	}
	*calls = nil
	if err := m.disable(); err != nil {
		t.Fatal(err)
	}
	if got := strings.Join(*calls, "\n"); got != "launchctl bootout gui/501/dbox\nlaunchctl print gui/501/dbox" {
		t.Errorf("disable commands:\n%s", got)
	}
}

func TestEnableOnLinuxWritesUnitAndEnablesIt(t *testing.T) {
	calls := capture(t)
	m := manager{goos: "linux", home: t.TempDir(), configHome: filepath.Join(t.TempDir(), "cfg"), uid: 1000}
	if err := m.enable("/home/me/go/bin/dbox"); err != nil {
		t.Fatal(err)
	}
	unit, err := os.ReadFile(filepath.Join(m.configHome, "systemd", "user", "dbox.service"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(unit), `ExecStart="/home/me/go/bin/dbox" run`) || !strings.Contains(string(unit), "WantedBy=default.target") {
		t.Errorf("unit:\n%s", unit)
	}
	if got := strings.Join(*calls, "\n"); got != "systemctl --user daemon-reload\nsystemctl --user enable --now dbox.service" {
		t.Errorf("commands:\n%s", got)
	}
}

func TestDisableRemovesTheDefinition(t *testing.T) {
	calls := capture(t)
	m := manager{goos: "linux", home: t.TempDir(), configHome: t.TempDir(), uid: 1000}
	if err := m.disable(); err == nil {
		t.Error("disable without a unit should fail")
	}
	if err := m.enable("/usr/bin/dbox"); err != nil {
		t.Fatal(err)
	}
	*calls = nil
	if err := m.disable(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(m.unitPath()); !os.IsNotExist(err) {
		t.Error("unit file should be gone")
	}
	if got := strings.Join(*calls, "\n"); got != "systemctl --user disable --now dbox.service\nsystemctl --user daemon-reload" {
		t.Errorf("commands:\n%s", got)
	}
}

func TestPlistEscapesPaths(t *testing.T) {
	if got := launchdPlist("/Apps/a&b/dbox", "/l.log"); !strings.Contains(got, "/Apps/a&amp;b/dbox") {
		t.Error(got)
	}
}
