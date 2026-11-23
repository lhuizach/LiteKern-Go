/* LiteKern Go: the first-run setup (see kernel/setup.h). */
#include "kernel/setup.h"
#include "kernel/desktop.h"
#include "kernel/errno.h"
#include "kernel/input.h"
#include "kernel/kern86_abi.h"
#include "kernel/lockscreen.h"
#include "kernel/machine.h"
#include "kernel/pmm.h"
#include "kernel/printk.h"
#include "kernel/screen.h"
#include "kernel/settings.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/timing.h"
#include "kernel/wallpaper.h"
#include "kernel/wm.h"

#define WHITE 0xffffffu
#define FADE_MS 380                             /* one page to the next */
#define WORD_MS 2700                            /* each hello: in, held, out */
#define WORD_IN 750
#define WORD_OUT 600
#define RISE 46                                 /* how far a word rises as it comes in */
#define BLUR_R 22                               /* each of three box blurs */
#define MASK_MAX (720 * 420)

enum { P_HELLO, P_LOOK, P_DONE, P_COUNT };
static const char *const words[] = { "hello", "bonjour", "hola", "ciao", "hallo", "hej", "ola", "salut" };
#define NWORDS ((int)(sizeof(words) / sizeof(words[0])))

static struct {
    int active, page, from_page;
    uint32_t page_at, start_at;
    struct gfx_surface bg, page_pic, prev_pic, tmp;    /* the blurred wallpaper; this page drawn; the last; a scratch */
    int word, mx, my, mw, mh;                           /* the hello's mask and where it goes */
    uint8_t mask[MASK_MAX];
    int light, accent, naccents;
    int down, down_x, down_y;
    struct gfx_rect btn, card[2], dot[8];
    int last_rise, last_alpha, drawn_word;
} su;

static struct gfx_surface *S(void) { return screen_surface(); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int inside(struct gfx_rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static int buffer(struct gfx_surface *s)
{
    if (s->px)
        return 0;
    uint32_t bytes = (uint32_t)S()->w * (uint32_t)S()->h * 4;
    uintptr_t phys = pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!phys)
        return -ENOMEM;
    *s = (struct gfx_surface){ (uint32_t *)phys, S()->w, S()->h, S()->w };
    return 0;
}

/* --- the background: the wallpaper, blurred hard and toned to the theme, made once ------------------------ */

static void box_pass(uint32_t *p, int n, int count, int along, int across, uint32_t *line)
{
    const int r = BLUR_R, k = 2 * BLUR_R + 1;
    for (int c = 0; c < count; c++) {
        uint32_t *q = p + (size_t)c * (size_t)across;
        int sr = 0, sg = 0, sb = 0;
        for (int i = -r; i <= r; i++) {
            uint32_t v = q[(size_t)(i < 0 ? 0 : i >= n ? n - 1 : i) * (size_t)along];
            sr += (int)(v >> 16 & 255); sg += (int)(v >> 8 & 255); sb += (int)(v & 255);
        }
        for (int i = 0; i < n; i++) {
            line[i] = (uint32_t)(sr / k) << 16 | (uint32_t)(sg / k) << 8 | (uint32_t)(sb / k);
            uint32_t out = q[(size_t)(i - r < 0 ? 0 : i - r) * (size_t)along];
            uint32_t in = q[(size_t)(i + r + 1 >= n ? n - 1 : i + r + 1) * (size_t)along];
            sr += (int)(in >> 16 & 255) - (int)(out >> 16 & 255);
            sg += (int)(in >> 8 & 255) - (int)(out >> 8 & 255);
            sb += (int)(in & 255) - (int)(out & 255);
        }
        for (int i = 0; i < n; i++)
            q[(size_t)i * (size_t)along] = line[i];
    }
}

static void make_bg(void)
{
    static uint32_t line[2048];
    struct gfx_surface *b = &su.bg;
    wallpaper_draw(b, 0, 0, 0);
    for (int pass = 0; pass < 3; pass++) {
        box_pass(b->px, b->w, b->h, 1, b->stride, line);
        box_pass(b->px, b->h, b->w, b->stride, 1, line);
    }
    /* toned: darker towards the bottom (where the buttons are), a little of the theme's colour over all */
    for (int y = 0; y < b->h; y++) {
        uint32_t dim = (uint32_t)(70 + 90 * y / b->h);
        gfx_darken(b, 0, y, b->w, 1, dim);
    }
}

/* --- the hello: a word in the big light face, shrunk into a coverage mask ----------------------------------- */

static void make_word(int i)
{
    const struct text_face *f = lockscreen_face(K86_CLOCK_FONTS + 1);   /* fonts/clock-hello.lkxf: Ubuntu Sans light, 240 px */
    if (!f)
        f = 0;
    struct gfx_surface *t = &su.tmp;
    if (!f) {                                       /* no big face: the heading style, unscaled */
        int tw = text_width(words[i], TEXT_HEADING), th = text_height(TEXT_HEADING);
        gfx_fill_rect(t, 0, 0, tw + 8, th, 0);
        text_draw(t, 4, 0, words[i], TEXT_HEADING, WHITE);
        su.mw = tw + 8; su.mh = th;
        for (int y = 0; y < th; y++)
            for (int x = 0; x < su.mw; x++)
                su.mask[y * su.mw + x] = (uint8_t)(t->px[y * t->stride + x] >> 8 & 255);
    } else {
        int tw = text_width_face(words[i], f) + 24, th = f->ascent + f->descent;
        tw = imin(tw, t->w);
        th = imin(th, t->h);
        gfx_fill_rect(t, 0, 0, tw, th, 0);
        text_draw_face(t, 12, 0, words[i], f, WHITE);
        /* the size it shows at: about a quarter of the screen tall, never wider than 84% of it */
        int want_h = S()->h * 19 / 100, mw = tw * want_h / th, mh = want_h;
        if (mw > S()->w * 84 / 100) { mh = mh * (S()->w * 84 / 100) / mw; mw = S()->w * 84 / 100; }
        while (mw * mh > MASK_MAX) { mw = mw * 9 / 10; mh = mh * 9 / 10; }
        su.mw = mw; su.mh = mh;
        for (int y = 0; y < mh; y++) {                  /* each mask pixel: the average of its source area */
            int y0 = y * th / mh, y1 = imax(y0 + 1, (y + 1) * th / mh);
            for (int x = 0; x < mw; x++) {
                int x0 = x * tw / mw, x1 = imax(x0 + 1, (x + 1) * tw / mw), sum = 0;
                for (int sy = y0; sy < y1; sy++)
                    for (int sx = x0; sx < x1; sx++)
                        sum += (int)(t->px[sy * t->stride + sx] >> 8 & 255);
                su.mask[y * mw + x] = (uint8_t)(sum / ((y1 - y0) * (x1 - x0)));
            }
        }
    }
    su.mx = (S()->w - su.mw) / 2;
    su.my = S()->h * 36 / 100 - su.mh / 2;
    su.word = i;
}

static int ease_out(int t, int d)                       /* 0..1000, quick then gentle */
{
    if (t <= 0) return 0;
    if (t >= d) return 1000;
    int u = 1000 - t * 1000 / d;
    return 1000 - u * u / 1000 * u / 1000;
}

/* Draws this frame's hello over the page picture in s: alpha 0..255, rise in px. */
static void draw_word(struct gfx_surface *s, int alpha, int rise)
{
    if (alpha <= 0)
        return;
    int y0 = su.my + rise;
    for (int y = 0; y < su.mh; y++) {
        int sy = y0 + y;
        if (sy < 0 || sy >= s->h)
            continue;
        uint32_t *row = s->px + (size_t)sy * (size_t)s->stride + su.mx;
        const uint8_t *m = su.mask + y * su.mw;
        for (int x = 0; x < su.mw; x++)
            if (m[x])
                row[x] = gfx_mix(WHITE, row[x], (uint32_t)(m[x] * alpha / 255));
    }
}

/* --- the pages ------------------------------------------------------------------------------------------ */

static void button(struct gfx_surface *s, struct gfx_rect r, const char *label, int alpha)
{
    const struct theme *t = theme_get();
    gfx_blend_round_rect(s, r.x, r.y, r.w, r.h, r.h / 2, t->accent_bg, (uint32_t)alpha);
    int tw = text_width(label, TEXT_BOLD);
    if (alpha > 200)
        text_draw(s, r.x + (r.w - tw) / 2, r.y + (r.h - text_height(TEXT_BOLD)) / 2, label, TEXT_BOLD, t->accent_fg);
}

static void centred(struct gfx_surface *s, int y, const char *str, enum text_style st, uint32_t c)
{
    text_draw(s, (s->w - text_width(str, st)) / 2, y, str, st, c);
}

/* A small home screen, as a card: the look it stands for. */
static void mock(struct gfx_surface *s, struct gfx_rect r, int light, int selected)
{
    uint32_t ac = 0x3584e4u;
    theme_accent(su.accent, &ac);
    uint32_t bg = light ? 0xf6f5f4u : 0x242424u, fg = light ? 0x2e2e2eu : 0xf0f0f0u, tile = light ? 0xffffffu : 0x383838u;
    if (selected)
        gfx_fill_round_rect(s, r.x - 7, r.y - 7, r.w + 14, r.h + 14, 34, ac);
    gfx_fill_round_rect(s, r.x, r.y, r.w, r.h, 28, bg);
    gfx_fill_round_rect(s, r.x + 22, r.y + 22, r.w / 3, 10, 5, fg);                  /* the clock */
    int cell = (r.w - 44) / 4;
    for (int i = 0; i < 12; i++) {
        int cx = r.x + 22 + (i % 4) * cell, cy = r.y + 70 + (i / 4) * (cell + 10);
        gfx_fill_round_rect(s, cx + 6, cy, cell - 12, cell - 12, 12, i % 5 == 0 ? ac : tile);
    }
    gfx_fill_round_rect(s, r.x + 16, r.y + r.h - 70, r.w - 32, 52, 22, tile);         /* the dock */
    for (int i = 0; i < 4; i++)
        gfx_fill_round_rect(s, r.x + 30 + i * (r.w - 60) / 4 + 6, r.y + r.h - 62, (r.w - 60) / 4 - 12, 36, 10, i == 0 ? ac : bg);
}

static void draw_page(struct gfx_surface *s, int page)
{
    gfx_blit(s, 0, 0, &su.bg, 0, 0, s->w, s->h);
    int w = s->w, h = s->h;
    su.btn = (struct gfx_rect){ (w - 460) / 2, h - 250, 460, 108 };
    if (page == P_HELLO) {
        centred(s, h - 370, "Welcome to LiteKern Go", TEXT_BODY, 0xd8d8d8u);
        button(s, su.btn, "Get started", 235);
        return;
    }
    if (page == P_LOOK) {
        centred(s, 150, "Choose your look", TEXT_HEADING, WHITE);
        centred(s, 214, "You can change it later in Settings", TEXT_SMALL, 0xc8c8c8u);
        int cw = (w - 3 * 56) / 2, ch = cw * 16 / 10;
        for (int i = 0; i < 2; i++) {
            su.card[i] = (struct gfx_rect){ 56 + i * (cw + 56), 300, cw, ch };
            mock(s, su.card[i], i, su.light == i);
            const char *l = i ? "Light" : "Dark";
            text_draw(s, su.card[i].x + (cw - text_width(l, TEXT_BOLD)) / 2, 300 + ch + 26, l, TEXT_BOLD, WHITE);
        }
        int y = 300 + ch + 110, n = su.naccents, d = 64, gap = imin(28, (w - 80 - n * d) / imax(1, n - 1));
        int x = (w - (n * d + (n - 1) * gap)) / 2;
        gfx_blend_round_rect(s, x - 28, y - 22, n * d + (n - 1) * gap + 56, d + 44, (d + 44) / 2, WHITE, 26);
        for (int i = 0; i < n; i++) {
            uint32_t c = 0;
            theme_accent(i, &c);
            su.dot[i] = (struct gfx_rect){ x + i * (d + gap), y, d, d };
            if (i == su.accent)
                gfx_fill_circle(s, su.dot[i].x + d / 2, y + d / 2, d / 2 + 6, WHITE);
            gfx_fill_circle(s, su.dot[i].x + d / 2, y + d / 2, d / 2, c);
        }
        button(s, su.btn, "Continue", 235);
        return;
    }
    /* done */
    const struct theme *t = theme_get();
    int cx = w / 2, cy = 330;
    gfx_fill_circle(s, cx, cy, 96, t->accent_bg);
    gfx_thick_line(s, cx - 42, cy + 2, cx - 12, cy + 34, 14, t->accent_fg);
    gfx_thick_line(s, cx - 12, cy + 34, cx + 46, cy - 28, 14, t->accent_fg);
    centred(s, 480, "You're all set", TEXT_HEADING, WHITE);
    static const char *const tips[3][2] = {
        { "Swipe up", "for all your apps" }, { "Hold the home screen", "to move apps and add widgets" },
        { "Hold Power", "to restart or turn off" } };
    struct gfx_rect c = { 56, 580, w - 112, 3 * 112 + 24 };
    gfx_blend_round_rect(s, c.x, c.y, c.w, c.h, 32, WHITE, 30);
    for (int i = 0; i < 3; i++) {
        int y = c.y + 22 + i * 112;
        gfx_fill_circle(s, c.x + 54, y + 50, 10, t->accent_bg);
        text_draw(s, c.x + 90, y + 14, tips[i][0], TEXT_BOLD, WHITE);
        text_draw(s, c.x + 90, y + 54, tips[i][1], TEXT_SMALL, 0xc8c8c8u);
    }
    button(s, su.btn, "Start", 235);
}

static void go(int page)
{
    gfx_blit(&su.prev_pic, 0, 0, S(), 0, 0, S()->w, S()->h);   /* what shows now, to fade from */
    su.from_page = su.page;
    su.page = page;
    su.page_at = uptime_ms();
    draw_page(&su.page_pic, page);
}

static void finish(void)
{
    kprintf("setup: done (%s, accent %s)\n", su.light ? "light" : "dark", theme_accent(su.accent, 0));
    su.active = 0;
    wallpaper_refresh();
    desktop_cover(0);
    desktop_refresh();
    wm_theme_changed();
    settings_save();
    if (lockscreen_active())
        lockscreen_redraw();                    /* it was waiting underneath (the login at boot): show it */
    plat_setup_finished();
}

/* --- the interface ------------------------------------------------------------------------------------------ */

void setup_start(void)
{
    if (buffer(&su.bg) || buffer(&su.page_pic) || buffer(&su.prev_pic) || buffer(&su.tmp)) {
        kprintf("setup: out of memory; skipped\n");
        return;
    }
    su.light = theme_is_light();
    su.accent = theme_accent_index();
    su.naccents = 0;
    while (su.naccents < 8 && theme_accent(su.naccents, 0))
        su.naccents++;
    desktop_cover(1);
    make_bg();
    make_word(0);
    su.active = 1;
    su.page = P_HELLO;
    su.page_at = su.start_at = uptime_ms();
    draw_page(&su.page_pic, P_HELLO);
    gfx_fill_rect(&su.prev_pic, 0, 0, S()->w, S()->h, 0);    /* it comes up from black */
    su.from_page = -1;
    su.drawn_word = -1;
    kprintf("setup: started\n");
}

int setup_active(void)
{
    return su.active;
}

void setup_tick(void)
{
    if (!su.active)
        return;
    struct gfx_surface *s = S();
    uint32_t now = uptime_ms();
    int fade = (int)(now - su.page_at), fading = fade < FADE_MS;
    int hello = su.page == P_HELLO, alpha = 0, rise = 0;
    if (hello) {
        int t = (int)(now - su.start_at) - 500;          /* a moment of the bare background first */
        int i = t < 0 ? 0 : (t / WORD_MS) % NWORDS, u = t < 0 ? -1 : t % WORD_MS;
        if (i != su.word)
            make_word(i);
        if (u >= 0) {
            int e = u < WORD_IN ? ease_out(u, WORD_IN) : 1000;
            int o = u > WORD_MS - WORD_OUT ? (WORD_MS - u) * 1000 / WORD_OUT : 1000;
            alpha = e * o / 1000 * 255 / 1000;
            rise = u < WORD_IN ? (1000 - e) * RISE / 1000 : -(1000 - o) * RISE / 3000;
        }
    }
    if (!fading && (!hello || (alpha == su.last_alpha && rise == su.last_rise && su.word == su.drawn_word)))
        return;                                     /* nothing moved */
    if (fading) {
        int a = ease_out(fade, FADE_MS) * 255 / 1000;
        gfx_blit(s, 0, 0, &su.prev_pic, 0, 0, s->w, s->h);
        gfx_blend(s, 0, 0, &su.page_pic, 0, 0, s->w, s->h, (uint32_t)a);
        screen_damage_all();
        if (hello)
            draw_word(s, alpha * a / 255, rise);
    } else {
        /* only the hello's band: the page there, then the word */
        int y0 = imax(0, su.my - RISE), y1 = imin(s->h, su.my + su.mh + RISE + 2);
        gfx_blit(s, 0, y0, &su.page_pic, 0, y0, s->w, y1 - y0);
        draw_word(s, alpha, rise);
        screen_damage(0, y0, s->w, y1 - y0);
    }
    su.last_alpha = alpha;
    su.last_rise = rise;
    su.drawn_word = su.word;
    screen_present();
}

static void tap(int x, int y)
{
    if (uptime_ms() - su.page_at < FADE_MS)
        return;
    if (su.page == P_HELLO) {
        if (inside(su.btn, x, y) || uptime_ms() - su.start_at > 1500)
            go(P_LOOK);
        return;
    }
    if (su.page == P_LOOK) {
        for (int i = 0; i < 2; i++)
            if (inside(su.card[i], x, y) && su.light != i) {
                su.light = i;
                theme_set(su.light, su.accent);
                draw_page(&su.page_pic, P_LOOK);
                gfx_blit(S(), 0, 0, &su.page_pic, 0, 0, S()->w, S()->h);
                screen_damage_all();
                screen_present();
                return;
            }
        for (int i = 0; i < su.naccents; i++) {
            struct gfx_rect r = su.dot[i];
            r.x -= 10; r.y -= 10; r.w += 20; r.h += 20;
            if (inside(r, x, y) && su.accent != i) {
                su.accent = i;
                theme_set(su.light, su.accent);
                draw_page(&su.page_pic, P_LOOK);
                gfx_blit(S(), 0, 0, &su.page_pic, 0, 0, S()->w, S()->h);
                screen_damage_all();
                screen_present();
                return;
            }
        }
        if (inside(su.btn, x, y))
            go(P_DONE);
        return;
    }
    if (inside(su.btn, x, y))
        finish();
}

void setup_pointer(int x, int y, uint8_t buttons)
{
    int down = buttons & 1;
    if (down && !su.down) {
        su.down_x = x;
        su.down_y = y;
    } else if (!down && su.down) {
        int dx = x - su.down_x, dy = y - su.down_y;
        if (dx * dx + dy * dy < 40 * 40)
            tap(x, y);
    }
    su.down = down;
}

void setup_key(uint16_t key, int pressed)
{
    if (key == KEY_PHONE_POWER) {               /* Power: the screen off and on again; the setup waits */
        if (!pressed) {
            int on = !machine_screen_on();
            machine_screen(on);
            if (on) {
                screen_damage_all();
                screen_present();
            }
        }
        return;
    }
    if (!pressed || uptime_ms() - su.page_at < FADE_MS)
        return;
    if (key == KEY_ENTER || key == KEY_SPACE) {
        if (su.page == P_DONE)
            finish();
        else
            go(su.page + 1);
    } else if ((key == KEY_ESC || key == KEY_PHONE_BACK) && su.page > P_HELLO) {
        go(su.page - 1);
    }
}
