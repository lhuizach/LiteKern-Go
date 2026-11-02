/* LiteKern X — Files: the picture viewer (see picture.h). */
#include "apps/files/picture.h"
#include "apps/files/image.h"
#include "kernel/errno.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"

#define FILE_MAX (32u * 1024 * 1024)   /* SYS_FS_READ's limit */
#define SHOWN_MAX (1024u * 768)         /* the fitted copy: never bigger than a screen */

static struct image pic;
static uint32_t file_size;
static char name[K86_NAME_MAX];
static uint32_t *shown;                 /* pic fitted to shown_w x shown_h */
static int shown_w, shown_h;

/* --- memory: one block at K86_HEAP, handed out in order, let go all at once --- */

static uint32_t used, mapped;

static void *alloc(uint32_t n)
{
    n = (n + 15) & ~15u;
    if (n > K86_HEAP_MAX - used)
        return 0;
    uint32_t need = used + n;
    if (need > mapped) {
        uint32_t grow = (need + 0xfffff) & ~0xfffffu;  /* a MiB at a time */
        if (grow > K86_HEAP_MAX)
            grow = K86_HEAP_MAX;
        if (k86_memory(grow))
            return 0;
        mapped = grow;
    }
    void *p = (void *)((uintptr_t)K86_HEAP + used);
    used = need;
    return p;                           /* new pages are zeroed (and freed ones go back) */
}

static void release(void)
{
    k86_memory(0);
    used = mapped = 0;
    pic.px = shown = 0;
}

/* --- names --------------------------------------------------------------------------- */

static int ends_with(const char *s, const char *ext)
{
    int n = (int)strlen(s), m = (int)strlen(ext);
    if (n <= m)
        return 0;
    for (int i = 0; i < m; i++) {
        char c = s[n - m + i];
        if ((c >= 'A' && c <= 'Z' ? c + 32 : c) != ext[i])
            return 0;
    }
    return 1;
}

int picture_named(const char *n)
{
    return ends_with(n, ".png") || ends_with(n, ".jpg") || ends_with(n, ".jpeg") || ends_with(n, ".bmp");
}

/* --- scaling ---------------------------------------------------------------------------- */

/* src (sw x sh) into dst (dw x dh): each pixel the average of what's under
 * it, alpha included (one pixel, when growing). */
static void scale(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        k86_uptime_ms();                    /* still here (a big one takes a while) */
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0)
                x1 = x0 + 1;
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++, n++) {
                    uint32_t c = src[yy * sw + xx], ca = c >> 24;
                    a += ca;                        /* colours weighted by alpha */
                    r += (c >> 16 & 255) * ca;
                    g += (c >> 8 & 255) * ca;
                    b += (c & 255) * ca;
                }
            dst[y * dw + x] = a ? (a / n) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a) : 0;
        }
    }
}

/* --- opening ---------------------------------------------------------------------------- */

static void still_here(void)
{
    k86_uptime_ms();                        /* any system call tells the watchdog */
}

int picture_open(int vol, const char *dir, const struct k86_dirent *e, const char **why)
{
    release();
    if (e->size > FILE_MAX) {
        *why = "Pictures up to 32 MB can be opened.";
        return -1;
    }
    uint8_t *data = alloc(e->size ? e->size : 1);
    if (!data) {
        *why = "There isn't enough memory to open it.";
        return -1;
    }
    int n = k86_read(vol, dir, e->name, data, e->size);
    if (n < 0) {
        release();
        *why = "The file couldn't be read.";
        return -1;
    }
    uint32_t t0 = k86_uptime_ms();
    image_busy = still_here;
    if (image_decode(data, (uint32_t)n, alloc, &pic, why)) {
        k86_logf("files: can't open picture %s: %s", e->name, *why);
        release();
        return -1;
    }
    shown = alloc(SHOWN_MAX * 4);
    if (!shown) {
        release();
        *why = "There isn't enough memory to show it.";
        return -1;
    }
    shown_w = shown_h = 0;
    file_size = (uint32_t)n;
    memcpy(name, e->name, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    k86_logf("files: picture %s %dx%d %s, decoded in %u ms", e->name, pic.w, pic.h, pic.format,
             k86_uptime_ms() - t0);
    return 0;
}

void picture_close(void)
{
    release();
}

/* --- drawing ---------------------------------------------------------------------------------- */

static void size_text(char *out, uint32_t bytes)
{
    char t[12];
    uint32_t v = bytes >= 1024 * 1024 ? bytes / (1024 * 1024) : bytes >= 1024 ? bytes / 1024 : bytes;
    int n = 0;
    do
        t[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        *out++ = t[--n];
    const char *unit = bytes >= 1024 * 1024 ? " MB" : bytes >= 1024 ? " KB" : " bytes";
    while (*unit)
        *out++ = *unit++;
    *out = '\0';
}

static char *num(char *p, int v)
{
    char t[12];
    int n = 0;
    do
        t[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        *p++ = t[--n];
    return p;
}

void picture_draw(struct gfx_surface *c, struct gfx_rect r)
{
    const struct theme *t = theme_get();
    int info_h = text_height(TEXT_SMALL) + 12;
    gfx_fill_rect(c, r.x, r.y, r.w, r.h, t->window_bg);
    if (!pic.px) {
        k86_present(r.x, r.y, r.w, r.h);
        return;
    }
    /* Fitted: as big as fits, never bigger than it is. */
    int aw = r.w, ah = r.h - info_h, w = pic.w, h = pic.h;
    if (w > aw || h > ah) {
        if ((int64_t)w * ah > (int64_t)h * aw) {
            h = (int)((int64_t)h * aw / w);
            w = aw;
        } else {
            w = (int)((int64_t)w * ah / h);
            h = ah;
        }
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if ((uint32_t)w * (uint32_t)h > SHOWN_MAX)
        w = (int)(SHOWN_MAX / (uint32_t)h);
    if (w != shown_w || h != shown_h) {
        scale(pic.px, pic.w, pic.h, shown, w, h);
        shown_w = w;
        shown_h = h;
    }
    int x = r.x + (aw - w) / 2, y = r.y + (ah - h) / 2;
    /* Transparent parts show a soft checkerboard, as image viewers do. */
    uint32_t c1 = t->light ? 0xffffff : 0x3a3a3a, c2 = t->light ? 0xe4e4e4 : 0x2e2e2e;
    for (int cy = 0; cy < h; cy += 8)
        for (int cx = 0; cx < w; cx += 8)
            gfx_fill_rect(c, x + cx, y + cy, w - cx < 8 ? w - cx : 8, h - cy < 8 ? h - cy : 8,
                          ((cx ^ cy) & 8) ? c2 : c1);
    gfx_blit_alpha(c, x, y, shown, w, h, w);

    char info[96], *p = num(info, pic.w);
    const char *times = " " TEXT_TIMES " ";
    while (*times)
        *p++ = *times++;
    p = num(p, pic.h);
    for (const char *s = ", "; *s; s++)
        *p++ = *s;
    for (const char *s = pic.format; *s; s++)
        *p++ = *s;
    for (const char *s = ", "; *s; s++)
        *p++ = *s;
    size_text(p, file_size);
    int iw = text_width(info, TEXT_SMALL);
    text_draw(c, r.x + (r.w - iw) / 2, r.y + r.h - info_h + 6, info, TEXT_SMALL, t->fg_dim);
    k86_present(r.x, r.y, r.w, r.h);
}

/* --- the wallpaper ------------------------------------------------------------------------------ */

int picture_wallpaper(int where, const char **why)
{
    if (!pic.px) {
        *why = "There's no picture open.";
        return -1;
    }
    /* Over black (a wallpaper has no transparency), at most 4096 across. */
    int w = pic.w, h = pic.h;
    if (w > 4096 || h > 4096) {
        if (w >= h) {
            h = (int)((int64_t)h * 4096 / w);
            w = 4096;
        } else {
            w = (int)((int64_t)w * 4096 / h);
            h = 4096;
        }
    }
    uint32_t *out = alloc((uint32_t)w * (uint32_t)h * 4);
    if (!out) {
        *why = "There isn't enough memory to make it the wallpaper.";
        return -1;
    }
    if (w != pic.w || h != pic.h)
        scale(pic.px, pic.w, pic.h, out, w, h);
    else
        memcpy(out, pic.px, (uint32_t)w * (uint32_t)h * 4);
    for (uint32_t i = 0; i < (uint32_t)w * (uint32_t)h; i++) {
        uint32_t c = out[i], a = c >> 24;
        out[i] = ((c >> 16 & 255) * a / 255) << 16 | ((c >> 8 & 255) * a / 255) << 8 | (c & 255) * a / 255;
    }
    int err = k86_wallpaper_image(out, w, h, w, where);
    used -= (uint32_t)(((uint32_t)w * (uint32_t)h * 4 + 15) & ~15u);    /* the copy's done with */
    if (err) {
        *why = "The wallpaper couldn't be changed.";
        return -1;
    }
    k86_logf("files: %s %s", where == K86_WALLPAPER_LOCK ? "lock screen picture" : "wallpaper", name);
    return 0;
}
