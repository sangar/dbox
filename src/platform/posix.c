/* The POSIX side of platform.h, shared by macOS and Linux. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "alloc.h"
#include "platform.h"

extern char **environ;

static_assert(sizeof(pthread_mutex_t) <= sizeof(Mutex), "Mutex storage is too small");
static_assert(sizeof(pthread_cond_t) <= sizeof(Cond), "Cond storage is too small");
static_assert(sizeof(pthread_t) <= sizeof(Thread), "Thread storage is too small");

#define NS_PER_SEC 1000000000LL
#define NS_PER_MS 1000000LL

Error err_sys(Err *err, const char *fmt, ...) {
    int saved = errno;
    Error code = saved == ENOENT ? ERR_NOT_FOUND : ERR_IO;
    if (!err) return code;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n < sizeof err->msg) snprintf(err->msg + n, sizeof err->msg - (size_t)n, ": %s", strerror(saved));
    return code;
}

/* ---- time ---- */

int64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

int64_t wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

void sleep_ms(int64_t ms) {
    struct timespec ts = {.tv_sec = (time_t)(ms / 1000), .tv_nsec = (long)(ms % 1000) * NS_PER_MS};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* ---- threads ---- */

static pthread_mutex_t *mu(Mutex *m) { return (pthread_mutex_t *)m->storage; }
static pthread_cond_t *cv(Cond *c) { return (pthread_cond_t *)c->storage; }
static pthread_t *th(Thread *t) { return (pthread_t *)t->storage; }

void mutex_init(Mutex *m) { pthread_mutex_init(mu(m), NULL); }
void mutex_destroy(Mutex *m) { pthread_mutex_destroy(mu(m)); }
void mutex_lock(Mutex *m) { pthread_mutex_lock(mu(m)); }
void mutex_unlock(Mutex *m) { pthread_mutex_unlock(mu(m)); }

void cond_init(Cond *c) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#ifndef __APPLE__
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(cv(c), &attr);
    pthread_condattr_destroy(&attr);
}

void cond_destroy(Cond *c) { pthread_cond_destroy(cv(c)); }
void cond_signal(Cond *c) { pthread_cond_signal(cv(c)); }
void cond_broadcast(Cond *c) { pthread_cond_broadcast(cv(c)); }
void cond_wait(Cond *c, Mutex *m) { pthread_cond_wait(cv(c), mu(m)); }

void cond_wait_until(Cond *c, Mutex *m, int64_t deadline_ns) {
    int64_t remaining = deadline_ns - monotonic_ns();
    if (remaining <= 0) return;
#ifdef __APPLE__
    struct timespec rel = {.tv_sec = remaining / NS_PER_SEC, .tv_nsec = remaining % NS_PER_SEC};
    pthread_cond_timedwait_relative_np(cv(c), mu(m), &rel);
#else
    struct timespec abs = {.tv_sec = deadline_ns / NS_PER_SEC, .tv_nsec = deadline_ns % NS_PER_SEC};
    pthread_cond_timedwait(cv(c), mu(m), &abs);
#endif
}

void thread_start(Thread *t, void *(*fn)(void *), void *arg) {
    if (pthread_create(th(t), NULL, fn, arg) != 0) {
        fputs("dbox: cannot start a thread\n", stderr);
        abort();
    }
}

void thread_join(Thread *t) { pthread_join(*th(t), NULL); }
void thread_detach(Thread *t) { pthread_detach(*th(t)); }

/* ---- files and directories ---- */

static void describe(FileStat *out, const struct stat *st) {
    out->exists = true;
    out->is_dir = S_ISDIR(st->st_mode);
    out->is_regular = S_ISREG(st->st_mode);
    out->size = st->st_size;
#ifdef __APPLE__
    out->mtime_ns = (int64_t)st->st_mtimespec.tv_sec * NS_PER_SEC + st->st_mtimespec.tv_nsec;
#else
    out->mtime_ns = (int64_t)st->st_mtim.tv_sec * NS_PER_SEC + st->st_mtim.tv_nsec;
#endif
    out->mode = st->st_mode & 0777;
}

static Error info_from(int rc, const char *path, const struct stat *st, FileStat *out, Err *err) {
    memset(out, 0, sizeof *out);
    if (rc == 0) {
        describe(out, st);
        return ERR_OK;
    }
    if (errno == ENOENT || errno == ENOTDIR) return ERR_OK;
    return err_sys(err, "%s", path);
}

Error file_info(const char *path, FileStat *out, Err *err) {
    struct stat st;
    return info_from(lstat(path, &st), path, &st, out, err);
}

Error file_info_follow(const char *path, FileStat *out, Err *err) {
    struct stat st;
    return info_from(stat(path, &st), path, &st, out, err);
}

Error file_info_fd(int fd, FileStat *out, Err *err) {
    struct stat st;
    memset(out, 0, sizeof *out);
    if (fstat(fd, &st) != 0) return err_sys(err, "fstat");
    describe(out, &st);
    return ERR_OK;
}

Error file_open_read(const char *path, int *fd, Err *err) {
    *fd = open(path, O_RDONLY | O_CLOEXEC);
    if (*fd >= 0) return ERR_OK;
    if (errno == ENOENT || errno == ENOTDIR) return ERR_NOT_FOUND;
    return err_sys(err, "%s", path);
}

Error file_create(const char *path, unsigned mode, int *fd, Err *err) {
    *fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mode_t)mode);
    return *fd >= 0 ? ERR_OK : err_sys(err, "%s", path);
}

Error file_mkstemp(char *template, int *fd, Err *err) {
    *fd = mkstemp(template);
    return *fd >= 0 ? ERR_OK : err_sys(err, "%s", template);
}

void file_close(int fd) { close(fd); }

Error file_read(int fd, void *buf, size_t n, size_t *got, Err *err) {
    for (;;) {
        ssize_t r = read(fd, buf, n);
        if (r >= 0) {
            *got = (size_t)r;
            return ERR_OK;
        }
        if (errno != EINTR) return err_sys(err, "read");
    }
}

Error file_pread(int fd, void *buf, size_t n, int64_t offset, size_t *got, Err *err) {
    for (;;) {
        ssize_t r = pread(fd, buf, n, offset);
        if (r >= 0) {
            *got = (size_t)r;
            return ERR_OK;
        }
        if (errno != EINTR) return err_sys(err, "read");
    }
}

bool file_write_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    return true;
}

Error file_seek_start(int fd, Err *err) { return lseek(fd, 0, SEEK_SET) < 0 ? err_sys(err, "seek") : ERR_OK; }

int64_t file_position(int fd) { return lseek(fd, 0, SEEK_CUR); }

Error file_truncate(int fd, Err *err) {
    if (lseek(fd, 0, SEEK_SET) < 0 || ftruncate(fd, 0) != 0) return err_sys(err, "truncate");
    return ERR_OK;
}

Error file_rename(const char *from, const char *to, Err *err) { return rename(from, to) == 0 ? ERR_OK : err_sys(err, "rename %s", to); }

Error file_remove(const char *path, Err *err) {
    if (unlink(path) == 0) return ERR_OK;
    if (errno == ENOENT || errno == ENOTDIR) return ERR_NOT_FOUND;
    return err_sys(err, "%s", path);
}

Error file_set_mtime(const char *path, int64_t ns, Err *err) {
    struct timespec times[2];
    times[0].tv_sec = times[1].tv_sec = (time_t)(ns / NS_PER_SEC);
    times[0].tv_nsec = times[1].tv_nsec = (long)(ns % NS_PER_SEC);
    return utimensat(AT_FDCWD, path, times, 0) == 0 ? ERR_OK : err_sys(err, "set mtime %s", path);
}

Error dir_create(const char *path, unsigned mode, Err *err) {
    if (mkdir(path, (mode_t)mode) == 0 || errno == EEXIST) return ERR_OK;
    return err_sys(err, "mkdir %s", path);
}

Error dir_remove(const char *path, Err *err) { return rmdir(path) == 0 ? ERR_OK : err_sys(err, "rmdir %s", path); }

static int compare_names(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

Error dir_list(const char *path, StrList *names, Err *err) {
    DIR *d = opendir(path);
    if (!d) return errno == ENOENT || errno == ENOTDIR ? ERR_NOT_FOUND : err_sys(err, "%s", path);
    size_t first = names->len;
    struct dirent *ent;
    while ((ent = readdir(d)))
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) strlist_push(names, ent->d_name);
    closedir(d);
    qsort(names->items + first, names->len - first, sizeof *names->items, compare_names);
    return ERR_OK;
}

Error dir_make_temp(char *template, Err *err) { return mkdtemp(template) ? ERR_OK : err_sys(err, "%s", template); }

Error dir_remove_tree(const char *path, Err *err) {
    FileStat info;
    Error e = file_info(path, &info, err);
    if (e != ERR_OK || !info.exists) return e;
    if (!info.is_dir) return file_remove(path, err);
    StrList names = {0};
    e = dir_list(path, &names, err);
    for (size_t i = 0; e == ERR_OK && i < names.len; i++) {
        size_t n = strlen(path) + 1 + strlen(names.items[i]) + 1;
        char *child = xmalloc(n);
        snprintf(child, n, "%s/%s", path, names.items[i]);
        e = dir_remove_tree(child, err);
        xfree(child);
    }
    strlist_free(&names);
    return e == ERR_OK ? dir_remove(path, err) : e;
}

char *path_resolve(const char *path) {
    char resolved[PATH_MAX];
    return realpath(path, resolved) ? xstrdup(resolved) : NULL;
}

bool path_is_executable(const char *path) { return access(path, X_OK) == 0; }

bool paths_are_same_file(const char *a, const char *b) {
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

bool glob_match(const char *pattern, const char *name) { return fnmatch(pattern, name, FNM_PATHNAME) == 0; }

/* ---- the process and its environment ---- */

int process_id(void) { return (int)getpid(); }

bool process_alive(int pid) { return kill((pid_t)pid, 0) == 0 || errno == EPERM; }

Error process_signal_reload(int pid, Err *err) {
    if (kill((pid_t)pid, SIGHUP) != 0) return err_set(err, ERR_PLATFORM, "signal pid %d: %s", pid, strerror(errno));
    return ERR_OK;
}

Error process_raise_file_limit(Err *err) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return err_set(err, ERR_PLATFORM, "getrlimit: %s", strerror(errno));
    if (limit.rlim_cur >= limit.rlim_max) return ERR_OK;
    limit.rlim_cur = limit.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &limit) == 0) return ERR_OK;
    /* macOS rejects RLIM_INFINITY for the soft limit; OPEN_MAX-sized values work. */
    limit.rlim_cur = 1 << 20;
    if (setrlimit(RLIMIT_NOFILE, &limit) == 0) return ERR_OK;
    return err_set(err, ERR_PLATFORM, "setrlimit: %s", strerror(errno));
}

static void describe_command(StrBuf *sb, char *const argv[]) {
    for (size_t i = 0; argv[i]; i++) sb_printf(sb, "%s%s", i ? " " : "", argv[i]);
}

static Error exit_status(char *const argv[], int rc, pid_t pid, const StrBuf *out, Err *err) {
    int status = 0;
    if (rc == 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) return ERR_OK;
    StrBuf cmd = {0};
    describe_command(&cmd, argv);
    Error e;
    if (rc) e = err_set(err, ERR_PLATFORM, "%s: %s", sb_cstr(&cmd), strerror(rc));
    else e = err_set(err, ERR_PLATFORM, "%s: exit status %d%s%s", sb_cstr(&cmd), WIFEXITED(status) ? WEXITSTATUS(status) : -1, out && out->len ? ": " : "", out && out->len ? out->data : "");
    sb_free(&cmd);
    return e;
}

Error process_run(char *const argv[], Err *err) {
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
    while (out.len > 0 && (out.data[out.len - 1] == '\n' || out.data[out.len - 1] == ' ')) out.len--;
    sb_cstr(&out);
    Error e = exit_status(argv, rc, pid, &out, err);
    sb_free(&out);
    return e;
}

Error process_run_interactive(char *const argv[], Err *err) {
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    return exit_status(argv, rc, pid, NULL, err);
}

int process_uid(void) { return (int)getuid(); }

const char *env_home(void) {
    const char *home = getenv("HOME");
    if (home && *home) return home;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : ".";
}

char *const *env_all(void) { return environ; }

void env_set(const char *name, const char *value) { setenv(name, value, 1); }

void env_unset(const char *name) { unsetenv(name); }

const char *host_short_name(char buf[256]) {
    if (gethostname(buf, 256) != 0) buf[0] = '\0';
    buf[255] = '\0';
    char *dot = strchr(buf, '.');
    if (dot && dot != buf) *dot = '\0';
    return buf;
}

/* ---- signals ---- */

static void stop_and_reload_set(sigset_t *set) {
    sigemptyset(set);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGHUP);
}

void signals_block(void) {
    sigset_t set;
    stop_and_reload_set(&set);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

Signal signals_wait(void) {
    sigset_t set;
    stop_and_reload_set(&set);
    for (;;) {
        int sig;
        if (sigwait(&set, &sig) != 0) continue;
        return sig == SIGHUP ? SIGNAL_RELOAD : SIGNAL_STOP;
    }
}

/* ---- sockets ---- */

Error net_listen(const char *addr, int *out, Err *err) {
    *out = -1;
    const char *colon = strrchr(addr, ':');
    if (!colon) return err_set(err, ERR_INVALID_ARGUMENT, "listen %s: want host:port", addr);
    char *host = xstrndup(addr, (size_t)(colon - addr));
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE | AI_NUMERICSERV};
    struct addrinfo *res;
    int rc = getaddrinfo(*host ? host : NULL, colon + 1, &hints, &res);
    xfree(host);
    if (rc != 0) return err_set(err, ERR_PLATFORM, "listen %s: %s", addr, gai_strerror(rc));
    int fd = -1;
    Error e = ERR_OK;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || listen(fd, 16) != 0) {
            e = err_set(err, ERR_PLATFORM, "listen %s: %s", addr, strerror(errno));
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) return e != ERR_OK ? e : err_set(err, ERR_PLATFORM, "listen %s: no usable address", addr);
    *out = fd;
    return ERR_OK;
}

void net_accept(int fd, int timeout_ms, int *client) {
    *client = -1;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, timeout_ms) <= 0) return;
    *client = accept(fd, NULL, NULL);
}

void net_set_timeouts(int fd, int seconds) {
    struct timeval timeout = {.tv_sec = seconds};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
}

Error net_free_port(char addr[64], Err *err) {
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe < 0) return err_set(err, ERR_PLATFORM, "socket: %s", strerror(errno));
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof sa;
    Error e = ERR_OK;
    if (bind(probe, (struct sockaddr *)&sa, sizeof sa) != 0 || getsockname(probe, (struct sockaddr *)&sa, &len) != 0) e = err_set(err, ERR_PLATFORM, "bind: %s", strerror(errno));
    else snprintf(addr, 64, "127.0.0.1:%d", ntohs(sa.sin_port));
    close(probe);
    return e;
}
