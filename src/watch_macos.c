#ifdef __APPLE__

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>

#include "watch.h"

/* FSEvents watches the whole tree and reports per-file events on a dispatch queue. */
struct WatchBackend {
    Watcher *watcher;
    FSEventStreamRef stream;
    dispatch_queue_t dispatch;
};

static void on_events(ConstFSEventStreamRef stream, void *info, size_t count, void *paths, const FSEventStreamEventFlags flags[],
                      const FSEventStreamEventId ids[]) {
    WatchBackend *b = info;
    char **list = paths;
    for (size_t i = 0; i < count; i++) {
        bool rescan = (flags[i] & kFSEventStreamEventFlagMustScanSubDirs) != 0;
        watcher_event(b->watcher, list[i], rescan);
    }
}

Error backend_open(Watcher *w, const char *root, WatchBackend **out, Err *err) {
    *out = NULL;
    WatchBackend *b = xcalloc(1, sizeof *b);
    b->watcher = w;
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

Error backend_add_dir(WatchBackend *b, const char *path, Err *err) { return ERR_OK; }

void backend_run(WatchBackend *b, Ctx *ctx) {
    ctx_lock(ctx);
    while (ctx_wait(ctx, 0)) {
    }
    ctx_unlock(ctx);
}

/* Running an empty block on the serial queue waits out a callback still in flight. */
static void drain_nothing(void *ctx) {}

void backend_close(WatchBackend *b) {
    FSEventStreamStop(b->stream);
    FSEventStreamInvalidate(b->stream);
    FSEventStreamRelease(b->stream);
    dispatch_sync_f(b->dispatch, NULL, drain_nothing);
    dispatch_release(b->dispatch);
    xfree(b);
}

#endif
