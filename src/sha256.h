#ifndef DBOX_SHA256_H
#define DBOX_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define SHA256_LEN 32
#define SHA256_HEX_LEN 65 /* 64 hex digits and the terminator */

typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t buffer[64];
    size_t buffered;
} Sha256;

void sha256_init(Sha256 *h);
void sha256_update(Sha256 *h, const void *data, size_t len);
void sha256_final(Sha256 *h, uint8_t out[SHA256_LEN]);
void sha256_hex(Sha256 *h, char out[SHA256_HEX_LEN]);
void sha256_of(const void *data, size_t len, char out[SHA256_HEX_LEN]);
void hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[SHA256_LEN]);
void hex_encode(const uint8_t *in, size_t n, char *out);

/* sha256_file hashes the file at path. */
[[nodiscard]] Error sha256_file(const char *path, char out[SHA256_HEX_LEN], Err *err);
/*
 * copy_fd copies in from its current position to out, feeding h when given,
 * and stores the number of bytes copied in *copied. out may be -1 to only hash.
 */
[[nodiscard]] Error copy_fd(int in, int out, Sha256 *h, int64_t *copied, Err *err);

#endif
