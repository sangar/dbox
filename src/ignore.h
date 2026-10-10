#pragma once

#include "mc/text/str.h"

/* The daemon's own directory under the synced root; never synced. */
#define STATE_DIR ".dbox"

/*
 * Ignore holds the patterns from the config. A pattern ending in "/" matches
 * a directory of that name anywhere in the tree. A pattern containing "/"
 * matches the whole relative path. Any other pattern matches the base name
 * of the path or of any directory above it.
 */
typedef struct Ignore {
    StringList dirs, paths, names;
} Ignore;

/* ignore_create sorts patterns into an Ignore whose lists live in the arena. */
Ignore ignore_create(Arena *arena, StringList patterns);
/* ignore_match reports whether rel, a slash-separated path under the root, is ignored. */
bool ignore_match(const Ignore *ignore, String rel, bool is_dir);
