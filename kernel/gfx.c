#include "kernel/gfx.h"
#include "kernel/font.h"

static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }

int gfx_rect_empty(struct gfx_rect r)
{
    return r.w <= 0 || r.h <= 0;
}

struct gfx_rect gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b)
{
    int x0 = max(a.x, b.x), y0 = max(a.y, b.y);
    int x1 = min(a.x + a.w, b.x + b.w), y1 = min(a.y + a.h, b.y + b.h);
    struct gfx_rect r = { x0, y0, x1 - x0, y1 - y0 };
    if (gfx_rect_empty(r))
        r.w = r.h = 0;
    return r;
}

struct gfx_rect gfx_rect_union(struct gfx_rect a, struct gfx_rect b)
{
    if (gfx_rect_empty(a))
        return b;
    if (gfx_rect_empty(b))
        return a;
    int x0 = min(a.x, b.x), y0 = min(a.y, b.y);
    int x1 = max(a.x + a.w, b.x + b.w), y1 = max(a.y + a.h, b.y + b.h);
    return (struct gfx_rect){ x0, y0, x1 - x0, y1 - y0 };
}

/* The part of (x, y, w, h) inside the surface. */
static struct gfx_rect clip(const struct gfx_surface *s, int x, int y, int w, int h)
{
    return gfx_rect_intersect((struct gfx_rect){ x, y, w, h }, (struct gfx_rect){ 0, 0, s->w, s->h });
}

void gfx_fill_rect(struct gfx_surface *s, int x, int y, int w, int h, uint32_t colour)
{
    struct gfx_rect r = clip(s, x, y, w, h);
    for (int row = 0; row < r.h; row++) {
        uint32_t *p = s->px + (r.y + row) * s->stride + r.x;
        for (int col = 0; col < r.w; col++)
            p[col] = colour;
    }
}

void gfx_rect_outline(struct gfx_surface *s, int x, int y, int w, int h, uint32_t colour)
{
    if (w <= 0 || h <= 0)
        return;
    gfx_fill_rect(s, x, y, w, 1, colour);
    gfx_fill_rect(s, x, y + h - 1, w, 1, colour);
    gfx_fill_rect(s, x, y + 1, 1, h - 2, colour);
    gfx_fill_rect(s, x + w - 1, y + 1, 1, h - 2, colour);
}

static void put(struct gfx_surface *s, int x, int y, uint32_t colour, uint32_t alpha);

void gfx_thick_line(struct gfx_surface *s, int x0, int y0, int x1, int y1, int width, uint32_t colour)
{
    /* In eighths of a pixel: the segment's ends at pixel centres, samples
     * at 2i+1 within each pixel, and the half width as the radius. */
    int64_t ax = 8 * x0 + 4, ay = 8 * y0 + 4, dx = 8 * (x1 - x0), dy = 8 * (y1 - y0);
    int64_t len2 = dx * dx + dy * dy, r2 = (int64_t)(4 * width) * (4 * width);
    int pad = width / 2 + 2;
    int minx = (x0 < x1 ? x0 : x1) - pad, maxx = (x0 > x1 ? x0 : x1) + pad;
    int miny = (y0 < y1 ? y0 : y1) - pad, maxy = (y0 > y1 ? y0 : y1) + pad;
    for (int py = miny; py <= maxy; py++)
        for (int px = minx; px <= maxx; px++) {
            int n = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    int64_t sx = 8 * px + 2 * i + 1 - ax, sy = 8 * py + 2 * j + 1 - ay, ex = sx, ey = sy;
                    if (len2) {
                        int64_t t = sx * dx + sy * dy;
                        t = t < 0 ? 0 : t > len2 ? len2 : t;
                        ex = sx - dx * t / len2;
                        ey = sy - dy * t / len2;
                    }
                    n += ex * ex + ey * ey <= r2;
                }
            if (n)
                put(s, px, py, colour, (uint32_t)(n * 255 / 16));
        }
}

void gfx_line(struct gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t colour)
{
    if (x0 == x1 || y0 == y1) {     /* straight: one clipped fill */
        gfx_fill_rect(s, min(x0, x1), min(y0, y1), (x0 > x1 ? x0 - x1 : x1 - x0) + 1,
                      (y0 > y1 ? y0 - y1 : y1 - y0) + 1, colour);
        return;
    }
    /* Bresenham, clipping per pixel. */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < s->w && y0 < s->h)
            s->px[y0 * s->stride + x0] = colour;
        if (x0 == x1 && y0 == y1)
            return;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Anti-aliased corners (Phase 3 §1): how much of each pixel of a radius-r
 * corner is inside the curve, from 4x4 samples, 0..255. Indexed from the
 * corner's outer edge, so all four corners share it by mirroring. */
#define AA_MAX_R 96                     /* LiteKern Go: 2x sizes, so bigger curves (the PIN pad's keys) */
static uint8_t cov_tab[AA_MAX_R][AA_MAX_R];
static int cov_r = -1;

static void cov_build(int r)
{
    if (r == cov_r)
        return;
    for (int py = 0; py < r; py++)
        for (int px = 0; px < r; px++) {
            int n = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    int dx = 8 * px + 2 * i + 1 - 8 * r, dy = 8 * py + 2 * j + 1 - 8 * r;
                    n += dx * dx + dy * dy <= 64 * r * r;
                }
            cov_tab[py][px] = (uint8_t)(n * 255 / 16);
        }
    cov_r = r;
}

static void put(struct gfx_surface *s, int x, int y, uint32_t colour, uint32_t alpha)
{
    if (!alpha || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return;
    uint32_t *p = s->px + y * s->stride + x;
    *p = alpha >= 255 ? colour : gfx_mix(colour, *p, alpha);
}

enum shape_mode { SHAPE_FILL, SHAPE_BLEND, SHAPE_OUTSIDE };

/* One rounded rectangle: SHAPE_FILL paints colour inside, SHAPE_BLEND blends
 * it in at `alpha`, SHAPE_OUTSIDE paints colour over the corners outside the
 * curve (rounding something already drawn square). Edges are anti-aliased. */
static void round_shape(struct gfx_surface *s, int x, int y, int w, int h, int r, uint32_t colour,
                        uint32_t alpha, enum shape_mode mode)
{
    if (w <= 0 || h <= 0)
        return;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r > AA_MAX_R)
        r = AA_MAX_R;
    cov_build(r);
    for (int row = 0; row < h; row++) {
        int dy = row < r ? row : row >= h - r ? h - 1 - row : -1;
        if (dy < 0) {                       /* a straight row */
            if (mode == SHAPE_OUTSIDE)
                continue;
            if (mode == SHAPE_FILL) {
                gfx_fill_rect(s, x, y + row, w, 1, colour);
            } else {
                struct gfx_rect c = clip(s, x, y + row, w, 1);
                uint32_t *p = s->px + c.y * s->stride;
                for (int col = c.x; col < c.x + c.w; col++)
                    p[col] = gfx_mix(colour, p[col], alpha);
            }
            continue;
        }
        for (int col = 0; col < w; col++) {
            int dx = col < r ? col : col >= w - r ? w - 1 - col : -1;
            uint32_t cov = dx < 0 ? 255 : cov_tab[dy][dx];
            if (mode == SHAPE_OUTSIDE)
                put(s, x + col, y + row, colour, 255 - cov);
            else if (mode == SHAPE_FILL)
                put(s, x + col, y + row, colour, cov);
            else
                put(s, x + col, y + row, colour, cov * alpha / 255);
        }
    }
}

void gfx_fill_round_rect(struct gfx_surface *s, int x, int y, int w, int h, int r, uint32_t colour)
{
    round_shape(s, x, y, w, h, r, colour, 255, SHAPE_FILL);
}

void gfx_fill_circle(struct gfx_surface *s, int cx, int cy, int r, uint32_t colour)
{
    gfx_fill_round_rect(s, cx - r, cy - r, 2 * r, 2 * r, r, colour);
}

void gfx_round_corners(struct gfx_surface *s, int x, int y, int w, int h, int r, uint32_t outside)
{
    round_shape(s, x, y, w, h, r, outside, 255, SHAPE_OUTSIDE);
}

struct gfx_surface gfx_sub(const struct gfx_surface *s, struct gfx_rect r)
{
    r = gfx_rect_intersect(r, (struct gfx_rect){ 0, 0, s->w, s->h });
    if (gfx_rect_empty(r))
        return (struct gfx_surface){ s->px, 0, 0, s->stride };
    return (struct gfx_surface){ s->px + r.y * s->stride + r.x, r.w, r.h, s->stride };
}

void gfx_blit(struct gfx_surface *dst, int dx, int dy,
              const struct gfx_surface *src, int sx, int sy, int w, int h)
{
    /* Clip against the source, then the destination, keeping both in step. */
    struct gfx_rect rs = gfx_rect_intersect((struct gfx_rect){ sx, sy, w, h },
                                            (struct gfx_rect){ 0, 0, src->w, src->h });
    dx += rs.x - sx;
    dy += rs.y - sy;
    struct gfx_rect rd = clip(dst, dx, dy, rs.w, rs.h);
    int ox = rs.x + (rd.x - dx), oy = rs.y + (rd.y - dy);
    for (int row = 0; row < rd.h; row++) {
        uint32_t *d = dst->px + (rd.y + row) * dst->stride + rd.x;
        const uint32_t *p = src->px + (oy + row) * src->stride + ox;
        for (int col = 0; col < rd.w; col++)
            d[col] = p[col];
    }
}

/* (a * alpha + b * (255 - alpha)) / 255 for one 8-bit channel, rounded. */
static uint32_t mix(uint32_t a, uint32_t b, uint32_t alpha)
{
    uint32_t v = a * alpha + b * (255 - alpha) + 128;
    return (v + (v >> 8)) >> 8;
}

uint32_t gfx_mix(uint32_t a, uint32_t b, uint32_t alpha)
{
    return mix(a >> 16 & 0xff, b >> 16 & 0xff, alpha) << 16 |
           mix(a >> 8 & 0xff, b >> 8 & 0xff, alpha) << 8 | mix(a & 0xff, b & 0xff, alpha);
}

void gfx_blend_round_rect(struct gfx_surface *s, int x, int y, int w, int h, int r,
                          uint32_t colour, uint32_t alpha)
{
    round_shape(s, x, y, w, h, r, colour, alpha, SHAPE_BLEND);
}

void gfx_darken(struct gfx_surface *s, int x, int y, int w, int h, uint32_t alpha)
{
    struct gfx_rect r = clip(s, x, y, w, h);
    for (int row = r.y; row < r.y + r.h; row++) {
        uint32_t *p = s->px + row * s->stride;
        for (int col = r.x; col < r.x + r.w; col++)
            p[col] = gfx_mix(0, p[col], alpha);
    }
}

void gfx_blit_alpha(struct gfx_surface *dst, int dx, int dy,
                    const uint32_t *argb, int w, int h, int stride)
{
    struct gfx_rect rd = clip(dst, dx, dy, w, h);
    for (int row = 0; row < rd.h; row++) {
        uint32_t *d = dst->px + (rd.y + row) * dst->stride + rd.x;
        const uint32_t *p = argb + (rd.y - dy + row) * stride + (rd.x - dx);
        for (int col = 0; col < rd.w; col++) {
            uint32_t s = p[col], a = s >> 24;
            if (a == 255) {
                d[col] = s & 0xffffff;
            } else if (a) {
                uint32_t o = d[col];
                d[col] = mix(s >> 16 & 0xff, o >> 16 & 0xff, a) << 16 |
                         mix(s >> 8 & 0xff, o >> 8 & 0xff, a) << 8 |
                         mix(s & 0xff, o & 0xff, a);
            }
        }
    }
}

void gfx_char(struct gfx_surface *s, int x, int y, char c, uint32_t fg, uint32_t bg)
{
    struct gfx_rect r = clip(s, x, y, FONT_W, FONT_H);
    const uint8_t *glyph = font_glyph((uint8_t)c);
    for (int row = r.y; row < r.y + r.h; row++) {
        uint8_t bits = glyph[row - y];
        uint32_t *p = s->px + row * s->stride;
        for (int col = r.x; col < r.x + r.w; col++) {
            if (bits & (0x80 >> (col - x)))
                p[col] = fg;
            else if (bg != GFX_TRANSPARENT)
                p[col] = bg;
        }
    }
}

int gfx_text(struct gfx_surface *s, int x, int y, const char *str, uint32_t fg, uint32_t bg)
{
    int x0 = x;
    for (; *str; str++, x += FONT_W)
        gfx_char(s, x, y, *str, fg, bg);
    return x - x0;
}

int gfx_text_width(const char *str)
{
    int n = 0;
    while (str[n])
        n++;
    return n * FONT_W;
}

/* --- Phase 3 §6: views, scaled copies, shadows -------------------------------- */

struct gfx_view gfx_view_clip(const struct gfx_view *v, struct gfx_rect r)
{
    struct gfx_rect local = gfx_rect_intersect((struct gfx_rect){ r.x - v->ox, r.y - v->oy, r.w, r.h },
                                               (struct gfx_rect){ 0, 0, v->s.w, v->s.h });
    if (gfx_rect_empty(local))
        return (struct gfx_view){ { v->s.px, 0, 0, v->s.stride }, v->ox, v->oy };
    return (struct gfx_view){ gfx_sub(&v->s, local), v->ox + local.x, v->oy + local.y };
}

/* Inside a hard-edged rounded rectangle of w x h (radius r)? */
static int in_round(int x, int y, int w, int h, int r)
{
    int cx = x < r ? r - x : x >= w - r ? x - (w - r - 1) : 0;
    int cy = y < r ? r - y : y >= h - r ? y - (h - r - 1) : 0;
    return !cx || !cy || cx * cx + cy * cy <= r * r;
}

void gfx_blit_scaled(struct gfx_surface *dst, struct gfx_rect d, const struct gfx_surface *src,
                     struct gfx_rect sr, uint32_t alpha, int radius)
{
    if (d.w <= 0 || d.h <= 0 || sr.w <= 0 || sr.h <= 0 || !alpha)
        return;
    struct gfx_rect c = clip(dst, d.x, d.y, d.w, d.h);
    uint32_t step_x = (uint32_t)(((uint64_t)sr.w << 16) / (uint32_t)d.w);
    uint32_t step_y = (uint32_t)(((uint64_t)sr.h << 16) / (uint32_t)d.h);
    if (radius > d.w / 2)
        radius = d.w / 2;
    if (radius > d.h / 2)
        radius = d.h / 2;
    for (int row = c.y; row < c.y + c.h; row++) {
        int ly = row - d.y;
        const uint32_t *sp = src->px + (sr.y + (int)((uint32_t)ly * step_y >> 16)) * src->stride + sr.x;
        uint32_t *dp = dst->px + row * dst->stride;
        int corner_row = ly < radius || ly >= d.h - radius;
        for (int col = c.x; col < c.x + c.w; col++) {
            int lx = col - d.x;
            if (corner_row && !in_round(lx, ly, d.w, d.h, radius))
                continue;
            uint32_t p = sp[(uint32_t)lx * step_x >> 16];
            dp[col] = alpha >= 255 ? p : gfx_mix(p, dp[col], alpha);
        }
    }
}

void gfx_blit_alpha_scaled(struct gfx_surface *dst, struct gfx_rect d, const uint32_t *argb, int w,
                           int h, int stride, uint32_t alpha)
{
    if (d.w <= 0 || d.h <= 0 || !alpha)
        return;
    if (d.w == w && d.h == h && alpha >= 255) {
        gfx_blit_alpha(dst, d.x, d.y, argb, w, h, stride);
        return;
    }
    struct gfx_rect c = clip(dst, d.x, d.y, d.w, d.h);
    if (d.w < w && d.h < h) {
        /* Smaller: each pixel the average of the source pixels it covers
         * (alpha-weighted), so icons stay smooth instead of losing pixels. */
        for (int row = c.y; row < c.y + c.h; row++) {
            int sy0 = (row - d.y) * h / d.h, sy1 = (row - d.y + 1) * h / d.h;
            uint32_t *dp = dst->px + row * dst->stride;
            for (int col = c.x; col < c.x + c.w; col++) {
                int sx0 = (col - d.x) * w / d.w, sx1 = (col - d.x + 1) * w / d.w;
                uint32_t sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
                for (int y = sy0; y < sy1; y++)
                    for (int x = sx0; x < sx1; x++, n++) {
                        uint32_t p = argb[y * stride + x], a = p >> 24;
                        sa += a;
                        sr += (p >> 16 & 0xff) * a;
                        sg += (p >> 8 & 0xff) * a;
                        sb += (p & 0xff) * a;
                    }
                if (!sa)
                    continue;
                uint32_t rgb = (sr / sa) << 16 | (sg / sa) << 8 | sb / sa, a = sa / n * alpha / 255;
                if (a)
                    dp[col] = gfx_mix(rgb, dp[col], a);
            }
        }
        return;
    }
    uint32_t step_x = (uint32_t)(((uint64_t)w << 16) / (uint32_t)d.w);
    uint32_t step_y = (uint32_t)(((uint64_t)h << 16) / (uint32_t)d.h);
    for (int row = c.y; row < c.y + c.h; row++) {
        const uint32_t *sp = argb + (int)((uint32_t)(row - d.y) * step_y >> 16) * stride;
        uint32_t *dp = dst->px + row * dst->stride;
        for (int col = c.x; col < c.x + c.w; col++) {
            uint32_t p = sp[(uint32_t)(col - d.x) * step_x >> 16];
            uint32_t a = (p >> 24) * alpha / 255;
            if (a)
                dp[col] = gfx_mix(p & 0xffffff, dp[col], a);
        }
    }
}

/* Soft shadow: rings of black, each a little bigger and fainter, drawn only
 * outside (x, y, w, h) (the window covers the rest). */
void gfx_shadow(struct gfx_surface *s, int x, int y, int w, int h, int r, int size, uint32_t strength)
{
    static const uint8_t weight[] = { 5, 4, 3, 3, 2, 2, 1, 1 };
    int layers = size < 8 ? size : 8;
    if (layers < 1)
        return;
    int grow = size / layers;
    /* The four bands around the rectangle, as views (local coordinates). */
    int m = size + 6;
    struct gfx_rect bands[4] = {
        { x - m, y - m, w + 2 * m, m + r },             /* above (and the top corners) */
        { x - m, y + h - r, w + 2 * m, m + r },         /* below */
        { x - m, y + r, m, h - 2 * r },                 /* left */
        { x + w, y + r, m, h - 2 * r },                 /* right */
    };
    for (int b = 0; b < 4; b++) {
        struct gfx_rect lr = gfx_rect_intersect(bands[b], (struct gfx_rect){ 0, 0, s->w, s->h });
        if (gfx_rect_empty(lr))
            continue;
        struct gfx_surface sub = gfx_sub(s, lr);
        for (int i = layers - 1; i >= 0; i--) {
            int g = (i + 1) * grow;
            uint32_t a = weight[i] * strength / 16;
            if (!a)
                continue;
            round_shape(&sub, x - g - lr.x, y - g + g / 3 - lr.y, w + 2 * g, h + 2 * g, r + g, 0, a,
                        SHAPE_BLEND);
        }
    }
}

void gfx_blend(struct gfx_surface *dst, int dx, int dy,
               const struct gfx_surface *src, int sx, int sy, int w, int h, uint32_t alpha)
{
    if (alpha >= 255) {
        gfx_blit(dst, dx, dy, src, sx, sy, w, h);
        return;
    }
    if (!alpha)
        return;
    struct gfx_rect rs = clip(src, sx, sy, w, h);       /* the source, then the destination */
    dx += rs.x - sx, dy += rs.y - sy;
    struct gfx_rect rd = clip(dst, dx, dy, rs.w, rs.h);
    for (int row = 0; row < rd.h; row++) {
        uint32_t *d = dst->px + (rd.y + row) * dst->stride + rd.x;
        const uint32_t *p = src->px + (rs.y + rd.y - dy + row) * src->stride + rs.x + (rd.x - dx);
        for (int col = 0; col < rd.w; col++) {
            uint32_t s = p[col], o = d[col];
            d[col] = mix(s >> 16 & 0xff, o >> 16 & 0xff, alpha) << 16 |
                     mix(s >> 8 & 0xff, o >> 8 & 0xff, alpha) << 8 | mix(s & 0xff, o & 0xff, alpha);
        }
    }
}

/* One box-blur pass along a line of n pixels, step apart, from `in` to `out`
 * (the edges repeat their end pixels). */
static void blur_line(const uint32_t *in, uint32_t *out, int n, int step, int r)
{
    int div = 2 * r + 1;
    uint32_t sr = 0, sg = 0, sb = 0;
    for (int i = -r; i <= r; i++) {
        uint32_t p = in[(i < 0 ? 0 : i >= n ? n - 1 : i) * step];
        sr += p >> 16 & 0xff, sg += p >> 8 & 0xff, sb += p & 0xff;
    }
    for (int i = 0; i < n; i++) {
        out[i * step] = (sr / div) << 16 | (sg / div) << 8 | sb / div;
        int add = i + r + 1, sub = i - r;
        uint32_t a = in[(add >= n ? n - 1 : add) * step], s = in[(sub < 0 ? 0 : sub) * step];
        sr += (a >> 16 & 0xff) - (s >> 16 & 0xff);
        sg += (a >> 8 & 0xff) - (s >> 8 & 0xff);
        sb += (a & 0xff) - (s & 0xff);
    }
}

void gfx_blur(struct gfx_surface *s, int radius, uint32_t *tmp)
{
    int qw = (s->w + 3) / 4, qh = (s->h + 3) / 4;
    uint32_t *a = tmp, *b = tmp + qw * qh;
    /* A quarter the size: each pixel the average of its 4x4. */
    for (int y = 0; y < qh; y++)
        for (int x = 0; x < qw; x++) {
            uint32_t r = 0, g = 0, bl = 0, n = 0;
            for (int yy = y * 4; yy < y * 4 + 4 && yy < s->h; yy++) {
                const uint32_t *p = s->px + yy * s->stride;
                for (int xx = x * 4; xx < x * 4 + 4 && xx < s->w; xx++, n++)
                    r += p[xx] >> 16 & 0xff, g += p[xx] >> 8 & 0xff, bl += p[xx] & 0xff;
            }
            a[y * qw + x] = (r / n) << 16 | (g / n) << 8 | bl / n;
        }
    /* Three passes each way. */
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < qh; y++)
            blur_line(a + y * qw, b + y * qw, qw, 1, radius);
        for (int x = 0; x < qw; x++)
            blur_line(b + x, a + x, qh, qw, radius);
    }
    /* Back up to full size, each pixel between its four nearest (bilinear). */
    for (int y = 0; y < s->h; y++) {
        int fy = (y * 2 + 1) * 256 / 8 - 128, y0 = fy < 0 ? 0 : fy >> 8, wy = fy < 0 ? 0 : fy & 255;
        int y1 = y0 + 1 < qh ? y0 + 1 : qh - 1;
        const uint32_t *r0 = a + y0 * qw, *r1 = a + y1 * qw;
        uint32_t *d = s->px + y * s->stride;
        for (int x = 0; x < s->w; x++) {
            int fx = (x * 2 + 1) * 256 / 8 - 128, x0 = fx < 0 ? 0 : fx >> 8, wx = fx < 0 ? 0 : fx & 255;
            int x1 = x0 + 1 < qw ? x0 + 1 : qw - 1;
            uint32_t p00 = r0[x0], p01 = r0[x1], p10 = r1[x0], p11 = r1[x1], out = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                int c00 = p00 >> sh & 0xff, c01 = p01 >> sh & 0xff, c10 = p10 >> sh & 0xff, c11 = p11 >> sh & 0xff;
                int top = c00 * 256 + (c01 - c00) * wx, bot = c10 * 256 + (c11 - c10) * wx;
                int c = (top * 256 + (bot - top) * wy) >> 16;
                out |= (uint32_t)c << sh;
            }
            d[x] = out;
        }
    }
}

static uint32_t isqrt32(uint32_t n)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

void gfx_glass(struct gfx_surface *dst, int x, int y, int w, int h, int r, const struct gfx_surface *frost,
               int fx, int fy, uint32_t tint, uint32_t tint_a, uint32_t alpha)
{
    if (w <= 0 || h <= 0 || !alpha)
        return;
    if (r > h / 2)
        r = h / 2;
    if (r > w / 2)
        r = w / 2;
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= dst->h)
            continue;
        /* How far this row's edge is in from the side, in 1/256 px. */
        int in = 0, dy = row < r ? r - row - 1 : row >= h - r ? row - (h - r) : -1;
        if (dy >= 0) {
            uint32_t d = (uint32_t)((2 * dy + 1) * (2 * dy + 1)), big = (uint32_t)(4 * r * r);
            in = r * 256 - (int)(isqrt32((big - d) << 12) * 2);
        }
        int ipx = in >> 8, edge = 256 - (in & 255);
        uint32_t *d = dst->px + sy * dst->stride;
        const uint32_t *sp = frost->px + (fy + row < 0 ? 0 : fy + row >= frost->h ? frost->h - 1 : fy + row) * frost->stride;
        for (int col = ipx; col < w - ipx; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= dst->w)
                continue;
            int fcol = fx + col;
            uint32_t p = sp[fcol < 0 ? 0 : fcol >= frost->w ? frost->w - 1 : fcol];
            p = mix(tint >> 16 & 0xff, p >> 16 & 0xff, tint_a) << 16 |
                mix(tint >> 8 & 0xff, p >> 8 & 0xff, tint_a) << 8 | mix(tint & 0xff, p & 0xff, tint_a);
            /* The rim: light from above, a little down the sides. */
            uint32_t lit = 0;
            if (row == 0 || (dy >= 0 && row < r && col <= ipx))
                lit = 120;
            else if (row == 1)
                lit = 64;
            if (col - ipx <= 1 || w - ipx - 1 - col <= 1) {
                uint32_t side = (uint32_t)(76 * (h - row) / h) + 16;
                lit = lit > side ? lit : side;
            }
            if (lit)
                p = mix(255, p >> 16 & 0xff, lit) << 16 | mix(255, p >> 8 & 0xff, lit) << 8 | mix(255, p & 0xff, lit);
            if (row == h - 1)
                p = mix(0, p >> 16 & 0xff, 44) << 16 | mix(0, p >> 8 & 0xff, 44) << 8 | mix(0, p & 0xff, 44);
            uint32_t a = alpha;
            if (col == ipx || col == w - ipx - 1)
                a = a * (uint32_t)edge / 256;
            uint32_t o = d[sx];
            d[sx] = a >= 255 ? p : mix(p >> 16 & 0xff, o >> 16 & 0xff, a) << 16 |
                                   mix(p >> 8 & 0xff, o >> 8 & 0xff, a) << 8 | mix(p & 0xff, o & 0xff, a);
        }
    }
}
