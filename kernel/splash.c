#include "kernel/splash.h"
#include "kernel/console.h"
#include "kernel/icon.h"
#include "kernel/pmm.h"
#include "kernel/printk.h"
#include "kernel/screen.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/timing.h"
#include "defaults.h"           /* DEFAULT_SPLASH */

#define BG          0x000000
#define LINE_FG     0x9aa3b2    /* "Powered by Linux": quieter than the logo's grey */
#define LINE        "Powered by Linux"
#define TUX_H       56          /* Tux, beside it */
#define LINE_BOTTOM 140         /* the line's middle, from the bottom of the screen */
#define FADE_IN_MS  300         /* the logo, out of the black */
#define MIN_MS      1500        /* shown at least this long, however quick the start */
#define CROSS_MS    400         /* into the lock screen or the home screen */

/* build/gen/splash.c (assets/splash.json: the logo, tools/make-logo.py, and
 * Tux, assets/tux.png: Larry Ewing's, see assets/CREDITS.md) */
extern const struct icon splash_images[];
extern const int splash_images_count;

static int active, brief;
static uint32_t started_ms;
static struct gfx_surface from, to;     /* the cross-fade: the splash, and what comes next */

static void wait_until(uint32_t t)
{
    while ((int32_t)(uptime_ms() - t) < 0)
        ;
}

/* The logo in the middle, and "Powered by Linux" with Tux near the bottom,
 * over black, at `alpha`. */
static void draw(uint32_t alpha)
{
    struct gfx_surface *s = screen_surface();
    const struct icon *logo = &splash_images[0];
    gfx_fill_rect(s, 0, 0, s->w, s->h, BG);
    gfx_blit_alpha_scaled(s, (struct gfx_rect){ (s->w - logo->w) / 2, (s->h - logo->h) / 2, logo->w, logo->h },
                          logo->px, logo->w, logo->h, logo->w, alpha);
    const struct icon *tux = splash_images_count > 1 ? &splash_images[1] : 0;
    int tw = tux ? TUX_H + 16 : 0, w = tw + text_width(LINE, TEXT_BODY), x = (s->w - w) / 2, cy = s->h - LINE_BOTTOM;
    if (tux)
        gfx_blit_alpha_scaled(s, (struct gfx_rect){ x, cy - TUX_H / 2, TUX_H, TUX_H }, tux->px, tux->w, tux->h, tux->w,
                              alpha);
    text_draw(s, x + tw, cy - text_height(TEXT_BODY) / 2, LINE, TEXT_BODY, gfx_mix(LINE_FG, BG, alpha));
    screen_damage_all();
    screen_present();
}

void splash_start(void)
{
    if (strcmp(DEFAULT_SPLASH, "logo") || !screen_ready() || !splash_images_count) {
        kprintf("splash: none (the boot log)\n");
        return;
    }
    console_set_visible(0);         /* still recorded: the Log app shows it */
    started_ms = uptime_ms();
    for (;;) {                      /* out of the black */
        if (brief) {
            draw(255);
            break;
        }
        uint32_t t = uptime_ms() - started_ms;
        draw(t >= FADE_IN_MS ? 255 : t * 255 / FADE_IN_MS);
        if (t >= FADE_IN_MS)
            break;
    }
    active = 1;
    kprintf("splash: logo\n");
}

/* LiteKern X's bar is gone (a start this quick only flashed it): the steps
 * are still called, and still mean the splash is up. */
void splash_progress(int percent)
{
    (void)percent;
}

int splash_active(void)
{
    return active;
}

static int copy_screen(struct gfx_surface *dst)
{
    struct gfx_surface *s = screen_surface();
    if (!dst->px) {
        uint32_t bytes = (uint32_t)s->w * (uint32_t)s->h * 4;
        uintptr_t phys = pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        if (!phys)
            return -1;
        *dst = (struct gfx_surface){ (uint32_t *)phys, s->w, s->h, s->w };
    }
    gfx_blit(dst, 0, 0, s, 0, 0, s->w, s->h);
    return 0;
}

static void free_screen(struct gfx_surface *b)
{
    if (!b->px)
        return;
    uint32_t bytes = (uint32_t)b->w * (uint32_t)b->h * 4;
    for (uint32_t off = 0; off < bytes; off += PAGE_SIZE)
        pmm_free((uintptr_t)b->px + off);
    b->px = 0;
}

void splash_set_brief(int on)
{
    brief = on;
}

int splash_fade_begin(void)
{
    if (!active || brief)
        return 0;
    wait_until(started_ms + MIN_MS);
    if (copy_screen(&from))
        return 0;                   /* no memory: it just cuts across */
    screen_hold(1);
    return 1;
}

void splash_fade_end(void)
{
    screen_hold(0);
    struct gfx_surface *s = screen_surface();
    if (from.px && !copy_screen(&to)) {
        uint32_t t0 = uptime_ms();
        for (;;) {
            uint32_t t = uptime_ms() - t0, a = t >= CROSS_MS ? 255 : t * 255 / CROSS_MS;
            if (a >= 255)
                break;
            for (int y = 0; y < s->h; y++) {
                uint32_t *d = s->px + y * s->stride, *f = from.px + y * from.stride, *n = to.px + y * to.stride;
                for (int x = 0; x < s->w; x++)
                    d[x] = gfx_mix(n[x], f[x], a);
            }
            screen_damage_all();
            screen_present();
        }
        gfx_blit(s, 0, 0, &to, 0, 0, s->w, s->h);
    }
    screen_damage_all();
    screen_present();
    free_screen(&from);
    free_screen(&to);
    kprintf("splash: faded out\n");
}

void splash_end(void)
{
    active = 0;
}
