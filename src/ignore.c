#include "ignore.h"

#include <fnmatch.h>
#include <string.h>

void ignore_init(Ignore *m, const char **patterns, size_t count) {
    memset(m, 0, sizeof *m);
    strlist_push(&m->dirs, STATE_DIR);
    for (size_t i = 0; i < count; i++) {
        const char *p = patterns[i];
        size_t n = strlen(p);
        if (n > 0 && p[n - 1] == '/') {
            strlist_push_owned(&m->dirs, xstrndup(p, n - 1));
        } else if (strchr(p, '/')) {
            strlist_push(&m->paths, p);
        } else {
            strlist_push(&m->names, p);
        }
    }
}

void ignore_free(Ignore *m) {
    strlist_free(&m->dirs);
    strlist_free(&m->paths);
    strlist_free(&m->names);
}

static bool match_any(const StrList *patterns, const char *name) {
    for (size_t i = 0; i < patterns->len; i++)
        if (fnmatch(patterns->items[i], name, FNM_PATHNAME) == 0) return true;
    return false;
}

bool ignore_match(const Ignore *m, const char *rel, bool is_dir) {
    if (match_any(&m->paths, rel)) return true;
    char segment[1024];
    for (const char *p = rel; *p;) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        bool segment_is_dir = is_dir || end != NULL;
        if (n < sizeof segment) {
            memcpy(segment, p, n);
            segment[n] = '\0';
            if (segment_is_dir && match_any(&m->dirs, segment)) return true;
            if (match_any(&m->names, segment)) return true;
        }
        if (!end) break;
        p = end + 1;
    }
    return false;
}
