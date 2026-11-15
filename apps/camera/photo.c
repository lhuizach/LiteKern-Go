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

#include <pthread.h>
#include <sys/resource.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    r0 = clamp10(r0);                   /* a channel past full scale is just full: left alone, the */
    g0 = clamp10(g0);                   /* matrix would take it off the others and tint the highlights */
    b0 = clamp10(b0);
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

/* The preview: one screen pixel for each sensor pixel (the screen is 720 wide, the
 * sensor 720 high once turned). The top 8 bits of every pixel go into a plane, then each
 * pixel's two missing colours come from a 5x5 window (Malvar-He-Cutler: a neighbour
 * average corrected by the pixel's own colour's curvature), which keeps edges free of
 * the coloured fringes a 2x2 cell gives.
 *
 * That's a lot for 30 frames a second on a Cortex-A53, so: the colour matrix and the
 * saturation are one matrix, worked out once; the screen is done in tiles small enough
 * for the sensor rows they need to stay in the cache (the picture's turned, so a screen
 * row is a sensor column); and the four cores take a quarter of the screen each. */
#define THREADS 4
#define TILE_ROWS 64                    /* screen rows: 64 sensor columns, one cache line */
#define TILE_COLS 120                   /* screen columns: 124 sensor rows, about 16 KB */

static struct {
    const uint8_t *frame;
    uint8_t *p8;
    const uint16_t *shade;
    uint32_t *out;
    int w, h, stride_px, red, blue;
    int k[9];                           /* colour matrix then saturation, 8.8 */
} pv;

/* The zoom: a middle part of the sensor's picture fills the screen. The screen's rows are the sensor's
 * columns and the other way (it's a quarter turn), and zoom 1 shows all of it, 1 to 1. The map from a
 * screen row to a sensor column (xrow) and a screen column to a sensor row (ycol) is made when the
 * size or the zoom changes. */
static int zoom_now = 1, map_w, map_h, map_zoom;
static int xrow[1400], ycol[800];

static int fdiv(int a, int b)           /* a / b, rounded down (b > 0) */
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

static void build_map(int w, int h)
{
    int z = zoom_now;
    for (int dy = 0; dy < h && dy < 1400; dy++)
        xrow[dy] = CAM_W / 2 + fdiv(2 * dy - h, 2 * z);
    for (int dx = 0; dx < w && dx < 800; dx++)
        ycol[dx] = CAM_H / 2 - 1 - fdiv(2 * dx - w, 2 * z);
    map_w = w;
    map_h = h;
    map_zoom = z;
}

void photo_zoom_set(int z)
{
    zoom_now = z < 1 ? 1 : z > 4 ? 4 : z;
}

/* Where a point of the screen is on the sensor (for focusing). */
void photo_screen_to_sensor(int sx, int sy, int *x, int *y)
{
    sx = sx < 0 ? 0 : sx >= map_w ? map_w - 1 : sx;
    sy = sy < 0 ? 0 : sy >= map_h ? map_h - 1 : sy;
    *x = map_w ? xrow[sy] : CAM_W / 2;
    *y = map_w ? ycol[sx] : CAM_H / 2;
}

static void preview_matrix(void)
{
    static const int ccm[9] = { 359, -77, -26, -51, 358, -51, -13, -90, 359 };
    static const int luma[3] = { 77, 151, 28 };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            long v = 0;
            for (int m = 0; m < 3; m++) {   /* saturation: S on the diagonal, (1 - S) of the luma */
                long sat = (i == m ? (long)SATURATION * 256 : 0) + (256 - SATURATION) * (long)luma[m];
                v += sat * ccm[m * 3 + j];
            }
            pv.k[i * 3 + j] = (int)(v >> 16);
        }
}

static void unpack_rows(int y0, int y1)
{
    int stride = cam_stride();
    for (int y = y0; y < y1; y++) {
        const uint8_t *src = pv.frame + (size_t)y * (size_t)stride;
        uint8_t *dst = pv.p8 + (size_t)y * CAM_W;
        for (int x = 0; x < CAM_W; x += 4, src += 5) {
            dst[x] = src[0] > 16 ? src[0] - 16 : 0;     /* black level, in 8 bits */
            dst[x + 1] = src[1] > 16 ? src[1] - 16 : 0;
            dst[x + 2] = src[2] > 16 ? src[2] - 16 : 0;
            dst[x + 3] = src[3] > 16 ? src[3] - 16 : 0;
        }
    }
}

static void preview_tile(int dy0, int dy1, int dx0, int dx1)
{
    const int *k = pv.k;
    for (int dy = dy0; dy < dy1; dy++) {
        int x = xrow[dy];
        if (x < 0 || x >= CAM_W)
            continue;
        int cx = x < 2 ? 2 : x > CAM_W - 3 ? CAM_W - 3 : x;
        uint32_t *line = pv.out + (size_t)dy * (size_t)pv.stride_px;
        const uint16_t *shade_col = pv.shade + x / 2;
        for (int dx = dx0; dx < dx1; dx++) {
            int y = ycol[dx];
            if (y < 0 || y >= CAM_H)
                continue;
            int cy = y < 2 ? 2 : y > CAM_H - 3 ? CAM_H - 3 : y;
            const uint8_t *c = pv.p8 + (size_t)cy * CAM_W + cx;
            int v = c[0], s1 = c[-1] + c[1], s2 = c[-CAM_W] + c[CAM_W];
            int h2 = c[-2] + c[2], v2 = c[-2 * CAM_W] + c[2 * CAM_W];
            int d4 = c[-CAM_W - 1] + c[-CAM_W + 1] + c[CAM_W - 1] + c[CAM_W + 1];
            int r, g, b;
            if (!((cx ^ cy) & 1)) {             /* a red or blue pixel */
                g = (4 * v + 2 * (s1 + s2) - h2 - v2) >> 3;
                int o = (12 * v + 4 * d4 - 3 * (h2 + v2)) >> 4;   /* the other colour */
                if (cy & 1) {
                    r = o;
                    b = v;
                } else {
                    r = v;
                    b = o;
                }
            } else {
                int along = (10 * v + 8 * s1 - 2 * h2 - 2 * d4 + v2) >> 4;     /* colour in this row */
                int across = (10 * v + 8 * s2 - 2 * v2 - 2 * d4 + h2) >> 4;    /* colour in the column */
                g = v;
                if (cy & 1) {
                    b = along;
                    r = across;
                } else {
                    r = along;
                    b = across;
                }
            }
            /* Shading and white balance, to 10 bits; past full scale is just full (left
             * alone, the matrix would take it off the others and tint the highlights). */
            int sh = shade_col[(y >> 1) * CELL_W];
            r = r < 0 ? 0 : (r * sh * pv.red) >> 14;
            g = g < 0 ? 0 : (g * sh) >> 6;
            b = b < 0 ? 0 : (b * sh * pv.blue) >> 14;
            r = r > 1023 ? 1023 : r;
            g = g > 1023 ? 1023 : g;
            b = b > 1023 ? 1023 : b;
            int R = (k[0] * r + k[1] * g + k[2] * b) >> 8;
            int G = (k[3] * r + k[4] * g + k[5] * b) >> 8;
            int B = (k[6] * r + k[7] * g + k[8] * b) >> 8;
            line[dx] = (uint32_t)gamma8[clamp10(R)] << 16 | (uint32_t)gamma8[clamp10(G)] << 8 | gamma8[clamp10(B)];
        }
    }
}

static void *preview_part(void *arg)
{
    int part = (int)(intptr_t)arg;
    int dy0 = pv.h * part / THREADS, dy1 = pv.h * (part + 1) / THREADS;
    for (int ty = dy0; ty < dy1; ty += TILE_ROWS)
        for (int tx = 0; tx < pv.w; tx += TILE_COLS)
            preview_tile(ty, ty + TILE_ROWS < dy1 ? ty + TILE_ROWS : dy1, tx, tx + TILE_COLS < pv.w ? tx + TILE_COLS : pv.w);
    return 0;
}

static void *unpack_part(void *arg)
{
    int part = (int)(intptr_t)arg;
    unpack_rows(CAM_H * part / THREADS, CAM_H * (part + 1) / THREADS);
    return 0;
}

/* Runs fn on each quarter, three on threads of their own and one here. */
static void on_all_cores(void *(*fn)(void *))
{
    pthread_t t[THREADS];
    int started[THREADS] = { 0 };
    for (int i = 1; i < THREADS; i++)
        started[i] = pthread_create(&t[i], 0, fn, (void *)(intptr_t)i) == 0;
    fn((void *)(intptr_t)0);
    for (int i = 1; i < THREADS; i++) {
        if (started[i])
            pthread_join(t[i], 0);
        else
            fn((void *)(intptr_t)i);        /* no thread: done here instead */
    }
}

void photo_preview(const uint8_t *frame, int red, int blue, uint32_t *out, int w, int h, int stride_px)
{
    static uint16_t *shade;
    static uint8_t *p8;
    gamma_init();
    if (!shade) {
        shade = malloc(sizeof(uint16_t) * CELL_W * CELL_H);
        if (!shade)
            return;
        for (int y = 0; y < CELL_H; y++)
            for (int x = 0; x < CELL_W; x++)
                shade[y * CELL_W + x] = (uint16_t)shade_at(x - CELL_W / 2, y - CELL_H / 2, CELL_W / 2, CELL_H / 2);
        preview_matrix();
    }
    if (!p8)
        p8 = malloc((size_t)CAM_W * CAM_H);
    if (!p8)
        return;
    pv.frame = frame;
    pv.p8 = p8;
    pv.shade = shade;
    pv.out = out;
    pv.w = w;
    pv.h = h;
    pv.stride_px = stride_px;
    if (map_w != w || map_h != h || map_zoom != zoom_now)
        build_map(w, h);
    pv.red = red;
    pv.blue = blue;
    on_all_cores(unpack_part);
    on_all_cores(preview_part);
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

/* All 10 bits at every pixel, of a frame of any size. Green is found first, by looking along
 * the edge rather than across it (Hamilton-Adams: whichever of the row or the column
 * changes less, with a correction from the pixel's own colour); then red and blue are filled
 * in from how far they sit from green at the nearest red and blue pixels, which keeps edges
 * from getting coloured fringes. Turned upright into rows of 3-byte pixels, each row after a
 * PNG filter byte. Three passes, each on all four cores, a quarter of the rows apiece. */
/* A pixel off the edge reads from the other side of the edge, which keeps its colour
 * (the mirror lands on a pixel of the same parity). */
static inline int mir(int v, int n)
{
    return v < 0 ? -v : v >= n ? 2 * (n - 1) - v : v;
}

static struct {
    const uint8_t *frame;
    int w, h, stride, red, blue, row_bytes;
    int x0, y0, fw, fh;                 /* the window taken from the frame: its corner, and the frame's size */
    uint16_t *raw;
    int16_t *gp;
    int *fx2;                           /* the lens shading's x part, per column */
    uint8_t *out;
} dv;

#define RAW(xx, yy) ((int)dv.raw[mir(yy, dv.h) * dv.w + mir(xx, dv.w)])
#define GRN(xx, yy) ((int)dv.gp[mir(yy, dv.h) * dv.w + mir(xx, dv.w)])

static void *dev_unpack(void *arg)
{
    int part = (int)(intptr_t)arg, y0 = dv.h * part / THREADS, y1 = dv.h * (part + 1) / THREADS;
    for (int y = y0; y < y1; y++) {
        const uint8_t *p = dv.frame + (size_t)(dv.y0 + y) * (size_t)dv.stride + (size_t)(dv.x0 / 4) * 5;
        uint16_t *r = dv.raw + (size_t)y * (size_t)dv.w;
        for (int x = 0; x < dv.w; x += 4, p += 5) {
            int low = p[4];
            for (int k = 0; k < 4; k++) {
                int v = (p[k] << 2 | (low >> (k * 2) & 3)) - CAM_BLACK;
                r[x + k] = (uint16_t)(v < 0 ? 0 : v);
            }
        }
    }
    return 0;
}

static void *dev_green(void *arg)
{
    int part = (int)(intptr_t)arg, y0 = dv.h * part / THREADS, y1 = dv.h * (part + 1) / THREADS;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < dv.w; x++) {
            int v = RAW(x, y);
            if ((x ^ y) & 1) {                  /* a green pixel already */
                dv.gp[(size_t)y * (size_t)dv.w + x] = (int16_t)v;
                continue;
            }
            int gl = RAW(x - 1, y), gr = RAW(x + 1, y), gu = RAW(x, y - 1), gd = RAW(x, y + 1);
            int dh = abs(gl - gr) + abs(2 * v - RAW(x - 2, y) - RAW(x + 2, y));
            int dvv = abs(gu - gd) + abs(2 * v - RAW(x, y - 2) - RAW(x, y + 2));
            int g;
            if (dh < dvv)
                g = (gl + gr) / 2 + (2 * v - RAW(x - 2, y) - RAW(x + 2, y)) / 4;
            else if (dvv < dh)
                g = (gu + gd) / 2 + (2 * v - RAW(x, y - 2) - RAW(x, y + 2)) / 4;
            else
                g = (gl + gr + gu + gd) / 4 + (4 * v - RAW(x - 2, y) - RAW(x + 2, y) - RAW(x, y - 2) - RAW(x, y + 2)) / 8;
            dv.gp[(size_t)y * (size_t)dv.w + x] = (int16_t)(g < 0 ? 0 : g > 1023 ? 1023 : g);
        }
    }
    return 0;
}

static void *dev_colour(void *arg)
{
    int part = (int)(intptr_t)arg, y0 = dv.h * part / THREADS, y1 = dv.h * (part + 1) / THREADS;
    for (int y = y0; y < y1; y++) {
        int fy = (dv.y0 + y - dv.fh / 2) * 256 / (dv.fh / 2), fy2 = fy * fy;
        for (int x = 0; x < dv.w; x++) {
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
            int sh = 256 + ((((dv.fx2[x] + fy2) >> 9) * SHADING) >> 8);
            uint8_t *px = dv.out + (size_t)x * (size_t)dv.row_bytes + 1 + (size_t)(dv.h - 1 - y) * 3;
            finish(((r * sh) >> 8) * dv.red >> 8, (g * sh) >> 8, ((b * sh) >> 8) * dv.blue >> 8, px);
        }
    }
    return 0;
}
#undef RAW
#undef GRN

static int develop(const uint8_t *frame, int fw, int fh, int x0, int y0, int w, int h, int stride, int red, int blue,
                   uint8_t *out, int row_bytes)
{
    dv.fw = fw;
    dv.fh = fh;
    dv.x0 = x0;
    dv.y0 = y0;
    dv.frame = frame;
    dv.w = w;
    dv.h = h;
    dv.stride = stride;
    dv.red = red;
    dv.blue = blue;
    dv.out = out;
    dv.row_bytes = row_bytes;
    dv.raw = malloc(sizeof(uint16_t) * (size_t)w * (size_t)h);
    dv.gp = malloc(sizeof(int16_t) * (size_t)w * (size_t)h);
    dv.fx2 = malloc(sizeof(int) * (size_t)w);
    if (!dv.raw || !dv.gp || !dv.fx2) {
        free(dv.raw);
        free(dv.gp);
        free(dv.fx2);
        return -1;
    }
    for (int x = 0; x < w; x++) {
        int fx = (x0 + x - fw / 2) * 256 / (fw / 2);
        dv.fx2[x] = fx * fx;
    }
    on_all_cores(dev_unpack);
    on_all_cores(dev_green);
    on_all_cores(dev_colour);
    free(dv.raw);
    free(dv.gp);
    free(dv.fx2);
    return 0;
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

int photo_thumb(uint32_t *out)
{
    char dir[K86_PATH_MAX];
    int vol = folder(dir);
    return vol >= 0 && k86_read(vol, dir, THUMB_FILE, out, THUMB * THUMB * 4) == THUMB * THUMB * 4;
}

/* --- saving: the work in a thread of its own, the saving on the app's thread ------------------
 *
 * Turning a frame into a PNG takes a couple of seconds, so it's done in a background thread
 * (at a lower priority, so the live picture keeps going), on all four cores for each step. The
 * app's own thread has to be the one to talk to the shell (one socket, no locking), so the thread
 * only makes the picture; photo_encode_finish() does the saving. */

struct photo_job {
    pthread_t th;
    int started;
    volatile int done;
    uint8_t *frame;                     /* ours: freed when the job is */
    int fw, fh, fstride, red, blue, zoom;
    uint8_t *png;
    size_t png_len;
    uint32_t thumb[THUMB * THUMB];
    int err;
    uint32_t ms_develop, ms_filter, ms_deflate, ms_total;
};

/* From the developed picture (pw wide, ph tall): its middle square, shrunk to THUMB x THUMB. */
static void thumb_make(const uint8_t *rows, int row, int pw, int ph, uint32_t *t)
{
    int side = pw, top = (ph - side) / 2;
    for (int y = 0; y < THUMB; y++)
        for (int x = 0; x < THUMB; x++) {
            const uint8_t *p = rows + (size_t)(top + y * side / THUMB) * (size_t)row + 1 + (size_t)(x * side / THUMB) * 3;
            t[y * THUMB + x] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
}

/* PNG's "up" filter, on four cores: each row as the change from the one above squeezes far better.
 * Into a second buffer, so no thread reads a row another has already changed. */
static struct {
    const uint8_t *rows;
    uint8_t *filt;
    int row, h;
} fl;

static void *filter_part(void *arg)
{
    int part = (int)(intptr_t)arg, y0 = fl.h * part / THREADS, y1 = fl.h * (part + 1) / THREADS;
    for (int y = y0; y < y1; y++) {
        const uint8_t *src = fl.rows + (size_t)y * (size_t)fl.row;
        uint8_t *dst = fl.filt + (size_t)y * (size_t)fl.row;
        if (!y) {
            memcpy(dst, src, (size_t)fl.row);        /* the first row: filter 0, as it is */
            dst[0] = 0;
            continue;
        }
        dst[0] = 2;
        const uint8_t *above = src - fl.row;
        for (int x = 1; x < fl.row; x++)
            dst[x] = (uint8_t)(src[x] - above[x]);
    }
    return 0;
}

/* Compression, on four cores: the rows in four bands, each compressed on its own and joined. */
static struct {
    const uint8_t *in[THREADS];
    size_t len[THREADS];
    uint8_t *out[THREADS];
    size_t cap[THREADS];
    uint8_t *work[THREADS];
    int packed[THREADS];
} cp;

static void *compress_part(void *arg)
{
    int k = (int)(intptr_t)arg;
    cp.packed[k] = deflate_part(cp.in[k], cp.len[k], cp.out[k], cp.cap[k], cp.work[k], k == THREADS - 1, 8);
    return 0;
}

/* The encode thread must not talk to the shell (the app has one socket, and its own thread uses it): a
 * call from here at the same moment as the main thread's would mix the two up, and the app would end
 * (libk86's broken()). So its clock is Linux's own. */
static uint32_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

static void *encode_thread(void *arg)
{
    struct photo_job *j = arg;
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    /* A zoom takes the middle of the frame (a window of a whole number of Bayer groups: 4 pixels across,
     * 2 down), and the picture is that, as it is. */
    int cw = (j->fw / j->zoom) & ~3, ch = (j->fh / j->zoom) & ~1;
    int x0 = ((j->fw - cw) / 2) & ~3, y0 = ((j->fh - ch) / 2) & ~1;
    int w = ch, h = cw, row = w * 3 + 1;           /* turned a quarter: the sensor's rows are the picture's columns */
    size_t raw_len = (size_t)row * (size_t)h;
    uint8_t *rows = calloc(raw_len, 1), *filt = malloc(raw_len), *png = 0;
    uint32_t t0 = ms_now(), t1, t2;
    j->err = -1;
    setpriority(PRIO_PROCESS, 0, 10);               /* this thread only: the live picture first */
    for (int k = 0; k < THREADS; k++) {
        cp.out[k] = 0;
        cp.work[k] = 0;
    }
    if (!rows || !filt)
        goto out;
    if (develop(j->frame, j->fw, j->fh, x0, y0, cw, ch, j->fstride, j->red, j->blue, rows, row))
        goto out;
    t1 = ms_now();
    j->ms_develop = t1 - t0;
    thumb_make(rows, row, w, h, j->thumb);
    fl.rows = rows;
    fl.filt = filt;
    fl.row = row;
    fl.h = h;
    on_all_cores(filter_part);
    t2 = ms_now();
    j->ms_filter = t2 - t1;
    for (int k = 0; k < THREADS; k++) {
        int y0 = h * k / THREADS, y1 = h * (k + 1) / THREADS;
        cp.in[k] = filt + (size_t)y0 * (size_t)row;
        cp.len[k] = (size_t)(y1 - y0) * (size_t)row;
        cp.cap[k] = cp.len[k] + cp.len[k] / 64 + 2048;
        cp.out[k] = malloc(cp.cap[k]);
        cp.work[k] = malloc(DEFLATE_WORK);
        if (!cp.out[k] || !cp.work[k])
            goto out;
    }
    on_all_cores(compress_part);
    size_t packed = 0;
    for (int k = 0; k < THREADS; k++) {
        if (cp.packed[k] < 0)
            goto out;
        packed += (size_t)cp.packed[k];
    }
    j->ms_deflate = ms_now() - t2;
    png = malloc(8 + 25 + 12 + 2 + packed + 4 + 12);
    if (!png)
        goto out;
    uint8_t *p = png;
    memcpy(p, signature, 8);
    p += 8;
    uint8_t *d = p + 8;
    d = put32(d, (uint32_t)w);
    d = put32(d, (uint32_t)h);
    memcpy(d, "\x08\x02\x00\x00\x00", 5);       /* 8 bits, RGB */
    p = chunk(p, "IHDR", 13);
    d = p + 8;
    *d++ = 0x78;
    *d++ = 0x01;
    for (int k = 0; k < THREADS; k++) {
        memcpy(d, cp.out[k], (size_t)cp.packed[k]);
        d += cp.packed[k];
    }
    put32(d, adler_of(filt, raw_len));
    p = chunk(p, "IDAT", (uint32_t)(packed + 6));
    p = chunk(p, "IEND", 0);
    j->png = png;
    j->png_len = (size_t)(p - png);
    png = 0;
    j->err = 0;
out:
    free(rows);
    free(filt);
    free(png);
    for (int k = 0; k < THREADS; k++) {
        free(cp.out[k]);
        free(cp.work[k]);
    }
    j->ms_total = ms_now() - t0;
    j->done = 1;
    return 0;
}

struct photo_job *photo_encode_start(uint8_t *frame, int w, int h, int stride, int red, int blue, int zoom)
{
    struct photo_job *j = calloc(1, sizeof(*j));
    if (!j)
        return 0;
    gamma_init();
    j->frame = frame;
    j->fw = w;
    j->fh = h;
    j->fstride = stride;
    j->red = red;
    j->blue = blue;
    j->zoom = zoom < 1 ? 1 : zoom > 4 ? 4 : zoom;
    if (pthread_create(&j->th, 0, encode_thread, j) == 0) {
        j->started = 1;
    } else {
        encode_thread(j);                          /* no thread to be had: done here, then */
    }
    return j;
}

int photo_encode_done(const struct photo_job *j)
{
    return j->done;
}

int photo_encode_finish(struct photo_job *j, char *name)
{
    char dir[K86_PATH_MAX];
    struct k86_time t;
    int err = -1;
    k86_logf("camera: finishing a photo (zoom %d, err %d, %u bytes)", (uint32_t)j->zoom, (uint32_t)j->err, (uint32_t)j->png_len);
    if (j->started)
        pthread_join(j->th, 0);
    if (!j->err) {
        uint32_t w0 = k86_uptime_ms();
        int vol = folder(dir);
        if (vol >= 0) {
            k86_write(vol, dir, THUMB_FILE, j->thumb, sizeof(j->thumb));
            k86_log("camera: the thumbnail is written");
            if (k86_time(&t))
                memset(&t, 0, sizeof(t));
            snprintf(name, 64, "IMG-%04u%02u%02u-%02u%02u%02u.png", t.year, t.month, t.day, t.hour, t.minute, t.second);
            if (k86_write(vol, dir, name, j->png, (uint32_t)j->png_len) >= 0)
                err = 0;
        }
        k86_logf("camera: encoded in %u ms (develop %u, filter %u, compress %u), %u bytes, written in %u ms", j->ms_total,
                 j->ms_develop, j->ms_filter, j->ms_deflate, (uint32_t)j->png_len, k86_uptime_ms() - w0);
    }
    free(j->frame);
    free(j->png);
    free(j);
    return err;
}

int photo_save(const uint8_t *frame, int fw, int fh, int fstride, int red, int blue, char *name)
{
    size_t len = (size_t)fstride * (size_t)fh;
    uint8_t *copy = malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, frame, len);
    struct photo_job *j = photo_encode_start(copy, fw, fh, fstride, red, blue, 1);
    return j ? photo_encode_finish(j, name) : -1;
}
