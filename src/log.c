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
    mutex_init(&l->mu);
}

void logger_destroy(Logger *l) { mutex_destroy(&l->mu); }

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

static void log_vat(Logger *l, LogLevel level, const char *msg, va_list ap) {
    if (level < l->level || !l->out) return;
    char stamp[48], scratch[48];
    format_time(wall_ns(), stamp);
    StrBuf sb = {0};
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
    mutex_lock(&l->mu);
    fputs(sb_cstr(&sb), l->out);
    fflush(l->out);
    mutex_unlock(&l->mu);
    sb_free(&sb);
}

void log_debug(Logger *l, const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    log_vat(l, LOG_DEBUG, msg, ap);
    va_end(ap);
}

void log_info(Logger *l, const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    log_vat(l, LOG_INFO, msg, ap);
    va_end(ap);
}

void log_warn(Logger *l, const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    log_vat(l, LOG_WARN, msg, ap);
    va_end(ap);
}

void log_error(Logger *l, const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    log_vat(l, LOG_ERROR, msg, ap);
    va_end(ap);
}
