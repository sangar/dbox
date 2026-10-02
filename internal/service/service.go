// Package service installs `dbox run` as a per-user login service: a launchd
// agent on macOS, a systemd user unit on Linux.
package service

import (
	"bytes"
	"encoding/xml"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const label = "dbox"

// run executes a service manager command; tests replace it.
var run = func(name string, args ...string) error {
	out, err := exec.Command(name, args...).CombinedOutput()
	if err != nil {
		return fmt.Errorf("%s %s: %w: %s", name, strings.Join(args, " "), err, bytes.TrimSpace(out))
	}
	return nil
}

type manager struct {
	goos       string
	home       string
	configHome string
	uid        int
}

func current() (manager, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return manager{}, err
	}
	configHome := os.Getenv("XDG_CONFIG_HOME")
	if configHome == "" {
		configHome = filepath.Join(home, ".config")
	}
	m := manager{goos: runtime.GOOS, home: home, configHome: configHome, uid: os.Getuid()}
	if !m.supported() {
		return m, fmt.Errorf("no user service manager known for %s; start `dbox run` from your session startup instead", runtime.GOOS)
	}
	return m, nil
}

func (m manager) supported() bool { return m.goos == "darwin" || m.goos == "linux" }

func (m manager) unitPath() string {
	if m.goos == "darwin" {
		return filepath.Join(m.home, "Library", "LaunchAgents", label+".plist")
	}
	return filepath.Join(m.configHome, "systemd", "user", label+".service")
}

func (m manager) logPath() string {
	return filepath.Join(m.home, "Library", "Logs", label+".log")
}

func (m manager) launchdDomain() string { return fmt.Sprintf("gui/%d", m.uid) }

func (m manager) definition(executable string) string {
	if m.goos == "darwin" {
		return launchdPlist(executable, m.logPath())
	}
	return systemdUnit(executable)
}

func (m manager) enable(executable string) error {
	path := m.unitPath()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(path, []byte(m.definition(executable)), 0o644); err != nil {
		return err
	}
	if m.goos == "darwin" {
		if err := m.bootout(); err != nil {
			return err
		}
		return run("launchctl", "bootstrap", m.launchdDomain(), path)
	}
	if err := run("systemctl", "--user", "daemon-reload"); err != nil {
		return err
	}
	return run("systemctl", "--user", "enable", "--now", label+".service")
}

func (m manager) disable() error {
	path := m.unitPath()
	if _, err := os.Stat(path); errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("no service installed at %s", path)
	}
	if m.goos == "darwin" {
		if err := m.bootout(); err != nil {
			return err
		}
	} else if err := run("systemctl", "--user", "disable", "--now", label+".service"); err != nil {
		return err
	}
	if err := os.Remove(path); err != nil {
		return err
	}
	if m.goos == "linux" {
		return run("systemctl", "--user", "daemon-reload")
	}
	return nil
}

// bootout unloads the agent if launchd has it and waits until it is gone.
// launchctl returns as soon as the stop signal is sent, and a bootstrap while
// the old registration lingers fails with an I/O error.
func (m manager) bootout() error {
	target := m.launchdDomain() + "/" + label
	err := run("launchctl", "bootout", target)
	if err != nil && strings.Contains(err.Error(), "No such process") {
		return nil
	}
	if err != nil {
		return err
	}
	for deadline := time.Now().Add(15 * time.Second); time.Now().Before(deadline); time.Sleep(100 * time.Millisecond) {
		if run("launchctl", "print", target) != nil {
			return nil
		}
	}
	return errors.New("launchctl bootout: the dbox agent did not stop within 15 seconds")
}

// Enable writes the service definition for executable and starts it now and
// at every login. It returns the path of the definition.
func Enable(executable string) (string, error) {
	m, err := current()
	if err != nil {
		return "", err
	}
	return m.unitPath(), m.enable(executable)
}

// Disable stops the service and removes its definition.
func Disable() (string, error) {
	m, err := current()
	if err != nil {
		return "", err
	}
	return m.unitPath(), m.disable()
}

// Installed reports whether a definition written by Enable exists.
func Installed() (path string, ok bool) {
	m, err := current()
	if err != nil {
		return "", false
	}
	_, err = os.Stat(m.unitPath())
	return m.unitPath(), err == nil
}

// ExecutablePath is the path a service should start: the one found on PATH
// when it is this same binary, because that survives upgrades that move the
// real file (Homebrew, go install), otherwise the running executable.
func ExecutablePath() (string, error) {
	exe, err := os.Executable()
	if err != nil {
		return "", err
	}
	onPath, err := exec.LookPath(label)
	if err != nil {
		return exe, nil
	}
	a, err1 := os.Stat(onPath)
	b, err2 := os.Stat(exe)
	if err1 == nil && err2 == nil && os.SameFile(a, b) {
		return onPath, nil
	}
	return exe, nil
}

func launchdPlist(executable, logPath string) string {
	return `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>` + label + `</string>
  <key>ProgramArguments</key><array><string>` + xmlEscape(executable) + `</string><string>run</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>` + xmlEscape(logPath) + `</string>
  <key>StandardErrorPath</key><string>` + xmlEscape(logPath) + `</string>
</dict></plist>
`
}

func systemdUnit(executable string) string {
	return `[Unit]
Description=dbox folder sync
After=network-online.target

[Service]
ExecStart="` + executable + `" run
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
`
}

func xmlEscape(s string) string {
	var buf bytes.Buffer
	_ = xml.EscapeText(&buf, []byte(s))
	return buf.String()
}
