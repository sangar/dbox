#include "service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LABEL "dbox"

Error service_run_command(void *user, char *const argv[], Err *err) { return process_run(argv, err); }

Error service_manager(ServiceManager *m, Err *err) {
    memset(m, 0, sizeof *m);
    m->home = env_home();
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(m->config_home_storage, sizeof m->config_home_storage, "%s", xdg);
    else snprintf(m->config_home_storage, sizeof m->config_home_storage, "%s/.config", m->home);
    m->config_home = m->config_home_storage;
    m->uid = process_uid();
    m->run = service_run_command;
    m->os = os_current();
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
        sleep_ms(100);
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
    FileStat info;
    Error e;
    if (file_info_follow(unit, &info, NULL) != ERR_OK || !info.exists) {
        e = err_set(err, ERR_NOT_FOUND, "no service installed at %s", unit);
    } else {
        char *stop[] = {"systemctl", "--user", "disable", "--now", LABEL ".service", NULL};
        e = m->os == OS_DARWIN ? bootout(m, err) : m->run(m->run_user, stop, err);
        if (e == ERR_OK) e = file_remove(unit, err);
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
    FileStat info;
    return file_info_follow(*unit, &info, NULL) == ERR_OK && info.exists;
}

static char *find_on_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return NULL;
    char *copy = xstrdup(path), *save = NULL;
    char *found = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
        char *candidate = path_join(*dir ? dir : ".", name);
        if (path_is_executable(candidate)) found = candidate;
        else xfree(candidate);
    }
    xfree(copy);
    return found;
}

Error service_executable_path(char **out, Err *err) {
    *out = NULL;
    char *exe = process_executable_path();
    if (!exe) return err_set(err, ERR_PLATFORM, "cannot find the running executable");
    char *on_path = find_on_path(LABEL);
    if (on_path && paths_are_same_file(on_path, exe)) {
        xfree(exe);
        *out = on_path;
        return ERR_OK;
    }
    xfree(on_path);
    *out = exe;
    return ERR_OK;
}
