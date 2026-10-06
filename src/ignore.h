#ifndef DBOX_IGNORE_H
#define DBOX_IGNORE_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

/* The daemon's own directory under the synced root; never synced. */
#define STATE_DIR ".dbox"

/*
 * Ignore holds the patterns from the config. A pattern ending in "/" matches
 * a directory of that name anywhere in the tree. A pattern containing "/"
 * matches the whole relative path. Any other pattern matches the base name
 * of the path or of any directory above it.
 */
typedef struct {
    StrList dirs, paths, names;
} Ignore;

void ignore_init(Ignore *m, const char **patterns, size_t count);
void ignore_free(Ignore *m);
/* ignore_match reports whether rel, a slash-separated path under the root, is ignored. */
bool ignore_match(const Ignore *m, const char *rel, bool is_dir);

#endif
