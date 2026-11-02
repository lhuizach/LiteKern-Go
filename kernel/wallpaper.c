#include "kernel/wallpaper.h"
#include "kernel/errno.h"
#include "kernel/deflate.h"
#include "kernel/inflate.h"
#include "kernel/pmm.h"
#include "kernel/ramdisk.h"
#include "kernel/string.h"
#include "kernel/printk.h"
#include "kernel/screen.h"
#include "kernel/theme.h"
#include "kernel/timing.h"

struct __attribute__((packed)) header {
    char magic[4];              /* "LKXW" */
    uint16_t w, h;
    uint32_t raw_len, packed_len, fnv;
    uint32_t top, bottom, left, right;
};

static struct gfx_surface image;    /* screen-sized, or px = 0 for the plain colour */
static int shown;                   /* image holds a wallpaper (else: the plain colour) */
static char current[32];            /* its name, without "-light" */
static int custom;                  /* it's a picture an app gave (WALLPAPER_CUSTOM) */

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p,
        pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Undo PNG-style filters in place (bpp 3). Returns the FNV-1a of the pixels,
 * or 0 with *ok = 0 on a bad filter type. */
static uint32_t unfilter(uint8_t *raw, int w, int h, int *ok)
{
    uint32_t fnv = 0x811c9dc5u;
    int stride = 1 + 3 * w;
    *ok = 1;
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + y * stride + 1, type = row[-1];
        const uint8_t *prev = y ? raw + (y - 1) * stride + 1 : 0;
        for (int i = 0; i < 3 * w; i++) {
            uint8_t a = i >= 3 ? row[i - 3] : 0, b = prev ? prev[i] : 0,
                    c = prev && i >= 3 ? prev[i - 3] : 0;
            switch (type) {
            case 0: break;
            case 1: row[i] = (uint8_t)(row[i] + a); break;
            case 2: row[i] = (uint8_t)(row[i] + b); break;
            case 3: row[i] = (uint8_t)(row[i] + ((a + b) >> 1)); break;
            case 4: row[i] = (uint8_t)(row[i] + paeth(a, b, c)); break;
            default: *ok = 0; return 0;
            }
            fnv = (fnv ^ row[i]) * 0x01000193u;
        }
    }
    return fnv;
}

static void free_frames(uintptr_t phys, uint32_t bytes)
{
    for (uint32_t off = 0; off < bytes; off += PAGE_SIZE)
        pmm_free(phys + off);
}

/* Unpack an .lkxw file into a fresh buffer of unfiltered rows (1 filter
 * byte + 3 * w bytes each), checked. The caller frees it (free_frames). */
static int unpack_data(const void *data, uint32_t size, const char *name, struct header *hd, uintptr_t *raw,
                       uint32_t *raw_bytes)
{
    if (size < sizeof(*hd))
        return -EINVAL;
    memcpy(hd, data, sizeof(*hd));
    if (size < sizeof(*hd) || memcmp(hd->magic, "LKXW", 4) || hd->packed_len > size - sizeof(*hd) ||
        !hd->w || !hd->h || hd->raw_len != (uint32_t)hd->h * (1 + 3u * hd->w))
        return -EINVAL;
    *raw_bytes = (hd->raw_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    *raw = pmm_alloc_contiguous(*raw_bytes / PAGE_SIZE);
    if (!*raw)
        return -ENOMEM;
    int ok = 0, got = inflate_raw((const uint8_t *)data + sizeof(*hd), hd->packed_len,
                                  (uint8_t *)*raw, hd->raw_len);
    uint32_t fnv = got == (int)hd->raw_len ? unfilter((uint8_t *)*raw, hd->w, hd->h, &ok) : 0;
    if (got != (int)hd->raw_len || !ok || fnv != hd->fnv) {
        kprintf("wallpaper: %s is corrupt (inflate %d, checksum %s)\n", name, got,
                ok && fnv != hd->fnv ? "wrong" : "not checked");
        free_frames(*raw, *raw_bytes);
        return -EINVAL;
    }
    return 0;
}

/* The same for wallpapers/<name>.lkxw in the ramdisk. */
static int unpack(const char *name, struct header *hd, uintptr_t *raw, uint32_t *raw_bytes)
{
    char path[64] = "wallpapers/";
    int n = (int)strlen(path);
    for (int i = 0; name[i] && n < (int)sizeof(path) - 6; i++)
        path[n++] = name[i];
    memcpy(path + n, ".lkxw", 6);
    const void *data;
    uint32_t size;
    if (ramdisk_find(path, &data, &size))
        return -ENOENT;
    return unpack_data(data, size, name, hd, raw, raw_bytes);
}

int wallpaper_init(const char *name)
{
    struct gfx_surface *scr = screen_surface();
    uint32_t bytes = ((uint32_t)scr->w * scr->h * 4 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uintptr_t img = pmm_alloc_contiguous(bytes / PAGE_SIZE);
    if (!img) {
        kprintf("wallpaper: out of memory; plain colour\n");
        return -ENOMEM;
    }
    image = (struct gfx_surface){ (uint32_t *)img, scr->w, scr->h, scr->w };
    int err = wallpaper_set(name);
    if (err)
        kprintf("wallpaper: %s unavailable; plain colour\n", name);
    return err;
}

/* A preset (wallpapers/<name>.lkxw, or "<name>-light" in the light style
 * where there is one) drawn into dst: centred, the edge colours filling
 * whatever it doesn't cover (a smaller dst shows its middle). */
static int draw_preset(const char *name, struct gfx_surface *dst, struct header *hd)
{
    uintptr_t raw;
    uint32_t raw_bytes;
    char variant[40];
    int n = 0;
    for (; name[n] && n < 30; n++)
        variant[n] = name[n];
    memcpy(variant + n, "-light", 7);
    int err = -ENOENT;
    if (theme_is_light())
        err = unpack(variant, hd, &raw, &raw_bytes);
    if (err)
        err = unpack(name, hd, &raw, &raw_bytes);
    if (err)
        return err;
    int ox = (dst->w - hd->w) / 2, oy = (dst->h - hd->h) / 2;
    gfx_fill_rect(dst, 0, 0, dst->w, oy, hd->top);
    gfx_fill_rect(dst, 0, oy + hd->h, dst->w, dst->h - oy - hd->h, hd->bottom);
    gfx_fill_rect(dst, 0, 0, ox, dst->h, hd->left);
    gfx_fill_rect(dst, ox + hd->w, 0, dst->w - ox - hd->w, dst->h, hd->right);
    int stride = 1 + 3 * hd->w;
    for (int y = 0; y < hd->h; y++) {
        int sy = oy + y;
        if (sy < 0 || sy >= dst->h)
            continue;
        const uint8_t *p = (const uint8_t *)raw + y * stride + 1;
        for (int x = 0; x < hd->w; x++, p += 3) {
            int sx = ox + x;
            if (sx >= 0 && sx < dst->w)
                dst->px[sy * dst->stride + sx] = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
        }
    }
    free_frames(raw, raw_bytes);
    return 0;
}

int wallpaper_render(const char *name, struct gfx_surface *dst)
{
    struct header hd;
    if (!name[0]) {
        gfx_fill_rect(dst, 0, 0, dst->w, dst->h, theme_get()->desktop_bg);
        return 0;
    }
    return draw_preset(name, dst, &hd);
}

int wallpaper_set(const char *name)
{
    struct header hd;
    uint32_t t0 = uptime_ms();
    int n;
    if (!image.px)
        return -ENODEV;
    if (!strcmp(name, WALLPAPER_CUSTOM))
        return custom ? 0 : -ENOENT;        /* already showing; it has no file to load */
    if (!name[0]) {             /* none: the theme's plain colour */
        shown = custom = 0;
        current[0] = '\0';
        kprintf("wallpaper: none (plain colour)\n");
        return 0;
    }
    int err = draw_preset(name, &image, &hd);
    if (err)
        return err;
    shown = 1;
    custom = 0;
    if (current != name) {
        for (n = 0; name[n] && n < (int)sizeof(current) - 1; n++)
            current[n] = name[n];
        current[n] = '\0';
    }
    kprintf("wallpaper: %s, %ux%u, %u KB packed, unpacked and checked in %u ms\n", current, hd.w,
            hd.h, hd.packed_len / 1024, uptime_ms() - t0);
    return 0;
}

int wallpaper_refresh(void)
{
    if (custom)
        return 0;                           /* the same in both styles */
    return current[0] ? wallpaper_set(current) : -ENOENT;
}

void wallpaper_scale_cover(const uint32_t *px, int w, int h, int stride, struct gfx_surface *dst)
{
    /* Fill dst, cutting off what's outside the middle: the part of the
     * picture used is dst's shape (sw x sh of it, at sx0, sy0). */
    int dw = dst->w, dh = dst->h, sw = w, sh = h;
    if ((int64_t)w * dh > (int64_t)h * dw)
        sw = (int)((int64_t)h * dw / dh);
    else
        sh = (int)((int64_t)w * dh / dw);
    if (sw < 1)
        sw = 1;
    if (sh < 1)
        sh = 1;
    int sx0 = (w - sw) / 2, sy0 = (h - sh) / 2;
    /* Each screen pixel is the average of the picture's pixels under it
     * (one pixel, when the picture is the smaller). */
    for (int y = 0; y < dh; y++) {
        int y0 = sy0 + (int)((int64_t)y * sh / dh), y1 = sy0 + (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = sx0 + (int)((int64_t)x * sw / dw), x1 = sx0 + (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0)
                x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint32_t *row = px + (uint32_t)yy * (uint32_t)stride;
                for (int xx = x0; xx < x1; xx++, n++) {
                    uint32_t c = row[xx];
                    r += c >> 16 & 0xff;
                    g += c >> 8 & 0xff;
                    b += c & 0xff;
                }
            }
            dst->px[y * dst->stride + x] = (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
}

int wallpaper_set_image(const uint32_t *px, int w, int h, int stride)
{
    if (!image.px)
        return -ENODEV;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || stride < w)
        return -EINVAL;
    uint32_t t0 = uptime_ms();
    int dw = image.w, dh = image.h;
    wallpaper_scale_cover(px, w, h, stride, &image);
    shown = custom = 1;
    memcpy(current, WALLPAPER_CUSTOM, sizeof(WALLPAPER_CUSTOM));
    kprintf("wallpaper: %s, %dx%d picture, scaled to %dx%d in %u ms\n", current, w, h, dw, dh,
            uptime_ms() - t0);
    return 0;
}

const char *wallpaper_current(void)
{
    return shown ? current : "";
}

int wallpaper_thumb(const char *name, uint32_t *out, int w, int h)
{
    struct header hd;
    uintptr_t raw;
    uint32_t raw_bytes;
    /* The build makes a small "<name>.thumb" for each: much quicker than
     * unpacking the whole wallpaper. Without one, the wallpaper itself. */
    char small[40];
    int k = 0;
    for (; name[k] && k < 30; k++)
        small[k] = name[k];
    memcpy(small + k, ".thumb", 7);
    int err = unpack(small, &hd, &raw, &raw_bytes);
    if (err)
        err = unpack(name, &hd, &raw, &raw_bytes);
    if (err)
        return err;
    /* Average a box of source pixels for each thumbnail pixel (every other
     * pixel of a full-size image: plenty for a preview). */
    int stride = 1 + 3 * hd.w, step = hd.w >= 2 * w ? 2 : 1;
    for (int ty = 0; ty < h; ty++) {
        int y0 = ty * hd.h / h, y1 = (ty + 1) * hd.h / h;
        if (y1 == y0)
            y1 = y0 + 1;
        for (int tx = 0; tx < w; tx++) {
            int x0 = tx * hd.w / w, x1 = (tx + 1) * hd.w / w;
            if (x1 == x0)
                x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1 && y < hd.h; y += step)
                for (int x = x0; x < x1 && x < hd.w; x += step, n++) {
                    const uint8_t *p = (const uint8_t *)raw + y * stride + 1 + x * 3;
                    r += p[0], g += p[1], b += p[2];
                }
            out[ty * w + tx] = n ? (r / n) << 16 | (g / n) << 8 | (b / n) : 0;
        }
    }
    free_frames(raw, raw_bytes);
    return 0;
}

int wallpaper_list(char names[][32], int max)
{
    int count = 0;
    for (int i = 0; i < ramdisk_count() && count < max; i++) {
        const char *f = ramdisk_name(i);
        int n = f ? (int)strlen(f) : 0;
        if (n < 17 || memcmp(f, "wallpapers/", 11) || strcmp(f + n - 5, ".lkxw"))
            continue;
        int len = n - 16;                   /* without "wallpapers/" and ".lkxw" */
        if (len >= 32 || (len > 6 && !memcmp(f + 11 + len - 6, "-light", 6)) ||
            (len > 6 && !memcmp(f + 11 + len - 6, ".thumb", 6)))
            continue;                       /* light variants and previews go with theirs */
        memcpy(names[count], f + 11, (size_t)len);
        names[count][len] = '\0';
        count++;
    }
    return count;
}

void wallpaper_draw(struct gfx_surface *dst, int ox, int oy, uint32_t dim)
{
    if (!shown) {
        uint32_t c = theme_get()->desktop_bg;
        gfx_fill_rect(dst, 0, 0, dst->w, dst->h, dim ? gfx_mix(0, c, dim) : c);
        return;
    }
    gfx_blit(dst, 0, 0, &image, ox, oy, dst->w, dst->h);
    if (dim)
        gfx_darken(dst, 0, 0, dst->w, dst->h, dim);
}

/* --- the custom wallpaper, packed (kernel/settings.h keeps it) ------------------------ */

static uint32_t filter_cost(const uint8_t *d, int n)
{
    uint32_t sum = 0;
    for (int i = 0; i < n; i++)
        sum += d[i] < 128 ? d[i] : 256u - d[i];
    return sum;
}

int wallpaper_pack(uintptr_t *phys, uint32_t *bytes, uint32_t *len)
{
    if (!custom || !shown)
        return -ENOENT;
    return wallpaper_pack_surface(&image, phys, bytes, len);
}

int wallpaper_pack_surface(const struct gfx_surface *src, uintptr_t *phys, uint32_t *bytes, uint32_t *len)
{
    int w = src->w, h = src->h, stride = 1 + 3 * w;
    uint32_t raw_len = (uint32_t)h * (uint32_t)stride;
    uint32_t raw_bytes = (raw_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t cap = sizeof(struct header) + raw_len + raw_len / 64 + 1024;
    uint32_t out_bytes = (cap + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), work_bytes = DEFLATE_WORK;
    uintptr_t raw = pmm_alloc_contiguous(raw_bytes / PAGE_SIZE);
    uintptr_t out = raw ? pmm_alloc_contiguous(out_bytes / PAGE_SIZE) : 0;
    uintptr_t work = out ? pmm_alloc_contiguous(work_bytes / PAGE_SIZE) : 0;
    if (!work) {
        if (raw)
            free_frames(raw, raw_bytes);
        if (out)
            free_frames(out, out_bytes);
        return -ENOMEM;
    }
    /* Each row with the PNG filter that suits it best (as tools/wallpaper-pack.py). */
    static uint8_t cur[3 * 4096], above[3 * 4096], cand[5][3 * 4096];
    struct header hd = { { 'L', 'K', 'X', 'W' }, (uint16_t)w, (uint16_t)h, raw_len, 0, 0x811c9dc5u, 0, 0, 0, 0 };
    for (int y = 0; y < h; y++) {
        const uint32_t *px = src->px + y * src->stride;
        for (int x = 0; x < w; x++) {
            cur[3 * x] = (uint8_t)(px[x] >> 16);
            cur[3 * x + 1] = (uint8_t)(px[x] >> 8);
            cur[3 * x + 2] = (uint8_t)px[x];
        }
        for (int i = 0; i < 3 * w; i++) {
            int a = i >= 3 ? cur[i - 3] : 0, b = y ? above[i] : 0, c = y && i >= 3 ? above[i - 3] : 0;
            cand[0][i] = cur[i];
            cand[1][i] = (uint8_t)(cur[i] - a);
            cand[2][i] = (uint8_t)(cur[i] - b);
            cand[3][i] = (uint8_t)(cur[i] - ((a + b) >> 1));
            cand[4][i] = (uint8_t)(cur[i] - paeth((uint8_t)a, (uint8_t)b, (uint8_t)c));
            hd.fnv = (hd.fnv ^ cur[i]) * 0x01000193u;
        }
        int best = 0;
        uint32_t best_cost = filter_cost(cand[0], 3 * w);
        for (int f = 1; f < 5; f++) {
            uint32_t cost = filter_cost(cand[f], 3 * w);
            if (cost < best_cost)
                best = f, best_cost = cost;
        }
        uint8_t *row = (uint8_t *)raw + y * stride;
        row[0] = (uint8_t)best;
        memcpy(row + 1, cand[best], (size_t)(3 * w));
        memcpy(above, cur, (size_t)(3 * w));
    }
    hd.top = hd.left = src->px[0];
    hd.bottom = hd.right = src->px[(h - 1) * src->stride + w - 1];
    int packed = deflate_raw((const uint8_t *)raw, raw_len, (uint8_t *)out + sizeof(hd),
                             cap - sizeof(hd), (void *)work);
    free_frames(raw, raw_bytes);
    free_frames(work, work_bytes);
    if (packed < 0) {
        free_frames(out, out_bytes);
        return packed;
    }
    hd.packed_len = (uint32_t)packed;
    memcpy((void *)out, &hd, sizeof(hd));
    *phys = out;
    *bytes = out_bytes;
    *len = sizeof(hd) + (uint32_t)packed;
    return 0;
}

void wallpaper_pack_free(uintptr_t phys, uint32_t bytes)
{
    free_frames(phys, bytes);
}

int wallpaper_render_packed(const void *data, uint32_t size, struct gfx_surface *dst)
{
    struct header hd;
    uintptr_t raw;
    uint32_t raw_bytes;
    int err = unpack_data(data, size, WALLPAPER_CUSTOM, &hd, &raw, &raw_bytes);
    if (err)
        return err;
    uint32_t px_bytes = ((uint32_t)hd.w * hd.h * 4 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uintptr_t px = pmm_alloc_contiguous(px_bytes / PAGE_SIZE);
    if (!px) {
        free_frames(raw, raw_bytes);
        return -ENOMEM;
    }
    int stride = 1 + 3 * hd.w;
    for (int y = 0; y < hd.h; y++) {
        const uint8_t *p = (const uint8_t *)raw + y * stride + 1;
        uint32_t *o = (uint32_t *)px + y * hd.w;
        for (int x = 0; x < hd.w; x++, p += 3)
            o[x] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    free_frames(raw, raw_bytes);
    wallpaper_scale_cover((const uint32_t *)px, hd.w, hd.h, hd.w, dst);
    free_frames(px, px_bytes);
    return 0;
}

int wallpaper_set_packed(const void *data, uint32_t size)
{
    if (!image.px)
        return -ENODEV;
    uint32_t t0 = uptime_ms();
    int err = wallpaper_render_packed(data, size, &image);
    if (err)
        return err;
    shown = custom = 1;
    memcpy(current, WALLPAPER_CUSTOM, sizeof(WALLPAPER_CUSTOM));
    kprintf("wallpaper: %s, %dx%d, unpacked in %u ms\n", current, image.w, image.h, uptime_ms() - t0);
    return 0;
}

uint32_t wallpaper_corner(void)
{
    return shown ? image.px[(image.h - 1) * image.stride + image.w - 1] : theme_get()->desktop_bg;
}
