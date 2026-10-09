/* LiteKern X — kernel/deflate.c on Linux (tests/image/test-image.sh).
 *
 * Compresses each test input, inflates it again with kernel/inflate.c and
 * checks it's the same; writes the compressed bytes to OUTDIR/<n>.deflate so
 * the script can check Python's zlib reads them too. Usage:
 * deflate_host OUTDIR [FILE...]: built-in inputs, then each FILE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/deflate.h"
#include "kernel/inflate.h"

static int failed;

static void one(const char *outdir, const char *what, const uint8_t *in, size_t n)
{
    static uint8_t work[DEFLATE_WORK];
    size_t cap = n + n / 64 + 1024;
    uint8_t *packed = malloc(cap), *back = malloc(n + 1);
    int got = deflate_raw(in, n, packed, cap, work);
    int ok = got > 0;
    int back_n = ok ? inflate_raw(packed, (size_t)got, back, n + 1) : -1;
    ok = ok && back_n == (int)n && !memcmp(in, back, n);
    printf("deflate: %s %s, %zu -> %d bytes (%d%%)\n", ok ? "ok  " : "FAIL", what, n, got,
           n ? (int)((int64_t)got * 100 / (int64_t)n) : 0);
    failed += !ok;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.deflate", outdir, what);
    FILE *f = fopen(path, "wb");
    if (f && got > 0)
        fwrite(packed, 1, (size_t)got, f);
    if (f)
        fclose(f);
    snprintf(path, sizeof(path), "%s/%s.raw", outdir, what);
    if ((f = fopen(path, "wb"))) {
        fwrite(in, 1, n, f);
        fclose(f);
    }
    free(packed);
    free(back);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    static uint8_t buf[3 * 1024 * 1024];
    one(argv[1], "empty", buf, 0);
    one(argv[1], "one-byte", (const uint8_t *)"x", 1);
    memset(buf, 0, sizeof(buf));
    one(argv[1], "zeros-3MiB", buf, sizeof(buf));
    uint32_t s = 12345;
    for (size_t i = 0; i < sizeof(buf); i++) {
        s = s * 1103515245u + 12345;
        buf[i] = (uint8_t)(s >> 16);
    }
    one(argv[1], "random-3MiB", buf, sizeof(buf));
    const char *text = "The quick brown fox jumps over the lazy dog. LiteKern X saves its settings. ";
    size_t tl = strlen(text), n = 0;
    while (n + tl < 200000)
        memcpy(buf + n, text, tl), n += tl;
    one(argv[1], "text", buf, n);
    for (size_t i = 0; i < 100000; i++)          /* long runs and far repeats */
        buf[i] = (uint8_t)(i / 700 % 3 ? i % 251 : 7);
    one(argv[1], "runs", buf, 100000);
    for (int a = 2; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f)
            return 2;
        size_t len = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        const char *base = strrchr(argv[a], '/');
        one(argv[1], base ? base + 1 : argv[a], buf, len);
    }
    printf("deflate: %s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}
