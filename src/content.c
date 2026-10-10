#include "content.h"

#include "mc/platform/platform.h"

Error content_copy(int in, int out, Sha256 *hash, int64_t *copied, Err *err) {
    char buffer[1 << 16];
    *copied = 0;
    for (;;) {
        size_t n;
        Error e = file_read(in, buffer, sizeof buffer, &n, err);
        if (e != ERR_OK || n == 0) return e;
        if (hash) sha256_update(hash, buffer, n);
        if (out >= 0 && (e = file_write(out, (String){buffer, n}, err)) != ERR_OK) return e;
        *copied += (int64_t)n;
    }
}

String content_digest(Arena *arena, Sha256 *hash) {
    uint8_t digest[SHA256_LEN];
    sha256_final(hash, digest);
    return hex_encode(arena, digest, sizeof digest);
}

Error content_hash_file(Arena *arena, String path, String *sha256, Err *err) {
    int fd;
    Error e = file_open_read(path, &fd, err);
    if (e == ERR_NOT_FOUND) return err_set(err, e, "%.*s: no such file", (int)path.len, path.data);
    if (e != ERR_OK) return e;
    Sha256 hash;
    sha256_init(&hash);
    int64_t copied;
    e = content_copy(fd, -1, &hash, &copied, err);
    file_close(fd);
    if (e == ERR_OK) *sha256 = content_digest(arena, &hash);
    return e;
}

String content_hash(Arena *arena, String data) {
    uint8_t digest[SHA256_LEN];
    sha256(data, digest);
    return hex_encode(arena, digest, sizeof digest);
}
