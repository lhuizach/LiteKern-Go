/* LiteKern X — Blocksmith: the blocks (see blocksmith.h). */
#include "apps/blocksmith/blocksmith.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"

/* Sizes, in pixels. */
#define PAD_X       10          /* inside a step's ends */
#define VAL_PAD     8           /* inside a value's ends */
#define GAP         6           /* between words and inputs */
#define ROW_MIN     36          /* a step's row */
#define HAT_TOP     10          /* a hat's taller top */
#define VAL_MIN     28          /* a value's height */
#define SLOT_H      22          /* an input typed into */
#define ARM         16          /* a C block's left side */
#define BOTTOM      20          /* its bottom bar */
#define MOUTH_MIN   24          /* an empty mouth */
#define ELSE_ROW    28
#define C_MIN_W     110
#define TAB_X       14          /* the tab on a step's top, like Blocksmith's logo */
#define TAB_W       18
#define TAB_H       5

#define LABEL_FONT  TEXT_BOLD
#define LIT_FONT    TEXT_BODY
#define LIT_INK     0x1b1330u   /* typed values: Blocksmith's ink on white */
#define WHITE       0xffffffu

struct block blocks[MAX_BLOCKS];
int16_t order[MAX_BLOCKS];
int norder;
char var_names[MAX_VARS][VAR_MAX];
int nvars;
int error_block = NO;

/* --- the blocks ------------------------------------------------------------------------- */

const char *const cat_names[CATS] = { "Events", "Control", "Stage", "Maths", "Variables", "Sensing" };

/* The block colours are content, like Blockly's: the same in both styles. */
uint32_t cat_colour(int cat)
{
    static const uint32_t c[CATS] = { 0xdf9a00, 0xf0752b, 0x2f8fe0, 0x2f9e68, 0xec5a3c, 0x8a62dc };
    return cat >= 0 && cat < CATS ? c[cat] : 0x808080;
}

const struct op_def ops[OPS] = {
    [OP_RUN]       = { "run", "when Run clicked", SH_HAT, CAT_EVENTS, { 0 } },
    [OP_KEY]       = { "key", "when %k key pressed", SH_HAT, CAT_EVENTS, { "space" } },
    [OP_CLICK]     = { "click", "when stage clicked", SH_HAT, CAT_EVENTS, { 0 } },
    [OP_WAIT]      = { "wait", "wait %n ms", SH_STACK, CAT_CONTROL, { "500" } },
    [OP_REPEAT]    = { "repeat", "repeat %n", SH_C, CAT_CONTROL, { "10" } },
    [OP_FOREVER]   = { "forever", "forever", SH_C_CAP, CAT_CONTROL, { 0 } },
    [OP_IF]        = { "if", "if %b then", SH_C, CAT_CONTROL, { 0 } },
    [OP_IFELSE]    = { "ifelse", "if %b then", SH_ELSE, CAT_CONTROL, { 0 } },
    [OP_UNTIL]     = { "until", "repeat until %b", SH_C, CAT_CONTROL, { 0 } },
    [OP_STOP]      = { "stop", "stop all", SH_CAP, CAT_CONTROL, { 0 } },
    [OP_CLEAR]     = { "clear", "fill stage %c", SH_STACK, CAT_STAGE, { "white" } },
    [OP_COLOUR]    = { "colour", "pen colour %c", SH_STACK, CAT_STAGE, { "blue" } },
    [OP_RGB]       = { "rgb", "pen rgb %n %n %n", SH_STACK, CAT_STAGE, { "255", "120", "0" } },
    [OP_CIRCLE]    = { "circle", "circle x %n y %n size %n", SH_STACK, CAT_STAGE, { "160", "120", "30" } },
    [OP_RECT]      = { "rect", "box x %n y %n w %n h %n", SH_STACK, CAT_STAGE, { "40", "40", "80", "50" } },
    [OP_LINE]      = { "line", "line %n %n to %n %n", SH_STACK, CAT_STAGE, { "0", "0", "320", "240" } },
    [OP_WRITE]     = { "write", "write %s x %n y %n", SH_STACK, CAT_STAGE, { "Hello!", "20", "20" } },
    [OP_PRINT]     = { "print", "print %s", SH_STACK, CAT_STAGE, { "Hello!" } },
    [OP_ADD]       = { "add", "%n + %n", SH_NUM, CAT_MATHS, { "", "" } },
    [OP_SUB]       = { "sub", "%n - %n", SH_NUM, CAT_MATHS, { "", "" } },
    [OP_MUL]       = { "mul", "%n " TEXT_TIMES " %n", SH_NUM, CAT_MATHS, { "", "" } },
    [OP_DIV]       = { "div", "%n " TEXT_DIVIDE " %n", SH_NUM, CAT_MATHS, { "", "" } },
    [OP_MOD]       = { "mod", "%n mod %n", SH_NUM, CAT_MATHS, { "", "" } },
    [OP_RANDOM]    = { "random", "random %n to %n", SH_NUM, CAT_MATHS, { "1", "10" } },
    [OP_LT]        = { "lt", "%n < %n", SH_BOOL, CAT_MATHS, { "", "50" } },
    [OP_EQ]        = { "eq", "%s = %s", SH_BOOL, CAT_MATHS, { "", "50" } },
    [OP_GT]        = { "gt", "%n > %n", SH_BOOL, CAT_MATHS, { "", "50" } },
    [OP_AND]       = { "and", "%b and %b", SH_BOOL, CAT_MATHS, { 0 } },
    [OP_OR]        = { "or", "%b or %b", SH_BOOL, CAT_MATHS, { 0 } },
    [OP_NOT]       = { "not", "not %b", SH_BOOL, CAT_MATHS, { 0 } },
    [OP_JOIN]      = { "join", "join %s %s", SH_NUM, CAT_MATHS, { "Hello ", "world" } },
    [OP_SET]       = { "set", "set %v to %s", SH_STACK, CAT_VARS, { "", "0" } },
    [OP_CHANGE]    = { "change", "change %v by %n", SH_STACK, CAT_VARS, { "", "1" } },
    [OP_VAR]       = { "var", "%v", SH_NUM, CAT_VARS, { "" } },
    [OP_MOUSEX]    = { "mousex", "mouse x", SH_NUM, CAT_SENSING, { 0 } },
    [OP_MOUSEY]    = { "mousey", "mouse y", SH_NUM, CAT_SENSING, { 0 } },
    [OP_MOUSEDOWN] = { "mousedown", "mouse down?", SH_BOOL, CAT_SENSING, { 0 } },
    [OP_KEYDOWN]   = { "keydown", "key %k pressed?", SH_BOOL, CAT_SENSING, { "space" } },
    [OP_TIMER]     = { "timer", "timer", SH_NUM, CAT_SENSING, { 0 } },
};

/* --- utilities ------------------------------------------------------------------------- */

void str_copy(char *dst, const char *src, int size)
{
    int n = 0;
    for (; src && src[n] && n < size - 1; n++)
        dst[n] = src[n];
    dst[n] = '\0';
}

int str_eq(const char *a, const char *b)
{
    return !strcmp(a, b);
}

int parse_int(const char *s, int32_t *out)
{
    while (*s == ' ')
        s++;
    int neg = *s == '-';
    if (neg)
        s++;
    int64_t v = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; s++, digits++)
        if (v < 4000000000LL)
            v = v * 10 + (*s - '0');
    while (*s == ' ')
        s++;
    if (*s || (neg && !digits))
        return -1;
    if (v > 2147483647LL)
        v = 2147483647LL;               /* as big as a number goes */
    *out = (int32_t)(neg ? -v : v);
    return 0;
}

void format_int(int32_t v, char *out)
{
    char d[12];
    int n = 0;
    uint32_t u = v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v;
    do
        d[n++] = (char)('0' + u % 10);
    while (u /= 10);
    if (v < 0)
        *out++ = '-';
    while (n)
        *out++ = d[--n];
    *out = '\0';
}

/* The spec's next token: its length, and whether it's an input ('n', 's', ...). */
static const char *token(const char *p, int *len, char *input)
{
    while (*p == ' ')
        p++;
    if (!*p)
        return 0;
    int n = 0;
    while (p[n] && p[n] != ' ')
        n++;
    *len = n;
    *input = p[0] == '%' && n == 2 ? p[1] : 0;
    return p;
}

int block_nargs(int op)
{
    int n = 0, len;
    char in;
    for (const char *p = ops[op].spec; (p = token(p, &len, &in)); p += len)
        n += in != 0;
    return n;
}

char block_slot_kind(int op, int i)
{
    int len;
    char in;
    for (const char *p = ops[op].spec; (p = token(p, &len, &in)); p += len)
        if (in && i-- == 0)
            return in;
    return 0;
}

static int shape_of(int b)
{
    return ops[blocks[b].op].shape;
}

int block_is_value(int b)
{
    int s = shape_of(b);
    return s == SH_NUM || s == SH_BOOL;
}

static int is_c(int b)
{
    int s = shape_of(b);
    return s == SH_C || s == SH_ELSE || s == SH_C_CAP;
}

int block_has_next(int b)
{
    int s = shape_of(b);
    return s != SH_CAP && s != SH_C_CAP && !block_is_value(b);
}

int block_has_prev(int b)
{
    return shape_of(b) != SH_HAT && !block_is_value(b);
}

int block_last(int b)
{
    while (blocks[b].next != NO)
        b = blocks[b].next;
    return b;
}

int block_top(int b)
{
    while (blocks[b].parent != NO)
        b = blocks[b].parent;
    return b;
}

/* --- the pool ---------------------------------------------------------------------------- */

int block_new(int op, int palette)
{
    for (int i = 0; i < MAX_BLOCKS; i++) {
        struct block *k = &blocks[i];
        if (k->used)
            continue;
        memset(k, 0, sizeof(*k));
        k->used = 1;
        k->op = (uint8_t)op;
        k->palette = (uint8_t)palette;
        k->pslot = PS_TOP;
        k->parent = k->next = k->sub[0] = k->sub[1] = NO;
        for (int a = 0; a < NARGS; a++) {
            k->arg[a] = NO;
            str_copy(k->lit[a], ops[op].defaults[a], LIT_MAX);
        }
        for (int a = 0; a < NARGS; a++)
            if (block_slot_kind(op, a) == 'v' && !k->lit[a][0] && nvars)
                str_copy(k->lit[a], var_names[0], LIT_MAX);
        return i;
    }
    return NO;
}

void block_free_tree(int b)
{
    while (b != NO) {
        struct block *k = &blocks[b];
        int next = k->next;
        for (int a = 0; a < NARGS; a++)
            block_free_tree(k->arg[a]);
        block_free_tree(k->sub[0]);
        block_free_tree(k->sub[1]);
        if (error_block == b)
            error_block = NO;
        k->used = 0;
        b = next;
    }
}

int block_copy_tree(int b, int palette)
{
    int c = block_new(blocks[b].op, palette);
    if (c == NO)
        return NO;
    memcpy(blocks[c].lit, blocks[b].lit, sizeof(blocks[c].lit));
    blocks[c].x = blocks[b].x;
    blocks[c].y = blocks[b].y;
    for (int a = 0; a < NARGS; a++)
        if (blocks[b].arg[a] != NO) {
            int v = block_copy_tree(blocks[b].arg[a], palette);
            if (v == NO) {
                block_free_tree(c);
                return NO;
            }
            block_attach(v, c, PS_ARG0 + a);
        }
    return c;
}

void order_add(int b)
{
    for (int i = 0; i < norder; i++)
        if (order[i] == b)
            return;
    order[norder++] = (int16_t)b;
}

void order_remove(int b)
{
    for (int i = 0; i < norder; i++)
        if (order[i] == b) {
            memmove(&order[i], &order[i + 1], (size_t)(norder - i - 1) * sizeof(order[0]));
            norder--;
            return;
        }
}

void order_front(int b)
{
    order_remove(b);
    order_add(b);
}

static int16_t *link_of(int parent, int pslot)
{
    struct block *p = &blocks[parent];
    if (pslot == PS_NEXT)
        return &p->next;
    if (pslot == PS_SUB0 || pslot == PS_SUB1)
        return &p->sub[pslot - PS_SUB0];
    if (pslot >= PS_ARG0 && pslot < PS_ARG0 + NARGS)
        return &p->arg[pslot - PS_ARG0];
    static int16_t nowhere;                 /* PS_TOP: not attached to anything */
    nowhere = NO;
    return &nowhere;
}

void block_detach(int b)
{
    struct block *k = &blocks[b];
    if (k->parent == NO)
        return;
    *link_of(k->parent, k->pslot) = NO;
    k->parent = NO;
    k->pslot = PS_TOP;
    if (!k->palette)
        order_add(b);
}

void block_attach(int b, int parent, int pslot)
{
    *link_of(parent, pslot) = (int16_t)b;
    blocks[b].parent = (int16_t)parent;
    blocks[b].pslot = (int8_t)pslot;
    order_remove(b);
}

/* --- menus ------------------------------------------------------------------------------- */

static const char *const key_names[] = { "space", "left", "right", "up", "down", "enter",
                                         "a", "d", "s", "w", "any" };
static const struct { const char *name; uint32_t rgb; } colours[] = {
    { "red", 0xe53935 }, { "orange", 0xfb8c00 }, { "yellow", 0xfdd835 }, { "green", 0x43a047 },
    { "blue", 0x1e88e5 }, { "purple", 0x8e24aa }, { "pink", 0xec407a }, { "brown", 0x8d6e63 },
    { "white", 0xffffff }, { "grey", 0x9e9e9e }, { "black", 0x000000 },
};
#define NKEYS ((int)(sizeof(key_names) / sizeof(key_names[0])))
#define NCOLOURS ((int)(sizeof(colours) / sizeof(colours[0])))

int menu_count(char kind)
{
    return kind == 'k' ? NKEYS : kind == 'c' ? NCOLOURS : kind == 'v' ? nvars : 0;
}

const char *menu_item(char kind, int i)
{
    if (i < 0 || i >= menu_count(kind))
        return "";
    return kind == 'k' ? key_names[i] : kind == 'c' ? colours[i].name : var_names[i];
}

int colour_value(const char *name, uint32_t *out)
{
    for (int i = 0; i < NCOLOURS; i++)
        if (str_eq(name, colours[i].name)) {
            *out = colours[i].rgb;
            return 0;
        }
    return -1;
}

/* --- layout ---------------------------------------------------------------------------------- */

static int lit_w(char kind, const char *text)
{
    int tw = text_width(text, LIT_FONT);
    switch (kind) {
    case 'n': return tw + 14 > 30 ? tw + 14 : 30;
    case 's': return tw + 12 > 34 ? tw + 12 : 34;
    case 'b': return 40;
    case 'c': return 44;
    default:  return tw + 26 > 40 ? tw + 26 : 40;   /* menus: the text, then an arrow */
    }
}

static int imax(int a, int b) { return a > b ? a : b; }

/* Sizes only: this block's row, the inputs in it (values in them first). */
static void measure(int b)
{
    struct block *k = &blocks[b];
    int value = block_is_value(b), hat = shape_of(b) == SH_HAT;
    int pad = value ? VAL_PAD : PAD_X, h = value ? VAL_MIN : ROW_MIN;
    int i = 0, len;
    char in;
    /* How tall the inputs make the row. */
    for (const char *p = ops[k->op].spec; (p = token(p, &len, &in)); p += len)
        if (in) {
            int sh = SLOT_H;
            if (k->arg[i] != NO) {
                measure(k->arg[i]);
                sh = blocks[k->arg[i]].h;
            }
            k->slot_h[i] = (int16_t)sh;
            h = imax(h, sh + (value ? 6 : 10));
            i++;
        }
    if (shape_of(b) == SH_BOOL)
        pad = h / 2 + 2;
    int top = hat ? HAT_TOP : 0, x = pad;
    i = 0;
    for (const char *p = ops[k->op].spec; (p = token(p, &len, &in)); p += len) {
        if (x > pad)
            x += GAP;
        if (in) {
            int w = k->arg[i] != NO ? blocks[k->arg[i]].w : lit_w(in, k->lit[i]);
            k->slot_x[i] = (int16_t)x;
            k->slot_w[i] = (int16_t)w;
            k->slot_y[i] = (int16_t)(top + (h - k->slot_h[i]) / 2);
            x += w;
            i++;
        } else {
            x += text_width_n(p, len, LABEL_FONT);
        }
    }
    k->w = (int16_t)(x + pad);
    k->row_h = (int16_t)(h + top);
    k->h = k->row_h;
    if (is_c(b) && k->w < C_MIN_W)
        k->w = C_MIN_W;
}

/* Positions: the values in b's inputs, now that b is placed. */
static void place_args(int b)
{
    struct block *k = &blocks[b];
    for (int i = 0; i < NARGS; i++) {
        int v = k->arg[i];
        if (v == NO)
            continue;
        blocks[v].x = (int16_t)(k->x + k->slot_x[i]);
        blocks[v].y = (int16_t)(k->y + k->slot_y[i]);
        place_args(v);
    }
}

/* A stack from (x, y): returns its height; *maxw: its width. */
static int layout_stack(int first, int x, int y, int *maxw)
{
    int cy = y, w = 0;
    for (int b = first; b != NO; b = blocks[b].next) {
        struct block *k = &blocks[b];
        k->x = (int16_t)x;
        k->y = (int16_t)cy;
        measure(b);
        w = imax(w, k->w);
        if (is_c(b)) {
            int iw = 0, ih = layout_stack(k->sub[0], x + ARM, cy + k->row_h, &iw);
            k->bot_y = (int16_t)(k->row_h + imax(ih, MOUTH_MIN));
            w = imax(w, ARM + iw);
            if (shape_of(b) == SH_ELSE) {
                k->mid_y = k->bot_y;
                ih = layout_stack(k->sub[1], x + ARM, cy + k->mid_y + ELSE_ROW, &iw);
                k->bot_y = (int16_t)(k->mid_y + ELSE_ROW + imax(ih, MOUTH_MIN));
                w = imax(w, ARM + iw);
            }
            k->h = (int16_t)(k->bot_y + BOTTOM);
        }
        place_args(b);
        cy += k->h;
    }
    *maxw = w;
    return cy - y;
}

void layout_script(int top)
{
    struct block *k = &blocks[top];
    if (block_is_value(top)) {
        measure(top);
        place_args(top);
        k->sw = k->w;
        k->sh = k->h;
        return;
    }
    int w;
    k->sh = (int16_t)layout_stack(top, k->x, k->y, &w);
    k->sw = (int16_t)w;
}

void layout_all(void)
{
    for (int i = 0; i < norder; i++)
        layout_script(order[i]);
}

void layout_move(int top, int x, int y)
{
    blocks[top].x = (int16_t)x;
    blocks[top].y = (int16_t)y;
    layout_script(top);
}

/* --- drawing --------------------------------------------------------------------------------- */

/* A pointed (boolean) shape: a hexagon whose ends come to a point. */
static void fill_hex(struct gfx_surface *s, int x, int y, int w, int h, uint32_t c)
{
    int half = h / 2;
    for (int r = 0; r < h; r++) {
        int d = 2 * r - (h - 1);
        int inset = (d < 0 ? -d : d) * half / (h > 1 ? h - 1 : 1);
        gfx_fill_rect(s, x + inset, y + r, w - 2 * inset, 1, c);
    }
}

static void arrow(struct gfx_surface *s, int x, int y, uint32_t c)
{
    for (int r = 0; r < 4; r++)
        gfx_fill_rect(s, x + r, y + r, 8 - 2 * r, 1, c);
}

static uint32_t darker(uint32_t c, int amount)
{
    return gfx_mix(0x000000, c, (uint32_t)amount);
}

static void draw_lit(struct gfx_surface *s, int x, int y, int w, int h, char kind, const char *text,
                     uint32_t col)
{
    int ty = y + (h - text_height(LIT_FONT)) / 2;
    switch (kind) {
    case 'n':
        gfx_fill_round_rect(s, x, y, w, h, h / 2, WHITE);
        text_draw(s, x + (w - text_width(text, LIT_FONT)) / 2, ty, text, LIT_FONT, LIT_INK);
        break;
    case 's':
        gfx_fill_round_rect(s, x, y, w, h, 4, WHITE);
        text_draw(s, x + 6, ty, text, LIT_FONT, LIT_INK);
        break;
    case 'b':
        fill_hex(s, x, y, w, h, darker(col, 70));
        break;
    case 'c': {
        uint32_t rgb = 0;
        colour_value(text, &rgb);
        gfx_fill_round_rect(s, x, y, w, h, h / 2, darker(col, 60));
        gfx_fill_circle(s, x + 12, y + h / 2, 8, WHITE);
        gfx_fill_circle(s, x + 12, y + h / 2, 7, rgb);
        arrow(s, x + w - 15, y + h / 2 - 2, WHITE);
        break;
    }
    default:                                /* 'v', 'k': a menu */
        gfx_fill_round_rect(s, x, y, w, h, h / 2, darker(col, 60));
        text_draw(s, x + 9, ty, text[0] ? text : "?", LIT_FONT, WHITE);
        arrow(s, x + w - 15, y + h / 2 - 2, WHITE);
        break;
    }
}

static void draw_value(struct gfx_surface *s, int ox, int oy, int b);

/* The words and inputs of b's first row. */
static void draw_row(struct gfx_surface *s, int ox, int oy, int b, uint32_t col)
{
    struct block *k = &blocks[b];
    int i = 0, len, x = k->x - ox;
    int top = shape_of(b) == SH_HAT ? HAT_TOP : 0;
    int ty = k->y - oy + top + (k->row_h - top - text_height(LABEL_FONT)) / 2;
    int pad = shape_of(b) == SH_BOOL ? k->row_h / 2 + 2 : block_is_value(b) ? VAL_PAD : PAD_X;
    int cx = x + pad;
    char in;
    for (const char *p = ops[k->op].spec; (p = token(p, &len, &in)); p += len) {
        if (cx > x + pad)
            cx += GAP;
        if (in) {
            if (k->arg[i] != NO)
                draw_value(s, ox, oy, k->arg[i]);
            else
                draw_lit(s, x + k->slot_x[i], k->y - oy + k->slot_y[i], k->slot_w[i], k->slot_h[i], in,
                         k->lit[i], col);
            cx = x + k->slot_x[i] + k->slot_w[i];
            i++;
        } else {
            cx += text_draw_n(s, cx, ty, p, len, LABEL_FONT, WHITE);
        }
    }
}

static void outline(struct gfx_surface *s, int x, int y, int w, int h, uint32_t c)
{
    gfx_rect_outline(s, x - 2, y - 2, w + 4, h + 4, c);
    gfx_rect_outline(s, x - 3, y - 3, w + 6, h + 6, c);
}

static void draw_value(struct gfx_surface *s, int ox, int oy, int b)
{
    struct block *k = &blocks[b];
    uint32_t col = cat_colour(ops[k->op].cat);
    int x = k->x - ox, y = k->y - oy;
    if (shape_of(b) == SH_BOOL) {
        fill_hex(s, x, y, k->w, k->h, darker(col, 50));
        fill_hex(s, x, y, k->w, k->h - 2, col);
    } else {
        gfx_fill_round_rect(s, x, y, k->w, k->h, k->h / 2, darker(col, 50));
        gfx_fill_round_rect(s, x, y, k->w, k->h - 2, (k->h - 2) / 2, col);
    }
    draw_row(s, ox, oy, b, col);
    if (b == error_block)
        outline(s, x, y, k->w, k->h, theme_get()->destructive);
}

static void draw_stack(struct gfx_surface *s, int ox, int oy, int first)
{
    for (int b = first; b != NO; b = blocks[b].next) {
        struct block *k = &blocks[b];
        uint32_t col = cat_colour(ops[k->op].cat), edge = darker(col, 50);
        int x = k->x - ox, y = k->y - oy, sh = shape_of(b);
        if (sh == SH_HAT) {
            gfx_fill_round_rect(s, x, y, k->w, k->row_h, 10, edge);
            gfx_fill_round_rect(s, x, y, k->w, k->row_h - 2, 10, col);
            /* A hat's rounded top: wider than the block's corners. */
            gfx_fill_round_rect(s, x, y - 6, 72, 20, 10, col);
        } else if (is_c(b)) {
            gfx_fill_round_rect(s, x, y, k->w, k->row_h + 6, 6, col);
            gfx_fill_rect(s, x, y + k->row_h, ARM, k->bot_y - k->row_h + 4, col);
            if (sh == SH_ELSE) {
                gfx_fill_round_rect(s, x, y + k->mid_y, k->w, ELSE_ROW, 6, col);
                gfx_fill_rect(s, x, y + k->mid_y, ARM, ELSE_ROW + 4, col);
                text_draw(s, x + PAD_X, y + k->mid_y + (ELSE_ROW - text_height(LABEL_FONT)) / 2, "else",
                          LABEL_FONT, WHITE);
            }
            gfx_fill_round_rect(s, x, y + k->bot_y, k->w, BOTTOM, 6, edge);
            gfx_fill_round_rect(s, x, y + k->bot_y, k->w, BOTTOM - 2, 6, col);
            gfx_fill_rect(s, x, y + k->bot_y - 2, ARM, 6, col);
        } else {
            gfx_fill_round_rect(s, x, y, k->w, k->h, 6, edge);
            gfx_fill_round_rect(s, x, y, k->w, k->h - 2, 6, col);
        }
        if (sh != SH_HAT)
            gfx_fill_round_rect(s, x + TAB_X, y - TAB_H, TAB_W, TAB_H + 3, 2, col);
        draw_row(s, ox, oy, b, col);
        if (is_c(b)) {
            draw_stack(s, ox, oy, k->sub[0]);
            if (sh == SH_ELSE)
                draw_stack(s, ox, oy, k->sub[1]);
        }
        if (b == error_block)
            outline(s, x, y, k->w, k->row_h, theme_get()->destructive);
    }
}

void draw_script(struct gfx_surface *s, int ox, int oy, int top)
{
    if (block_is_value(top))
        draw_value(s, ox, oy, top);
    else
        draw_stack(s, ox, oy, top);
}

void draw_block_only(struct gfx_surface *s, int ox, int oy, int b)
{
    draw_script(s, ox, oy, b);
}

/* --- hit tests ---------------------------------------------------------------------------------- */

static int inside(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

static int hit_row(int b, int px, int py, int *slot)
{
    struct block *k = &blocks[b];
    for (int i = 0; i < NARGS; i++) {
        if (!block_slot_kind(k->op, i))
            break;
        if (k->arg[i] != NO) {
            int r = hit_row(k->arg[i], px, py, slot);
            if (r != NO)
                return r;
        } else if (inside(px, py, k->x + k->slot_x[i], k->y + k->slot_y[i], k->slot_w[i], k->slot_h[i])) {
            *slot = i;
            return b;
        }
    }
    if (block_is_value(b) && inside(px, py, k->x, k->y, k->w, k->h)) {
        *slot = -1;
        return b;
    }
    return NO;
}

static int hit_stack(int first, int px, int py, int *slot)
{
    for (int b = first; b != NO; b = blocks[b].next) {
        struct block *k = &blocks[b];
        if (is_c(b)) {
            int r = hit_stack(k->sub[0], px, py, slot);
            if (r == NO && shape_of(b) == SH_ELSE)
                r = hit_stack(k->sub[1], px, py, slot);
            if (r != NO)
                return r;
        }
        int r = hit_row(b, px, py, slot);
        if (r != NO)
            return r;
        int top = shape_of(b) == SH_HAT ? -6 : 0;
        int body = inside(px, py, k->x, k->y + top, k->w, k->row_h - top);
        if (is_c(b))
            body |= inside(px, py, k->x, k->y, ARM, k->h) ||
                    inside(px, py, k->x, k->y + k->bot_y, k->w, BOTTOM) ||
                    (shape_of(b) == SH_ELSE && inside(px, py, k->x, k->y + k->mid_y, k->w, ELSE_ROW));
        if (body) {
            *slot = -1;
            return b;
        }
    }
    return NO;
}

int hit_script(int top, int x, int y, int *slot)
{
    struct block *k = &blocks[top];
    if (!inside(x, y, k->x - 4, k->y - 8, k->sw + 8, k->sh + 12))
        return NO;
    return block_is_value(top) ? hit_row(top, x, y, slot) : hit_stack(top, x, y, slot);
}

/* --- snapping --------------------------------------------------------------------------------- */

struct snap_ctx {
    int drag, last, range2;
    struct snap best;
    int best_d;
};

static void consider(struct snap_ctx *c, int px, int py, int qx, int qy, int target, int pslot,
                     int mx, int my, int mw, int mh)
{
    int dx = px - qx, dy = py - qy, d = dx * dx + dy * dy;
    if (d > c->range2 || (c->best.ok && d >= c->best_d))
        return;
    c->best = (struct snap){ 1, target, pslot, mx, my, mw, mh };
    c->best_d = d;
}

/* Every input a dragged value could go in. */
static void snap_values(struct snap_ctx *c, int b)
{
    struct block *k = &blocks[b], *d = &blocks[c->drag];
    int is_bool = shape_of(c->drag) == SH_BOOL;
    for (int i = 0; i < NARGS; i++) {
        char kind = block_slot_kind(k->op, i);
        if (!kind)
            break;
        if (k->arg[i] != NO) {
            snap_values(c, k->arg[i]);
            continue;
        }
        if ((kind == 'b') != is_bool || kind == 'v' || kind == 'k' || kind == 'c')
            continue;
        int sx = k->x + k->slot_x[i], sy = k->y + k->slot_y[i];
        consider(c, d->x, d->y + d->h / 2, sx, sy + k->slot_h[i] / 2, b, PS_ARG0 + i, sx, sy,
                 k->slot_w[i], k->slot_h[i]);
    }
}

static void snap_stack(struct snap_ctx *c, int first)
{
    struct block *d = &blocks[c->drag];
    int hat = shape_of(c->drag) == SH_HAT, follows = block_has_next(c->last);
    for (int b = first; b != NO; b = blocks[b].next) {
        struct block *k = &blocks[b];
        snap_values(c, b);
        if (is_c(b)) {
            snap_stack(c, k->sub[0]);
            if (shape_of(b) == SH_ELSE)
                snap_stack(c, k->sub[1]);
        }
        if (block_is_value(c->drag) || hat)
            continue;
        if (block_has_next(b) && (k->next == NO || follows))
            consider(c, d->x, d->y, k->x, k->y + k->h, b, PS_NEXT, k->x, k->y + k->h - 2, 80, 5);
        if (is_c(b) && (k->sub[0] == NO || follows))
            consider(c, d->x, d->y, k->x + ARM, k->y + k->row_h, b, PS_SUB0, k->x + ARM, k->y + k->row_h - 2,
                     80, 5);
        if (shape_of(b) == SH_ELSE && (k->sub[1] == NO || follows))
            consider(c, d->x, d->y, k->x + ARM, k->y + k->mid_y + ELSE_ROW, b, PS_SUB1, k->x + ARM,
                     k->y + k->mid_y + ELSE_ROW - 2, 80, 5);
    }
}

struct snap find_snap(int drag, int range)
{
    struct snap_ctx c = { drag, block_last(drag), range * range, { 0 }, 0 };
    struct block *d = &blocks[drag];
    for (int i = 0; i < norder; i++) {
        int t = order[i];
        if (t == drag)
            continue;
        if (block_is_value(drag)) {
            if (block_is_value(t))
                snap_values(&c, t);
            else
                snap_stack(&c, t);
            continue;
        }
        if (block_is_value(t))
            continue;
        snap_stack(&c, t);
        /* Above a script: the dragged stack's bottom meets its top. */
        if (block_has_prev(t) && block_has_next(c.last))
            consider(&c, d->x, d->y + d->sh, blocks[t].x, blocks[t].y, t, PS_TOP, blocks[t].x,
                     blocks[t].y - 3, 80, 5);
    }
    return c.best;
}

void apply_snap(int drag, const struct snap *s)
{
    if (!s->ok)
        return;
    int last = block_last(drag);
    if (s->pslot == PS_TOP) {
        int t = s->target, h = blocks[drag].sh;
        int tx = blocks[t].x, ty = blocks[t].y;
        block_attach(t, last, PS_NEXT);
        layout_move(drag, tx, ty - h);
        return;
    }
    if (s->pslot >= PS_ARG0) {
        order_remove(drag);
        block_attach(drag, s->target, s->pslot);
        return;
    }
    int16_t *link = link_of(s->target, s->pslot);
    int old = *link;
    order_remove(drag);
    block_attach(drag, s->target, s->pslot);
    if (old != NO)
        block_attach(old, last, PS_NEXT);
}
