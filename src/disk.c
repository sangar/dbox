#include "content.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "store.h"

#define DISK_TEMP_PREFIX ".dbox-upload-"

/* Disk stores objects as files under root/prefix. The ETag is size and modification time; the content hash is computed on Head. */
typedef struct {
    Store base;
    String dir;
} Disk;

static String object_path(Arena *arena, const Disk *disk, String key) { return path_join(arena, disk->dir, key); }

static Object describe(Arena *arena, String key, const FileInfo *info) {
    return (Object){
        .key = key,
        .size = info->size,
        .mtime_ns = info->mtime_ns,
        .etag = str_format(arena, "%lld-%lld", (long long)info->size, (long long)info->mtime_ns),
        .sha256 = S(""),
    };
}

[[nodiscard]] static Error disk_put(Store *store, Cancel *cancel, Arena *arena, String key, int fd, int64_t size, Meta meta, String *etag, Err *err) {
    Disk *disk = (Disk *)store;
    String target = object_path(arena, disk, key);
    String dir = path_dir(target);
    String tmp = S("");
    int out = -1;
    int64_t copied;
    Error e = dir_create_all(dir, 0755, err);
    if (e != ERR_OK) goto out;
    if ((e = file_create_unique(arena, path_join(arena, dir, S(DISK_TEMP_PREFIX "XXXXXX")), &tmp, &out, err)) != ERR_OK) goto out;
    if ((e = file_seek(fd, 0, err)) != ERR_OK) goto out;
    if ((e = content_copy(fd, out, nullptr, &copied, err)) != ERR_OK) goto out;
    file_close(out);
    out = -1;
    if (meta.mtime_ns != 0 && (e = file_set_mtime(tmp, meta.mtime_ns, err)) != ERR_OK) goto out;
    if ((e = file_rename(tmp, target, err)) != ERR_OK) goto out;
    FileInfo info;
    if ((e = file_info_follow(target, &info, err)) != ERR_OK) goto out;
    if (!info.exists) {
        e = err_set(err, ERR_IO, "%.*s: vanished after rename", (int)target.len, target.data);
        goto out;
    }
    *etag = describe(arena, key, &info).etag;
out:
    if (out >= 0) file_close(out);
    if (e != ERR_OK && tmp.len > 0) (void)file_remove(tmp, nullptr);
    return e;
}

[[nodiscard]] static Error open_object(Arena *arena, const Disk *disk, String key, Object *object, int *fd, Err *err) {
    int f;
    Error e = file_open_read(object_path(arena, disk, key), &f, err);
    if (e != ERR_OK) return e;
    FileInfo info;
    e = file_info_fd(f, &info, err);
    if (e == ERR_OK && !info.is_regular) e = ERR_NOT_FOUND;
    if (e != ERR_OK) {
        file_close(f);
        return e;
    }
    *object = describe(arena, key, &info);
    *fd = f;
    return ERR_OK;
}

[[nodiscard]] static Error disk_get(Store *store, Cancel *cancel, Arena *arena, String key, Object *object, int *fd, Err *err) {
    return open_object(arena, (Disk *)store, key, object, fd, err);
}

[[nodiscard]] static Error disk_head(Store *store, Cancel *cancel, Arena *arena, String key, Object *object, Err *err) {
    int fd;
    Error e = open_object(arena, (Disk *)store, key, object, &fd, err);
    if (e != ERR_OK) return e;
    Sha256 hash;
    sha256_init(&hash);
    int64_t copied;
    e = content_copy(fd, -1, &hash, &copied, err);
    file_close(fd);
    if (e == ERR_OK) object->sha256 = content_digest(arena, &hash);
    return e;
}

[[nodiscard]] static Error disk_remove(Store *store, Cancel *cancel, Arena *arena, String key, Err *err) {
    Error e = file_remove(object_path(arena, (Disk *)store, key), err);
    return e == ERR_NOT_FOUND ? ERR_OK : e;
}

[[nodiscard]] static Error walk(Arena *arena, String dir, String rel, ObjectList *objects, Err *err) {
    StringList names = {0};
    Error e = dir_list(arena, dir, &names, err);
    if (e == ERR_NOT_FOUND) e = err_set(err, e, "%.*s: no such directory", (int)dir.len, dir.data);
    for (size_t i = 0; e == ERR_OK && i < names.count; i++) {
        String name = names.items[i];
        String path = path_join(arena, dir, name);
        String key = rel.len > 0 ? path_join(arena, rel, name) : name;
        FileInfo info;
        if (file_info(path, &info, nullptr) != ERR_OK || !info.exists) continue;
        if (info.is_dir) e = walk(arena, path, key, objects, err);
        else if (info.is_regular && !str_starts_with(name, S(DISK_TEMP_PREFIX))) objectlist_push(arena, objects, describe(arena, key, &info));
    }
    return e;
}

[[nodiscard]] static Error disk_list(Store *store, Cancel *cancel, Arena *arena, ObjectList *objects, Err *err) {
    Disk *disk = (Disk *)store;
    *objects = (ObjectList){0};
    FileInfo info;
    Error e = file_info_follow(disk->dir, &info, err);
    if (e == ERR_OK && info.exists) e = walk(arena, disk->dir, S(""), objects, err);
    return e;
}

static const StoreOps disk_ops = {disk_put, disk_get, disk_head, disk_remove, disk_list, nullptr};

Store *disk_open(Arena *arena, String root, String prefix) {
    Disk *disk = arena_push(arena, sizeof *disk);
    disk->base.ops = &disk_ops;
    String dir = prefix.len > 0 ? path_join(arena, root, prefix) : root;
    disk->dir = path_clean(arena, dir);
    return &disk->base;
}
