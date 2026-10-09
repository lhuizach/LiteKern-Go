/* LiteKern X — SHA-256 (FIPS 180-4), for password hashes (kernel/users.h).
 * Tested against the standard vectors (tests/image/sha256_host.c). */
#ifndef LKX_SHA256_H
#define LKX_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct sha256 {
    uint32_t h[8];
    uint64_t len;               /* bytes so far */
    uint8_t buf[64];
    uint32_t n;                 /* bytes in buf */
};

void sha256_init(struct sha256 *c);
void sha256_add(struct sha256 *c, const void *data, size_t len);
void sha256_done(struct sha256 *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

#endif
