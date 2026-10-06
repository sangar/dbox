#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void transform(Sha256 *h, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h->state[0], b = h->state[1], c = h->state[2], d = h->state[3];
    uint32_t e = h->state[4], f = h->state[5], g = h->state[6], hh = h->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h->state[0] += a;
    h->state[1] += b;
    h->state[2] += c;
    h->state[3] += d;
    h->state[4] += e;
    h->state[5] += f;
    h->state[6] += g;
    h->state[7] += hh;
}

void sha256_init(Sha256 *h) {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h->state, init, sizeof init);
    h->length = 0;
    h->buffered = 0;
}

void sha256_update(Sha256 *h, const void *data, size_t len) {
    const uint8_t *p = data;
    h->length += len;
    if (h->buffered) {
        size_t take = min_size(64 - h->buffered, len);
        memcpy(h->buffer + h->buffered, p, take);
        h->buffered += take;
        p += take;
        len -= take;
        if (h->buffered < 64) return;
        transform(h, h->buffer);
        h->buffered = 0;
    }
    for (; len >= 64; p += 64, len -= 64) transform(h, p);
    memcpy(h->buffer, p, len);
    h->buffered = len;
}

void sha256_final(Sha256 *h, uint8_t out[SHA256_LEN]) {
    uint64_t bits = h->length * 8;
    uint8_t pad = 0x80;
    sha256_update(h, &pad, 1);
    uint8_t zero = 0;
    while (h->buffered != 56) sha256_update(h, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(h, len, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h->state[i];
    }
}

void hex_encode(const uint8_t *in, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 15];
    }
    out[n * 2] = '\0';
}

void sha256_hex(Sha256 *h, char out[SHA256_HEX_LEN]) {
    uint8_t digest[SHA256_LEN];
    sha256_final(h, digest);
    hex_encode(digest, SHA256_LEN, out);
}

void sha256_of(const void *data, size_t len, char out[SHA256_HEX_LEN]) {
    Sha256 h;
    sha256_init(&h);
    sha256_update(&h, data, len);
    sha256_hex(&h, out);
}

void hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[SHA256_LEN]) {
    uint8_t k[64] = {0};
    if (key_len > 64) {
        Sha256 kh;
        sha256_init(&kh);
        sha256_update(&kh, key, key_len);
        sha256_final(&kh, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    uint8_t inner[SHA256_LEN];
    Sha256 h;
    sha256_init(&h);
    sha256_update(&h, ipad, 64);
    sha256_update(&h, data, len);
    sha256_final(&h, inner);
    sha256_init(&h);
    sha256_update(&h, opad, 64);
    sha256_update(&h, inner, SHA256_LEN);
    sha256_final(&h, out);
}

Error copy_fd(int in, int out, Sha256 *h, int64_t *copied, Err *err) {
    char buf[1 << 16];
    *copied = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return err_sys(err, "read");
        }
        if (n == 0) return ERR_OK;
        if (h) sha256_update(h, buf, (size_t)n);
        if (out >= 0 && !write_all(out, buf, (size_t)n)) return err_sys(err, "write");
        *copied += n;
    }
}

Error sha256_file(const char *path, char out[SHA256_HEX_LEN], Err *err) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return err_sys(err, "%s", path);
    Sha256 h;
    sha256_init(&h);
    int64_t copied;
    Error e = copy_fd(fd, -1, &h, &copied, err);
    close(fd);
    if (e == ERR_OK) sha256_hex(&h, out);
    return e;
}
