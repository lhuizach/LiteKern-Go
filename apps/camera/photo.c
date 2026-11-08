/* Camera: from raw sensor data to a picture. The black level comes off, lens
 * shading is evened out, white is balanced, and a little saturation and a gamma of
 * 2 go on. No colour matrix yet. The sensor's mounted sideways, so everything is
 * turned a quarter turn to come out upright on the phone.
 *
 * The preview takes the top 8 bits of each 2x2 Bayer cell; a photo interpolates
 * all 10 bits at every pixel and is saved as a PNG. Whole numbers throughout: this
 * runs on a Cortex-A53, and the preview has a few milliseconds a frame. */
#include "cam.h"
#include "kernel/deflate.h"
#include "sdk/kern86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHADING 230                     /* the brightening at the corners, 8.8 */
#define SATURATION 282                  /* 8.8: 1.10, on top of the matrix */
#define CELL_W (CAM_W / 2)
#define CELL_H (CAM_H / 2)

static uint8_t gamma8[1024];            /* 10-bit linear to 8-bit */
static int gamma_ready;

static void gamma_init(void)
{
    if (gamma_ready)
        return;
    gamma_ready = 1;
    for (uint32_t i = 0, r = 0; i < 1024; i++) {    /* sqrt(i / 1023) * 255 */
        uint32_t want = i * 65025 / 1023;
        while ((r + 1) * (r + 1) <= want)
            r++;
        gamma8[i] = (uint8_t)r;
    }
}

/* 8.8 gain for lens shading at a point, from the middle out. */
static inline int shade_at(int dx, int dy, int rx, int ry)
{
    int fx = dx * 256 / rx, fy = dy * 256 / ry;
    return 256 + (((fx * fx + fy * fy) >> 9) * SHADING >> 8);
}

static inline int clamp10(int v)
{
    return v < 0 ? 0 : v > 1023 ? 1023 : v;
}

/* Colour matrix (8.8, each row adds up to 256 so greys stay grey): the sensor's red, green
 * and blue filters overlap, which makes colours dull; this pulls them apart. A generic
 * matrix, not measured for this sensor. Then saturation, then to 8 bits.
 * r, g, b: 10-bit linear, white balanced. */
static inline void finish(int r0, int g0, int b0, uint8_t *out)
{
    int r = (359 * r0 - 77 * g0 - 26 * b0) >> 8;
    int g = (-51 * r0 + 358 * g0 - 51 * b0) >> 8;
    int b = (-13 * r0 - 90 * g0 + 359 * b0) >> 8;
    int l = (77 * r + 151 * g + 28 * b) >> 8;
    out[0] = gamma8[clamp10(l + ((r - l) * SATURATION >> 8))];
    out[1] = gamma8[clamp10(l + ((g - l) * SATURATION >> 8))];
    out[2] = gamma8[clamp10(l + ((b - l) * SATURATION >> 8))];
}

void photo_balance(const uint8_t *frame, int *red, int *blue)
{
    int64_t sr = 0, sg = 0, sb = 0;
    int stride = cam_stride();
    for (int y = 0; y + 1 < CAM_H; y += 16) {
        const uint8_t *top = frame + (size_t)y * (size_t)stride, *low = top + stride;
        for (int x = 0; x + 1 < CAM_W; x += 16) {
            int r = top[x / 4 * 5 + x % 4] - CAM_BLACK / 4, g = top[x / 4 * 5 + 1] - CAM_BLACK / 4;
            int b = low[x / 4 * 5 + 1] - CAM_BLACK / 4;
            if (g < 4 || g > 190)
                continue;                       /* noise, or blown out */
            sr += r > 0 ? r : 0;
            sg += g;
            sb += b > 0 ? b : 0;
        }
    }
    if (sr > 0 && sb > 0 && sg > 0) {
        *red = (int)(sg * 256 / sr);
        *blue = (int)(sg * 256 / sb);
        *red = *red > 1024 ? 1024 : *red;
        *blue = *blue > 1024 ? 1024 : *blue;
    }
}

void photo_preview(const uint8_t *frame, int red, int blue, uint32_t *out, int w, int h, int stride_px)
{
    static uint16_t *shade;
    int stride = cam_stride();
    gamma_init();
    if (!shade) {
        shade = malloc(sizeof(uint16_t) * CELL_W * CELL_H);
        if (!shade)
            return;
        for (int y = 0; y < CELL_H; y++)
            for (int x = 0; x < CELL_W; x++)
                shade[y * CELL_W + x] = (uint16_t)shade_at(x - CELL_W / 2, y - CELL_H / 2, CELL_W / 2, CELL_H / 2);
    }
    /* A quarter turn clockwise: the sensor's rows run down the screen, right to left. */
    int top_skip = (CELL_W * 2 - h) / 2, left_skip = (CELL_H * 2 - w) / 2;
    for (int dy = 0; dy + 1 < h; dy += 2) {
        int sx = (dy + top_skip) / 2;
        if (sx < 0 || sx >= CELL_W)
            continue;
        uint32_t *line = out + (size_t)dy * (size_t)stride_px;
        for (int dx = 0; dx + 1 < w; dx += 2) {
            int sy = CELL_H - 1 - (dx + left_skip) / 2;
            if (sy < 0 || sy >= CELL_H)
                continue;
            const uint8_t *top = frame + (size_t)sy * 2 * (size_t)stride, *low = top + stride;
            int col = sx * 2, at = col / 4 * 5 + col % 4, s = shade[sy * CELL_W + sx];
            int r = (top[at] - CAM_BLACK / 4) * 4, g = (top[at + 1] - CAM_BLACK / 4) * 4, b = (low[at + 1] - CAM_BLACK / 4) * 4;
            uint8_t px[3];
            r = r < 0 ? 0 : r;
            g = g < 0 ? 0 : g;
            b = b < 0 ? 0 : b;
            finish(((r * s) >> 8) * red >> 8, (g * s) >> 8, ((b * s) >> 8) * blue >> 8, px);
            uint32_t c = (uint32_t)px[0] << 16 | (uint32_t)px[1] << 8 | px[2];
            line[dx] = line[dx + 1] = c;
            line[dx + stride_px] = line[dx + 1 + stride_px] = c;
        }
    }
}

/* --- a photo ------------------------------------------------------------------- */

static uint32_t crc_table[256];

static uint32_t crc_of(const uint8_t *p, size_t n, uint32_t crc)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    for (size_t i = 0; i < n; i++)
        crc = crc_table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return crc;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return p + 4;
}

/* A chunk at p, its data already at p + 8. Returns where the next goes. */
static uint8_t *chunk(uint8_t *p, const char *type, uint32_t len)
{
    put32(p, len);
    memcpy(p + 4, type, 4);
    return put32(p + 8 + len, ~crc_of(p + 4, len + 4, 0xffffffffu));
}

static uint32_t adler_of(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n > 5552 ? 5552 : n;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

/* All 10 bits at every pixel. Green is found first, by looking along the edge rather
 * than across it (Hamilton-Adams: whichever of the row or the column changes less, with
 * a correction from the pixel's own colour); then red and blue are filled in from how far
 * they sit from green at the nearest red and blue pixels, which keeps edges from
 * getting coloured fringes. Turned upright into rows of 3-byte pixels, each row after a
 * PNG filter byte. */
/* A pixel off the edge reads from the other side of the edge, which keeps its colour
 * (the mirror lands on a pixel of the same parity). */
static inline int mir(int v, int n)
{
    return v < 0 ? -v : v >= n ? 2 * (n - 1) - v : v;
}

static void develop(const uint8_t *frame, int red, int blue, uint8_t *out, int row_bytes)
{
    uint16_t *raw = malloc(sizeof(uint16_t) * CAM_W * CAM_H);
    int stride = cam_stride();
    if (!raw)
        return;
    for (int y = 0; y < CAM_H; y++) {
        const uint8_t *p = frame + (size_t)y * (size_t)stride;
        uint16_t *r = raw + y * CAM_W;
        for (int x = 0; x < CAM_W; x += 4, p += 5) {
            int low = p[4];
            for (int k = 0; k < 4; k++) {
                int v = (p[k] << 2 | (low >> (k * 2) & 3)) - CAM_BLACK;
                r[x + k] = (uint16_t)(v < 0 ? 0 : v);
            }
        }
    }
    int16_t *gp = malloc(sizeof(int16_t) * CAM_W * CAM_H);
    if (!gp) {
        free(raw);
        return;
    }
#define RAW(xx, yy) ((int)raw[mir(yy, CAM_H) * CAM_W + mir(xx, CAM_W)])
#define GRN(xx, yy) ((int)gp[mir(yy, CAM_H) * CAM_W + mir(xx, CAM_W)])
    for (int y = 0; y < CAM_H; y++) {
        for (int x = 0; x < CAM_W; x++) {
            int v = RAW(x, y);
            if ((x ^ y) & 1) {                  /* a green pixel already */
                gp[y * CAM_W + x] = (int16_t)v;
                continue;
            }
            int gl = RAW(x - 1, y), gr = RAW(x + 1, y), gu = RAW(x, y - 1), gd = RAW(x, y + 1);
            int dh = abs(gl - gr) + abs(2 * v - RAW(x - 2, y) - RAW(x + 2, y));
            int dv = abs(gu - gd) + abs(2 * v - RAW(x, y - 2) - RAW(x, y + 2));
            int g;
            if (dh < dv)
                g = (gl + gr) / 2 + (2 * v - RAW(x - 2, y) - RAW(x + 2, y)) / 4;
            else if (dv < dh)
                g = (gu + gd) / 2 + (2 * v - RAW(x, y - 2) - RAW(x, y + 2)) / 4;
            else
                g = (gl + gr + gu + gd) / 4 + (4 * v - RAW(x - 2, y) - RAW(x + 2, y) - RAW(x, y - 2) - RAW(x, y + 2)) / 8;
            gp[y * CAM_W + x] = (int16_t)(g < 0 ? 0 : g > 1023 ? 1023 : g);
        }
    }
    for (int y = 0; y < CAM_H; y++) {
        for (int x = 0; x < CAM_W; x++) {
            int g = GRN(x, y), r, b;
            if (!(y & 1) && !(x & 1)) {         /* red: blue from the four corners */
                r = RAW(x, y);
                b = g + ((RAW(x - 1, y - 1) - GRN(x - 1, y - 1) + RAW(x + 1, y - 1) - GRN(x + 1, y - 1) +
                          RAW(x - 1, y + 1) - GRN(x - 1, y + 1) + RAW(x + 1, y + 1) - GRN(x + 1, y + 1)) >> 2);
            } else if ((y & 1) && (x & 1)) {    /* blue: red from the four corners */
                b = RAW(x, y);
                r = g + ((RAW(x - 1, y - 1) - GRN(x - 1, y - 1) + RAW(x + 1, y - 1) - GRN(x + 1, y - 1) +
                          RAW(x - 1, y + 1) - GRN(x - 1, y + 1) + RAW(x + 1, y + 1) - GRN(x + 1, y + 1)) >> 2);
            } else if (!(y & 1)) {              /* green in a red row: red beside, blue above and below */
                r = g + ((RAW(x - 1, y) - GRN(x - 1, y) + RAW(x + 1, y) - GRN(x + 1, y)) >> 1);
                b = g + ((RAW(x, y - 1) - GRN(x, y - 1) + RAW(x, y + 1) - GRN(x, y + 1)) >> 1);
            } else {                            /* green in a blue row */
                b = g + ((RAW(x - 1, y) - GRN(x - 1, y) + RAW(x + 1, y) - GRN(x + 1, y)) >> 1);
                r = g + ((RAW(x, y - 1) - GRN(x, y - 1) + RAW(x, y + 1) - GRN(x, y + 1)) >> 1);
            }
            r = r < 0 ? 0 : r;
            b = b < 0 ? 0 : b;
            int sh = shade_at(x - CAM_W / 2, y - CAM_H / 2, CAM_W / 2, CAM_H / 2);
            uint8_t *px = out + (size_t)x * (size_t)row_bytes + 1 + (size_t)(CAM_H - 1 - y) * 3;
            finish(((r * sh) >> 8) * red >> 8, (g * sh) >> 8, ((b * sh) >> 8) * blue >> 8, px);
        }
    }
#undef RAW
#undef GRN
    free(gp);
    free(raw);
}

static int folder(char *dir)
{
    char home[K86_PATH_MAX], pictures[K86_PATH_MAX + 16];
    int vol = k86_home(home);
    if (vol < 0)
        return vol;
    snprintf(pictures, sizeof(pictures), "%s%sPictures", home, strcmp(home, "/") ? "/" : "");
    k86_create(vol, home, "Pictures", 1);       /* already there: that's fine */
    k86_create(vol, pictures, "Camera", 1);
    snprintf(dir, K86_PATH_MAX, "%.200s/Camera", pictures);
    return vol;
}

#define THUMB_FILE "last-photo.thumb"

/* From the developed picture: its middle, shrunk to THUMB x THUMB. */
static void thumb_save(const uint8_t *rows, int row, int vol, const char *dir)
{
    static uint32_t t[THUMB * THUMB];
    int side = CAM_H, top = (CAM_W - side) / 2;
    for (int y = 0; y < THUMB; y++)
        for (int x = 0; x < THUMB; x++) {
            const uint8_t *p = rows + (size_t)(top + y * side / THUMB) * (size_t)row + 1 + (size_t)(x * side / THUMB) * 3;
            t[y * THUMB + x] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
    k86_write(vol, dir, THUMB_FILE, t, sizeof(t));
}

int photo_thumb(uint32_t *out)
{
    char dir[K86_PATH_MAX];
    int vol = folder(dir);
    return vol >= 0 && k86_read(vol, dir, THUMB_FILE, out, THUMB * THUMB * 4) == THUMB * THUMB * 4;
}

int photo_save(const uint8_t *frame, int red, int blue, char *name)
{
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    int w = CAM_H, h = CAM_W, row = w * 3 + 1, err = -1;
    size_t raw_len = (size_t)row * (size_t)h, room = raw_len + raw_len / 64 + 2048;
    uint8_t *rows = calloc(raw_len, 1), *png = malloc(room), *work = malloc(DEFLATE_WORK);
    char dir[K86_PATH_MAX];
    struct k86_time t;
    gamma_init();
    if (!rows || !png || !work)
        goto out;
    develop(frame, red, blue, rows, row);
    int vol = folder(dir);
    if (vol < 0)
        goto out;
    thumb_save(rows, row, vol, dir);
    /* PNG's "up" filter: each row as the change from the one above squeezes far better. */
    for (int y = h - 1; y > 0; y--) {
        uint8_t *now = rows + (size_t)y * (size_t)row, *above = now - row;
        now[0] = 2;
        for (int x = 1; x < row; x++)
            now[x] = (uint8_t)(now[x] - above[x]);
    }
    uint8_t *p = png;
    memcpy(p, signature, 8);
    p += 8;
    uint8_t *d = p + 8;
    d = put32(d, (uint32_t)w);
    d = put32(d, (uint32_t)h);
    memcpy(d, "\x08\x02\x00\x00\x00", 5);       /* 8 bits, RGB */
    p = chunk(p, "IHDR", 13);
    d = p + 8;
    d[0] = 0x78;
    d[1] = 0x01;
    int packed = deflate_raw(rows, raw_len, d + 2, room - (size_t)(d + 2 - png) - 64, work);
    if (packed < 0)
        goto out;
    put32(d + 2 + packed, adler_of(rows, raw_len));
    p = chunk(p, "IDAT", (uint32_t)packed + 6);
    p = chunk(p, "IEND", 0);

    if (k86_time(&t))
        memset(&t, 0, sizeof(t));
    snprintf(name, 64, "IMG-%04u%02u%02u-%02u%02u%02u.png", t.year, t.month, t.day, t.hour, t.minute, t.second);
    if (k86_write(vol, dir, name, png, (uint32_t)(p - png)) >= 0)
        err = 0;
out:
    free(rows);
    free(png);
    free(work);
    return err;
}
