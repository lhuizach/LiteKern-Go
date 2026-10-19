/* LiteKern X on Linux — screenshots as PNG (platform/platform.h). Stored
 * (uncompressed) deflate blocks: bigger files, no compressor needed. */
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t head[8], tail[4];
    be32(head, (uint32_t)n);
    memcpy(head + 4, type, 4);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), data, n);
    be32(tail, c);
    return fwrite(head, 1, 8, f) == 8 && fwrite(data, 1, n, f) == n && fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

int plat_screenshot(const char *path)
{
    int w, h;
    const uint32_t *px = plat_framebuffer(&w, &h);
    if (!px)
        return -1;
    size_t row = 1 + (size_t)w * 3, raw = row * (size_t)h;
    size_t blocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + blocks * 5 + 4;
    uint8_t *img = malloc(raw), *z = malloc(zlen);
    if (!img || !z) {
        free(img), free(z);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        uint8_t *o = img + (size_t)y * row;
        *o++ = 0;                           /* filter: none */
        for (int x = 0; x < w; x++) {
            uint32_t p = px[y * w + x];
            *o++ = (uint8_t)(p >> 16), *o++ = (uint8_t)(p >> 8), *o++ = (uint8_t)p;
        }
    }
    uint8_t *o = z;
    *o++ = 0x78, *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t at = 0; at < raw;) {
        size_t n = raw - at > 65535 ? 65535 : raw - at;
        *o++ = at + n == raw;               /* BFINAL, stored */
        *o++ = (uint8_t)n, *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n, *o++ = (uint8_t)(~n >> 8);
        memcpy(o, img + at, n);
        for (size_t i = 0; i < n; i++) {
            a = (a + img[at + i]) % 65521;
            b = (b + a) % 65521;
        }
        o += n;
        at += n;
    }
    be32(o, b << 16 | a);
    o += 4;
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;   /* 8-bit RGB */
    FILE *f = fopen(path, "wb");
    int err = !f || fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) != 8 || chunk(f, "IHDR", ihdr, 13) ||
              chunk(f, "IDAT", z, (size_t)(o - z)) || chunk(f, "IEND", 0, 0);
    if (f && fclose(f))
        err = 1;
    free(img), free(z);
    return err ? -1 : 0;
}
