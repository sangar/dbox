/* dbox keeps a local folder in sync with S3-compatible stores. */
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "daemon.h"
#include "engine.h"
#include "http.h"
#include "index.h"
#include "mc/platform/service.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"
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
    "Every command takes --config FILE (default %.*s, env DBOX_CONFIG).\n";

/* Exit statuses: 1 for failures, 2 for mistakes on the command line. */
enum { EXIT_USAGE = 2, CONFIG_BLOCK = 64 * 1024, FAILED_SHOWN = 10 };

#define SERVICE_NAME "dbox"

/* Signals is what the signal thread shares with the run it may cancel. */
typedef struct {
    Mutex mutex;
    Cancel *current;
    bool has_pending;
    Signal pending;
} Signals;

/* App is the process-wide state, created in main and passed down. */
typedef struct {
    Arena *arena;
    Logger *log;
    Signals signals;
} App;

static void print_usage(Arena *arena, FILE *out) {
    String path = config_default_path(arena);
    fprintf(out, usage_text, (int)path.len, path.data);
}

static int fail(const Err *err) {
    fprintf(stderr, "dbox: %s\n", err->msg);
    return EXIT_FAILURE;
}

[[gnu::format(printf, 1, 2)]] static int usage_fail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("dbox: ", stderr);
    vfprintf(stderr, format, args);
    fputs(" (see dbox --help)\n", stderr);
    va_end(args);
    return EXIT_USAGE;
}

/* use_logger replaces the process logger with one at the config's level and format. */
static void use_logger(App *app, String level_name, String format_name) {
    LogLevel level = LOG_INFO;
    LogFormat format = LOG_TEXT;
    (void)log_parse_level(level_name, &level);
    (void)log_parse_format(format_name, &format);
    if (app->log) logger_destroy(app->log);
    app->log = logger_create(level, format, stderr);
}

/* ---- flags ---- */

enum { FLAG_ONCE = 1, FLAG_DRY_RUN = 2, FLAG_FORCE = 4, FLAG_INIT = 8 };

typedef struct {
    String config;
    bool once, dry_run, force, init;
    StringList positional;
} Flags;

/* parse_flags reads the flags a command allows, in any position, and keeps the rest as positional arguments. It returns an exit status, or -1 to continue. */
static int parse_flags(Arena *arena, Flags *f, int allowed, int argc, char **argv) {
    *f = (Flags){.config = config_default_path(arena)};
    for (int i = 0; i < argc; i++) {
        String arg = S(argv[i]);
        if (!str_starts_with(arg, S("-")) || str_equal(arg, S("-"))) {
            strlist_push(arena, &f->positional, arg);
            continue;
        }
        String name = str_trim_prefix(str_trim_prefix(arg, S("-")), S("-"));
        if (str_equal(name, S("h")) || str_equal(name, S("help"))) {
            print_usage(arena, stdout);
            return EXIT_SUCCESS;
        }
        String key, value;
        bool has_value = str_cut(name, '=', &key, &value);
        if (str_equal(key, S("config"))) {
            if (!has_value) {
                if (i + 1 >= argc) return usage_fail("flag needs an argument: %s", argv[i]);
                value = S(argv[++i]);
            }
            f->config = value;
            continue;
        }
        if ((allowed & FLAG_ONCE) && str_equal(name, S("once"))) f->once = true;
        else if ((allowed & FLAG_DRY_RUN) && str_equal(name, S("dry-run"))) f->dry_run = true;
        else if ((allowed & FLAG_FORCE) && str_equal(name, S("force"))) f->force = true;
        else if ((allowed & FLAG_INIT) && str_equal(name, S("init"))) f->init = true;
        else return usage_fail("flag provided but not defined: %s", argv[i]);
    }
    return -1;
}

static String state_file(Arena *arena, const Config *config, const char *name) { return path_join(arena, config_state_dir(arena, config), S(name)); }
static String pid_path(Arena *arena, const Config *config) { return state_file(arena, config, "daemon.pid"); }
static String index_path(Arena *arena, const Config *config) { return state_file(arena, config, "index.db"); }

/* ---- signals ---- */

/* signal_main waits for the signals the main thread blocked and cancels whatever run is current. */
static void *signal_main(void *argument) {
    Signals *s = argument;
    for (;;) {
        Signal signal = signals_wait();
        mutex_lock(&s->mutex);
        s->pending = signal;
        s->has_pending = true;
        if (s->current) cancel_request(s->current);
        mutex_unlock(&s->mutex);
    }
    return nullptr;
}

[[nodiscard]] static Error watch_signals(Signals *s, Err *err) {
    mutex_init(&s->mutex);
    signals_block();
    Thread thread;
    Error e = thread_start(&thread, signal_main, s, err);
    if (e == ERR_OK) thread_detach(&thread);
    return e;
}

/* begin_run makes cancel the one signals cancel; it reports false when a stop signal already arrived. */
static bool begin_run(Signals *s, Cancel *cancel) {
    mutex_lock(&s->mutex);
    bool stopped = s->has_pending && s->pending == SIGNAL_STOP;
    if (!stopped) s->current = cancel;
    mutex_unlock(&s->mutex);
    return !stopped;
}

/* end_run forgets the current run and reports whether a reload was asked for, consuming that request. */
static bool end_run(Signals *s) {
    mutex_lock(&s->mutex);
    s->current = nullptr;
    bool reload = s->has_pending && s->pending == SIGNAL_RELOAD;
    if (reload) s->has_pending = false;
    mutex_unlock(&s->mutex);
    return reload;
}

/* ---- run ---- */

typedef struct {
    const Config *config;
    Index *index;
    Engine *engine;
} Running;

[[nodiscard]] static Error metrics(void *context, Arena *arena, StringBuilder *out, Err *err) {
    Running *r = context;
    str_builder_append_format(out, "dbox_upload_backlog %zu\n", engine_backlog(r->engine));
    StringList names = config_store_names(arena, r->config);
    for (size_t i = 0; i < names.count; i++) {
        Stats s;
        Error e = index_stats(r->index, names.items[i], &s, err);
        if (e != ERR_OK) return e;
        int n = (int)names.items[i].len;
        const char *name = names.items[i].data;
        const char *role = role_name(config_store(r->config, names.items[i])->role);
        str_builder_append_format(out, "dbox_files{store=\"%.*s\",role=\"%s\"} %lld\n", n, name, role, (long long)s.files);
        str_builder_append_format(out, "dbox_replicas{store=\"%.*s\",role=\"%s\",state=\"verified\"} %lld\n", n, name, role, (long long)s.verified);
        str_builder_append_format(out, "dbox_replicas{store=\"%.*s\",role=\"%s\",state=\"pending\"} %lld\n", n, name, role, (long long)s.pending);
        str_builder_append_format(out, "dbox_replicas{store=\"%.*s\",role=\"%s\",state=\"failed\"} %lld\n", n, name, role, (long long)s.failed);
        str_builder_append_format(out, "dbox_verified_bytes{store=\"%.*s\",role=\"%s\"} %lld\n", n, name, role, (long long)s.verified_bytes);
    }
    return ERR_OK;
}

static int64_t backlog(void *context) { return (int64_t)engine_backlog(((Running *)context)->engine); }

typedef Error (*EngineFn)(Engine *engine, Index *index, Cancel *cancel, const Config *config, Logger *log, Err *err);

[[nodiscard]] static Error with_engine(App *app, Arena *arena, Cancel *cancel, const Config *config, bool dry_run, EngineFn run, Err *err) {
    if (config_primary(config).len == 0) return err_set(err, ERR_INVALID_ARGUMENT, "%.*s: no stores configured", (int)config->path.len, config->path.data);
    Index *index;
    Error result = index_open(arena, index_path(arena, config), &index, err);
    if (result != ERR_OK) return result;
    StoreSet stores;
    Engine *engine;
    result = store_open_all(arena, config, &stores, err);
    if (result == ERR_OK) {
        use_logger(app, config->daemon.log_level, config->daemon.log_format);
        result = engine_create(arena, config, index, &stores, (EngineOptions){.dry_run = dry_run, .log = app->log}, &engine, err);
    }
    if (result == ERR_OK) {
        result = run(engine, index, cancel, config, app->log, err);
        engine_destroy(engine);
    }
    index_close(index);
    return result;
}

[[nodiscard]] static Error run_once(Engine *engine, Index *index, Cancel *cancel, const Config *config, Logger *log, Err *err) { return engine_once(engine, cancel, err); }

typedef struct {
    Cancel *cancel;
    const Config *config;
    Logger *log;
    Running running;
} ServeArgument;

static void *serve_http(void *argument) {
    ServeArgument *s = argument;
    Err err;
    if (daemon_serve(s->cancel, s->config->daemon.listen, metrics, backlog, &s->running, &err) != ERR_OK) {
        LogAttr attrs[] = {log_string("addr", s->config->daemon.listen), log_err(&err)};
        log_error(s->log, "health listener", countof(attrs), attrs);
    }
    return nullptr;
}

[[nodiscard]] static Error run_daemon(Engine *engine, Index *index, Cancel *cancel, const Config *config, Logger *log, Err *err) {
    ServeArgument argument = {.cancel = cancel, .config = config, .log = log, .running = {config, index, engine}};
    Thread http;
    Error result = thread_start(&http, serve_http, &argument, err);
    if (result != ERR_OK) return result;
    result = engine_run(engine, cancel, err);
    cancel_request(cancel);
    thread_join(&http);
    return result;
}

/* serve runs the daemon for one config until cancel is requested. */
[[nodiscard]] static Error serve(App *app, Arena *arena, Cancel *cancel, const Config *config, Err *err) {
    use_logger(app, config->daemon.log_level, config->daemon.log_format);
    Error result = dir_create_all(config_state_dir(arena, config), 0755, err);
    if (result != ERR_OK) return result;
    String pid = pid_path(arena, config);
    result = daemon_write_pid(arena, pid, err);
    if (result != ERR_OK) return result;
    if (config_primary(config).len > 0) {
        result = with_engine(app, arena, cancel, config, false, run_daemon, err);
    } else {
        LogAttr attrs[] = {log_string("config", config->path)};
        log_warn(app->log, "no stores configured; add one to the config and send SIGHUP", countof(attrs), attrs);
        cancel_lock(cancel);
        while (cancel_wait(cancel, 0)) {
        }
        cancel_unlock(cancel);
    }
    daemon_remove_pid(pid);
    return result;
}

/* run_once_or_dry runs `dbox run --once` or `--dry-run`. */
static int run_once_or_dry(App *app, const Flags *f) {
    Err err;
    Config config;
    Arena *arena = arena_create(CONFIG_BLOCK);
    int rc = EXIT_FAILURE;
    if (config_load(arena, f->config, &config, &err) == ERR_OK) {
        int other;
        if (!f->dry_run && daemon_running(arena, pid_path(arena, &config), &other)) {
            (void)err_set(&err, ERR_PLATFORM, "the daemon is already syncing this folder (pid %d); stop it before running --once", other);
        } else {
            Cancel cancel;
            cancel_init(&cancel);
            begin_run(&app->signals, &cancel);
            if (with_engine(app, arena, &cancel, &config, f->dry_run, run_once, &err) == ERR_OK) rc = EXIT_SUCCESS;
            (void)end_run(&app->signals);
            cancel_destroy(&cancel);
        }
    }
    arena_destroy(arena);
    return rc == EXIT_SUCCESS ? rc : fail(&err);
}

static int cmd_run(App *app, int argc, char **argv) {
    Flags f;
    int rc = parse_flags(app->arena, &f, FLAG_ONCE | FLAG_DRY_RUN, argc, argv);
    if (rc >= 0) return rc;
    Err err;
    if (watch_signals(&app->signals, &err) != ERR_OK) return fail(&err);
    if (f.once || f.dry_run) return run_once_or_dry(app, &f);

    use_logger(app, S("info"), S("text"));
    if (process_raise_file_limit(&err) != ERR_OK) {
        LogAttr attrs[] = {log_err(&err)};
        log_warn(app->log, "raise open file limit", countof(attrs), attrs);
    }
    for (;;) {
        Arena *arena = arena_create(CONFIG_BLOCK);
        Config config;
        if (config_load(arena, f.config, &config, &err) != ERR_OK) {
            arena_destroy(arena);
            return fail(&err);
        }
        Cancel cancel;
        cancel_init(&cancel);
        Error served = ERR_OK;
        if (begin_run(&app->signals, &cancel)) served = serve(app, arena, &cancel, &config, &err);
        bool reload = end_run(&app->signals);
        cancel_destroy(&cancel);
        if (reload) {
            LogAttr attrs[] = {log_string("path", config.path)};
            log_info(app->log, "reloading config", countof(attrs), attrs);
            if (served != ERR_OK) {
                LogAttr stopped[] = {log_err(&err)};
                log_warn(app->log, "stopped for reload", countof(stopped), stopped);
            }
            arena_destroy(arena);
            continue;
        }
        arena_destroy(arena);
        return served == ERR_OK ? EXIT_SUCCESS : fail(&err);
    }
}

/* ---- status ---- */

static double percent(const Stats *s) {
    if (s->bytes == 0) return s->files == s->verified ? 100 : 0;
    return 100.0 * (double)s->verified_bytes / (double)s->bytes;
}

static void print_service(Arena *arena) {
    ServiceManager manager;
    if (service_manager(arena, &manager, nullptr) == ERR_OK && service_installed(arena, &manager, S(SERVICE_NAME))) {
        String path = service_path(arena, &manager, S(SERVICE_NAME));
        printf("service: enabled, %.*s\n", (int)path.len, path.data);
    } else {
        printf("service: not enabled (run `dbox service enable` to start dbox run at login)\n");
    }
}

static String failure_line(Arena *arena, String store, String path, String cause) {
    return str_format(arena, "%.*s  %.*s  %.*s", (int)store.len, store.data, (int)path.len, path.data, (int)cause.len, cause.data);
}

/* print_stores prints the per-store table and the failed copies below it. */
[[nodiscard]] static Error print_stores(Arena *arena, const Config *config, Index *index, Err *err) {
    printf("\n%-12s %-9s %8s %8s %8s %8s %10s\n", "STORE", "ROLE", "FILES", "VERIFIED", "PENDING", "FAILED", "COPIED");
    UploadFailureList uploads;
    Error e = index_failed_uploads(index, arena, &uploads, err);
    if (e != ERR_OK) return e;
    int64_t queued;
    String backlog_text = daemon_ask(arena, config->daemon.listen, &queued) ? str_format(arena, "%lld", (long long)queued) : S("-");
    StringList failed = {0};
    StringList names = config_store_names(arena, config);
    String primary = config_primary(config);
    for (size_t i = 0; i < names.count; i++) {
        String name = names.items[i];
        Stats s;
        ReplicaList copies;
        if ((e = index_stats(index, name, &s, err)) != ERR_OK || (e = index_failed(index, arena, name, FAILED_SHOWN, &copies, err)) != ERR_OK) return e;
        for (size_t j = 0; j < copies.count; j++) strlist_push(arena, &failed, failure_line(arena, copies.items[j].store, copies.items[j].path, copies.items[j].last_error));
        const char *role = role_name(config_store(config, name)->role);
        if (str_equal(name, primary)) {
            for (size_t j = 0; j < min_size(uploads.count, FAILED_SHOWN); j++) strlist_push(arena, &failed, failure_line(arena, name, uploads.items[j].path, uploads.items[j].last_error));
            /* Every indexed file is on the primary by definition, so its row shows what is still on the way there instead. */
            printf("%-12.*s %-9s %8lld %8lld %8.*s %8zu %10s\n", (int)name.len, name.data, role, (long long)s.files, (long long)s.verified, (int)backlog_text.len, backlog_text.data,
                   uploads.count, "-");
            continue;
        }
        printf("%-12.*s %-9s %8lld %8lld %8lld %8lld %9.0f%%\n", (int)name.len, name.data, role, (long long)s.files, (long long)s.verified, (long long)s.pending, (long long)s.failed,
               percent(&s));
    }
    if (failed.count > 0) {
        printf("\nfailed copies (`dbox retry STORE` to try again):\n");
        for (size_t i = 0; i < failed.count; i++) printf("  %.*s\n", (int)failed.items[i].len, failed.items[i].data);
    }
    return ERR_OK;
}

static int cmd_status(Arena *arena, int argc, char **argv) {
    Flags f;
    int rc = parse_flags(arena, &f, 0, argc, argv);
    if (rc >= 0) return rc;
    Err err;
    Config config;
    if (config_load(arena, f.config, &config, &err) != ERR_OK) return fail(&err);
    printf("config:  %.*s\nroot:    %.*s\n", (int)config.path.len, config.path.data, (int)config.sync.root.len, config.sync.root.data);
    int running;
    if (daemon_running(arena, pid_path(arena, &config), &running)) printf("daemon:  running, pid %d\n", running);
    else printf("daemon:  not running\n");
    print_service(arena);
    if (config.store_count == 0) {
        printf("stores:  none configured\n");
        return EXIT_SUCCESS;
    }
    Index *index;
    if (index_open(arena, index_path(arena, &config), &index, &err) != ERR_OK) return fail(&err);
    Error e = print_stores(arena, &config, index, &err);
    index_close(index);
    return e == ERR_OK ? EXIT_SUCCESS : fail(&err);
}

/* store_arg parses a command that takes one store name, loads the config and checks the store exists. It returns an exit status, or -1 to continue. */
static int store_arg(Arena *arena, const char *command, int allowed, int argc, char **argv, Flags *f, Config *config, String *name, Err *err) {
    int rc = parse_flags(arena, f, allowed, argc, argv);
    if (rc >= 0) return rc;
    if (f->positional.count != 1) return usage_fail("usage: dbox %s STORE", command);
    if (config_load(arena, f->config, config, err) != ERR_OK) return fail(err);
    *name = f->positional.items[0];
    if (config_store(config, *name)) return -1;
    (void)err_set(err, ERR_NOT_FOUND, "no store named \"%.*s\" in %.*s", (int)name->len, name->data, (int)config->path.len, config->path.data);
    return fail(err);
}

static int cmd_retry(Arena *arena, int argc, char **argv) {
    Flags f;
    Config config = {0};
    String name = S("");
    Err err;
    int rc = store_arg(arena, "retry", 0, argc, argv, &f, &config, &name, &err);
    if (rc >= 0) return rc;
    Index *index;
    if (index_open(arena, index_path(arena, &config), &index, &err) != ERR_OK) return fail(&err);
    int64_t count;
    Error e;
    if (str_equal(name, config_primary(&config))) {
        e = index_retry_failed_uploads(index, &count, &err);
        String within = fmt_duration(arena, config.sync.pull_interval_ns);
        if (e == ERR_OK)
            printf("%lld failed uploads to %.*s are due again; the daemon retries them within %.*s\n", (long long)count, (int)name.len, name.data, (int)within.len, within.data);
    } else {
        e = index_retry_failed(index, name, &count, &err);
        if (e == ERR_OK) printf("%lld failed copies on %.*s are pending again\n", (long long)count, (int)name.len, name.data);
    }
    index_close(index);
    return e == ERR_OK ? EXIT_SUCCESS : fail(&err);
}

static int cmd_check(Arena *arena, int argc, char **argv) {
    Flags f;
    Config config = {0};
    String name = S("");
    Err err;
    int rc = store_arg(arena, "check", 0, argc, argv, &f, &config, &name, &err);
    if (rc >= 0) return rc;
    Store *store;
    if (store_open(arena, config_store(&config, name), config.sync.part_size, state_file(arena, &config, "tmp"), &store, &err) != ERR_OK) return fail(&err);
    Cancel cancel;
    cancel_init(&cancel);
    Err inner;
    Error checked = store_check(&cancel, arena, store, &inner);
    cancel_destroy(&cancel);
    if (checked != ERR_OK) {
        (void)err_set(&err, checked, "%.*s: %s", (int)name.len, name.data, inner.msg);
        return fail(&err);
    }
    printf("%.*s: ok, wrote, read back and deleted a probe object\n", (int)name.len, name.data);
    return EXIT_SUCCESS;
}

/* tell_daemon asks a running daemon to reload, and says so when it is one. */
[[nodiscard]] static Error tell_daemon(Arena *arena, const Config *config, const char *reloading, Err *err) {
    bool reloaded;
    Err inner;
    Error e = daemon_reload(arena, pid_path(arena, config), &reloaded, &inner);
    if (e != ERR_OK) return err_set(err, e, "signal daemon: %s", inner.msg);
    if (reloaded) printf("%s\n", reloading);
    return ERR_OK;
}

static int cmd_promote(Arena *arena, int argc, char **argv) {
    Flags f;
    Config config = {0};
    String name = S("");
    Err err;
    int rc = store_arg(arena, "promote", FLAG_FORCE, argc, argv, &f, &config, &name, &err);
    if (rc >= 0) return rc;
    if (config_store(&config, name)->role == ROLE_PRIMARY) {
        printf("%.*s is already the primary\n", (int)name.len, name.data);
        return EXIT_SUCCESS;
    }
    Index *index;
    if (index_open(arena, index_path(arena, &config), &index, &err) != ERR_OK) return fail(&err);
    Stats stats;
    Error e = index_stats(index, name, &stats, &err);
    index_close(index);
    if (e != ERR_OK) return fail(&err);
    if (stats_unverified(&stats) > 0 && !f.force) {
        (void)err_set(&err, ERR_INVALID_ARGUMENT, "%.*s lacks a verified copy of %lld of %lld files; wait for backfill (see `dbox status`) or pass --force", (int)name.len, name.data,
                      (long long)stats_unverified(&stats), (long long)stats.files);
        return fail(&err);
    }
    if (config_promote(arena, config.path, name, &err) != ERR_OK) return fail(&err);
    String old = config_primary(&config);
    printf("%.*s is now the primary, %.*s is a mirror (%.*s)\n", (int)name.len, name.data, (int)old.len, old.data, (int)config.path.len, config.path.data);
    return tell_daemon(arena, &config, "the running daemon is reloading its config", &err) == ERR_OK ? EXIT_SUCCESS : fail(&err);
}

static int cmd_service(Arena *arena, int argc, char **argv) {
    bool enable = argc == 1 && str_equal(S(argv[0]), S("enable"));
    if (!enable && !(argc == 1 && str_equal(S(argv[0]), S("disable")))) return usage_fail("usage: dbox service enable|disable");
    Err err;
    ServiceManager manager;
    if (service_manager(arena, &manager, &err) != ERR_OK) return fail(&err);
    String path = service_path(arena, &manager, S(SERVICE_NAME));
    if (!enable) {
        if (service_disable(arena, &manager, S(SERVICE_NAME), &err) != ERR_OK) return fail(&err);
        printf("stopped and removed %.*s\n", (int)path.len, path.data);
        return EXIT_SUCCESS;
    }
    String executable;
    if (service_executable_path(arena, S(SERVICE_NAME), &executable, &err) != ERR_OK) return fail(&err);
    Service service = {
        .name = S(SERVICE_NAME),
        .description = S("dbox folder sync"),
        .argv = strlist_of(arena, 2, (const char *const[]){str_cstr(arena, executable), "run"}),
        .log_path = path_join(arena, env_home(arena), S("Library/Logs/dbox.log")),
    };
    if (service_enable(arena, &manager, &service, &err) != ERR_OK) return fail(&err);
    printf("dbox run starts now and at every login (%.*s)\n", (int)path.len, path.data);
    return EXIT_SUCCESS;
}

/* run_editor runs $VISUAL or $EDITOR through the shell, so values with arguments such as "code --wait" work. */
[[nodiscard]] static Error run_editor(Arena *arena, String path, Err *err) {
    String editor;
    if (!env_get(arena, S("VISUAL"), &editor) || editor.len == 0)
        if (!env_get(arena, S("EDITOR"), &editor) || editor.len == 0) editor = S("vi");
    String script = str_format(arena, "%.*s \"$1\"", (int)editor.len, editor.data);
    StringList argv = strlist_of(arena, 5, (const char *const[]){"sh", "-c", script.data, "sh", str_cstr(arena, path)});
    int exit_code;
    Err inner;
    Error e = process_run_interactive(arena, argv, &exit_code, &inner);
    if (e != ERR_OK) return err_set(err, e, "editor \"%.*s\": %s", (int)editor.len, editor.data, inner.msg);
    if (exit_code != 0) return err_set(err, ERR_PLATFORM, "editor \"%.*s\" exited with status %d", (int)editor.len, editor.data, exit_code);
    return ERR_OK;
}

/* edit_config opens the config in the user's editor, creating it first if needed, then validates it and asks a running daemon to reload it. */
[[nodiscard]] static Error edit_config(Arena *arena, String path, Err *err) {
    bool created;
    Error e = config_write_starter(path, &created, err);
    if (e == ERR_OK) e = run_editor(arena, path, err);
    if (e != ERR_OK) return e;
    Config config;
    Err inner;
    if ((e = config_load(arena, path, &config, &inner)) != ERR_OK) return err_set(err, e, "%s\nrun `dbox config edit` again to fix it", inner.msg);
    bool reloaded;
    if ((e = daemon_reload(arena, pid_path(arena, &config), &reloaded, err)) != ERR_OK) return e;
    printf(reloaded ? "config is valid; the daemon is reloading it\n" : "config is valid\n");
    return ERR_OK;
}

static int cmd_config(Arena *arena, int argc, char **argv) {
    Flags f;
    int rc = parse_flags(arena, &f, FLAG_INIT, argc, argv);
    if (rc >= 0) return rc;
    Err err;
    if (f.positional.count == 1 && str_equal(f.positional.items[0], S("edit"))) return edit_config(arena, f.config, &err) == ERR_OK ? EXIT_SUCCESS : fail(&err);
    if (f.positional.count > 0) return usage_fail("unknown config action \"%.*s\" (want edit)", (int)f.positional.items[0].len, f.positional.items[0].data);
    if (f.init) {
        bool created;
        if (config_write_starter(f.config, &created, &err) != ERR_OK) return fail(&err);
        printf(created ? "wrote %.*s\n" : "%.*s already exists\n", (int)f.config.len, f.config.data);
        return EXIT_SUCCESS;
    }
    Config config;
    if (config_load(arena, f.config, &config, &err) != ERR_OK) return fail(&err);
    String rendered = config_render(arena, &config);
    printf("# %.*s\n%.*s", (int)config.path.len, config.path.data, (int)rendered.len, rendered.data);
    return EXIT_SUCCESS;
}

static int cmd_reload(Arena *arena, int argc, char **argv) {
    Flags f;
    int rc = parse_flags(arena, &f, 0, argc, argv);
    if (rc >= 0) return rc;
    Err err;
    Config config;
    if (config_load(arena, f.config, &config, &err) != ERR_OK) return fail(&err);
    String pid = pid_path(arena, &config);
    bool reloaded;
    Err inner;
    Error signalled = daemon_reload(arena, pid, &reloaded, &inner);
    if (signalled != ERR_OK) (void)err_set(&err, signalled, "signal daemon: %s", inner.msg);
    else if (!reloaded) (void)err_set(&err, ERR_NOT_FOUND, "no daemon is running (no live pid in %.*s)", (int)pid.len, pid.data);
    if (signalled != ERR_OK || !reloaded) return fail(&err);
    printf("config is valid; the daemon is reloading it\n");
    return EXIT_SUCCESS;
}

static int dispatch(App *app, int argc, char **argv) {
    Arena *arena = app->arena;
    if (argc < 2) {
        print_usage(arena, stdout);
        return EXIT_SUCCESS;
    }
    String command = S(argv[1]);
    int n = argc - 2;
    char **rest = argv + 2;
    if (str_equal(command, S("run"))) return cmd_run(app, n, rest);
    if (str_equal(command, S("status"))) return cmd_status(arena, n, rest);
    if (str_equal(command, S("retry"))) return cmd_retry(arena, n, rest);
    if (str_equal(command, S("check"))) return cmd_check(arena, n, rest);
    if (str_equal(command, S("promote"))) return cmd_promote(arena, n, rest);
    if (str_equal(command, S("service"))) return cmd_service(arena, n, rest);
    if (str_equal(command, S("config"))) return cmd_config(arena, n, rest);
    if (str_equal(command, S("reload"))) return cmd_reload(arena, n, rest);
    if (str_equal(command, S("version")) || str_equal(command, S("--version"))) {
        printf("dbox %s\n", DBOX_VERSION);
        return EXIT_SUCCESS;
    }
    if (str_equal(command, S("help")) || str_equal(command, S("-h")) || str_equal(command, S("--help"))) {
        print_usage(arena, stdout);
        return EXIT_SUCCESS;
    }
    return usage_fail("unknown command \"%s\"", argv[1]);
}

int main(int argc, char **argv) {
    http_global_init();
    App app = {.arena = arena_create(0)};
    int rc = dispatch(&app, argc, argv);
    if (app.log) logger_destroy(app.log);
    arena_destroy(app.arena);
    return rc;
}
