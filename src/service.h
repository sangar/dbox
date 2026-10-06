#ifndef DBOX_SERVICE_H
#define DBOX_SERVICE_H

#include <stdbool.h>

#include "util.h"

/* `dbox run` as a per-user login service: a launchd agent on macOS, a systemd user unit on Linux. */

typedef enum { OS_DARWIN, OS_LINUX, OS_OTHER } ServiceOs;

/* ServiceRunner executes a service manager command such as launchctl or systemctl; tests record them instead. */
typedef Error (*ServiceRunner)(void *user, char *const argv[], Err *err);

typedef struct {
    ServiceOs os;
    const char *home;
    const char *config_home;
    int uid;
    ServiceRunner run;
    void *run_user;
    char config_home_storage[1024];
} ServiceManager;

/* service_run_command is the ServiceRunner that spawns the command and captures its output. */
[[nodiscard]] Error service_run_command(void *user, char *const argv[], Err *err);

/* service_manager describes this user's session; it fails where no service manager is known. */
[[nodiscard]] Error service_manager(ServiceManager *m, Err *err);
char *service_unit_path(const ServiceManager *m);
[[nodiscard]] Error service_manager_enable(const ServiceManager *m, const char *executable, Err *err);
[[nodiscard]] Error service_manager_disable(const ServiceManager *m, Err *err);
char *service_launchd_plist(const char *executable, const char *log_path);

/* service_enable writes the definition for executable and starts it now and at every login; *unit receives the definition's path. */
[[nodiscard]] Error service_enable(const char *executable, char **unit, Err *err);
[[nodiscard]] Error service_disable(char **unit, Err *err);
/* service_installed reports whether a definition written by service_enable exists, and where. */
bool service_installed(char **unit);
/*
 * service_executable_path is the dbox found on PATH when it is this same
 * binary, because that survives upgrades that move the real file, otherwise
 * the running executable.
 */
[[nodiscard]] Error service_executable_path(char **out, Err *err);

#endif
