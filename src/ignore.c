#include "ignore.h"

#include "mc/text/glob.h"

Ignore ignore_create(Arena *arena, StringList patterns) {
    Ignore ignore = {0};
    strlist_push(arena, &ignore.dirs, S(STATE_DIR));
    for (size_t i = 0; i < patterns.count; i++) {
        String p = patterns.items[i];
        if (str_ends_with(p, S("/"))) strlist_push(arena, &ignore.dirs, str_trim_suffix(p, S("/")));
        else if (str_contains(p, S("/"))) strlist_push(arena, &ignore.paths, p);
        else strlist_push(arena, &ignore.names, p);
    }
    return ignore;
}

static bool match_any(StringList patterns, String name) {
    for (size_t i = 0; i < patterns.count; i++)
        if (glob_match(patterns.items[i], name)) return true;
    return false;
}

bool ignore_match(const Ignore *ignore, String rel, bool is_dir) {
    if (match_any(ignore->paths, rel)) return true;
    String rest = rel;
    while (rest.len > 0) {
        String segment;
        bool more = str_cut(rest, '/', &segment, &rest);
        if ((is_dir || more) && match_any(ignore->dirs, segment)) return true;
        if (match_any(ignore->names, segment)) return true;
    }
    return false;
}
