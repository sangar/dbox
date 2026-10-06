#include "log.h"

#include <stdarg.h>
#include <string.h>

void logger_init(Logger *l, const char *level, const char *format, FILE *out) {
    l->level = LOG_INFO;
    if (level && strcmp(level, "debug") == 0) l->level = LOG_DEBUG;
    if (level && strcmp(level, "warn") == 0) l->level = LOG_WARN;
    if (level && strcmp(level, "error") == 0) l->level = LOG_ERROR;
    l->json = format && strcmp(format, "json") == 0;
    l->out = out;
    pthread_mutex_init(&l->mu, NULL);
}

void logger_destroy(Logger *l) { pthread_mutex_destroy(&l->mu); }

Logger *logger_discard(void) {
    static Logger discard;
    static bool ready;
    if (!ready) {
        logger_init(&discard, "error", "text", NULL);
        ready = true;
    }
    return &discard;
}

static const char *level_name(LogLevel level) {
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO: return "INFO";
    case LOG_WARN: return "WARN";
    default: return "ERROR";
    }
}

static bool needs_quotes(const char *s) {
    if (!*s) return true;
    for (; *s; s++)
        if (*s == ' ' || *s == '"' || *s == '=' || *s == '\\' || (unsigned char)*s < 0x20) return true;
    return false;
}

static void write_quoted(StrBuf *sb, const char *s) {
    sb_putc(sb, '"');
    for (; *s; s++) {
        switch (*s) {
        case '"': sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        default:
            if ((unsigned char)*s < 0x20) {
                sb_printf(sb, "\\u%04x", (unsigned char)*s);
            } else {
                sb_putc(sb, *s);
            }
        }
    }
    sb_putc(sb, '"');
}

static void write_text_value(StrBuf *sb, const char *s) {
    if (needs_quotes(s)) {
        write_quoted(sb, s);
    } else {
        sb_puts(sb, s);
    }
}

static const char *attr_string(const LogAttr *a, char buf[48]) {
    switch (a->kind) {
    case ATTR_STR: return a->str ? a->str : "";
    case ATTR_INT: snprintf(buf, 48, "%lld", a->num); return buf;
    case ATTR_DURATION: return format_duration(a->num, buf);
    default: return "";
    }
}

void log_at(Logger *l, LogLevel level, const char *msg, ...) {
    if (level < l->level || !l->out) return;
    char stamp[48], scratch[48];
    format_time(wall_ns(), stamp);
    StrBuf sb = {0};
    va_list ap;
    va_start(ap, msg);
    if (l->json) {
        sb_printf(&sb, "{\"time\":\"%s\",\"level\":\"%s\",\"msg\":", stamp, level_name(level));
        write_quoted(&sb, msg);
        for (;;) {
            LogAttr a = va_arg(ap, LogAttr);
            if (a.kind == ATTR_END) break;
            sb_putc(&sb, ',');
            write_quoted(&sb, a.key);
            sb_putc(&sb, ':');
            if (a.kind == ATTR_INT) {
                sb_printf(&sb, "%lld", a.num);
            } else {
                write_quoted(&sb, attr_string(&a, scratch));
            }
        }
        sb_puts(&sb, "}\n");
    } else {
        sb_printf(&sb, "time=%s level=%s msg=", stamp, level_name(level));
        write_text_value(&sb, msg);
        for (;;) {
            LogAttr a = va_arg(ap, LogAttr);
            if (a.kind == ATTR_END) break;
            sb_printf(&sb, " %s=", a.key);
            write_text_value(&sb, attr_string(&a, scratch));
        }
        sb_putc(&sb, '\n');
    }
    va_end(ap);
    pthread_mutex_lock(&l->mu);
    fputs(sb_cstr(&sb), l->out);
    fflush(l->out);
    pthread_mutex_unlock(&l->mu);
    sb_free(&sb);
}
