/* LiteKern X — Files: pictures (see image.h).
 *
 *   PNG   every colour type and bit depth, palettes, transparency (tRNS),
 *         interlaced (Adam7) too; inflated with kernel/inflate.c
 *   JPEG  baseline and progressive (Huffman), greyscale or YCbCr, any
 *         sampling, restart markers; turned the right way up from the
 *         camera's EXIF orientation. Not lossless or arithmetic-coded ones
 *   BMP   1, 4, 8, 24 and 32 bits a pixel, uncompressed
 *
 * The JPEG decoder follows the structure of Sean Barrett's public-domain
 * stb_image (its integer IDCT and progressive refinement included). */
#include "apps/files/image.h"
#include "kernel/inflate.h"
#include "kernel/string.h"

static const char *err_why;
void (*image_busy)(void);

static void busy(void)
{
    if (image_busy)
        image_busy();
}

static int fail(const char *why)
{
    err_why = why;
    return -1;
}

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int too_big(uint32_t w, uint32_t h)
{
    return !w || !h || w > 16384 || h > 16384 || (uint64_t)w * h > IMAGE_MAX_PIXELS;
}

static uint32_t *new_pixels(image_alloc_fn alloc, int w, int h)
{
    return alloc((uint32_t)w * (uint32_t)h * 4);
}

static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

int image_recognise(const uint8_t *d, uint32_t len)
{
    if (len >= 8 && !memcmp(d, png_sig, 8))
        return 1;
    if (len >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff)
        return 1;
    if (len >= 26 && d[0] == 'B' && d[1] == 'M')
        return 1;
    return 0;
}

/* --- PNG ------------------------------------------------------------------------------- */

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

/* Undo one row's filter; prev is the row above (NULL for the first). */
static int unfilter(uint8_t *row, const uint8_t *prev, uint32_t n, uint32_t bpp, uint8_t type)
{
    for (uint32_t i = 0; i < n; i++) {
        int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
        switch (type) {
        case 0: break;
        case 1: row[i] = (uint8_t)(row[i] + a); break;
        case 2: row[i] = (uint8_t)(row[i] + b); break;
        case 3: row[i] = (uint8_t)(row[i] + ((a + b) >> 1)); break;
        case 4: row[i] = (uint8_t)(row[i] + paeth(a, b, c)); break;
        default: return -1;
        }
    }
    return 0;
}

struct png {
    uint32_t w, h, depth, ctype, channels, bits;    /* bits: a pixel's */
    uint32_t pal[256];
    int has_key;
    uint16_t key[3];            /* tRNS for greyscale / RGB: this colour is transparent */
};

static uint32_t sample(const uint8_t *row, uint32_t i, uint32_t depth)
{
    if (depth == 8)
        return row[i];
    if (depth == 16)
        return be16(row + 2 * i);
    uint32_t bit = i * depth;
    return (uint32_t)(row[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
}

static uint32_t to8(uint32_t v, uint32_t depth)
{
    if (depth == 16)
        return v >> 8;
    if (depth == 8)
        return v;
    return v * 255 / ((1u << depth) - 1);
}

static uint32_t png_pixel(const struct png *p, const uint8_t *row, uint32_t x)
{
    uint32_t c = p->channels, d = p->depth, s[4];
    for (uint32_t k = 0; k < c; k++)
        s[k] = sample(row, x * c + k, d);
    uint32_t r, g, b, a = 255;
    switch (p->ctype) {
    case 0:
        r = g = b = to8(s[0], d);
        if (p->has_key && s[0] == p->key[0])
            a = 0;
        break;
    case 2:
        r = to8(s[0], d), g = to8(s[1], d), b = to8(s[2], d);
        if (p->has_key && s[0] == p->key[0] && s[1] == p->key[1] && s[2] == p->key[2])
            a = 0;
        break;
    case 3:
        return p->pal[s[0] & 255];
    case 4:
        r = g = b = to8(s[0], d), a = to8(s[1], d);
        break;
    default:
        r = to8(s[0], d), g = to8(s[1], d), b = to8(s[2], d), a = to8(s[3], d);
        break;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

static int png_decode(const uint8_t *d, uint32_t len, image_alloc_fn alloc, struct image *out)
{
    static const uint8_t ax[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay[7] = { 0, 0, 4, 0, 2, 0, 1 },
                         sx[7] = { 8, 8, 4, 4, 2, 2, 1 }, sy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    struct png p;
    memset(&p, 0, sizeof(p));
    for (int i = 0; i < 256; i++)
        p.pal[i] = 0xff000000u;
    uint8_t *z = alloc(len);                /* the IDAT chunks, joined */
    uint32_t zn = 0, interlace = 0;
    int seen_ihdr = 0, seen_end = 0;
    if (!z)
        return fail("There isn't enough memory to open it.");
    for (uint32_t pos = 8; pos + 12 <= len && !seen_end;) {
        uint32_t n = be32(d + pos);
        const uint8_t *type = d + pos + 4, *c = d + pos + 8;
        if (n > len - pos - 12)
            return fail("The PNG file is cut short.");
        if (!memcmp(type, "IHDR", 4)) {
            if (n < 13)
                return fail("The PNG file is damaged.");
            p.w = be32(c), p.h = be32(c + 4), p.depth = c[8], p.ctype = c[9], interlace = c[12];
            static const uint8_t ch[7] = { 1, 0, 3, 1, 2, 0, 4 };
            if (p.ctype > 6 || !ch[p.ctype] || c[10] || c[11] || interlace > 1)
                return fail("This kind of PNG isn't supported.");
            p.channels = ch[p.ctype];
            uint32_t dp = p.depth;
            int ok = dp == 8 || (dp == 16 && p.ctype != 3) ||
                     ((dp == 1 || dp == 2 || dp == 4) && (p.ctype == 0 || p.ctype == 3));
            if (!ok)
                return fail("This kind of PNG isn't supported.");
            if (too_big(p.w, p.h))
                return fail("The picture is too big to open (24 megapixels at most).");
            p.bits = p.channels * p.depth;
            seen_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            for (uint32_t i = 0; i < n / 3 && i < 256; i++)
                p.pal[i] = 0xff000000u | (uint32_t)c[3 * i] << 16 | (uint32_t)c[3 * i + 1] << 8 | c[3 * i + 2];
        } else if (!memcmp(type, "tRNS", 4)) {
            if (p.ctype == 3)
                for (uint32_t i = 0; i < n && i < 256; i++)
                    p.pal[i] = (p.pal[i] & 0xffffff) | (uint32_t)c[i] << 24;
            else if (p.ctype == 0 && n >= 2)
                p.has_key = 1, p.key[0] = (uint16_t)be16(c);
            else if (p.ctype == 2 && n >= 6)
                p.has_key = 1, p.key[0] = (uint16_t)be16(c), p.key[1] = (uint16_t)be16(c + 2),
                p.key[2] = (uint16_t)be16(c + 4);
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(z + zn, c, n);
            zn += n;
        } else if (!memcmp(type, "IEND", 4)) {
            seen_end = 1;
        }
        pos += n + 12;
    }
    if (!seen_ihdr || zn < 3)
        return fail("The PNG file is damaged.");
    if ((z[0] & 15) != 8 || (z[0] << 8 | z[1]) % 31 || (z[1] & 0x20))
        return fail("The PNG file is damaged.");

    /* How much the rows take, unpacked: every pass's, filter bytes included. */
    uint32_t passes = interlace ? 7 : 1, raw_len = 0, bpp = p.bits >= 8 ? p.bits / 8 : 1;
    uint32_t pw[7], ph[7];
    for (uint32_t k = 0; k < passes; k++) {
        pw[k] = interlace ? (p.w > ax[k] ? (p.w - ax[k] + sx[k] - 1) / sx[k] : 0) : p.w;
        ph[k] = interlace ? (p.h > ay[k] ? (p.h - ay[k] + sy[k] - 1) / sy[k] : 0) : p.h;
        if (pw[k] && ph[k])
            raw_len += ph[k] * (1 + (pw[k] * p.bits + 7) / 8);
    }
    uint8_t *raw = alloc(raw_len);
    out->px = new_pixels(alloc, (int)p.w, (int)p.h);
    if (!raw || !out->px)
        return fail("There isn't enough memory to open it.");
    inflate_progress = image_busy;
    int got = inflate_raw(z + 2, zn - 2, raw, raw_len);
    inflate_progress = 0;
    if (got < 0 || (uint32_t)got < raw_len)
        return fail("The PNG file is damaged.");
    uint8_t *r = raw;
    for (uint32_t k = 0; k < passes; k++) {
        if (!pw[k] || !ph[k])
            continue;
        uint32_t stride = (pw[k] * p.bits + 7) / 8;
        const uint8_t *prev = 0;
        for (uint32_t y = 0; y < ph[k]; y++, r += 1 + stride) {
            if (!(y & 31))
                busy();
            if (unfilter(r + 1, prev, stride, bpp, r[0]))
                return fail("The PNG file is damaged.");
            prev = r + 1;
            uint32_t oy = interlace ? ay[k] + y * sy[k] : y;
            for (uint32_t x = 0; x < pw[k]; x++) {
                uint32_t ox = interlace ? ax[k] + x * sx[k] : x;
                out->px[oy * p.w + ox] = png_pixel(&p, r + 1, x);
            }
        }
    }
    out->w = (int)p.w;
    out->h = (int)p.h;
    out->format = "PNG";
    return 0;
}

/* --- BMP ---------------------------------------------------------------------------------- */

static int bmp_decode(const uint8_t *d, uint32_t len, image_alloc_fn alloc, struct image *out)
{
    uint32_t off = le32(d + 10), dib = le32(d + 14);
    if (dib < 40 || 14 + dib > len)
        return fail("This kind of BMP isn't supported.");
    int32_t w = (int32_t)le32(d + 18), h = (int32_t)le32(d + 22);
    uint32_t bits = le16(d + 28), comp = le32(d + 30), ncol = le32(d + 46);
    int top_down = h < 0;
    if (top_down)
        h = -h;
    if (w <= 0 || too_big((uint32_t)w, (uint32_t)h))
        return fail(w <= 0 ? "The BMP file is damaged." : "The picture is too big to open (24 megapixels at most).");
    if (!(comp == 0 || (comp == 3 && bits == 32)) ||
        !(bits == 1 || bits == 4 || bits == 8 || bits == 24 || bits == 32))
        return fail("This kind of BMP isn't supported (compressed ones aren't).");
    uint32_t stride = ((uint32_t)w * bits + 31) / 32 * 4;
    if (off > len || (uint64_t)stride * (uint32_t)h > len - off)
        return fail("The BMP file is cut short.");
    uint32_t pal[256];
    if (bits <= 8) {
        if (!ncol || ncol > (1u << bits))
            ncol = 1u << bits;
        uint32_t po = 14 + dib;
        for (uint32_t i = 0; i < 256; i++)
            pal[i] = 0xff000000u;
        for (uint32_t i = 0; i < ncol && po + 4 * i + 3 < len; i++)
            pal[i] = 0xff000000u | (le32(d + po + 4 * i) & 0xffffff);
    }
    out->px = new_pixels(alloc, w, h);
    if (!out->px)
        return fail("There isn't enough memory to open it.");
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = d + off + (uint32_t)(top_down ? y : h - 1 - y) * stride;
        uint32_t *o = out->px + (uint32_t)y * (uint32_t)w;
        for (int x = 0; x < w; x++) {
            if (bits == 32) {
                o[x] = le32(row + 4 * x);
                any_alpha |= (o[x] >> 24) != 0;
            } else if (bits == 24) {
                o[x] = 0xff000000u | (uint32_t)row[3 * x + 2] << 16 | (uint32_t)row[3 * x + 1] << 8 | row[3 * x];
            } else {
                o[x] = pal[sample(row, (uint32_t)x, bits)];
            }
        }
    }
    if (bits == 32 && !any_alpha)           /* "XRGB": the fourth byte is unused */
        for (uint32_t i = 0; i < (uint32_t)w * (uint32_t)h; i++)
            out->px[i] |= 0xff000000u;
    out->w = w;
    out->h = h;
    out->format = "BMP";
    return 0;
}

/* --- JPEG ---------------------------------------------------------------------------------- */

static const uint8_t dezigzag[64 + 15] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,     /* a bad run can't go past the end */
};

#define FAST_BITS 9

struct huff {
    uint8_t fast[1 << FAST_BITS];
    uint16_t code[256];
    uint8_t values[256];
    uint8_t size[257];
    uint32_t maxcode[18];
    int delta[17];
};

struct comp {
    int id, h, v, tq, hd, ha, dc_pred;
    int x, y, w2, h2;           /* real size; the padded plane's */
    uint8_t *plane;
    int16_t *coef;              /* progressive: every block's coefficients */
    int bw, bh;                 /* the plane in blocks */
};

struct jpeg {
    const uint8_t *p, *end;
    uint32_t w, h;
    int ncomp, hmax, vmax, mcux, mcuy, progressive, restart;
    struct comp c[3];
    struct huff dc[4], ac[4];
    uint16_t q[4][64];          /* in zigzag order */
    uint32_t buf;
    int bits, nomore, marker, eob_run;
    int order[3], nscan;        /* this scan's components */
    int ss, se, ah, al;
    int orientation;            /* EXIF: 1 = the right way up */
    int adobe_rgb;
};

static int build_huff(struct huff *h, const uint8_t *count)
{
    int k = 0, code = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < count[i]; j++) {
            if (k >= 256)
                return -1;
            h->size[k++] = (uint8_t)(i + 1);
        }
    h->size[k] = 0;
    k = 0;
    int j;
    for (j = 1; j <= 16; j++) {
        h->delta[j] = k - code;
        if (h->size[k] == j) {
            while (h->size[k] == j)
                h->code[k++] = (uint16_t)code++;
            if (code - 1 >= (1 << j))
                return -1;
        }
        h->maxcode[j] = (uint32_t)code << (16 - j);
        code <<= 1;
    }
    h->maxcode[j] = 0xffffffffu;
    memset(h->fast, 255, sizeof(h->fast));
    for (int i = 0; i < k; i++) {
        int s = h->size[i];
        if (s <= FAST_BITS) {
            int c = h->code[i] << (FAST_BITS - s), m = 1 << (FAST_BITS - s);
            for (int n = 0; n < m; n++)
                h->fast[c + n] = (uint8_t)i;
        }
    }
    return 0;
}

static void grow(struct jpeg *j)
{
    while (j->bits <= 24) {
        uint32_t b = 0;
        if (!j->nomore && j->p < j->end) {
            b = *j->p++;
            if (b == 0xff) {
                uint32_t c = j->p < j->end ? *j->p : 0;
                while (c == 0xff && j->p + 1 < j->end)
                    c = *++j->p;
                if (c != 0) {               /* a marker: no more data in this stretch */
                    j->marker = (int)c;
                    j->p--;
                    j->nomore = 1;
                    b = 0;
                } else {
                    j->p++;                 /* 0xff00 is a 0xff byte */
                }
            }
        } else {
            j->nomore = 1;
        }
        j->buf |= b << (24 - j->bits);
        j->bits += 8;
    }
}

static int decode(struct jpeg *j, const struct huff *h)
{
    if (j->bits < 16)
        grow(j);
    int c = (int)(j->buf >> (32 - FAST_BITS)) & ((1 << FAST_BITS) - 1), k = h->fast[c];
    if (k < 255) {
        int s = h->size[k];
        if (s > j->bits)
            return -1;
        j->buf <<= s;
        j->bits -= s;
        return h->values[k];
    }
    uint32_t t = j->buf >> 16;
    for (k = FAST_BITS + 1; k < 17; k++)
        if (t < h->maxcode[k])
            break;
    if (k == 17 || k > j->bits)
        return -1;
    c = (int)((j->buf >> (32 - k)) & ((1u << k) - 1)) + h->delta[k];
    if (c < 0 || c > 255)
        return -1;
    j->bits -= k;
    j->buf <<= k;
    return h->values[c];
}

static int getbits(struct jpeg *j, int n)
{
    if (!n)
        return 0;
    if (j->bits < n)
        grow(j);
    int v = (int)(j->buf >> (32 - n));
    j->buf <<= n;
    j->bits -= n;
    return v;
}

static int extend(struct jpeg *j, int n)
{
    int v = getbits(j, n);
    return n && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}

/* Integer IDCT (stb_image's, libjpeg's "islow" in spirit): 12-bit fixed point. */
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                        \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                            \
    p2 = s2; p3 = s6;                                                                  \
    p1 = (p2 + p3) * 2217;                                                             \
    t2 = p1 + p3 * -7568;                                                              \
    t3 = p1 + p2 * 3135;                                                               \
    p2 = s0; p3 = s4;                                                                  \
    t0 = (p2 + p3) * 4096; t1 = (p2 - p3) * 4096;                                      \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2;                            \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1;                                                \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2;                            \
    p5 = (p3 + p4) * 4816;                                                             \
    t0 = t0 * 1223; t1 = t1 * 8410; t2 = t2 * 12586; t3 = t3 * 6149;                   \
    p1 = p5 + p1 * -3686; p2 = p5 + p2 * -10498;                                       \
    p3 = p3 * -8035; p4 = p4 * -1598;                                                  \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static uint8_t clamp8(int x)
{
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

static void idct(uint8_t *out, int stride, const int16_t *data)
{
    int val[64], *v = val;
    const int16_t *d = data;
    for (int i = 0; i < 8; i++, d++, v++) {
        if (!d[8] && !d[16] && !d[24] && !d[32] && !d[40] && !d[48] && !d[56]) {
            int dc = d[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
        } else {
            IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[0] = (x0 + t3) >> 10; v[56] = (x0 - t3) >> 10;
            v[8] = (x1 + t2) >> 10; v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10; v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10; v[32] = (x3 - t0) >> 10;
        }
    }
    v = val;
    for (int i = 0; i < 8; i++, v += 8, out += stride) {
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        out[0] = clamp8((x0 + t3) >> 17); out[7] = clamp8((x0 - t3) >> 17);
        out[1] = clamp8((x1 + t2) >> 17); out[6] = clamp8((x1 - t2) >> 17);
        out[2] = clamp8((x2 + t1) >> 17); out[5] = clamp8((x2 - t1) >> 17);
        out[3] = clamp8((x3 + t0) >> 17); out[4] = clamp8((x3 - t0) >> 17);
    }
}

/* Baseline: one block, decoded, dequantised and turned into pixels. */
static int block_baseline(struct jpeg *j, struct comp *c, uint8_t *out, int stride)
{
    int16_t data[64];
    memset(data, 0, sizeof(data));
    const uint16_t *q = j->q[c->tq];
    int t = decode(j, &j->dc[c->hd]);
    if (t < 0 || t > 15)
        return -1;
    c->dc_pred += t ? extend(j, t) : 0;
    data[0] = (int16_t)(c->dc_pred * q[0]);
    for (int k = 1; k < 64;) {
        int rs = decode(j, &j->ac[c->ha]);
        if (rs < 0)
            return -1;
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r != 15)
                break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return -1;
        data[dezigzag[k]] = (int16_t)(extend(j, s) * q[k]);
        k++;
    }
    idct(out, stride, data);
    return 0;
}

static int block_dc(struct jpeg *j, struct comp *c, int16_t *data)
{
    if (j->ah == 0) {
        int t = decode(j, &j->dc[c->hd]);
        if (t < 0 || t > 15)
            return -1;
        c->dc_pred += t ? extend(j, t) : 0;
        data[0] = (int16_t)(c->dc_pred * (1 << j->al));
    } else if (getbits(j, 1)) {
        data[0] = (int16_t)(data[0] + (1 << j->al));
    }
    return 0;
}

static int block_ac(struct jpeg *j, struct comp *c, int16_t *data)
{
    const struct huff *h = &j->ac[c->ha];
    if (j->ah == 0) {                       /* first pass at these coefficients */
        if (j->eob_run) {
            j->eob_run--;
            return 0;
        }
        for (int k = j->ss; k <= j->se;) {
            int rs = decode(j, h);
            if (rs < 0)
                return -1;
            int r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r < 15) {
                    j->eob_run = (1 << r) - 1;
                    if (r)
                        j->eob_run += getbits(j, r);
                    break;
                }
                k += 16;
            } else {
                k += r;
                if (k > 63)
                    return -1;
                data[dezigzag[k]] = (int16_t)(extend(j, s) * (1 << j->al));
                k++;
            }
        }
        return 0;
    }
    /* Refinement: one more bit of each coefficient already seen. */
    int16_t bit = (int16_t)(1 << j->al);
    if (j->eob_run) {
        j->eob_run--;
        for (int k = j->ss; k <= j->se; k++) {
            int16_t *p = &data[dezigzag[k]];
            if (*p && getbits(j, 1) && !(*p & bit))
                *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
        }
        return 0;
    }
    int k = j->ss;
    do {
        int rs = decode(j, h);
        if (rs < 0)
            return -1;
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r < 15) {
                j->eob_run = (1 << r) - 1;
                if (r)
                    j->eob_run += getbits(j, r);
                r = 64;                     /* the rest of the block */
            }
        } else {
            if (s != 1)
                return -1;
            s = getbits(j, 1) ? bit : -bit;
        }
        while (k <= j->se) {
            int16_t *p = &data[dezigzag[k++]];
            if (*p) {
                if (getbits(j, 1) && !(*p & bit))
                    *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
            } else {
                if (!r) {
                    *p = (int16_t)s;
                    break;
                }
                r--;
            }
        }
    } while (k <= j->se);
    return 0;
}

/* After a restart marker: everything starts again. */
static int restart(struct jpeg *j)
{
    j->buf = 0;
    j->bits = 0;
    j->nomore = 0;
    j->eob_run = 0;
    for (int i = 0; i < j->ncomp; i++)
        j->c[i].dc_pred = 0;
    if (j->marker >= 0xd0 && j->marker <= 0xd7) {
        j->p += 2;                          /* past the RSTn */
        j->marker = 0;
        return 0;
    }
    /* Not found where expected: look for it. */
    while (j->p + 1 < j->end && !(j->p[0] == 0xff && j->p[1] >= 0xd0 && j->p[1] <= 0xd7))
        j->p++;
    if (j->p + 1 >= j->end)
        return -1;
    j->p += 2;
    j->marker = 0;
    return 0;
}

static int scan(struct jpeg *j)
{
    j->buf = 0;
    j->bits = 0;
    j->nomore = 0;
    j->marker = 0;
    j->eob_run = 0;
    for (int i = 0; i < j->ncomp; i++)
        j->c[i].dc_pred = 0;
    int todo = j->restart ? j->restart : 0x7fffffff;
    if (j->nscan == 1) {                    /* one component: its blocks, in order */
        struct comp *c = &j->c[j->order[0]];
        int bw = (c->x + 7) / 8, bh = (c->y + 7) / 8;
        for (int by = 0; by < bh; by++, busy())
            for (int bx = 0; bx < bw; bx++) {
                int r;
                if (!j->progressive)
                    r = block_baseline(j, c, c->plane + by * 8 * c->w2 + bx * 8, c->w2);
                else {
                    int16_t *data = c->coef + 64 * (by * c->bw + bx);
                    r = j->ss == 0 ? block_dc(j, c, data) : block_ac(j, c, data);
                }
                if (r)
                    return -1;
                if (--todo <= 0) {
                    if (j->bits < 24)
                        grow(j);
                    if (by * bw + bx + 1 < bw * bh && restart(j))
                        return -1;
                    todo = j->restart;
                }
            }
        return 0;
    }
    for (int my = 0; my < j->mcuy; my++, busy())   /* interleaved: MCU by MCU */
        for (int mx = 0; mx < j->mcux; mx++) {
            for (int k = 0; k < j->nscan; k++) {
                struct comp *c = &j->c[j->order[k]];
                for (int y = 0; y < c->v; y++)
                    for (int x = 0; x < c->h; x++) {
                        int bx = mx * c->h + x, by = my * c->v + y, r;
                        if (!j->progressive)
                            r = block_baseline(j, c, c->plane + by * 8 * c->w2 + bx * 8, c->w2);
                        else
                            r = block_dc(j, c, c->coef + 64 * (by * c->bw + bx));
                        if (r)
                            return -1;
                    }
            }
            if (--todo <= 0) {
                if (j->bits < 24)
                    grow(j);
                if (my * j->mcux + mx + 1 < j->mcux * j->mcuy && restart(j))
                    return -1;
                todo = j->restart;
            }
        }
    return 0;
}

/* EXIF (APP1): only the orientation tag is wanted. */
static void exif(struct jpeg *j, const uint8_t *s, uint32_t n)
{
    if (n < 14 || memcmp(s, "Exif\0\0", 6))
        return;
    const uint8_t *t = s + 6;
    uint32_t tn = n - 6;
    int le = t[0] == 'I';
    if (!((t[0] == 'I' && t[1] == 'I') || (t[0] == 'M' && t[1] == 'M')))
        return;
#define R16(o) (le ? le16(t + (o)) : be16(t + (o)))
#define R32(o) (le ? le32(t + (o)) : be32(t + (o)))
    uint32_t ifd = R32(4);
    if (ifd + 2 > tn)
        return;
    uint32_t count = R16(ifd);
    for (uint32_t i = 0; i < count && ifd + 2 + 12 * (i + 1) <= tn; i++) {
        uint32_t e = ifd + 2 + 12 * i;
        if (R16(e) == 0x0112) {
            uint32_t o = R16(e + 8);
            if (o >= 1 && o <= 8)
                j->orientation = (int)o;
            return;
        }
    }
#undef R16
#undef R32
}

static int jpeg_decode(const uint8_t *d, uint32_t len, image_alloc_fn alloc, struct image *out)
{
    static struct jpeg j;                   /* big (its tables): not on the stack */
    memset(&j, 0, sizeof(j));
    j.p = d + 2;
    j.end = d + len;
    j.orientation = 1;
    int frame = 0;
    for (;;) {
        while (j.p < j.end && *j.p != 0xff)
            j.p++;                          /* stray bytes before a marker */
        while (j.p < j.end && *j.p == 0xff)
            j.p++;
        if (j.p >= j.end)
            return fail(frame ? "The JPEG file is cut short." : "The JPEG file is damaged.");
        int m = *j.p++;
        if (m == 0xd9)                      /* EOI */
            break;
        if (m == 0x01 || (m >= 0xd0 && m <= 0xd7))
            continue;
        if (j.p + 2 > j.end)
            return fail("The JPEG file is cut short.");
        uint32_t n = be16(j.p);
        if (n < 2 || n > (uint32_t)(j.end - j.p))
            return fail("The JPEG file is cut short.");
        const uint8_t *s = j.p + 2, *send = j.p + n;
        j.p += n;
        n -= 2;
        switch (m) {
        case 0xdb:                          /* DQT */
            while (s < send) {
                int pq = s[0] >> 4, tq = s[0] & 15;
                s++;
                if (tq > 3 || send - s < (pq ? 128 : 64))
                    return fail("The JPEG file is damaged.");
                for (int k = 0; k < 64; k++, s += pq ? 2 : 1)
                    j.q[tq][k] = (uint16_t)(pq ? be16(s) : *s);
            }
            break;
        case 0xc4:                          /* DHT */
            while (s + 17 <= send) {
                int tc = s[0] >> 4, th = s[0] & 15, total = 0;
                if (tc > 1 || th > 3)
                    return fail("The JPEG file is damaged.");
                for (int k = 0; k < 16; k++)
                    total += s[1 + k];
                if (total > 256 || s + 17 + total > send)
                    return fail("The JPEG file is damaged.");
                struct huff *h = tc ? &j.ac[th] : &j.dc[th];
                if (build_huff(h, s + 1))
                    return fail("The JPEG file is damaged.");
                memcpy(h->values, s + 17, (size_t)total);
                s += 17 + total;
            }
            break;
        case 0xdd:                          /* DRI */
            if (n < 2)
                return fail("The JPEG file is damaged.");
            j.restart = (int)be16(s);
            break;
        case 0xe1:
            exif(&j, s, n);
            break;
        case 0xee:                          /* Adobe: says whether it's RGB, not YCbCr */
            if (n >= 12 && !memcmp(s, "Adobe", 5) && s[11] == 0)
                j.adobe_rgb = 1;
            break;
        case 0xc0: case 0xc1: case 0xc2: {  /* SOF: baseline, extended, progressive */
            if (frame || n < 6)
                return fail("The JPEG file is damaged.");
            frame = 1;
            j.progressive = m == 0xc2;
            if (s[0] != 8)
                return fail("Only 8-bit JPEGs are supported.");
            j.h = be16(s + 1);
            j.w = be16(s + 3);
            j.ncomp = s[5];
            if (j.ncomp != 1 && j.ncomp != 3)
                return fail(j.ncomp == 4 ? "CMYK JPEGs aren't supported." : "The JPEG file is damaged.");
            if (n < 6 + 3u * (uint32_t)j.ncomp)
                return fail("The JPEG file is damaged.");
            if (too_big(j.w, j.h))
                return fail(!j.w || !j.h ? "The JPEG file is damaged."
                                         : "The picture is too big to open (24 megapixels at most).");
            j.hmax = j.vmax = 1;
            for (int i = 0; i < j.ncomp; i++) {
                struct comp *c = &j.c[i];
                c->id = s[6 + 3 * i];
                c->h = s[7 + 3 * i] >> 4;
                c->v = s[7 + 3 * i] & 15;
                c->tq = s[8 + 3 * i];
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3)
                    return fail("The JPEG file is damaged.");
                if (c->h > j.hmax)
                    j.hmax = c->h;
                if (c->v > j.vmax)
                    j.vmax = c->v;
            }
            j.mcux = (int)(j.w + 8 * j.hmax - 1) / (8 * j.hmax);
            j.mcuy = (int)(j.h + 8 * j.vmax - 1) / (8 * j.vmax);
            for (int i = 0; i < j.ncomp; i++) {
                struct comp *c = &j.c[i];
                c->x = (int)(j.w * c->h + j.hmax - 1) / j.hmax;
                c->y = (int)(j.h * c->v + j.vmax - 1) / j.vmax;
                c->w2 = j.mcux * c->h * 8;
                c->h2 = j.mcuy * c->v * 8;
                c->bw = c->w2 / 8;
                c->bh = c->h2 / 8;
                c->plane = alloc((uint32_t)c->w2 * (uint32_t)c->h2);
                if (j.progressive)
                    c->coef = alloc((uint32_t)c->w2 * (uint32_t)c->h2 * 2);
                if (!c->plane || (j.progressive && !c->coef))
                    return fail("There isn't enough memory to open it.");
            }
            break;
        }
        case 0xc3: case 0xc5: case 0xc6: case 0xc7: case 0xc9: case 0xca: case 0xcb:
        case 0xcd: case 0xce: case 0xcf:
            return fail("This kind of JPEG (lossless or arithmetic-coded) isn't supported.");
        case 0xda: {                        /* SOS: the scan's data follows */
            if (!frame || n < 1 || n < 1 + 2u * s[0] + 3 || s[0] < 1 || s[0] > j.ncomp)
                return fail("The JPEG file is damaged.");
            j.nscan = s[0];
            for (int k = 0; k < j.nscan; k++) {
                int id = s[1 + 2 * k], which = -1;
                for (int i = 0; i < j.ncomp; i++)
                    if (j.c[i].id == id)
                        which = i;
                if (which < 0)
                    return fail("The JPEG file is damaged.");
                j.order[k] = which;
                j.c[which].hd = s[2 + 2 * k] >> 4;
                j.c[which].ha = s[2 + 2 * k] & 15;
                if (j.c[which].hd > 3 || j.c[which].ha > 3)
                    return fail("The JPEG file is damaged.");
            }
            const uint8_t *t = s + 1 + 2 * j.nscan;
            j.ss = t[0];
            j.se = t[1];
            j.ah = t[2] >> 4;
            j.al = t[2] & 15;
            if (j.progressive) {
                if (j.ss > 63 || j.se > 63 || j.ss > j.se || j.ah > 13 || j.al > 13 ||
                    (j.ss == 0 && j.se != 0) || (j.ss != 0 && j.nscan != 1))
                    return fail("The JPEG file is damaged.");
            } else {
                j.ss = 0;
                j.se = 63;
            }
            if (scan(&j))
                return fail("The JPEG file is damaged.");
            /* The scan ends at the next marker (scan() stops before it). */
            while (j.p + 1 < j.end && !(j.p[0] == 0xff && j.p[1] != 0 && !(j.p[1] >= 0xd0 && j.p[1] <= 0xd7)))
                j.p++;
            break;
        }
        default:
            break;                          /* APPn, COM and the rest: skipped */
        }
    }
    if (!frame)
        return fail("The JPEG file is damaged.");

    /* Progressive: every coefficient is in now; dequantise and transform. */
    if (j.progressive)
        for (int i = 0; i < j.ncomp; i++) {
            struct comp *c = &j.c[i];
            const uint16_t *q = j.q[c->tq];
            for (int by = 0; by < c->bh; by++, busy())
                for (int bx = 0; bx < c->bw; bx++) {
                    int16_t *data = c->coef + 64 * (by * c->bw + bx);
                    for (int k = 0; k < 64; k++)
                        data[dezigzag[k]] = (int16_t)(data[dezigzag[k]] * q[k]);
                    idct(c->plane + by * 8 * c->w2 + bx * 8, c->w2, data);
                }
        }

    /* Colour: each chroma sample covers h/hmax x v/vmax of the pixels. */
    uint32_t *px = new_pixels(alloc, (int)j.w, (int)j.h);
    if (!px)
        return fail("There isn't enough memory to open it.");
    for (uint32_t y = 0; y < j.h; y++) {
        if (!(y & 63))
            busy();
        uint32_t *o = px + y * j.w;
        const uint8_t *yr = j.c[0].plane + (y * (uint32_t)j.c[0].v / (uint32_t)j.vmax) * (uint32_t)j.c[0].w2;
        if (j.ncomp == 1) {
            for (uint32_t x = 0; x < j.w; x++) {
                uint32_t v = yr[x * (uint32_t)j.c[0].h / (uint32_t)j.hmax];
                o[x] = 0xff000000u | v << 16 | v << 8 | v;
            }
            continue;
        }
        const uint8_t *br = j.c[1].plane + (y * (uint32_t)j.c[1].v / (uint32_t)j.vmax) * (uint32_t)j.c[1].w2;
        const uint8_t *rr = j.c[2].plane + (y * (uint32_t)j.c[2].v / (uint32_t)j.vmax) * (uint32_t)j.c[2].w2;
        for (uint32_t x = 0; x < j.w; x++) {
            int Y = yr[x * (uint32_t)j.c[0].h / (uint32_t)j.hmax];
            int cb = br[x * (uint32_t)j.c[1].h / (uint32_t)j.hmax];
            int cr = rr[x * (uint32_t)j.c[2].h / (uint32_t)j.hmax];
            if (j.adobe_rgb) {
                o[x] = 0xff000000u | (uint32_t)Y << 16 | (uint32_t)cb << 8 | (uint32_t)cr;
                continue;
            }
            cb -= 128;
            cr -= 128;
            int yy = (Y << 16) + 32768;
            int r = (yy + 91881 * cr) >> 16;
            int g = (yy - 22554 * cb - 46802 * cr) >> 16;
            int b = (yy + 116130 * cb) >> 16;
            o[x] = 0xff000000u | (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
        }
    }

    /* The right way up (EXIF orientation 2..8: mirrored and/or turned). */
    int o = j.orientation, w = (int)j.w, h = (int)j.h;
    if (o > 1) {
        int ow = o >= 5 ? h : w, oh = o >= 5 ? w : h;
        uint32_t *t = new_pixels(alloc, ow, oh);
        if (!t)
            return fail("There isn't enough memory to open it.");
        for (int y = 0; y < h; y++, busy())
            for (int x = 0; x < w; x++) {
                int dx, dy;
                switch (o) {
                case 2: dx = w - 1 - x; dy = y; break;
                case 3: dx = w - 1 - x; dy = h - 1 - y; break;
                case 4: dx = x; dy = h - 1 - y; break;
                case 5: dx = y; dy = x; break;
                case 6: dx = h - 1 - y; dy = x; break;
                case 7: dx = h - 1 - y; dy = w - 1 - x; break;
                default: dx = y; dy = w - 1 - x; break;
                }
                t[dy * ow + dx] = px[y * w + x];
            }
        px = t;
        w = ow;
        h = oh;
    }
    out->px = px;
    out->w = w;
    out->h = h;
    out->format = "JPEG";
    return 0;
}

int image_decode(const uint8_t *data, uint32_t len, image_alloc_fn alloc, struct image *out,
                 const char **why)
{
    int r;
    err_why = "This isn't a picture LiteKern X can open (PNG, JPEG or BMP).";
    memset(out, 0, sizeof(*out));
    if (len >= 8 && !memcmp(data, png_sig, 8))
        r = png_decode(data, len, alloc, out);
    else if (len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
        r = jpeg_decode(data, len, alloc, out);
    else if (len >= 54 && data[0] == 'B' && data[1] == 'M')
        r = bmp_decode(data, len, alloc, out);
    else
        r = -1;
    if (r)
        *why = err_why;
    return r;
}
