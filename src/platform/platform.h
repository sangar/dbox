#ifndef DBOX_PLATFORM_H
#define DBOX_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "strbuf.h"

/*
 * Everything dbox needs from the operating system, in portable types. The
 * implementations in this directory are the only files that include OS
 * headers or test which OS they are built for.
 */

/* ---- time ---- */

int64_t monotonic_ns(void);
int64_t wall_ns(void);
void sleep_ms(int64_t ms);

/* ---- threads ---- */

typedef struct {
    alignas(8) unsigned char storage[64];
} Mutex;

typedef struct {
    alignas(8) unsigned char storage[64];
} Cond;

typedef struct {
    alignas(8) unsigned char storage[16];
} Thread;

void mutex_init(Mutex *m);
void mutex_destroy(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);

void cond_init(Cond *c);
void cond_destroy(Cond *c);
void cond_signal(Cond *c);
void cond_broadcast(Cond *c);
void cond_wait(Cond *c, Mutex *m);
/* cond_wait_until returns when signalled or once the monotonic deadline has passed. */
void cond_wait_until(Cond *c, Mutex *m, int64_t deadline_ns);

void thread_start(Thread *t, void *(*fn)(void *), void *arg);
void thread_join(Thread *t);
void thread_detach(Thread *t);

/* ---- files and directories ---- */

typedef struct {
    bool exists, is_dir, is_regular;
    int64_t size;
    int64_t mtime_ns;
    unsigned mode; /* permission bits */
} FileStat;

/* file_info describes path without following a final symlink; a missing path is ERR_OK with exists false. */
[[nodiscard]] Error file_info(const char *path, FileStat *out, Err *err);
/* file_info_follow is file_info through symlinks. */
[[nodiscard]] Error file_info_follow(const char *path, FileStat *out, Err *err);
[[nodiscard]] Error file_info_fd(int fd, FileStat *out, Err *err);
/* file_open_read is ERR_NOT_FOUND when path or a directory on the way does not exist. */
[[nodiscard]] Error file_open_read(const char *path, int *fd, Err *err);
/* file_create opens path for writing, creating or truncating it. */
[[nodiscard]] Error file_create(const char *path, unsigned mode, int *fd, Err *err);
/* file_mkstemp replaces the trailing XXXXXX of template with a unique name and opens that file. */
[[nodiscard]] Error file_mkstemp(char *template, int *fd, Err *err);
void file_close(int fd);
/* file_read reads up to n bytes; *got is 0 at end of file. */
[[nodiscard]] Error file_read(int fd, void *buf, size_t n, size_t *got, Err *err);
[[nodiscard]] Error file_pread(int fd, void *buf, size_t n, int64_t offset, size_t *got, Err *err);
bool file_write_all(int fd, const void *data, size_t len);
[[nodiscard]] Error file_seek_start(int fd, Err *err);
int64_t file_position(int fd);
[[nodiscard]] Error file_truncate(int fd, Err *err);
[[nodiscard]] Error file_rename(const char *from, const char *to, Err *err);
/* file_remove is ERR_NOT_FOUND when nothing was there. */
[[nodiscard]] Error file_remove(const char *path, Err *err);
[[nodiscard]] Error file_set_mtime(const char *path, int64_t ns, Err *err);

/* dir_create is ERR_OK when the directory already exists. */
[[nodiscard]] Error dir_create(const char *path, unsigned mode, Err *err);
[[nodiscard]] Error dir_remove(const char *path, Err *err);
/* dir_list appends the sorted entry names of path, without . and .., to names; a missing path is ERR_NOT_FOUND. */
[[nodiscard]] Error dir_list(const char *path, StrList *names, Err *err);
/* dir_make_temp replaces the trailing XXXXXX of template with a unique name and creates that directory. */
[[nodiscard]] Error dir_make_temp(char *template, Err *err);
[[nodiscard]] Error dir_remove_tree(const char *path, Err *err);

/* path_resolve returns the canonical absolute path, owned by the caller, or NULL. */
char *path_resolve(const char *path);
bool path_is_executable(const char *path);
bool paths_are_same_file(const char *a, const char *b);
/* glob_match is shell-style matching where * and ? do not cross a slash. */
bool glob_match(const char *pattern, const char *name);

/* ---- the process and its environment ---- */

typedef enum { OS_DARWIN, OS_LINUX, OS_OTHER } Os;

Os os_current(void);
int process_id(void);
bool process_alive(int pid);
/* process_signal_reload asks pid to re-read its config. */
[[nodiscard]] Error process_signal_reload(int pid, Err *err);
/* process_executable_path returns the running binary's resolved path, owned by the caller, or NULL. */
char *process_executable_path(void);
/* process_raise_file_limit lifts the soft open-file limit to the hard limit. */
[[nodiscard]] Error process_raise_file_limit(Err *err);
/* process_run runs argv to completion with its output captured; a failure's message carries that output. */
[[nodiscard]] Error process_run(char *const argv[], Err *err);
/* process_run_interactive runs argv sharing this process's terminal. */
[[nodiscard]] Error process_run_interactive(char *const argv[], Err *err);
int process_uid(void);
const char *env_home(void);
/* env_all is the environment as NAME=value strings, NULL-terminated. */
char *const *env_all(void);
void env_set(const char *name, const char *value);
void env_unset(const char *name);
const char *host_short_name(char buf[256]);

/* ---- signals ---- */

typedef enum { SIGNAL_STOP, SIGNAL_RELOAD } Signal;

/* signals_block keeps stop and reload signals from interrupting any thread; call it before starting threads. */
void signals_block(void);
/* signals_wait blocks until a stop or reload signal arrives. */
Signal signals_wait(void);

/* ---- sockets ---- */

/* net_listen binds host:port and listens on it. */
[[nodiscard]] Error net_listen(const char *addr, int *fd, Err *err);
/* net_accept waits up to timeout_ms for a connection; *client is -1 when none arrived. */
void net_accept(int fd, int timeout_ms, int *client);
void net_set_timeouts(int fd, int seconds);
/* net_free_port finds a loopback address with a free port, as host:port. */
[[nodiscard]] Error net_free_port(char addr[64], Err *err);

/* ---- file system events ---- */

typedef struct WatchBackend WatchBackend;
typedef void (*WatchEventFn)(void *user, const char *abs_path, bool rescan_subdirs);

[[nodiscard]] Error watch_backend_open(const char *root, WatchEventFn fn, void *user, WatchBackend **out, Err *err);
/* watch_backend_add_dir watches one directory; it is a no-op where the backend watches whole trees. */
[[nodiscard]] Error watch_backend_add_dir(WatchBackend *b, const char *path, Err *err);
/* watch_backend_poll delivers events through fn, waiting up to timeout_ms for them. */
void watch_backend_poll(WatchBackend *b, int timeout_ms);
void watch_backend_close(WatchBackend *b);

/* err_sys is err_set with the OS's description of the last failure appended; ENOENT maps to ERR_NOT_FOUND, else ERR_IO. */
[[nodiscard]] Error err_sys(Err *err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
