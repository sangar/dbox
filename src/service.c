#include "service.h"

#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

#define LABEL "dbox"

Error service_run_command(void *user, char *const argv[], Err *err) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return err_set(err, ERR_PLATFORM, "pipe: %s", strerror(errno));
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    StrBuf out = {0};
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0) sb_append(&out, buf, (size_t)n);
    close(pipefd[0]);
    int status = 0;
    Error e = ERR_OK;
    if (!(rc == 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        StrBuf cmd = {0};
        for (size_t i = 0; argv[i]; i++) sb_printf(&cmd, "%s%s", i ? " " : "", argv[i]);
        while (out.len > 0 && (out.data[out.len - 1] == '\n' || out.data[out.len - 1] == ' ')) out.len--;
        if (rc) e = err_set(err, ERR_PLATFORM, "%s: %s", sb_cstr(&cmd), strerror(rc));
        else e = err_set(err, ERR_PLATFORM, "%s: exit status %d: %s", sb_cstr(&cmd), WIFEXITED(status) ? WEXITSTATUS(status) : -1, sb_cstr(&out));
        sb_free(&cmd);
    }
    sb_free(&out);
    return e;
}

Error service_manager(ServiceManager *m, Err *err) {
    memset(m, 0, sizeof *m);
    m->home = home_dir();
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(m->config_home_storage, sizeof m->config_home_storage, "%s", xdg);
    else snprintf(m->config_home_storage, sizeof m->config_home_storage, "%s/.config", m->home);
    m->config_home = m->config_home_storage;
    m->uid = (int)getuid();
    m->run = service_run_command;
#if defined(__APPLE__)
    m->os = OS_DARWIN;
#elif defined(__linux__)
    m->os = OS_LINUX;
#else
    m->os = OS_OTHER;
#endif
    if (m->os == OS_OTHER) return err_set(err, ERR_PLATFORM, "no user service manager known for this system; start `dbox run` from your session startup instead");
    return ERR_OK;
}

char *service_unit_path(const ServiceManager *m) {
    if (m->os == OS_DARWIN) return path_join(m->home, "Library/LaunchAgents/" LABEL ".plist");
    return path_join(m->config_home, "systemd/user/" LABEL ".service");
}

static void xml_escape(StrBuf *sb, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&': sb_puts(sb, "&amp;"); break;
        case '<': sb_puts(sb, "&lt;"); break;
        case '>': sb_puts(sb, "&gt;"); break;
        case '"': sb_puts(sb, "&#34;"); break;
        case '\'': sb_puts(sb, "&#39;"); break;
        default: sb_putc(sb, *s);
        }
    }
}

char *service_launchd_plist(const char *executable, const char *log_path) {
    StrBuf sb = {0};
    sb_puts(&sb,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\"><dict>\n"
            "  <key>Label</key><string>" LABEL "</string>\n"
            "  <key>ProgramArguments</key><array><string>");
    xml_escape(&sb, executable);
    sb_puts(&sb, "</string><string>run</string></array>\n"
                 "  <key>RunAtLoad</key><true/>\n"
                 "  <key>KeepAlive</key><true/>\n"
                 "  <key>StandardOutPath</key><string>");
    xml_escape(&sb, log_path);
    sb_puts(&sb, "</string>\n  <key>StandardErrorPath</key><string>");
    xml_escape(&sb, log_path);
    sb_puts(&sb, "</string>\n</dict></plist>\n");
    return sb.data;
}

static char *systemd_unit(const char *executable) {
    StrBuf sb = {0};
    sb_printf(&sb,
              "[Unit]\n"
              "Description=dbox folder sync\n"
              "After=network-online.target\n"
              "\n"
              "[Service]\n"
              "ExecStart=\"%s\" run\n"
              "Restart=on-failure\n"
              "RestartSec=5\n"
              "\n"
              "[Install]\n"
              "WantedBy=default.target\n",
              executable);
    return sb.data;
}

static char *definition(const ServiceManager *m, const char *executable) {
    if (m->os != OS_DARWIN) return systemd_unit(executable);
    char *log = path_join(m->home, "Library/Logs/" LABEL ".log");
    char *plist = service_launchd_plist(executable, log);
    xfree(log);
    return plist;
}

static void launchd_target(const ServiceManager *m, char buf[64]) { snprintf(buf, 64, "gui/%d/" LABEL, m->uid); }

/*
 * bootout unloads the agent if launchd has it and waits until it is gone.
 * launchctl returns as soon as the stop signal is sent, and a bootstrap while
 * the old registration lingers fails with an I/O error.
 */
[[nodiscard]] static Error bootout(const ServiceManager *m, Err *err) {
    char target[64];
    launchd_target(m, target);
    char *argv[] = {"launchctl", "bootout", target, NULL};
    Error e = m->run(m->run_user, argv, err);
    if (e != ERR_OK) return strstr(err->msg, "No such process") ? ERR_OK : e;
    char *print[] = {"launchctl", "print", target, NULL};
    for (int i = 0; i < 150; i++) {
        Err ignored;
        if (m->run(m->run_user, print, &ignored) != ERR_OK) return ERR_OK;
        usleep(100 * 1000);
    }
    return err_set(err, ERR_PLATFORM, "launchctl bootout: the dbox agent did not stop within 15 seconds");
}

Error service_manager_enable(const ServiceManager *m, const char *executable, Err *err) {
    char *unit = service_unit_path(m);
    char *text = definition(m, executable);
    char *dir = path_dir(unit);
    Error e = mkdir_p(dir, 0755, err);
    if (e == ERR_OK) e = write_file(unit, text, strlen(text), 0644, err);
    if (e == ERR_OK && m->os == OS_DARWIN) {
        char domain[32];
        snprintf(domain, sizeof domain, "gui/%d", m->uid);
        char *argv[] = {"launchctl", "bootstrap", domain, unit, NULL};
        e = bootout(m, err);
        if (e == ERR_OK) e = m->run(m->run_user, argv, err);
    } else if (e == ERR_OK) {
        char *reload[] = {"systemctl", "--user", "daemon-reload", NULL};
        char *enable[] = {"systemctl", "--user", "enable", "--now", LABEL ".service", NULL};
        e = m->run(m->run_user, reload, err);
        if (e == ERR_OK) e = m->run(m->run_user, enable, err);
    }
    xfree(dir);
    xfree(text);
    xfree(unit);
    return e;
}

Error service_manager_disable(const ServiceManager *m, Err *err) {
    char *unit = service_unit_path(m);
    struct stat st;
    Error e;
    if (stat(unit, &st) != 0) {
        e = err_set(err, ERR_NOT_FOUND, "no service installed at %s", unit);
    } else {
        char *stop[] = {"systemctl", "--user", "disable", "--now", LABEL ".service", NULL};
        e = m->os == OS_DARWIN ? bootout(m, err) : m->run(m->run_user, stop, err);
        if (e == ERR_OK && unlink(unit) != 0) e = err_sys(err, "%s", unit);
        if (e == ERR_OK && m->os != OS_DARWIN) {
            char *reload[] = {"systemctl", "--user", "daemon-reload", NULL};
            e = m->run(m->run_user, reload, err);
        }
    }
    xfree(unit);
    return e;
}

Error service_enable(const char *executable, char **unit, Err *err) {
    ServiceManager m;
    Error e = service_manager(&m, err);
    if (e != ERR_OK) return e;
    *unit = service_unit_path(&m);
    return service_manager_enable(&m, executable, err);
}

Error service_disable(char **unit, Err *err) {
    ServiceManager m;
    Error e = service_manager(&m, err);
    if (e != ERR_OK) return e;
    *unit = service_unit_path(&m);
    return service_manager_disable(&m, err);
}

bool service_installed(char **unit) {
    ServiceManager m;
    Err err;
    *unit = NULL;
    if (service_manager(&m, &err) != ERR_OK) return false;
    *unit = service_unit_path(&m);
    struct stat st;
    return stat(*unit, &st) == 0;
}

static char *running_executable(void) {
    char buf[PATH_MAX];
#ifdef __APPLE__
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return NULL;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
#endif
    char resolved[PATH_MAX];
    return xstrdup(realpath(buf, resolved) ? resolved : buf);
}

static char *find_on_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return NULL;
    char *copy = xstrdup(path), *save = NULL;
    char *found = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
        char *candidate = path_join(*dir ? dir : ".", name);
        if (access(candidate, X_OK) == 0) found = candidate;
        else xfree(candidate);
    }
    xfree(copy);
    return found;
}

Error service_executable_path(char **out, Err *err) {
    *out = NULL;
    char *exe = running_executable();
    if (!exe) return err_set(err, ERR_PLATFORM, "cannot find the running executable");
    char *on_path = find_on_path(LABEL);
    struct stat a, b;
    if (on_path && stat(on_path, &a) == 0 && stat(exe, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
        xfree(exe);
        *out = on_path;
        return ERR_OK;
    }
    xfree(on_path);
    *out = exe;
    return ERR_OK;
}
