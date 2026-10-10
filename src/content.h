#pragma once

#include "mc/core/error.h"
#include "mc/crypto/sha256.h"

/* File content as dbox compares it: copied between descriptors and identified by its SHA-256 in lowercase hex. */

/* content_copy copies in from its current position to its end into out, feeding hash when given; out -1 only hashes. */
[[nodiscard]] Error content_copy(int in, int out, Sha256 *hash, int64_t *copied, Err *err);
/* content_digest finishes hash and returns the digest as hex in the arena. */
String content_digest(Arena *arena, Sha256 *hash);
/* content_hash_file hashes the file at path. */
[[nodiscard]] Error content_hash_file(Arena *arena, String path, String *sha256, Err *err);
/* content_hash is the hex SHA-256 of data. */
String content_hash(Arena *arena, String data);
