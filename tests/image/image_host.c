/* LiteKern X — the picture decoders (apps/files/image.c) on Linux, with
 * AddressSanitizer, for tests/image/test-image.sh.
 *
 * Usage: image_host FILE OUT.rgba
 * Prints "ok WxH FORMAT" and writes the pixels as R G B A bytes, or prints
 * "error <why>". Memory comes from malloc, zeroed, as SYS_MEMORY's is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/files/image.h"

static void *alloc(uint32_t n)
{
    return calloc(1, n ? n : 1);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: image_host FILE OUT.rgba\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n + 1);
    if (fread(data, 1, (size_t)n, f) != (size_t)n)
        return 2;
    fclose(f);
    struct image im;
    const char *why = 0;
    if (image_decode(data, (uint32_t)n, alloc, &im, &why)) {
        printf("error %s\n", why);
        return 0;
    }
    FILE *o = fopen(argv[2], "wb");
    for (int i = 0; i < im.w * im.h; i++) {
        uint32_t c = im.px[i];
        uint8_t b[4] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 24) };
        fwrite(b, 1, 4, o);
    }
    fclose(o);
    printf("ok %dx%d %s\n", im.w, im.h, im.format);
    return 0;
}
