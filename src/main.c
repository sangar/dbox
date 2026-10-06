/* dbox keeps a local folder in sync with S3-compatible stores. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "ctx.h"
#include "daemon.h"
#include "engine.h"
#include "http.h"
#include "index.h"
#include "log.h"
#include "platform/platform.h"
#include "service.h"
#include "store.h"

#ifndef DBOX_VERSION
#define DBOX_VERSION "dev"
#endif

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

/* Signals is what the signal thread shares with the run it may cancel. */
typedef struct {
    Mutex mu;
    Ctx *current;
    bool has_pending;
    Signal pending;
} Signals;

/* App is the process-wide state, created in main and passed down. */
typedef struct {
    Logger log;
    Signals signals;
} App;

static void print_usage(FILE *out) {
    char *path = config_default_path();
    fprintf(out, usage_text, path);
    xfree(path);
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
    xfree(f->config);
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
            xfree(f->config);
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
    xfree(state);
    return p;
}

static char *index_path(const Config *cfg) {
    char *state = config_state_dir(cfg);
    char *p = path_join(state, "index.db");
    xfree(state);
    return p;
}

/* ---- signals ---- */

/* signal_main waits for the signals the main thread blocked and cancels whatever run is current. */
static void *signal_main(void *arg) {
    Signals *s = arg;
    for (;;) {
        Signal sig = signals_wait();
        mutex_lock(&s->mu);
        s->pending = sig;
        s->has_pending = true;
        if (s->current) ctx_cancel(s->current);
        mutex_unlock(&s->mu);
    }
    return NULL;
}

static void watch_signals(Signals *s) {
    mutex_init(&s->mu);
    signals_block();
    Thread t;
    thread_start(&t, signal_main, s);
    thread_detach(&t);
}

/* begin_run makes ctx the one signals cancel; it reports false when a stop signal already arrived. */
static bool begin_run(Signals *s, Ctx *ctx) {
    mutex_lock(&s->mu);
    bool stopped = s->has_pending && s->pending == SIGNAL_STOP;
    if (!stopped) s->current = ctx;
    mutex_unlock(&s->mu);
    return !stopped;
}

/* end_run forgets the current run and reports whether a reload was asked for, consuming that request. */
static bool end_run(Signals *s) {
    mutex_lock(&s->mu);
    s->current = NULL;
    bool reload = s->has_pending && s->pending == SIGNAL_RELOAD;
    if (reload) s->has_pending = false;
    mutex_unlock(&s->mu);
    return reload;
}

/* ---- run ---- */

typedef struct {
    const Config *cfg;
    Index *idx;
    Engine *engine;
} Running;

[[nodiscard]] static Error metrics(void *arg, StrBuf *out, Err *err) {
    Running *r = arg;
    sb_printf(out, "dbox_upload_backlog %zu\n", engine_backlog(r->engine));
    Arena a;
    arena_init(&a, 4096);
    size_t n;
    const char **names = config_store_names(r->cfg, &a, &n);
    Error e = ERR_OK;
    for (size_t i = 0; e == ERR_OK && i < n; i++) {
        Stats s;
        e = index_stats(r->idx, names[i], &s, err);
        if (e != ERR_OK) break;
        const char *role = role_name(config_store(r->cfg, names[i])->role);
        sb_printf(out, "dbox_files{store=\"%s\",role=\"%s\"} %lld\n", names[i], role, (long long)s.files);
        sb_printf(out, "dbox_replicas{store=\"%s\",role=\"%s\",state=\"verified\"} %lld\n", names[i], role, (long long)s.verified);
        sb_printf(out, "dbox_replicas{store=\"%s\",role=\"%s\",state=\"pending\"} %lld\n", names[i], role, (long long)s.pending);
        sb_printf(out, "dbox_replicas{store=\"%s\",role=\"%s\",state=\"failed\"} %lld\n", names[i], role, (long long)s.failed);
        sb_printf(out, "dbox_verified_bytes{store=\"%s\",role=\"%s\"} %lld\n", names[i], role, (long long)s.verified_bytes);
    }
    arena_free(&a);
    return e;
}

static long backlog(void *arg) { return (long)engine_backlog(((Running *)arg)->engine); }

typedef Error (*EngineFn)(Engine *e, Index *idx, Ctx *ctx, const Config *cfg, Logger *log, Err *err);

[[nodiscard]] static Error with_engine(App *app, Ctx *ctx, const Config *cfg, bool dry_run, EngineFn fn, Err *err) {
    if (!config_primary(cfg)) return err_set(err, ERR_INVALID_ARGUMENT, "%s: no stores configured", cfg->path);
    char *ip = index_path(cfg);
    Index *idx;
    Error result = index_open(ip, &idx, err);
    xfree(ip);
    if (result != ERR_OK) return result;
    StoreSet stores;
    result = store_open_all(ctx, cfg, &stores, err);
    if (result == ERR_OK) {
        logger_init(&app->log, cfg->daemon.log_level, cfg->daemon.log_format, stderr);
        Engine *e = engine_new(cfg, idx, &stores, (EngineOptions){.dry_run = dry_run, .log = &app->log});
        result = fn(e, idx, ctx, cfg, &app->log, err);
        engine_free(e);
        storeset_close(&stores);
    }
    index_close(idx);
    return result;
}

[[nodiscard]] static Error run_once(Engine *e, Index *idx, Ctx *ctx, const Config *cfg, Logger *log, Err *err) { return engine_once(e, ctx, err); }

typedef struct {
    Ctx *ctx;
    const Config *cfg;
    Logger *log;
    Running running;
} ServeArg;

static void *serve_http(void *arg) {
    ServeArg *s = arg;
    Err err;
    if (daemon_serve(s->ctx, s->cfg->daemon.listen, metrics, backlog, &s->running, &err) != ERR_OK)
        log_error(s->log, "health listener", log_str("addr", s->cfg->daemon.listen), log_err(&err), log_end());
    return NULL;
}

[[nodiscard]] static Error run_daemon(Engine *e, Index *idx, Ctx *ctx, const Config *cfg, Logger *log, Err *err) {
    ServeArg arg = {.ctx = ctx, .cfg = cfg, .log = log, .running = {cfg, idx, e}};
    Thread http;
    thread_start(&http, serve_http, &arg);
    Error result = engine_run(e, ctx, err);
    ctx_cancel(ctx);
    thread_join(&http);
    return result;
}

/* serve runs the daemon for one config until ctx is done. */
[[nodiscard]] static Error serve(App *app, Ctx *ctx, const Config *cfg, Err *err) {
    logger_init(&app->log, cfg->daemon.log_level, cfg->daemon.log_format, stderr);
    char *state = config_state_dir(cfg);
    Error result = mkdir_p(state, 0755, err);
    xfree(state);
    if (result != ERR_OK) return result;
    char *pid = pid_path(cfg);
    result = daemon_write_pid(pid, err);
    if (result == ERR_OK) {
        if (!config_primary(cfg)) {
            log_warn(&app->log, "no stores configured; add one to the config and send SIGHUP", log_str("config", cfg->path), log_end());
            ctx_lock(ctx);
            while (ctx_wait(ctx, 0)) {
            }
            ctx_unlock(ctx);
        } else {
            result = with_engine(app, ctx, cfg, false, run_daemon, err);
        }
        daemon_remove_pid(pid);
    }
    xfree(pid);
    return result;
}

static int cmd_run(App *app, int argc, char **argv) {
    Flags f;
    int rc = parse_flags(&f, FLAG_ONCE | FLAG_DRY_RUN, argc, argv);
    if (rc >= 0) {
        flags_free(&f);
        return rc;
    }
    watch_signals(&app->signals);
    Err err;
    Config cfg;
    if (f.once || f.dry_run) {
        rc = EXIT_FAILURE;
        if (config_load(f.config, &cfg, &err) == ERR_OK) {
            char *pid = pid_path(&cfg);
            int other;
            if (daemon_running(pid, &other) && !f.dry_run) {
                (void)err_set(&err, ERR_PLATFORM, "the daemon is already syncing this folder (pid %d); stop it before running --once", other);
            } else {
                Ctx ctx;
                ctx_init(&ctx);
                begin_run(&app->signals, &ctx);
                if (with_engine(app, &ctx, &cfg, f.dry_run, run_once, &err) == ERR_OK) rc = EXIT_SUCCESS;
                (void)end_run(&app->signals);
                ctx_destroy(&ctx);
            }
            xfree(pid);
            config_free(&cfg);
        }
        flags_free(&f);
        return rc == EXIT_SUCCESS ? rc : fail(&err);
    }

    logger_init(&app->log, "info", "text", stderr);
    if (process_raise_file_limit(&err) != ERR_OK) log_warn(&app->log, "raise open file limit", log_err(&err), log_end());
    for (;;) {
        if (config_load(f.config, &cfg, &err) != ERR_OK) {
            flags_free(&f);
            return fail(&err);
        }
        Ctx ctx;
        ctx_init(&ctx);
        Error served = ERR_OK;
        if (begin_run(&app->signals, &ctx)) served = serve(app, &ctx, &cfg, &err);
        bool reload = end_run(&app->signals);
        ctx_destroy(&ctx);
        if (reload) {
            log_info(&app->log, "reloading config", log_str("path", cfg.path), log_end());
            if (served != ERR_OK) log_warn(&app->log, "stopped for reload", log_err(&err), log_end());
            config_free(&cfg);
            continue;
        }
        config_free(&cfg);
        flags_free(&f);
        return served == ERR_OK ? EXIT_SUCCESS : fail(&err);
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
    Error loaded = config_load(f.config, &cfg, &err);
    flags_free(&f);
    if (loaded != ERR_OK) return fail(&err);
    printf("config:  %s\nroot:    %s\n", cfg.path, cfg.sync.root);
    char *pid = pid_path(&cfg);
    int running;
    if (daemon_running(pid, &running)) printf("daemon:  running, pid %d\n", running);
    else printf("daemon:  not running\n");
    xfree(pid);
    char *unit;
    if (service_installed(&unit)) printf("service: enabled, %s\n", unit);
    else printf("service: not enabled (run `dbox service enable` to start dbox run at login)\n");
    xfree(unit);
    if (cfg.store_count == 0) {
        printf("stores:  none configured\n");
        config_free(&cfg);
        return EXIT_SUCCESS;
    }

    char *ip = index_path(&cfg);
    Index *idx;
    Error opened = index_open(ip, &idx, &err);
    xfree(ip);
    if (opened != ERR_OK) {
        config_free(&cfg);
        return fail(&err);
    }
    rc = EXIT_FAILURE;
    Arena a;
    arena_init(&a, 16 * 1024);
    printf("\n%-12s %-9s %8s %8s %8s %8s %10s\n", "STORE", "ROLE", "FILES", "VERIFIED", "PENDING", "FAILED", "COPIED");
    UploadFailure *uploads;
    size_t upload_count;
    if (index_failed_uploads(idx, &a, &uploads, &upload_count, &err) != ERR_OK) goto out;
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
        if (index_stats(idx, names[i], &s, &err) != ERR_OK || index_failed(idx, &a, names[i], 10, &copies, &copy_count, &err) != ERR_OK) {
            strlist_free(&failed);
            goto out;
        }
        for (size_t j = 0; j < copy_count; j++) {
            char *line = arena_printf(&a, "%s  %s  %s", copies[j].store, copies[j].path, copies[j].last_error);
            strlist_push(&failed, line);
        }
        const char *role = role_name(config_store(&cfg, names[i])->role);
        if (strcmp(names[i], primary) == 0) {
            for (size_t j = 0; j < min_size(upload_count, 10); j++) strlist_push(&failed, arena_printf(&a, "%s  %s  %s", names[i], uploads[j].path, uploads[j].last_error));
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
    if (config_load(f->config, cfg, err) != ERR_OK) return fail(err);
    *name = f->positional.items[0];
    if (!config_store(cfg, *name)) {
        (void)err_set(err, ERR_NOT_FOUND, "no store named \"%s\" in %s", *name, cfg->path);
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
    Index *idx;
    Error opened = index_open(ip, &idx, &err);
    xfree(ip);
    rc = EXIT_FAILURE;
    if (opened == ERR_OK) {
        int64_t n;
        if (strcmp(name, config_primary(&cfg)) == 0) {
            if (index_retry_failed_uploads(idx, &n, &err) == ERR_OK) {
                char buf[48];
                printf("%lld failed uploads to %s are due again; the daemon retries them within %s\n", (long long)n, name, format_duration(cfg.sync.pull_interval_ns, buf));
                rc = EXIT_SUCCESS;
            }
        } else if (index_retry_failed(idx, name, &n, &err) == ERR_OK) {
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
    Store *s;
    if (store_open(&ctx, config_store(&cfg, name), cfg.sync.part_size, tmp, &s, &err) == ERR_OK) {
        Err inner;
        Error checked = store_check(&ctx, s, &inner);
        if (checked == ERR_OK) {
            printf("%s: ok, wrote, read back and deleted a probe object\n", name);
            rc = EXIT_SUCCESS;
        } else {
            (void)err_set(&err, checked, "%s: %s", name, inner.msg);
        }
        store_close(s);
    }
    xfree(tmp);
    xfree(state);
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
    Index *idx;
    if (index_open(ip, &idx, &err) != ERR_OK) {
        xfree(ip);
        goto out;
    }
    xfree(ip);
    Stats stats;
    Error e = index_stats(idx, name, &stats, &err);
    index_close(idx);
    if (e != ERR_OK) goto out;
    if (stats_unverified(&stats) > 0 && !f.force) {
        (void)err_set(&err, ERR_INVALID_ARGUMENT, "%s lacks a verified copy of %lld of %lld files; wait for backfill (see `dbox status`) or pass --force", name,
                      (long long)stats_unverified(&stats), (long long)stats.files);
        goto out;
    }
    if (config_promote(cfg.path, name, &err) != ERR_OK) goto out;
    printf("%s is now the primary, %s is a mirror (%s)\n", name, config_primary(&cfg), cfg.path);
    char *pid = pid_path(&cfg);
    bool reloaded;
    Err inner;
    e = daemon_reload(pid, &reloaded, &inner);
    xfree(pid);
    if (e != ERR_OK) {
        (void)err_set(&err, e, "signal daemon: %s", inner.msg);
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
        if (service_disable(&unit, &err) == ERR_OK) {
            printf("stopped and removed %s\n", unit);
            rc = EXIT_SUCCESS;
        }
    } else {
        char *exe;
        if (service_executable_path(&exe, &err) == ERR_OK && service_enable(exe, &unit, &err) == ERR_OK) {
            printf("dbox run starts now and at every login (%s)\n", unit);
            rc = EXIT_SUCCESS;
        }
        xfree(exe);
    }
    xfree(unit);
    return rc == EXIT_SUCCESS ? rc : fail(&err);
}

/* run_editor runs $VISUAL or $EDITOR through the shell, so values with arguments such as "code --wait" work. */
[[nodiscard]] static Error run_editor(const char *path, Err *err) {
    const char *editor = getenv("VISUAL");
    if (!editor || !*editor) editor = getenv("EDITOR");
    if (!editor || !*editor) editor = "vi";
    char *script = xmalloc(strlen(editor) + 8);
    sprintf(script, "%s \"$1\"", editor);
    char *argv[] = {"sh", "-c", script, "sh", (char *)path, NULL};
    Err inner;
    Error e = process_run_interactive(argv, &inner);
    if (e != ERR_OK) e = err_set(err, e, "editor \"%s\": %s", editor, inner.msg);
    xfree(script);
    return e;
}

/* edit_config opens the config in the user's editor, creating it first if needed, then validates it and asks a running daemon to reload it. */
[[nodiscard]] static Error edit_config(const char *path, Err *err) {
    bool created;
    Error e = config_write_starter(path, &created, err);
    if (e == ERR_OK) e = run_editor(path, err);
    if (e != ERR_OK) return e;
    Config cfg;
    Err inner;
    e = config_load(path, &cfg, &inner);
    if (e != ERR_OK) return err_set(err, e, "%s\nrun `dbox config edit` again to fix it", inner.msg);
    char *pid = pid_path(&cfg);
    bool reloaded;
    e = daemon_reload(pid, &reloaded, err);
    xfree(pid);
    config_free(&cfg);
    if (e == ERR_OK) printf(reloaded ? "config is valid; the daemon is reloading it\n" : "config is valid\n");
    return e;
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
        if (edit_config(f.config, &err) == ERR_OK) rc = EXIT_SUCCESS;
    } else if (f.positional.len > 0) {
        rc = usage_fail("unknown config action \"%s\" (want edit)", f.positional.items[0]);
        flags_free(&f);
        return rc;
    } else if (f.init) {
        bool created;
        if (config_write_starter(f.config, &created, &err) == ERR_OK) {
            printf(created ? "wrote %s\n" : "%s already exists\n", f.config);
            rc = EXIT_SUCCESS;
        }
    } else {
        Config cfg;
        if (config_load(f.config, &cfg, &err) == ERR_OK) {
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
    Error loaded = config_load(f.config, &cfg, &err);
    flags_free(&f);
    if (loaded != ERR_OK) return fail(&err);
    char *pid = pid_path(&cfg);
    bool reloaded;
    Err inner;
    rc = EXIT_FAILURE;
    Error signalled = daemon_reload(pid, &reloaded, &inner);
    if (signalled != ERR_OK) (void)err_set(&err, signalled, "signal daemon: %s", inner.msg);
    else if (!reloaded) (void)err_set(&err, ERR_NOT_FOUND, "no daemon is running (no live pid in %s)", pid);
    else {
        printf("config is valid; the daemon is reloading it\n");
        rc = EXIT_SUCCESS;
    }
    xfree(pid);
    config_free(&cfg);
    return rc == EXIT_SUCCESS ? rc : fail(&err);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(stdout);
        return EXIT_SUCCESS;
    }
    http_global_init();
    App app = {0};
    const char *cmd = argv[1];
    int n = argc - 2;
    char **rest = argv + 2;
    if (strcmp(cmd, "run") == 0) return cmd_run(&app, n, rest);
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
