#ifdef __APPLE__

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "platform.h"

Os os_current(void) { return OS_DARWIN; }

char *process_executable_path(void) {
    char buf[PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return NULL;
    char *resolved = path_resolve(buf);
    return resolved ? resolved : xstrdup(buf);
}

/* FSEvents watches the whole tree and reports per-file events on a dispatch queue. */
struct WatchBackend {
    WatchEventFn fn;
    void *user;
    FSEventStreamRef stream;
    dispatch_queue_t dispatch;
};

static void on_events(ConstFSEventStreamRef stream, void *info, size_t count, void *paths, const FSEventStreamEventFlags flags[],
                      const FSEventStreamEventId ids[]) {
    WatchBackend *b = info;
    char **list = paths;
    for (size_t i = 0; i < count; i++) {
        bool rescan = (flags[i] & kFSEventStreamEventFlagMustScanSubDirs) != 0;
        b->fn(b->user, list[i], rescan);
    }
}

Error watch_backend_open(const char *root, WatchEventFn fn, void *user, WatchBackend **out, Err *err) {
    *out = NULL;
    WatchBackend *b = xcalloc(1, sizeof *b);
    b->fn = fn;
    b->user = user;
    CFStringRef s = CFStringCreateWithCString(NULL, root, kCFStringEncodingUTF8);
    CFArrayRef paths = CFArrayCreate(NULL, (const void **)&s, 1, &kCFTypeArrayCallBacks);
    CFRelease(s);
    FSEventStreamContext ctx = {.info = b};
    b->stream = FSEventStreamCreate(NULL, on_events, &ctx, paths, kFSEventStreamEventIdSinceNow, 0.1,
                                    kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    if (!b->stream) {
        xfree(b);
        return err_set(err, ERR_PLATFORM, "FSEvents: cannot create event stream for %s", root);
    }
    b->dispatch = dispatch_queue_create("dbox.fsevents", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(b->stream, b->dispatch);
    if (!FSEventStreamStart(b->stream)) {
        FSEventStreamInvalidate(b->stream);
        FSEventStreamRelease(b->stream);
        dispatch_release(b->dispatch);
        xfree(b);
        return err_set(err, ERR_PLATFORM, "FSEvents: cannot start event stream for %s", root);
    }
    *out = b;
    return ERR_OK;
}

Error watch_backend_add_dir(WatchBackend *b, const char *path, Err *err) { return ERR_OK; }

/* Events arrive on the dispatch queue, so polling only has to pass the time. */
void watch_backend_poll(WatchBackend *b, int timeout_ms) { sleep_ms(timeout_ms); }

/* Running an empty block on the serial queue waits out a callback still in flight. */
static void drain_nothing(void *ctx) {}

void watch_backend_close(WatchBackend *b) {
    FSEventStreamStop(b->stream);
    FSEventStreamInvalidate(b->stream);
    FSEventStreamRelease(b->stream);
    dispatch_sync_f(b->dispatch, NULL, drain_nothing);
    dispatch_release(b->dispatch);
    xfree(b);
}

#endif
