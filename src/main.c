/* dbox keeps a local folder in sync with S3-compatible stores. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "ctx.h"
#include "daemon.h"
#include "engine.h"
#include "index.h"
#include "log.h"
#include "service.h"
#include "store.h"

#ifndef DBOX_VERSION
#define DBOX_VERSION "dev"
#endif

extern char **environ;

static const char usage_text[] =
    "dbox - keep a folder in sync with S3-compatible stores\n"
    "\n"
    "Usage:\n"
    "  dbox run [--once] [--dry-run]     the daemon; --once reconciles, copies to mirrors and exits\n"
    "  dbox status                       daemon and service state, per-store copies, failed files\n"
    "  dbox retry STORE                  make failed copies on STORE pending again\n"
    "  dbox check STORE                  write, read back and delete a probe object on STORE\n"
    "  dbox promote STORE [--force]      make STORE the primary; the old primary becomes a mirror\n"
    "  dbox service enable|disable       run dbox run at login (launchd agent or systemd user unit)\n"
    "  dbox config [--init]              show the effective config, or write a starter file\n"
    "  dbox config edit                  open the config in $VISUAL or $EDITOR, then check and reload it\n"
    "  dbox reload                       check the config and ask the running daemon to re-read it\n"
    "  dbox version\n"
    "\n"
    "Every command takes --config FILE (default %s, env DBOX_CONFIG).\n";

/* Exit statuses: 1 for failures, 2 for mistakes on the command line. */
enum { EXIT_USAGE = 2 };

static Logger logger;

static void print_usage(FILE *out) {
    char *path = config_default_path();
    fprintf(out, usage_text, path);
    free(path);
}

static int fail(const Err *err) {
    fprintf(stderr, "dbox: %s\n", err->msg);
    return EXIT_FAILURE;
}

static int usage_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static int usage_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("dbox: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputs(" (see dbox --help)\n", stderr);
    va_end(ap);
    return EXIT_USAGE;
}

/* ---- flags ---- */

enum { FLAG_ONCE = 1, FLAG_DRY_RUN = 2, FLAG_FORCE = 4, FLAG_INIT = 8 };

typedef struct {
    char *config;
    bool once, dry_run, force, init;
    StrList positional;
} Flags;

static void flags_free(Flags *f) {
    free(f->config);
    strlist_free(&f->positional);
}

/* parse_flags reads the flags a command allows, in any position, and keeps the rest as positional arguments. It returns an exit status, or -1 to continue. */
static int parse_flags(Flags *f, int allowed, int argc, char **argv) {
    memset(f, 0, sizeof *f);
    f->config = config_default_path();
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-' || strcmp(arg, "-") == 0) {
            strlist_push(&f->positional, arg);
            continue;
        }
        const char *name = arg + (arg[1] == '-' ? 2 : 1);
        if (strcmp(name, "h") == 0 || strcmp(name, "help") == 0) {
            print_usage(stdout);
            return EXIT_SUCCESS;
        }
        if (has_prefix(name, "config")) {
            const char *value = name[6] == '=' ? name + 7 : NULL;
            if (name[6] != '=' && name[6] != '\0') return usage_fail("flag provided but not defined: %s", arg);
            if (!value) {
                if (i + 1 >= argc) return usage_fail("flag needs an argument: %s", arg);
                value = argv[++i];
            }
            free(f->config);
            f->config = xstrdup(value);
            continue;
        }
        if ((allowed & FLAG_ONCE) && strcmp(name, "once") == 0) f->once = true;
        else if ((allowed & FLAG_DRY_RUN) && strcmp(name, "dry-run") == 0) f->dry_run = true;
        else if ((allowed & FLAG_FORCE) && strcmp(name, "force") == 0) f->force = true;
        else if ((allowed & FLAG_INIT) && strcmp(name, "init") == 0) f->init = true;
        else return usage_fail("flag provided but not defined: %s", arg);
    }
    return -1;
}

static char *pid_path(const Config *cfg) {
    char *state = config_state_dir(cfg);
    char *p = path_join(state, "daemon.pid");
    free(state);
    return p;
}

static char *index_path(const Config *cfg) {
    char *state = config_state_dir(cfg);
    char *p = path_join(state, "index.db");
    free(state);
    return p;
}

/* ---- signals ---- */

static pthread_mutex_t signal_mu = PTHREAD_MUTEX_INITIALIZER;
static Ctx *current_ctx;
static int pending_signal;

/* signal_main waits for the signals the main thread blocked and cancels whatever run is current. */
static void *signal_main(void *arg) {
    sigset_t *set = arg;
    for (;;) {
        int sig;
        if (sigwait(set, &sig) != 0) continue;
        pthread_mutex_lock(&signal_mu);
        pending_signal = sig;
        if (current_ctx) ctx_cancel(current_ctx);
        pthread_mutex_unlock(&signal_mu);
    }
    return NULL;
}

static void watch_signals(void) {
    static sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    pthread_t t;
    pthread_create(&t, NULL, signal_main, &set);
    pthread_detach(t);
}

/* begin_run makes ctx the one signals cancel; it reports false when a stop signal already arrived. */
static bool begin_run(Ctx *ctx) {
    pthread_mutex_lock(&signal_mu);
    bool stopped = pending_signal == SIGINT || pending_signal == SIGTERM;
    if (!stopped) current_ctx = ctx;
    pthread_mutex_unlock(&signal_mu);
    return !stopped;
}

static int end_run(void) {
    pthread_mutex_lock(&signal_mu);
    current_ctx = NULL;
    int sig = pending_signal;
    if (sig == SIGHUP) pending_signal = 0;
    pthread_mutex_unlock(&signal_mu);
    return sig;
}

/* ---- run ---- */

typedef struct {
    const Config *cfg;
    Index *idx;
    Engine *engine;
} Running;

static bool metrics(void *arg, StrBuf *out, Err *err) {
    Running *r = arg;
    sb_printf(out, "dbox_upload_backlog %zu\n", engine_backlog(r->engine));
    Arena a;
    arena_init(&a, 4096);
    size_t n;
    const char **names = config_store_names(r->cfg, &a, &n);
    bool ok = true;
    for (size_t i = 0; ok && i < n; i++) {
        Stats s;
        ok = index_stats(r->idx, names[i], &s, err);
        if (!ok) break;
        const char *role = role_name(config_store(r->cfg, names[i])->role);
        sb_printf(out, "dbox_files{store=\"%s\",role=\"%s\"} %lld\n", names[i], role, (long long)s.files);
        sb_printf(out, "dbox_replicas{store=\"%s\",role=\"%s\",state=\"verified\"} %lld\n", names[i], role, (long long)s.verified);
        sb_printf(out, "dbox_replicas{store=\"%s\",role=\"%s\",state=\"pending\"} %lld\n", names[i], role, (long long)s.pending);
        sb_printf(out, "dbox_replicas{store=\"%s\",role=\"%s\",state=\"failed\"} %lld\n", names[i], role, (long long)s.failed);
        sb_printf(out, "dbox_verified_bytes{store=\"%s\",role=\"%s\"} %lld\n", names[i], role, (long long)s.verified_bytes);
    }
    arena_free(&a);
    return ok;
}

static long backlog(void *arg) { return (long)engine_backlog(((Running *)arg)->engine); }

typedef bool (*EngineFn)(Engine *e, Index *idx, Ctx *ctx, const Config *cfg, Err *err);

static bool with_engine(Ctx *ctx, const Config *cfg, bool dry_run, EngineFn fn, Err *err) {
    if (!config_primary(cfg)) {
        err_set(err, "%s: no stores configured", cfg->path);
        return false;
    }
    char *ip = index_path(cfg);
    Index *idx = index_open(ip, err);
    free(ip);
    if (!idx) return false;
    StoreSet stores;
    bool ok = store_open_all(ctx, cfg, &stores, err);
    if (ok) {
        logger_init(&logger, cfg->daemon.log_level, cfg->daemon.log_format, stderr);
        Engine *e = engine_new(cfg, idx, &stores, (EngineOptions){.dry_run = dry_run, .log = &logger});
        ok = fn(e, idx, ctx, cfg, err);
        engine_free(e);
        storeset_close(&stores);
    }
    index_close(idx);
    return ok;
}

static bool run_once(Engine *e, Index *idx, Ctx *ctx, const Config *cfg, Err *err) { return engine_once(e, ctx, err); }

typedef struct {
    Ctx *ctx;
    const Config *cfg;
    Running running;
} ServeArg;

static void *serve_http(void *arg) {
    ServeArg *s = arg;
    Err err;
    if (!daemon_serve(s->ctx, s->cfg->daemon.listen, metrics, backlog, &s->running, &err))
        log_error(&logger, "health listener", LS("addr", s->cfg->daemon.listen), LERR(&err));
    return NULL;
}

static bool run_daemon(Engine *e, Index *idx, Ctx *ctx, const Config *cfg, Err *err) {
    ServeArg arg = {.ctx = ctx, .cfg = cfg, .running = {cfg, idx, e}};
    pthread_t http;
    pthread_create(&http, NULL, serve_http, &arg);
    bool ok = engine_run(e, ctx, err);
    ctx_cancel(ctx);
    pthread_join(http, NULL);
    return ok;
}

/* serve runs the daemon for one config until ctx is done. */
static bool serve(Ctx *ctx, const Config *cfg, Err *err) {
    logger_init(&logger, cfg->daemon.log_level, cfg->daemon.log_format, stderr);
    char *state = config_state_dir(cfg);
    bool ok = mkdir_p(state, 0755, err);
    free(state);
    if (!ok) return false;
    char *pid = pid_path(cfg);
    ok = daemon_write_pid(pid, err);
    if (ok) {
        if (!config_primary(cfg)) {
            log_warn(&logger, "no stores configured; add one to the config and send SIGHUP", LS("config", cfg->path));
            ctx_lock(ctx);
            while (ctx_wait(ctx, 0)) {
            }
            ctx_unlock(ctx);
        } else {
            ok = with_engine(ctx, cfg, false, run_daemon, err);
        }
        daemon_remove_pid(pid);
    }
    free(pid);
    return ok;
}

static int cmd_run(int argc, char **argv) {
    Flags f;
    int rc = parse_flags(&f, FLAG_ONCE | FLAG_DRY_RUN, argc, argv);
    if (rc >= 0) {
        flags_free(&f);
        return rc;
    }
    watch_signals();
    Err err;
    Config cfg;
    if (f.once || f.dry_run) {
        rc = EXIT_FAILURE;
        if (config_load(f.config, &cfg, &err)) {
            char *pid = pid_path(&cfg);
            pid_t other;
            if (daemon_running(pid, &other) && !f.dry_run) {
                err_set(&err, "the daemon is already syncing this folder (pid %d); stop it before running --once", (int)other);
            } else {
                Ctx ctx;
                ctx_init(&ctx);
                begin_run(&ctx);
                if (with_engine(&ctx, &cfg, f.dry_run, run_once, &err)) rc = EXIT_SUCCESS;
                end_run();
                ctx_destroy(&ctx);
            }
            free(pid);
            config_free(&cfg);
        }
        flags_free(&f);
        return rc == EXIT_SUCCESS ? rc : fail(&err);
    }

    logger_init(&logger, "info", "text", stderr);
    if (!daemon_raise_file_limit(&err)) log_warn(&logger, "raise open file limit", LERR(&err));
    for (;;) {
        if (!config_load(f.config, &cfg, &err)) {
            flags_free(&f);
            return fail(&err);
        }
        Ctx ctx;
        ctx_init(&ctx);
        bool ok = true;
        if (begin_run(&ctx)) ok = serve(&ctx, &cfg, &err);
        int sig = end_run();
        ctx_destroy(&ctx);
        if (sig == SIGHUP) {
            log_info(&logger, "reloading config", LS("path", cfg.path));
            if (!ok) log_warn(&logger, "stopped for reload", LERR(&err));
            config_free(&cfg);
            continue;
        }
        config_free(&cfg);
        flags_free(&f);
        return ok ? EXIT_SUCCESS : fail(&err);
    }
}

/* ---- status ---- */

static double percent(const Stats *s) {
    if (s->bytes == 0) return s->files == s->verified ? 100 : 0;
    return 100.0 * (double)s->verified_bytes / (double)s->bytes;
}

static int cmd_status(int argc, char **argv) {
    Flags f;
    int rc = parse_flags(&f, 0, argc, argv);
    if (rc >= 0) {
        flags_free(&f);
        return rc;
    }
    Err err;
    Config cfg;
    bool loaded = config_load(f.config, &cfg, &err);
    flags_free(&f);
    if (!loaded) return fail(&err);
    printf("config:  %s\nroot:    %s\n", cfg.path, cfg.sync.root);
    char *pid = pid_path(&cfg);
    pid_t running;
    if (daemon_running(pid, &running)) printf("daemon:  running, pid %d\n", (int)running);
    else printf("daemon:  not running\n");
    free(pid);
    char *unit;
    if (service_installed(&unit)) printf("service: enabled, %s\n", unit);
    else printf("service: not enabled (run `dbox service enable` to start dbox run at login)\n");
    free(unit);
    if (cfg.store_count == 0) {
        printf("stores:  none configured\n");
        config_free(&cfg);
        return EXIT_SUCCESS;
    }

    char *ip = index_path(&cfg);
    Index *idx = index_open(ip, &err);
    free(ip);
    if (!idx) {
        config_free(&cfg);
        return fail(&err);
    }
    rc = EXIT_FAILURE;
    Arena a;
    arena_init(&a, 16 * 1024);
    printf("\n%-12s %-9s %8s %8s %8s %8s %10s\n", "STORE", "ROLE", "FILES", "VERIFIED", "PENDING", "FAILED", "COPIED");
    UploadFailure *uploads;
    size_t upload_count;
    if (!index_failed_uploads(idx, &a, &uploads, &upload_count, &err)) goto out;
    char backlog_text[32] = "-";
    long backlog;
    if (daemon_ask(cfg.daemon.listen, &backlog)) snprintf(backlog_text, sizeof backlog_text, "%ld", backlog);
    StrList failed = {0};
    size_t n;
    const char **names = config_store_names(&cfg, &a, &n);
    const char *primary = config_primary(&cfg);
    for (size_t i = 0; i < n; i++) {
        Stats s;
        Replica *copies;
        size_t copy_count;
        if (!index_stats(idx, names[i], &s, &err) || !index_failed(idx, &a, names[i], 10, &copies, &copy_count, &err)) {
            strlist_free(&failed);
            goto out;
        }
        for (size_t j = 0; j < copy_count; j++) {
            char *line = arena_printf(&a, "%s  %s  %s", copies[j].store, copies[j].path, copies[j].last_error);
            strlist_push(&failed, line);
        }
        const char *role = role_name(config_store(&cfg, names[i])->role);
        if (strcmp(names[i], primary) == 0) {
            for (size_t j = 0; j < MIN(upload_count, 10); j++) strlist_push(&failed, arena_printf(&a, "%s  %s  %s", names[i], uploads[j].path, uploads[j].last_error));
            /* Every indexed file is on the primary by definition, so its row shows what is still on the way there instead. */
            printf("%-12s %-9s %8lld %8lld %8s %8zu %10s\n", names[i], role, (long long)s.files, (long long)s.verified, backlog_text, upload_count, "-");
            continue;
        }
        printf("%-12s %-9s %8lld %8lld %8lld %8lld %9.0f%%\n", names[i], role, (long long)s.files, (long long)s.verified, (long long)s.pending, (long long)s.failed, percent(&s));
    }
    if (failed.len > 0) {
        printf("\nfailed copies (`dbox retry STORE` to try again):\n");
        for (size_t i = 0; i < failed.len; i++) printf("  %s\n", failed.items[i]);
    }
    strlist_free(&failed);
    rc = EXIT_SUCCESS;
out:
    arena_free(&a);
    index_close(idx);
    config_free(&cfg);
    return rc == EXIT_SUCCESS ? rc : fail(&err);
}

/* store_arg parses a command that takes one store name, loads the config and checks the store exists. It returns an exit status, or -1 to continue. */
static int store_arg(const char *command, int allowed, int argc, char **argv, Flags *f, Config *cfg, const char **name, Err *err) {
    int rc = parse_flags(f, allowed, argc, argv);
    if (rc >= 0) return rc;
    if (f->positional.len != 1) return usage_fail("usage: dbox %s STORE", command);
    if (!config_load(f->config, cfg, err)) return fail(err);
    *name = f->positional.items[0];
    if (!config_store(cfg, *name)) {
        err_set(err, "no store named \"%s\" in %s", *name, cfg->path);
        config_free(cfg);
        return fail(err);
    }
    return -1;
}

static int cmd_retry(int argc, char **argv) {
    Flags f;
    Config cfg;
    const char *name;
    Err err;
    int rc = store_arg("retry", 0, argc, argv, &f, &cfg, &name, &err);
    if (rc >= 0) {
        flags_free(&f);
        return rc;
    }
    char *ip = index_path(&cfg);
    Index *idx = index_open(ip, &err);
    free(ip);
    rc = EXIT_FAILURE;
    if (idx) {
        int64_t n;
        if (strcmp(name, config_primary(&cfg)) == 0) {
            if (index_retry_failed_uploads(idx, &n, &err)) {
                char buf[48];
                printf("%lld failed uploads to %s are due again; the daemon retries them within %s\n", (long long)n, name, format_duration(cfg.sync.pull_interval_ns, buf));
                rc = EXIT_SUCCESS;
            }
        } else if (index_retry_failed(idx, name, &n, &err)) {
            printf("%lld failed copies on %s are pending again\n", (long long)n, name);
            rc = EXIT_SUCCESS;
        }
        index_close(idx);
    }
    config_free(&cfg);
    flags_free(&f);
    return rc == EXIT_SUCCESS ? rc : fail(&err);
}

static int cmd_check(int argc, char **argv) {
    Flags f;
    Config cfg;
    const char *name;
    Err err;
    int rc = store_arg("check", 0, argc, argv, &f, &cfg, &name, &err);
    if (rc >= 0) {
        flags_free(&f);
        return rc;
    }
    Ctx ctx;
    ctx_init(&ctx);
    char *state = config_state_dir(&cfg);
    char *tmp = path_join(state, "tmp");
    rc = EXIT_FAILURE;
    Store *s = store_open(&ctx, config_store(&cfg, name), cfg.sync.part_size, tmp, &err);
    if (s) {
        Err inner;
        if (store_check(&ctx, s, &inner)) {
            printf("%s: ok, wrote, read back and deleted a probe object\n", name);
            rc = EXIT_SUCCESS;
        } else {
            err_set(&err, "%s: %s", name, inner.msg);
        }
        store_close(s);
    }
    free(tmp);
    free(state);
    ctx_destroy(&ctx);
    config_free(&cfg);
    flags_free(&f);
    return rc == EXIT_SUCCESS ? rc : fail(&err);
}

static int cmd_promote(int argc, char **argv) {
    Flags f;
    Config cfg;
    const char *name;
    Err err;
    int rc = store_arg("promote", FLAG_FORCE, argc, argv, &f, &cfg, &name, &err);
    if (rc >= 0) {
        flags_free(&f);
        return rc;
    }
    rc = EXIT_FAILURE;
    if (config_store(&cfg, name)->role == ROLE_PRIMARY) {
        printf("%s is already the primary\n", name);
        rc = EXIT_SUCCESS;
        goto out;
    }
    char *ip = index_path(&cfg);
    Index *idx = index_open(ip, &err);
    free(ip);
    if (!idx) goto out;
    Stats stats;
    bool ok = index_stats(idx, name, &stats, &err);
    index_close(idx);
    if (!ok) goto out;
    if (stats_unverified(&stats) > 0 && !f.force) {
        err_set(&err, "%s lacks a verified copy of %lld of %lld files; wait for backfill (see `dbox status`) or pass --force", name,
                (long long)stats_unverified(&stats), (long long)stats.files);
        goto out;
    }
    if (!config_promote(cfg.path, name, &err)) goto out;
    printf("%s is now the primary, %s is a mirror (%s)\n", name, config_primary(&cfg), cfg.path);
    char *pid = pid_path(&cfg);
    bool reloaded;
    Err inner;
    ok = daemon_reload(pid, &reloaded, &inner);
    free(pid);
    if (!ok) {
        err_set(&err, "signal daemon: %s", inner.msg);
        goto out;
    }
    if (reloaded) printf("the running daemon is reloading its config\n");
    rc = EXIT_SUCCESS;
out:
    config_free(&cfg);
    flags_free(&f);
    return rc == EXIT_SUCCESS ? rc : fail(&err);
}

static int cmd_service(int argc, char **argv) {
    if (argc != 1 || (strcmp(argv[0], "enable") != 0 && strcmp(argv[0], "disable") != 0)) return usage_fail("usage: dbox service enable|disable");
    Err err;
    char *unit = NULL;
    int rc = EXIT_FAILURE;
    if (strcmp(argv[0], "disable") == 0) {
        if (service_disable(&unit, &err)) {
            printf("stopped and removed %s\n", unit);
            rc = EXIT_SUCCESS;
        }
    } else {
        char *exe = service_executable_path(&err);
        if (exe && service_enable(exe, &unit, &err)) {
            printf("dbox run starts now and at every login (%s)\n", unit);
            rc = EXIT_SUCCESS;
        }
        free(exe);
    }
    free(unit);
    return rc == EXIT_SUCCESS ? rc : fail(&err);
}

/* run_editor runs $VISUAL or $EDITOR through the shell, so values with arguments such as "code --wait" work. */
static bool run_editor(const char *path, Err *err) {
    const char *editor = getenv("VISUAL");
    if (!editor || !*editor) editor = getenv("EDITOR");
    if (!editor || !*editor) editor = "vi";
    char *script = xmalloc(strlen(editor) + 8);
    sprintf(script, "%s \"$1\"", editor);
    char *argv[] = {"sh", "-c", script, "sh", (char *)path, NULL};
    pid_t pid;
    int rc = posix_spawnp(&pid, "sh", NULL, NULL, argv, environ);
    int status = 0;
    bool ok = rc == 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) err_set(err, "editor \"%s\": exit status %d", editor, WIFEXITED(status) ? WEXITSTATUS(status) : rc);
    free(script);
    return ok;
}

/* edit_config opens the config in the user's editor, creating it first if needed, then validates it and asks a running daemon to reload it. */
static bool edit_config(const char *path, Err *err) {
    bool created;
    if (!config_write_starter(path, &created, err) || !run_editor(path, err)) return false;
    Config cfg;
    Err inner;
    if (!config_load(path, &cfg, &inner)) {
        err_set(err, "%s\nrun `dbox config edit` again to fix it", inner.msg);
        return false;
    }
    char *pid = pid_path(&cfg);
    bool reloaded;
    bool ok = daemon_reload(pid, &reloaded, err);
    free(pid);
    config_free(&cfg);
    if (ok) printf(reloaded ? "config is valid; the daemon is reloading it\n" : "config is valid\n");
    return ok;
}

static int cmd_config(int argc, char **argv) {
    Flags f;
    int rc = parse_flags(&f, FLAG_INIT, argc, argv);
    if (rc >= 0) {
        flags_free(&f);
        return rc;
    }
    Err err;
    rc = EXIT_FAILURE;
    if (f.positional.len == 1 && strcmp(f.positional.items[0], "edit") == 0) {
        if (edit_config(f.config, &err)) rc = EXIT_SUCCESS;
    } else if (f.positional.len > 0) {
        rc = usage_fail("unknown config action \"%s\" (want edit)", f.positional.items[0]);
        flags_free(&f);
        return rc;
    } else if (f.init) {
        bool created;
        if (config_write_starter(f.config, &created, &err)) {
            printf(created ? "wrote %s\n" : "%s already exists\n", f.config);
            rc = EXIT_SUCCESS;
        }
    } else {
        Config cfg;
        if (config_load(f.config, &cfg, &err)) {
            StrBuf out = {0};
            config_render(&cfg, &out);
            printf("# %s\n%s", cfg.path, sb_cstr(&out));
            sb_free(&out);
            config_free(&cfg);
            rc = EXIT_SUCCESS;
        }
    }
    flags_free(&f);
    return rc == EXIT_SUCCESS ? rc : fail(&err);
}

static int cmd_reload(int argc, char **argv) {
    Flags f;
    int rc = parse_flags(&f, 0, argc, argv);
    if (rc >= 0) {
        flags_free(&f);
        return rc;
    }
    Err err;
    Config cfg;
    bool loaded = config_load(f.config, &cfg, &err);
    flags_free(&f);
    if (!loaded) return fail(&err);
    char *pid = pid_path(&cfg);
    bool reloaded;
    Err inner;
    rc = EXIT_FAILURE;
    if (!daemon_reload(pid, &reloaded, &inner)) err_set(&err, "signal daemon: %s", inner.msg);
    else if (!reloaded) err_set(&err, "no daemon is running (no live pid in %s)", pid);
    else {
        printf("config is valid; the daemon is reloading it\n");
        rc = EXIT_SUCCESS;
    }
    free(pid);
    config_free(&cfg);
    return rc == EXIT_SUCCESS ? rc : fail(&err);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(stdout);
        return EXIT_SUCCESS;
    }
    const char *cmd = argv[1];
    int n = argc - 2;
    char **rest = argv + 2;
    if (strcmp(cmd, "run") == 0) return cmd_run(n, rest);
    if (strcmp(cmd, "status") == 0) return cmd_status(n, rest);
    if (strcmp(cmd, "retry") == 0) return cmd_retry(n, rest);
    if (strcmp(cmd, "check") == 0) return cmd_check(n, rest);
    if (strcmp(cmd, "promote") == 0) return cmd_promote(n, rest);
    if (strcmp(cmd, "service") == 0) return cmd_service(n, rest);
    if (strcmp(cmd, "config") == 0) return cmd_config(n, rest);
    if (strcmp(cmd, "reload") == 0) return cmd_reload(n, rest);
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("dbox %s\n", DBOX_VERSION);
        return EXIT_SUCCESS;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        print_usage(stdout);
        return EXIT_SUCCESS;
    }
    return usage_fail("unknown command \"%s\"", cmd);
}
