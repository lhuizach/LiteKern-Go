#include "kernel/osk.h"
#include "kernel/keymap.h"
#include "kernel/printk.h"
#include "kernel/screen.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/timing.h"
#include "kernel/wm.h"

#define ROWS 4
#define KEY_H 96
#define GAP 10                  /* between keys, and around them */
#define OSK_H (ROWS * KEY_H + (ROWS + 1) * GAP)
#define PREVIEW_H 104           /* the pressed key's letter, shown above the finger */
#define REPEAT_DELAY_MS 500
#define REPEAT_MS 60
#define DOUBLE_SHIFT_MS 350     /* Shift twice this quickly: caps lock */
#define SLIDE_MS 180            /* up from the bottom edge, and back down */

enum kind { K_CHAR, K_SHIFT, K_BACK, K_PAGE, K_SPACE, K_ENTER };

struct key {
    const char *label;          /* letters: lower case (drawn upper case when shifted) */
    uint16_t sc;                /* its scancode (kernel/input.h): the layout gives the character */
    uint8_t shift;              /* typed with Shift (symbols) */
    uint8_t w;                  /* width in half keys */
    uint8_t kind;
};

#define C(l, sc) { l, sc, 0, 2, K_CHAR }
#define CS(l, sc) { l, sc, 1, 2, K_CHAR }
#define END { 0, 0, 0, 0, 0 }

/* Two pages, four rows each (US QWERTY scancodes). */
static const struct key pages[2][ROWS][11] = {
    {   /* letters */
        { C("q", 0x10), C("w", 0x11), C("e", 0x12), C("r", 0x13), C("t", 0x14), C("y", 0x15), C("u", 0x16),
          C("i", 0x17), C("o", 0x18), C("p", 0x19), END },
        { C("a", 0x1e), C("s", 0x1f), C("d", 0x20), C("f", 0x21), C("g", 0x22), C("h", 0x23), C("j", 0x24),
          C("k", 0x25), C("l", 0x26), END },
        { { "Shift", 0, 0, 3, K_SHIFT }, C("z", 0x2c), C("x", 0x2d), C("c", 0x2e), C("v", 0x2f), C("b", 0x30),
          C("n", 0x31), C("m", 0x32), { "Del", KEY_BACKSPACE, 0, 3, K_BACK }, END },
        { { "123", 0, 0, 4, K_PAGE }, C(",", 0x33), { "space", KEY_SPACE, 0, 8, K_SPACE }, C(".", 0x34),
          { "Enter", KEY_ENTER, 0, 4, K_ENTER }, END },
    },
    {   /* numbers and symbols */
        { C("1", 0x02), C("2", 0x03), C("3", 0x04), C("4", 0x05), C("5", 0x06), C("6", 0x07), C("7", 0x08),
          C("8", 0x09), C("9", 0x0a), C("0", 0x0b), END },
        { C("-", 0x0c), C("/", 0x35), CS(":", 0x27), C(";", 0x27), CS("(", 0x0a), CS(")", 0x0b), CS("$", 0x05),
          CS("&", 0x08), CS("@", 0x03), CS("\"", 0x28), END },
        { CS("#", 0x04), CS("%", 0x06), CS("?", 0x35), CS("!", 0x02), C("'", 0x28), CS("+", 0x0d), C("=", 0x0d),
          CS("_", 0x0c), { "Del", KEY_BACKSPACE, 0, 4, K_BACK }, END },
        { { "ABC", 0, 0, 4, K_PAGE }, C(",", 0x33), { "space", KEY_SPACE, 0, 8, K_SPACE }, C(".", 0x34),
          { "Enter", KEY_ENTER, 0, 4, K_ENTER }, END },
    },
};

static int shown, page, mode;
static int leaving;             /* hidden, still sliding down out of sight */
static uint32_t slide_ms;       /* when the slide began */
static int shift;               /* 0 off, 1 for the next letter, 2 caps lock */
static uint32_t shift_ms;       /* when Shift was last tapped */
static int held_row = -1, held_col = -1;    /* the key under the finger */
static uint32_t held_ms, repeat_at;

static struct gfx_surface *S(void) { return screen_surface(); }
static const struct theme *T(void) { return theme_get(); }

static int top(void) { return S()->h - OSK_H; }

static int row_units(int r)
{
    int n = 0;
    for (int c = 0; pages[page][r][c].label; c++)
        n += pages[page][r][c].w;
    return n;
}

static struct gfx_rect key_rect(int r, int c)
{
    int unit2 = (S()->w - GAP) / 20;        /* half a key, gap included */
    int x = (S()->w - row_units(r) * unit2) / 2 + GAP / 2;
    for (int k = 0; k < c; k++)
        x += pages[page][r][k].w * unit2;
    const struct key *key = &pages[page][r][c];
    return (struct gfx_rect){ x + GAP / 2, top() + GAP + r * (KEY_H + GAP), key->w * unit2 - GAP, KEY_H };
}

static int key_at(int x, int y, int *row, int *col)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; pages[page][r][c].label; c++) {
            struct gfx_rect k = key_rect(r, c);
            /* the gaps belong to the nearest key, so no tap falls between */
            if (x >= k.x - GAP / 2 && x < k.x + k.w + GAP / 2 && y >= k.y - GAP / 2 && y < k.y + k.h + GAP / 2) {
                *row = r;
                *col = c;
                return 1;
            }
        }
    return 0;
}

int osk_shown(void) { return shown; }
int osk_mode(void) { return mode; }
int osk_height(void) { return shown ? OSK_H : 0; }

struct gfx_rect osk_bounds(void)
{
    return (struct gfx_rect){ 0, top() - PREVIEW_H, S()->w, OSK_H + PREVIEW_H };
}

void osk_show(int show)
{
    if (mode == show)
        return;
    mode = show;
    page = show == 2;                   /* a PIN: the numbers first */
    if (shown == !!show) {
        shell_damage(osk_bounds());
        return;
    }
    shown = !!show;
    leaving = !shown;
    slide_ms = uptime_ms();
    shift = 0;
    held_row = held_col = -1;
    kprintf("osk: %s\n", shown ? "shown" : "hidden");
    shell_damage(osk_bounds());
}

static void send(uint16_t sc, int with_shift)
{
    struct key_event k;
    if (with_shift) {
        k = keymap_event(KEY_LSHIFT, 1);
        shell_osk_key(&k);
    }
    k = keymap_event(sc, 1);
    shell_osk_key(&k);
    k = keymap_event(sc, 0);
    shell_osk_key(&k);
    if (with_shift) {
        k = keymap_event(KEY_LSHIFT, 0);
        shell_osk_key(&k);
    }
}

static int is_letter(const struct key *k)
{
    return k->kind == K_CHAR && k->label[0] >= 'a' && k->label[0] <= 'z' && !k->label[1];
}

static void press(const struct key *k)
{
    switch (k->kind) {
    case K_CHAR: {
        int up = is_letter(k) && shift;
        send(k->sc, k->shift || up);
        kprintf("osk: key %s%s\n", k->label, up ? " (shift)" : "");
        if (shift == 1 && is_letter(k))
            shift = 0;                      /* one capital */
        break;
    }
    case K_SPACE:
    case K_ENTER:
        send(k->sc, 0);
        kprintf("osk: key %s\n", k->label);
        break;
    case K_BACK:
        send(KEY_BACKSPACE, 0);
        break;
    case K_SHIFT: {
        uint32_t now = uptime_ms();
        if (shift == 1 && now - shift_ms <= DOUBLE_SHIFT_MS)
            shift = 2;                      /* twice quickly: caps lock */
        else
            shift = shift ? 0 : 1;
        shift_ms = now;
        kprintf("osk: shift %s\n", shift == 2 ? "locked" : shift ? "on" : "off");
        break;
    }
    case K_PAGE:
        page = !page;
        kprintf("osk: page %s\n", page ? "123" : "ABC");
        break;
    }
}

int osk_pointer(int x, int y, uint8_t buttons)
{
    static uint8_t last;
    int down = (buttons & MOUSE_LEFT) && !(last & MOUSE_LEFT);
    int up = !(buttons & MOUSE_LEFT) && (last & MOUSE_LEFT);
    last = buttons;
    if (!shown)
        return 0;
    if (held_row < 0 && (y < top() || !down))
        return y >= top();              /* the keyboard's own area: nothing under it */
    int r, c;
    int on = key_at(x, y, &r, &c);
    shell_damage(osk_bounds());
    if (down) {
        if (!on)
            return 1;
        held_row = r;
        held_col = c;
        held_ms = uptime_ms();
        repeat_at = held_ms + REPEAT_DELAY_MS;
        if (pages[page][r][c].kind == K_BACK)
            press(&pages[page][r][c]);  /* at once, then it repeats */
        return 1;
    }
    if (held_row < 0)
        return 1;
    if (up || !(buttons & MOUSE_LEFT)) {
        /* Keys type when the finger lifts, on the key it went down on (a
         * slide to the next key types that one instead, as phones do). */
        const struct key *k = on ? &pages[page][r][c] : 0;
        held_row = held_col = -1;
        if (k && k->kind != K_BACK)
            press(k);
        return 1;
    }
    if (on && (r != held_row || c != held_col)) {
        held_row = r;
        held_col = c;
        repeat_at = uptime_ms() + REPEAT_DELAY_MS;
    }
    return 1;
}

/* How far down it is, sliding: 0 in place, OSK_H out of sight. Eased out. */
static int slide_off(void)
{
    uint32_t el = uptime_ms() - slide_ms;
    if (el >= SLIDE_MS)
        return leaving ? OSK_H : 0;
    int t = (int)(el * 1024 / SLIDE_MS), u = 1024 - t;
    int e = 1024 - (int)((int64_t)u * u / 1024 * u / 1024);     /* 0..1024, fast then settling */
    return leaving ? OSK_H * e / 1024 : OSK_H - OSK_H * e / 1024;
}

int osk_moving(void)
{
    return (shown || leaving) && uptime_ms() - slide_ms < SLIDE_MS + 20;
}

void osk_tick(void)
{
    if (shown || leaving) {
        uint32_t el = uptime_ms() - slide_ms;
        if (el < SLIDE_MS + 20) {
            shell_damage((struct gfx_rect){ 0, top() - PREVIEW_H, S()->w, OSK_H + PREVIEW_H });
            shell_flush();
        } else if (leaving) {
            leaving = 0;                    /* all the way down: gone */
            shell_damage(osk_bounds());
            shell_flush();
        }
    }
    if (!shown || held_row < 0 || pages[page][held_row][held_col].kind != K_BACK)
        return;
    uint32_t now = uptime_ms();
    if ((int32_t)(now - repeat_at) >= 0) {
        send(KEY_BACKSPACE, 0);
        repeat_at = now + REPEAT_MS;
    }
}

static void label_of(const struct key *k, char out[8])
{
    int n = 0;
    for (const char *p = k->label; *p && n < 7; p++)
        out[n++] = is_letter(k) && shift ? (char)(*p - 32) : *p;
    out[n] = '\0';
}

void osk_draw(struct gfx_surface *dst, int ox, int oy)
{
    if (!shown && !leaving)
        return;
    oy -= slide_off();                  /* drawn lower while it slides */
    const struct theme *t = T();
    uint32_t bg = t->headerbar_bg;
    gfx_fill_rect(dst, 0 - ox, top() - oy, S()->w, OSK_H, bg);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; pages[page][r][c].label; c++) {
            const struct key *k = &pages[page][r][c];
            struct gfx_rect kr = key_rect(r, c);
            int held = r == held_row && c == held_col;
            int special = k->kind != K_CHAR && k->kind != K_SPACE;
            uint32_t face = held ? t->button_active
                          : k->kind == K_SHIFT && shift ? t->accent_bg
                          : special ? t->button_bg : t->view_bg;
            gfx_fill_round_rect(dst, kr.x - ox, kr.y - oy, kr.w, kr.h, t->radius, face);
            char text[8];
            label_of(k, text);
            if (k->kind == K_SHIFT && shift == 2)
                text[0] = 'S', text[1] = 'H', text[2] = 'I', text[3] = 'F', text[4] = 'T', text[5] = '\0';
            if (k->kind == K_SPACE)
                text[0] = '\0';
            enum text_style st = k->kind == K_CHAR ? TEXT_BODY : TEXT_SMALL;
            uint32_t fg = k->kind == K_SHIFT && shift ? t->accent_fg : t->fg;
            text_draw(dst, kr.x + (kr.w - text_width(text, st)) / 2 - ox,
                      kr.y + (kr.h - text_height(st)) / 2 - oy, text, st, fg);
        }
    /* The pressed letter, big, above the finger. */
    if (held_row >= 0) {
        const struct key *k = &pages[page][held_row][held_col];
        if (k->kind == K_CHAR) {
            struct gfx_rect kr = key_rect(held_row, held_col);
            struct gfx_rect p = { kr.x - 12, kr.y - PREVIEW_H, kr.w + 24, PREVIEW_H - 8 };
            if (p.x < 0)
                p.x = 0;
            if (p.x + p.w > S()->w)
                p.x = S()->w - p.w;
            char text[8];
            label_of(k, text);
            gfx_fill_round_rect(dst, p.x - ox, p.y - oy, p.w, p.h, t->radius * 2, t->dialog_bg);
            text_draw(dst, p.x + (p.w - text_width(text, TEXT_HEADING)) / 2 - ox,
                      p.y + (p.h - text_height(TEXT_HEADING)) / 2 - oy, text, TEXT_HEADING, t->fg);
        }
    }
}
