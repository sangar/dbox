#ifndef DBOX_SERVICE_H
#define DBOX_SERVICE_H

#include <stdbool.h>

#include "util.h"

/* `dbox run` as a per-user login service: a launchd agent on macOS, a systemd user unit on Linux. */

typedef enum { OS_DARWIN, OS_LINUX, OS_OTHER } ServiceOs;

typedef struct {
    ServiceOs os;
    const char *home;
    const char *config_home;
    int uid;
} ServiceManager;

/* service_run executes a service manager command; tests replace it. */
extern bool (*service_run)(char *const argv[], Err *err);

/* service_manager describes this user's session; it fails where no service manager is known. */
bool service_manager(ServiceManager *m, Err *err);
char *service_unit_path(const ServiceManager *m);
bool service_manager_enable(const ServiceManager *m, const char *executable, Err *err);
bool service_manager_disable(const ServiceManager *m, Err *err);
char *service_launchd_plist(const char *executable, const char *log_path);

/* service_enable writes the definition for executable and starts it now and at every login; *unit receives the definition's path. */
bool service_enable(const char *executable, char **unit, Err *err);
bool service_disable(char **unit, Err *err);
/* service_installed reports whether a definition written by service_enable exists, and where. */
bool service_installed(char **unit);
/*
 * service_executable_path is the dbox found on PATH when it is this same
 * binary, because that survives upgrades that move the real file, otherwise
 * the running executable.
 */
char *service_executable_path(Err *err);

#endif
