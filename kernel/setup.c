/* LiteKern Go: the first-run setup (see kernel/setup.h). */
#include "kernel/setup.h"
#include "kernel/desktop.h"
#include "kernel/errno.h"
#include "kernel/input.h"
#include "kernel/kern86_abi.h"
#include "kernel/lockscreen.h"
#include "kernel/machine.h"
#include "kernel/net.h"
#include "kernel/timezones.h"
#include "kernel/users.h"
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

enum { P_HELLO, P_WIFI, P_ZONE, P_PRIVACY, P_NAME, P_PIN, P_LOOK, P_DONE, P_COUNT };
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
    /* Wi-Fi */
    struct k86_wifi nets[8];
    int nnets, sel, entering, kb_sym, kb_shift, show_pw, joining, joined, join_bad, pw_len;
    uint32_t poll_at, spin_at;
    char pw[64], joined_ssid[K86_SSID_MAX];
    struct gfx_rect rows[8], rescan, back, field, showbtn;
    struct { struct gfx_rect r; char c; } keys[48];
    int nkeys;
    /* time zone */
    int zone, zscroll, zlast_y, zmoved;
    /* the first run only: your name and a PIN */
    int first, name_len, pin_len;
    char name[32], pin[10];
    const char *kb_act;
    struct gfx_rect pad[12], skip;
    char padc[12];
} su;

/* The name and PIN pages are for the first run; "Run Setup Again" leaves who you are alone. */
static int hidden(int page)
{
    return !su.first && (page == P_NAME || page == P_PIN);
}

static int step(int page, int dir)
{
    do
        page += dir;
    while (page > P_HELLO && page < P_DONE && hidden(page));
    return page;
}

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
    text_draw(s, r.x + (r.w - tw) / 2, r.y + (r.h - text_height(TEXT_BOLD)) / 2, label, TEXT_BOLD,
              alpha > 200 ? t->accent_fg : gfx_mix(t->accent_fg, 0x000000u, 120));        /* dimmed while it can't be pressed */
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

/* --- Wi-Fi: the list, the password and the keyboard for it ---------------------------------------------------- */

static void wifi_scan(void)
{
    su.nnets = net_scan(su.nets, 8);
    if (su.nnets < 0)
        su.nnets = 0;
}

static void wifi_poll(void)
{
    struct k86_net n;
    net_status(&n);
    if (n.wifi == K86_NET_UP) {
        su.joining = 0;
        su.joined = 1;
        su.join_bad = 0;
        memcpy(su.joined_ssid, n.ssid, sizeof(su.joined_ssid));
    } else if (n.wifi == K86_NET_WRONG_KEY) {
        su.joining = 0;
        su.join_bad = 1;
    }
}

static void bars(struct gfx_surface *s, int x, int y, int signal, uint32_t c)
{
    for (int i = 0; i < 4; i++) {
        int h = 10 + i * 8;
        gfx_fill_round_rect(s, x + i * 12, y + 34 - h, 8, h, 3, signal * 4 / 100 >= i ? c : gfx_mix(c, 0x000000u, 70));
    }
}

/* A loader: twelve dots round a circle, the lit one moving on (no trigonometry: the circle's points are a table). */
static void spinner(struct gfx_surface *s, int cx, int cy, int r, uint32_t c)
{
    static const signed char sn[12] = { 0, 50, 87, 100, 87, 50, 0, -50, -87, -100, -87, -50 };
    int head = (int)(uptime_ms() / 80) % 12;
    for (int i = 0; i < 12; i++) {
        int age = (head - i + 12) % 12, a = 255 - age * 20;
        int x = cx + sn[i] * r / 100, y = cy - sn[(i + 3) % 12] * r / 100;
        gfx_fill_circle(s, x, y, imax(2, r / 7 + (age == 0)), gfx_mix(c, 0x000000u, (uint32_t)a));
    }
}

static void lock_icon(struct gfx_surface *s, int x, int y, uint32_t c)
{
    gfx_fill_round_rect(s, x, y + 10, 24, 18, 4, c);
    gfx_rect_outline(s, x + 5, y, 14, 14, c);
    gfx_rect_outline(s, x + 6, y + 1, 12, 12, c);
}

static const char *const kb_rows[2][3] = {
    { "qwertyuiop", "asdfghjkl", "zxcvbnm" },
    { "1234567890", "-/:;()&@\"$", ".,?!'#%+=_" },
};

static void draw_keyboard(struct gfx_surface *s)
{
    const struct theme *t = theme_get();
    int w = s->w, kh = 88, gap = 9, y0 = s->h - 4 * (kh + gap) - 20, n = 0;
    su.nkeys = 0;
    for (int r = 0; r < 3; r++) {
        const char *row = kb_rows[su.kb_sym][r];
        int len = (int)strlen(row), kw = (w - 2 * 14 - 9 * gap) / 10;
        int side = r == 2 ? kw * 3 / 2 : 0, x0 = (w - len * kw - (len - 1) * gap) / 2;
        if (r == 2)
            x0 = 14 + side + gap;
        for (int i = 0; i < len; i++) {
            char c = row[i];
            if (!su.kb_sym && su.kb_shift && c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            struct gfx_rect k = { x0 + i * (kw + gap), y0 + r * (kh + gap), kw, kh };
            su.keys[n].r = k; su.keys[n++].c = c;
            gfx_blend_round_rect(s, k.x, k.y, k.w, k.h, 16, WHITE, 38);
            char one[2] = { c, 0 };
            text_draw(s, k.x + (k.w - text_width(one, TEXT_BODY)) / 2, k.y + (k.h - text_height(TEXT_BODY)) / 2, one, TEXT_BODY, WHITE);
        }
        if (r == 2) {
            struct gfx_rect sh = { 14, y0 + r * (kh + gap), side, kh }, bk = { w - 14 - side, y0 + r * (kh + gap), side, kh };
            su.keys[n].r = sh; su.keys[n++].c = 1;                              /* shift (or #+=) */
            su.keys[n].r = bk; su.keys[n++].c = 2;                              /* backspace */
            gfx_blend_round_rect(s, sh.x, sh.y, sh.w, sh.h, 16, su.kb_shift && !su.kb_sym ? t->accent_bg : WHITE, su.kb_shift && !su.kb_sym ? 230 : 20);
            gfx_blend_round_rect(s, bk.x, bk.y, bk.w, bk.h, 16, WHITE, 20);
            const char *l1 = "^", *l2 = "<";
            text_draw(s, sh.x + (sh.w - text_width(l1, TEXT_BOLD)) / 2, sh.y + (sh.h - text_height(TEXT_BOLD)) / 2, l1, TEXT_BOLD, WHITE);
            text_draw(s, bk.x + (bk.w - text_width(l2, TEXT_BOLD)) / 2, bk.y + (bk.h - text_height(TEXT_BOLD)) / 2, l2, TEXT_BOLD, WHITE);
        }
    }
    int by = y0 + 3 * (kh + gap), mode_w = 150, join_w = 190;
    struct gfx_rect mode = { 14, by, mode_w, kh }, sp = { 14 + mode_w + gap, by, w - 28 - mode_w - join_w - 2 * gap, kh },
                    jn = { w - 14 - join_w, by, join_w, kh };
    su.keys[n].r = mode; su.keys[n++].c = 3;
    su.keys[n].r = sp; su.keys[n++].c = ' ';
    su.keys[n].r = jn; su.keys[n++].c = 4;
    su.nkeys = n;
    gfx_blend_round_rect(s, mode.x, mode.y, mode.w, mode.h, 16, WHITE, 20);
    gfx_blend_round_rect(s, sp.x, sp.y, sp.w, sp.h, 16, WHITE, 38);
    gfx_blend_round_rect(s, jn.x, jn.y, jn.w, jn.h, 16, t->accent_bg, 240);
    text_draw(s, mode.x + (mode.w - text_width(su.kb_sym ? "abc" : "123", TEXT_BOLD)) / 2, mode.y + (mode.h - text_height(TEXT_BOLD)) / 2,
              su.kb_sym ? "abc" : "123", TEXT_BOLD, WHITE);
    const char *act = su.kb_act ? su.kb_act : "Join";
    text_draw(s, jn.x + (jn.w - text_width(act, TEXT_BOLD)) / 2, jn.y + (jn.h - text_height(TEXT_BOLD)) / 2, act, TEXT_BOLD, t->accent_fg);
}

static void draw_chrome(struct gfx_surface *s, int page)
{
    if (page == P_HELLO)
        return;
    su.back = (struct gfx_rect){ 14, 44, 96, 96 };
    int cx = 62, cy = 92;
    gfx_thick_line(s, cx + 8, cy - 18, cx - 10, cy, 5, WHITE);
    gfx_thick_line(s, cx - 10, cy, cx + 8, cy + 18, 5, WHITE);
    int n = 0;                                                               /* the progress: a wide dot for this page */
    for (int i = 1; i < P_COUNT; i++)
        n += !hidden(i);
    int x = (s->w - (n * 24 + 30)) / 2;
    for (int i = 1; i < P_COUNT; i++) {
        if (hidden(i))
            continue;
        int cur = i == page;
        gfx_fill_round_rect(s, x, 84, cur ? 44 : 14, 14, 7, cur ? WHITE : gfx_mix(WHITE, 0x000000u, 90));
        x += (cur ? 44 : 14) + 10;
    }
}

static void secondary(struct gfx_surface *s, struct gfx_rect r, const char *label)
{
    gfx_blend_round_rect(s, r.x, r.y, r.w, r.h, r.h / 2, WHITE, 30);
    text_draw(s, r.x + (r.w - text_width(label, TEXT_BOLD)) / 2, r.y + (r.h - text_height(TEXT_BOLD)) / 2, label, TEXT_BOLD, WHITE);
}

static void draw_page(struct gfx_surface *s, int page)
{
    gfx_blit(s, 0, 0, &su.bg, 0, 0, s->w, s->h);
    int w = s->w, h = s->h;
    const struct theme *t = theme_get();
    su.btn = (struct gfx_rect){ (w - 460) / 2, h - 250, 460, 108 };
    memset(su.rows, 0, sizeof(su.rows));
    if (page == P_HELLO) {
        centred(s, h - 370, "Welcome to LiteKern Go", TEXT_BODY, 0xd8d8d8u);
        button(s, su.btn, "Get started", 235);
        return;
    }
    if (page == P_WIFI) {
        draw_chrome(s, page);
        if (su.entering) {                                  /* the password */
            centred(s, 190, "Enter the password", TEXT_HEADING, WHITE);
            centred(s, 252, su.nets[su.sel].ssid, TEXT_BODY, 0xc8c8c8u);
            su.field = (struct gfx_rect){ 44, 330, w - 88 - 150, 100 };
            su.showbtn = (struct gfx_rect){ w - 44 - 140, 330, 140, 100 };
            gfx_blend_round_rect(s, su.field.x, su.field.y, su.field.w, su.field.h, 28, WHITE, 36);
            secondary(s, su.showbtn, su.show_pw ? "Hide" : "Show");
            char dots[72];
            int n = su.pw_len < 40 ? su.pw_len : 40, k = 0;
            for (int i = 0; i < n; i++)
                dots[k++] = su.show_pw ? su.pw[su.pw_len - n + i] : '*';
            dots[k] = 0;
            if (k)
                text_draw_fit(s, su.field.x + 28, su.field.y + (su.field.h - text_height(TEXT_BODY)) / 2, dots, su.field.w - 56, TEXT_BODY, WHITE);
            else
                text_draw(s, su.field.x + 28, su.field.y + (su.field.h - text_height(TEXT_BODY)) / 2, "Password", TEXT_BODY, 0x909090u);
            if (su.join_bad)
                centred(s, 460, "That password didn't work. Try again.", TEXT_SMALL, 0xff8f7au);
            else if (su.joining) {
                centred(s, 460, "Connecting...", TEXT_SMALL, 0xc8c8c8u);
                spinner(s, w / 2 - text_width("Connecting...", TEXT_SMALL) / 2 - 30, 460 + text_height(TEXT_SMALL) / 2, 14, WHITE);
            }
            su.kb_act = "Join";
            draw_keyboard(s);
            return;
        }
        centred(s, 190, "Connect to Wi-Fi", TEXT_HEADING, WHITE);
        centred(s, 252, "To get weather, music and the web", TEXT_SMALL, 0xc8c8c8u);
        su.rescan = (struct gfx_rect){ w - 44 - 170, 300, 170, 64 };
        secondary(s, su.rescan, "Rescan");
        text_draw(s, 44, 314, su.nnets ? "Networks" : "No networks found", TEXT_BOLD, 0xe0e0e0u);
        for (int i = 0; i < su.nnets && i < 6; i++) {
            struct gfx_rect r = { 36, 390 + i * 104, w - 72, 92 };
            su.rows[i] = r;
            int me = su.joined && !strcmp(su.nets[i].ssid, su.joined_ssid);
            gfx_blend_round_rect(s, r.x, r.y, r.w, r.h, 26, me ? t->accent_bg : WHITE, me ? 170 : 28);
            text_draw_fit(s, r.x + 28, r.y + (r.h - text_height(TEXT_BODY)) / 2, su.nets[i].ssid, r.w - 200, TEXT_BODY, WHITE);
            bars(s, r.x + r.w - 76, r.y + 28, su.nets[i].signal, WHITE);
            if (su.joining && i == su.sel)
                spinner(s, r.x + r.w - 190, r.y + r.h / 2, 18, WHITE);
            if (su.nets[i].secure)
                lock_icon(s, r.x + r.w - 124, r.y + 30, 0xe0e0e0u);
        }
        if (su.joining)
            centred(s, 390 + 6 * 104 + 6, "Connecting...", TEXT_SMALL, 0xc8c8c8u);
        else if (su.joined)
            centred(s, 390 + 6 * 104 + 6, "Connected", TEXT_SMALL, 0xa6f0b8u);
        if (su.joined)
            button(s, su.btn, "Continue", 235);
        else
            secondary(s, su.btn, "Skip for now");
        return;
    }
    if (page == P_ZONE) {
        int top = 290, bot = h - 270, rh = 92, n = LKX_ZONES, total = n * rh;
        su.zscroll = su.zscroll < 0 ? 0 : su.zscroll > total - (bot - top) ? total - (bot - top) : su.zscroll;
        for (int i = 0; i < n; i++) {
            int y = top + i * rh - su.zscroll;
            if (y + rh < top || y > bot)
                continue;
            struct gfx_rect r = { 36, y, w - 72, rh - 10 };
            gfx_blend_round_rect(s, r.x, r.y, r.w, r.h, 26, i == su.zone ? t->accent_bg : WHITE, i == su.zone ? 210 : 26);
            text_draw(s, r.x + 30, r.y + (r.h - text_height(TEXT_BODY)) / 2, lkx_zones[i].name, TEXT_BODY, i == su.zone ? t->accent_fg : WHITE);
        }
        gfx_blit(s, 0, 0, &su.bg, 0, 0, w, top);                        /* the list scrolls under the header and the button */
        gfx_blit(s, 0, bot, &su.bg, 0, bot, w, h - bot);
        draw_chrome(s, page);
        centred(s, 190, "Where are you?", TEXT_HEADING, WHITE);
        centred(s, 246, "So your clock shows the right time", TEXT_SMALL, 0xc8c8c8u);
        button(s, su.btn, "Continue", 235);
        return;
    }
    if (page == P_PRIVACY) {
        draw_chrome(s, page);
        int cx = w / 2, cy = 360;
        gfx_fill_circle(s, cx, cy, 96, t->accent_bg);
        gfx_fill_round_rect(s, cx - 38, cy - 8, 76, 56, 12, t->accent_fg);
        gfx_rect_outline(s, cx - 24, cy - 48, 48, 48, t->accent_fg);
        gfx_rect_outline(s, cx - 23, cy - 47, 46, 46, t->accent_fg);
        gfx_rect_outline(s, cx - 22, cy - 46, 44, 44, t->accent_fg);
        centred(s, 500, "Your phone, your data", TEXT_HEADING, WHITE);
        static const char *const lines[3][2] = {
            { "Nothing is sent anywhere", "unless you send it yourself" },
            { "No accounts, no tracking", "and no ads or analytics" },
            { "Photos and files", "stay on this phone" } };
        struct gfx_rect c = { 56, 600, w - 112, 3 * 112 + 24 };
        gfx_blend_round_rect(s, c.x, c.y, c.w, c.h, 32, WHITE, 30);
        for (int i = 0; i < 3; i++) {
            int y = c.y + 22 + i * 112;
            gfx_fill_circle(s, c.x + 54, y + 50, 10, t->accent_bg);
            text_draw(s, c.x + 90, y + 14, lines[i][0], TEXT_BOLD, WHITE);
            text_draw(s, c.x + 90, y + 54, lines[i][1], TEXT_SMALL, 0xc8c8c8u);
        }
        button(s, su.btn, "Continue", 235);
        return;
    }
    if (page == P_NAME) {
        draw_chrome(s, page);
        centred(s, 190, "What's your name?", TEXT_HEADING, WHITE);
        centred(s, 252, "It goes on your lock screen", TEXT_SMALL, 0xc8c8c8u);
        su.field = (struct gfx_rect){ 44, 330, w - 88, 100 };
        su.showbtn = (struct gfx_rect){ 0, 0, 0, 0 };
        gfx_blend_round_rect(s, su.field.x, su.field.y, su.field.w, su.field.h, 28, WHITE, 36);
        if (su.name_len)
            text_draw_fit(s, su.field.x + 28, su.field.y + (su.field.h - text_height(TEXT_BODY)) / 2, su.name, su.field.w - 56, TEXT_BODY, WHITE);
        else
            text_draw(s, su.field.x + 28, su.field.y + (su.field.h - text_height(TEXT_BODY)) / 2, "Name", TEXT_BODY, 0x909090u);
        su.kb_act = "Next";
        draw_keyboard(s);
        return;
    }
    if (page == P_PIN) {
        draw_chrome(s, page);
        centred(s, 190, "Lock your phone?", TEXT_HEADING, WHITE);
        centred(s, 252, "Choose a PIN of 4 to 8 digits, or use no lock", TEXT_SMALL, 0xc8c8c8u);
        int dx = (w - (8 * 52)) / 2;
        for (int i = 0; i < 8; i++)
            gfx_fill_circle(s, dx + i * 52 + 26, 340, 12, i < su.pin_len ? WHITE : gfx_mix(WHITE, 0x000000u, 80));
        static const char pc[12] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', 0, '0', 2 };
        int kw = 170, kh = 96, gx = 24, gy = 16, x0 = (w - 3 * kw - 2 * gx) / 2, y0 = 410;
        for (int i = 0; i < 12; i++) {
            su.padc[i] = pc[i];
            if (!pc[i]) { su.pad[i] = (struct gfx_rect){ 0, 0, 0, 0 }; continue; }
            struct gfx_rect k = { x0 + (i % 3) * (kw + gx), y0 + (i / 3) * (kh + gy), kw, kh };
            su.pad[i] = k;
            gfx_blend_round_rect(s, k.x, k.y, k.w, k.h, kh / 2, WHITE, pc[i] == 2 ? 18 : 36);
            char one[2] = { pc[i], 0 };
            if (pc[i] == 2) {
                gfx_thick_line(s, k.x + k.w / 2 + 14, k.y + kh / 2 - 14, k.x + k.w / 2 - 14, k.y + kh / 2, 4, WHITE);
                gfx_thick_line(s, k.x + k.w / 2 - 14, k.y + kh / 2, k.x + k.w / 2 + 14, k.y + kh / 2 + 14, 4, WHITE);
            } else {
                text_draw(s, k.x + (k.w - text_width(one, TEXT_HEADING)) / 2, k.y + (k.h - text_height(TEXT_HEADING)) / 2, one, TEXT_HEADING, WHITE);
            }
        }
        button(s, su.btn, "Set PIN", su.pin_len >= 4 ? 235 : 90);
        su.skip = (struct gfx_rect){ (w - 340) / 2, h - 124, 340, 80 };
        text_draw(s, su.skip.x + (su.skip.w - text_width("Use no lock", TEXT_BOLD)) / 2, su.skip.y + (su.skip.h - text_height(TEXT_BOLD)) / 2, "Use no lock", TEXT_BOLD, 0xe0e0e0u);
        return;
    }
    if (page == P_LOOK) {
        draw_chrome(s, page);
        centred(s, 190, "Choose your look", TEXT_HEADING, WHITE);
        centred(s, 246, "You can change it later in Settings", TEXT_SMALL, 0xc8c8c8u);
        int cw = (w - 3 * 56) / 2, ch = cw * 14 / 10;
        for (int i = 0; i < 2; i++) {
            su.card[i] = (struct gfx_rect){ 56 + i * (cw + 56), 320, cw, ch };
            mock(s, su.card[i], i, su.light == i);
            const char *l = i ? "Light" : "Dark";
            text_draw(s, su.card[i].x + (cw - text_width(l, TEXT_BOLD)) / 2, 320 + ch + 26, l, TEXT_BOLD, WHITE);
        }
        int y = 320 + ch + 104, n = su.naccents, d = 64, gap = imin(28, (w - 80 - n * d) / imax(1, n - 1));
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

static void show_now(int page)
{
    draw_page(&su.page_pic, page);
    gfx_blit(S(), 0, 0, &su.page_pic, 0, 0, S()->w, S()->h);
    screen_damage_all();
    screen_present();
}

static void go(int page)
{
    gfx_blit(&su.prev_pic, 0, 0, S(), 0, 0, S()->w, S()->h);   /* what shows now, to fade from */
    su.from_page = su.page;
    su.page = page;
    su.page_at = uptime_ms();
    if (page == P_WIFI) {
        su.entering = 0;
        wifi_scan();
    }
    if (page == P_NAME && !su.name_len && users_count() > 0) {          /* the name it has now, to change */
        const struct user *u = users_get(0);
        if (u)
            for (su.name_len = 0; u->name[su.name_len] && su.name_len < 24; su.name_len++)
                su.name[su.name_len] = u->name[su.name_len];
        su.name[su.name_len] = 0;
    }
    if (page == P_PIN)
        su.pin_len = 0;
    draw_page(&su.page_pic, page);
}

static void apply_account(void)
{
    if (!su.first)
        return;
    if (users_count() == 0) {
        if (users_add(su.name_len && !users_bad_name(su.name) ? su.name : "User", su.pin_len >= 4 ? su.pin : "", 1) >= 0)
            users_set_current(0);
        return;
    }
    if (su.name_len && !users_bad_name(su.name))
        users_rename(0, su.name);
    if (su.pin_len >= 4)
        users_set_password(0, su.pin);
    users_set_current(0);                              /* you stay logged in: this is your first start */
}

static void finish(void)
{
    apply_account();
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

void setup_start(int first)
{
    if (buffer(&su.bg) || buffer(&su.page_pic) || buffer(&su.prev_pic) || buffer(&su.tmp)) {
        kprintf("setup: out of memory; skipped\n");
        return;
    }
    /* the scratch holds a whole word at 240 px ("bonjour" is about 900 wide): the screen's pixels, shaped wide */
    su.tmp.w = su.tmp.stride = 2048;
    su.tmp.h = (int)((uint32_t)S()->w * (uint32_t)S()->h / 2048);
    su.first = first;
    su.name_len = su.pin_len = 0;
    su.name[0] = su.pin[0] = 0;
    su.light = theme_is_light();
    su.zone = 0;
    su.zscroll = 0;
    su.entering = su.joining = su.joined = su.join_bad = 0;
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
    if (su.page == P_WIFI && su.joining && (int32_t)(now - su.spin_at) >= 80 && now - su.page_at >= FADE_MS) {
        su.spin_at = now;                           /* the loader turns */
        show_now(P_WIFI);
    }
    if (su.page == P_WIFI && su.joining && (int32_t)(now - su.poll_at) >= 600) {
        su.poll_at = now;
        wifi_poll();
        if (su.joined || su.join_bad) {
            if (su.joined)
                su.entering = 0;
            show_now(P_WIFI);
        }
    }
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

static void next_from_name(void);

static void key_press(char c)
{
    if (su.page == P_NAME) {
        if (c == 1) {
            su.kb_shift = !su.kb_shift;
        } else if (c == 2) {
            if (su.name_len > 0)
                su.name[--su.name_len] = 0;
        } else if (c == 3) {
            su.kb_sym = !su.kb_sym;
        } else if (c == 4) {
            next_from_name();
        } else if (su.name_len < 24) {
            su.name[su.name_len++] = c;
            su.name[su.name_len] = 0;
            if (su.kb_shift && !su.kb_sym)
                su.kb_shift = 0;
        }
        return;
    }
    if (c == 1) {                                       /* shift */
        su.kb_shift = !su.kb_shift;
    } else if (c == 2) {                                /* backspace */
        if (su.pw_len > 0)
            su.pw[--su.pw_len] = 0;
    } else if (c == 3) {                                /* 123 / abc */
        su.kb_sym = !su.kb_sym;
    } else if (c == 4) {                                /* join */
        if (su.pw_len > 0) {
            su.joining = 1;
            su.join_bad = 0;
            su.poll_at = uptime_ms();
            net_join(su.nets[su.sel].ssid, su.pw);
        }
    } else if (su.pw_len < 63) {
        su.pw[su.pw_len++] = c;
        su.pw[su.pw_len] = 0;
        if (su.kb_shift && !su.kb_sym)
            su.kb_shift = 0;
    }
    su.join_bad = c == 4 ? su.join_bad : 0;
}

static void next_from_name(void)
{
    go(step(P_NAME, 1));
}

static void tap(int x, int y)
{
    if (uptime_ms() - su.page_at < FADE_MS)
        return;
    if (su.page != P_HELLO && su.page != P_DONE && inside(su.back, x, y)) {
        if (su.page == P_WIFI && su.entering) {
            su.entering = 0;
            show_now(P_WIFI);
        } else {
            go(step(su.page, -1));
        }
        return;
    }
    if (su.page == P_HELLO) {
        if (inside(su.btn, x, y) || uptime_ms() - su.start_at > 1500)
            go(P_WIFI);
        return;
    }
    if (su.page == P_WIFI) {
        if (su.entering) {
            if (inside(su.showbtn, x, y))
                su.show_pw = !su.show_pw;
            else
                for (int i = 0; i < su.nkeys; i++)
                    if (inside(su.keys[i].r, x, y)) {
                        key_press(su.keys[i].c);
                        break;
                    }
            show_now(P_WIFI);
            if (su.joined && !su.joining) {
                su.entering = 0;
                show_now(P_WIFI);
            }
            return;
        }
        for (int i = 0; i < 6 && i < su.nnets; i++)
            if (inside(su.rows[i], x, y)) {
                su.sel = i;
                su.pw_len = 0;
                su.pw[0] = 0;
                su.join_bad = 0;
                if (su.nets[i].secure && !su.nets[i].known) {
                    su.entering = 1;
                } else {
                    su.joining = 1;
                    su.poll_at = uptime_ms();
                    net_join(su.nets[i].ssid, "");
                }
                show_now(P_WIFI);
                return;
            }
        if (inside(su.rescan, x, y)) {
            wifi_scan();
            show_now(P_WIFI);
            return;
        }
        if (inside(su.btn, x, y))
            go(P_ZONE);
        return;
    }
    if (su.page == P_ZONE) {
        if (inside(su.btn, x, y)) {
            go(step(P_ZONE, 1));
            return;
        }
        for (int i = 0; i < LKX_ZONES; i++) {
            int yy = 290 + i * 92 - su.zscroll;
            if (y >= 290 && y < S()->h - 270 && y >= yy && y < yy + 82 && x >= 36 && x < S()->w - 36) {
                su.zone = i;
                machine_set_timezone(i);
                show_now(P_ZONE);
                return;
            }
        }
        return;
    }
    if (su.page == P_PRIVACY) {
        if (inside(su.btn, x, y))
            go(step(P_PRIVACY, 1));
        return;
    }
    if (su.page == P_NAME) {
        for (int i = 0; i < su.nkeys; i++)
            if (inside(su.keys[i].r, x, y)) {
                key_press(su.keys[i].c);
                if (su.page == P_NAME)
                    show_now(P_NAME);
                break;
            }
        return;
    }
    if (su.page == P_PIN) {
        for (int i = 0; i < 12; i++)
            if (su.padc[i] && inside(su.pad[i], x, y)) {
                if (su.padc[i] == 2) {
                    if (su.pin_len > 0)
                        su.pin[--su.pin_len] = 0;
                } else if (su.pin_len < 8) {
                    su.pin[su.pin_len++] = su.padc[i];
                    su.pin[su.pin_len] = 0;
                }
                show_now(P_PIN);
                return;
            }
        if (inside(su.btn, x, y) && su.pin_len >= 4)
            go(step(P_PIN, 1));
        else if (inside(su.skip, x, y)) {
            su.pin_len = 0;
            su.pin[0] = 0;
            go(step(P_PIN, 1));
        }
        return;
    }
    if (su.page == P_LOOK) {
        for (int i = 0; i < 2; i++)
            if (inside(su.card[i], x, y) && su.light != i) {
                su.light = i;
                theme_set(su.light, su.accent);
                show_now(P_LOOK);
                return;
            }
        for (int i = 0; i < su.naccents; i++) {
            struct gfx_rect r = su.dot[i];
            r.x -= 10; r.y -= 10; r.w += 20; r.h += 20;
            if (inside(r, x, y) && su.accent != i) {
                su.accent = i;
                theme_set(su.light, su.accent);
                show_now(P_LOOK);
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
        su.zlast_y = y;
        su.zmoved = 0;
    } else if (down && su.page == P_ZONE && y > 290 && y < S()->h - 270) {          /* dragging the list */
        int dy = y - su.zlast_y;
        if (su.zmoved || dy * dy > 20 * 20) {
            su.zmoved = 1;
            su.zscroll -= dy;
            su.zlast_y = y;
            show_now(P_ZONE);
        }
    } else if (!down && su.down) {
        int dx = x - su.down_x, dy = y - su.down_y;
        if (!su.zmoved && dx * dx + dy * dy < 40 * 40)
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
    if (su.page == P_WIFI && su.entering && key != KEY_ESC && key != KEY_PHONE_BACK)
        return;                                  /* (typing is the on-screen keys) */
    if (key == KEY_ENTER || key == KEY_SPACE) {
        if (su.page == P_DONE)
            finish();
        else if (su.page != P_NAME && su.page != P_PIN)
            go(step(su.page, 1));
    } else if ((key == KEY_ESC || key == KEY_PHONE_BACK) && su.page > P_HELLO) {
        if (su.page == P_WIFI && su.entering) {
            su.entering = 0;
            show_now(P_WIFI);
        } else {
            go(step(su.page, -1));
        }
    }
}
