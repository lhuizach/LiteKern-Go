/* LiteKern X — Calculator, a KERN86 app (Phase 3 §3): GNOME Calculator's
 * basic mode, kept small.
 *
 * Apps have no floating point (-mgeneral-regs-only), so numbers are whole
 * numbers of millionths: exact for six decimal places, up to 11 digits
 * before the point. Anything beyond, or a division by zero, shows "Error"
 * instead of a wrong answer. Keyboard: digits, '.', + - * / %, Enter or '='
 * to work it out, Backspace, Delete or C to clear, N to change the sign.
 * LiteKern Go: the keys fill the phone's screen under a big display, and
 * there's a Backspace key (the top row's second); the phone's Back closes it.
 * Results are logged ("user: calculator: 12 + 30 = 42") for the tests. */
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"

#define SCALE   1000000LL                   /* millionths */
#define LIMIT   (99999999999LL * SCALE)     /* 11 digits before the point */
#define DIGITS  11

#define COLS 4
#define ROWS 5
#define GAP 16
#define MARGIN 24

static struct k86_window win;
static struct gfx_surface canvas;

static int64_t acc;             /* the left-hand number */
static char op;                 /* '+', '-', '*', '/', or 0 */
static char entry[32];          /* what's being typed ("" = nothing yet) */
static int64_t shown;           /* the number shown when nothing is typed */
static int error;
static char expr[80];           /* the line above: "12 +" or "12 + 30 =" */

struct key {
    const char *label;
    char action;                /* the character the key types */
    int col, row, span;
    enum wg_button_style style;
};

static const struct key keys[] = {
    { "C", 'c', 0, 0, 1, WG_BUTTON_DESTRUCTIVE },
    { "Del", '\b', 1, 0, 1, WG_BUTTON_NORMAL },
    { "%", '%', 2, 0, 1, WG_BUTTON_NORMAL },
    { TEXT_DIVIDE, '/', 3, 0, 1, WG_BUTTON_NORMAL },
    { "7", '7', 0, 1, 1, WG_BUTTON_NORMAL }, { "8", '8', 1, 1, 1, WG_BUTTON_NORMAL },
    { "9", '9', 2, 1, 1, WG_BUTTON_NORMAL }, { TEXT_TIMES, '*', 3, 1, 1, WG_BUTTON_NORMAL },
    { "4", '4', 0, 2, 1, WG_BUTTON_NORMAL }, { "5", '5', 1, 2, 1, WG_BUTTON_NORMAL },
    { "6", '6', 2, 2, 1, WG_BUTTON_NORMAL }, { TEXT_MINUS, '-', 3, 2, 1, WG_BUTTON_NORMAL },
    { "1", '1', 0, 3, 1, WG_BUTTON_NORMAL }, { "2", '2', 1, 3, 1, WG_BUTTON_NORMAL },
    { "3", '3', 2, 3, 1, WG_BUTTON_NORMAL }, { "+", '+', 3, 3, 1, WG_BUTTON_NORMAL },
    { "+/" TEXT_MINUS, 'n', 0, 4, 1, WG_BUTTON_NORMAL }, { "0", '0', 1, 4, 1, WG_BUTTON_NORMAL },
    { ".", '.', 2, 4, 1, WG_BUTTON_NORMAL },
    { "=", '=', 3, 4, 1, WG_BUTTON_SUGGESTED },
};
#define NKEYS ((int)(sizeof(keys) / sizeof(keys[0])))

static struct wg_button buttons[NKEYS];
static struct gfx_rect display;

/* --- numbers -------------------------------------------------------------------- */

static int64_t abs64(int64_t v)
{
    return v < 0 ? -v : v;
}

/* "12.5" -> 12500000. The typed text is always well formed. */
static int64_t parse(const char *s)
{
    int neg = *s == '-';
    int64_t whole = 0, frac = 0, unit = SCALE / 10;
    if (neg)
        s++;
    for (; *s && *s != '.'; s++)
        whole = whole * 10 + (*s - '0');
    if (*s == '.')
        for (s++; *s && unit; s++, unit /= 10)
            frac += (*s - '0') * unit;
    int64_t v = whole * SCALE + frac;
    return neg ? -v : v;
}

/* 12500000 -> "12.5". */
static void format(int64_t v, char *out)
{
    char digits[24];
    int n = 0, neg = v < 0;
    uint64_t u = (uint64_t)abs64(v), whole = u / SCALE, frac = u % SCALE;
    do
        digits[n++] = (char)('0' + whole % 10);
    while (whole /= 10);
    char *p = out;
    if (neg && (u != 0))
        for (const char *m = TEXT_MINUS; *m; m++)
            *p++ = *m;
    while (n)
        *p++ = digits[--n];
    if (frac) {
        *p++ = '.';
        for (int64_t unit = SCALE / 10; frac && unit; unit /= 10) {
            *p++ = (char)('0' + frac / unit);
            frac %= unit;
        }
    }
    *p = '\0';
}

/* a op b, or -1 on overflow / division by zero. */
static int compute(int64_t a, char o, int64_t b, int64_t *out)
{
    int64_t r;
    switch (o) {
    case '+': r = a + b; break;
    case '-': r = a - b; break;
    case '*': {
        /* |a| * |b| / SCALE must stay under LIMIT, checked in whole units
         * first so nothing overflows on the way. */
        int64_t ua = abs64(a), ub = abs64(b), ah = ua / SCALE, al = ua % SCALE, bh = ub / SCALE;
        if (ah && bh && ah > (LIMIT / SCALE) / bh)
            return -1;                      /* the whole parts alone are too big */
        r = ah * ub + al * (ub / SCALE) + al * (ub % SCALE) / SCALE;
        if ((a < 0) != (b < 0))
            r = -r;
        break;
    }
    case '/': {
        if (!b)
            return -1;
        int64_t ua = abs64(a), ub = abs64(b), q = ua / ub, rem = ua % ub;
        if (q > LIMIT / SCALE)
            return -1;
        r = q * SCALE;
        for (int64_t unit = SCALE / 10; unit; unit /= 10) {   /* long division, 6 places */
            rem *= 10;
            r += rem / ub * unit;
            rem %= ub;
        }
        if ((a < 0) != (b < 0))
            r = -r;
        break;
    }
    default:
        r = b;
        break;
    }
    if (abs64(r) > LIMIT)
        return -1;
    *out = r;
    return 0;
}

static const char *op_text(char o)
{
    return o == '+' ? "+" : o == '-' ? TEXT_MINUS : o == '*' ? TEXT_TIMES : TEXT_DIVIDE;
}

/* The log is plain ASCII (tests grep it): the symbols become - x / . */
static const char *ascii(const char *s)
{
    static char out[96];
    int n = 0;
    for (; *s && n < (int)sizeof(out) - 1; s++) {
        unsigned char c = (unsigned char)*s;
        out[n++] = c == 0x80 ? '-' : c == 0x81 ? 'x' : c == 0x82 ? '/' : (char)c;
    }
    out[n] = '\0';
    return out;
}

/* The number on the display now. */
static int64_t current(void)
{
    return entry[0] && strcmp(entry, "-") ? parse(entry) : shown;
}

static void set_expr(int64_t left, char o, int64_t right, int with_right)
{
    char a[32], b[32];
    format(left, a);
    char *p = expr;
    for (const char *s = a; *s; s++)
        *p++ = *s;
    *p++ = ' ';
    for (const char *s = op_text(o); *s; s++)
        *p++ = *s;
    if (with_right) {
        format(right, b);
        *p++ = ' ';
        for (const char *s = b; *s; s++)
            *p++ = *s;
        *p++ = ' ';
        *p++ = '=';
    }
    *p = '\0';
}

static void clear(void)
{
    acc = shown = 0;
    op = 0;
    entry[0] = expr[0] = '\0';
    error = 0;
}

static void press(char c)
{
    if (error && c != 'c')
        clear();
    int len = (int)strlen(entry);
    if (c >= '0' && c <= '9') {
        int digits = 0, dot = 0;
        for (int i = 0; entry[i]; i++) {
            dot |= entry[i] == '.';
            digits += entry[i] >= '0' && entry[i] <= '9' && !dot;
        }
        if (!dot && digits >= DIGITS)
            return;
        if (dot && len - (int)(strchr(entry, '.') - entry) > 6)
            return;                         /* six places is all there is */
        if (!strcmp(entry, "0"))
            len = 0;                        /* "07" is just "7" */
        entry[len] = c;
        entry[len + 1] = '\0';
    } else if (c == '.' || c == ',') {
        if (strchr(entry, '.'))
            return;
        if (!len || !strcmp(entry, "-"))
            entry[len++] = '0';
        entry[len] = '.';
        entry[len + 1] = '\0';
    } else if (c == '\b') {
        if (len)
            entry[len - 1] = '\0';
    } else if (c == 'c') {
        clear();
    } else if (c == 'n') {
        if (entry[0] == '-')
            memmove(entry, entry + 1, (size_t)len);
        else if (entry[0]) {
            memmove(entry + 1, entry, (size_t)len + 1);
            entry[0] = '-';
        } else {
            shown = -shown;
        }
    } else if (c == '%') {
        int64_t v = current() / 100;
        if (op && (op == '+' || op == '-'))
            compute(acc, '*', v, &v);       /* 200 + 10% = 220, as calculators do */
        shown = v;
        entry[0] = '\0';
    } else if (c == '+' || c == '-' || c == '*' || c == '/') {
        if (op && entry[0]) {               /* 2 + 3 * : finish 2 + 3 first */
            int64_t r;
            if (compute(acc, op, current(), &r)) {
                error = 1;
                return;
            }
            acc = shown = r;
        } else {
            acc = current();
        }
        op = c;
        entry[0] = '\0';
        set_expr(acc, op, 0, 0);
    } else if (c == '=') {
        if (!op)
            return;
        int64_t right = current(), r;
        set_expr(acc, op, right, 1);
        if (compute(acc, op, right, &r)) {
            error = 1;
            k86_logf("calculator: %s error", ascii(expr));
        } else {
            char text[32];
            format(r, text);
            char line[96];
            int n = 0;
            for (const char *q = ascii(expr); *q; q++)
                line[n++] = *q;
            line[n] = 0;
            k86_logf("calculator: %s %s", line, ascii(text));
            shown = r;
        }
        op = 0;
        entry[0] = '\0';
    }
}

/* --- drawing ---------------------------------------------------------------------- */

/* The display takes the top third; the keys share the rest, as big as fit. */
static void layout(void)
{
    int w = canvas.w - 2 * MARGIN, x0 = MARGIN, y0 = MARGIN;
    int dh = canvas.h / 3;
    if (dh < 160)
        dh = 160;
    display = (struct gfx_rect){ x0, y0, w, dh };
    int top = y0 + dh + GAP;
    int bw = (w - (COLS - 1) * GAP) / COLS, bh = (canvas.h - MARGIN - top - (ROWS - 1) * GAP) / ROWS;
    if (bh > bw)
        bh = bw;                            /* never taller than wide */
    top = canvas.h - MARGIN - ROWS * bh - (ROWS - 1) * GAP;     /* the keys at the bottom, under the thumb */
    display.h = top - GAP - y0;
    for (int i = 0; i < NKEYS; i++) {
        const struct key *k = &keys[i];
        struct wg_button keep = buttons[i];
        buttons[i] = (struct wg_button){
            .r = { x0 + k->col * (bw + GAP), top + k->row * (bh + GAP), k->span * bw + (k->span - 1) * GAP, bh },
            .label = k->label, .style = k->style, .pressed = keep.pressed, .hover = keep.hover };
    }
}

static int is_op(char c)
{
    return c == '+' || c == '-' || c == '*' || c == '/';
}

/* A key: round, its label big. Digits plain, operators tinted, = the accent. */
static void draw_key(int i)
{
    const struct theme *t = theme_get();
    struct wg_button *b = &buttons[i];
    const struct key *k = &keys[i];
    struct gfx_rect r = b->r;
    uint32_t bg = k->style == WG_BUTTON_SUGGESTED ? t->accent_bg
                : is_op(k->action) ? gfx_mix(t->accent_bg, t->button_bg, 70)
                : k->style == WG_BUTTON_DESTRUCTIVE ? gfx_mix(t->destructive, t->button_bg, 60) : t->button_bg;
    uint32_t fg = k->style == WG_BUTTON_SUGGESTED ? t->accent_fg : t->fg;
    if (b->pressed)
        bg = gfx_mix(t->fg, bg, 50);
    gfx_fill_rect(&canvas, r.x, r.y, r.w, r.h, t->window_bg);
    gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, (r.h < r.w ? r.h : r.w) / 2, bg);
    enum text_style st = (k->action >= '0' && k->action <= '9') || is_op(k->action) || k->action == '=' ||
                         k->action == '.' ? TEXT_LARGE : TEXT_HEADING;     /* words smaller: C, Del, % */
    text_draw(&canvas, r.x + (r.w - text_width(k->label, st)) / 2, r.y + (r.h - text_height(st)) / 2, k->label,
              st, fg);
    b->dirty = 0;
}

static void draw_display(void)
{
    const struct theme *t = theme_get();
    struct gfx_rect d = display;
    gfx_fill_rect(&canvas, d.x - 2, d.y - 2, d.w + 4, d.h + 4, t->window_bg);
    gfx_fill_round_rect(&canvas, d.x, d.y, d.w, d.h, 24, t->light ? t->view_bg : gfx_mix(t->ink, t->window_bg, 20));
    char value[40];
    if (error)
        memcpy(value, "Error", 6);
    else if (entry[0] && strcmp(entry, "-"))
        format(parse(entry), value);
    else if (entry[0])
        memcpy(value, TEXT_MINUS, sizeof(TEXT_MINUS));
    else
        format(shown, value);
    /* While typing, keep the trailing "." or "0"s the user typed. */
    if (!error && entry[0] && strchr(entry, '.')) {
        const char *dot = strchr(entry, '.');
        int have = (int)strlen(dot);            /* "." plus the typed places */
        char *vdot = strchr(value, '.');
        int got = vdot ? (int)strlen(vdot) : 0;
        char *end = value + strlen(value);
        if (!vdot)
            *end++ = '.', got = 1;
        for (; got < have; got++)
            *end++ = '0';
        *end = '\0';
    }
    int pad = 28;
    text_draw_fit(&canvas, d.x + d.w - pad - text_width(expr, TEXT_HEADING), d.y + 24, expr,
                  d.w - 2 * pad, TEXT_HEADING, t->fg_dim);
    int vw = text_width(value, TEXT_LARGE);
    text_draw(&canvas, d.x + d.w - pad - vw, d.y + d.h - text_height(TEXT_LARGE) - 24, value,
              TEXT_LARGE, error ? t->destructive : t->fg);
    k86_present(d.x - 2, d.y - 2, d.w + 4, d.h + 4);
}

static void draw_all(void)
{
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, theme_get()->window_bg);
    for (int i = 0; i < NKEYS; i++)
        draw_key(i);
    k86_present(0, 0, canvas.w, canvas.h);
    draw_display();
}

static void draw_dirty(void)
{
    for (int i = 0; i < NKEYS; i++)
        if (buttons[i].dirty) {
            draw_key(i);
            k86_present(buttons[i].r.x, buttons[i].r.y, buttons[i].r.w, buttons[i].r.h);
        }
}

/* --- events ------------------------------------------------------------------------- */

static void key(const struct key_event *k)
{
    if (!k->pressed)
        return;
    char c = 0;
    if (k->key == KEY_ENTER)
        c = '=';
    else if (k->key == KEY_BACKSPACE)
        c = '\b';
    else if (k->key == KEY_DELETE)
        c = 'c';
    else if (k->ascii >= 'A' && k->ascii <= 'Z')
        c = (char)(k->ascii + 32);
    else if (k->ascii && strchr("0123456789.,+-*/%=cn", k->ascii))
        c = (char)k->ascii;
    if (!c)
        return;
    press(c);
    draw_display();
}

static void pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    for (int i = 0; i < NKEYS; i++)
        if (wg_button_pointer(&buttons[i], &p)) {
            press(keys[i].action);
            draw_display();
        }
    draw_dirty();
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    clear();
    layout();
    draw_all();
    k86_log("calculator: open");
    for (;;) {
        struct k86_event ev;
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_KEY:     key(&ev.key); break;
        case K86_EVENT_POINTER: pointer(&ev); break;
        case K86_EVENT_THEME:   draw_all(); break;
        case K86_EVENT_RESIZE:  canvas.w = ev.x; canvas.h = ev.y; layout(); draw_all(); break;
        case K86_EVENT_CLOSE:   return 0;
        default:                break;
        }
    }
}
