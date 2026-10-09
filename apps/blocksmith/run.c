/* LiteKern X — Blocksmith: running scripts (see blocksmith.h).
 *
 * Each running script is a thread: the block it's at, and a stack of the
 * loops and ifs it's inside. Threads take turns, a little each timer tick,
 * so the app always gets back to k86_wait_event() quickly. As in Scratch, a
 * loop pauses at the end of each time round once something was drawn in
 * this tick: that's what makes "forever: move a ball" an animation, one
 * frame a tick, instead of a blur. */
#include "apps/blocksmith/blocksmith.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "sdk/kern86.h"

#define MAX_THREADS 16
#define MAX_DEPTH   24          /* loops and ifs inside each other */
#define MAX_EVAL    32          /* values inside each other */
#define TICK_MS     24          /* the most one tick runs for */
#define NKEYS_MENU  11          /* the key menu (blocks.c): ... "w", "any" */

uint32_t stage_px[STAGE_W * STAGE_H];
struct gfx_surface stage = { stage_px, STAGE_W, STAGE_H, STAGE_W };
struct console_line console[CONSOLE_LINES];
int nconsole;

struct value {
    int text;                   /* 1: s holds it; 0: n does */
    int32_t n;
    char s[TEXT_MAX];
};

struct frame {
    int16_t block;
    int32_t count;              /* "repeat": times left */
};

struct thread {
    int used;
    int16_t top, pc;            /* the script; the block it runs next (NO: the end of a stack) */
    int depth;
    struct frame f[MAX_DEPTH];
    uint32_t wake;              /* "wait": uptime_ms() to carry on at */
};

static struct thread threads[MAX_THREADS];
static struct value vars[MAX_VARS];
static int mode;                /* Run was pressed (or a script clicked): until Stop */
static int stop_all, drew, changed;
static uint32_t pen = 0x1e88e5, t0, seed = 1;
static int mouse_x, mouse_y, mouse_down;
static uint8_t keys_down[NKEYS_MENU];

/* --- the console and the stage ----------------------------------------------------------- */

void console_clear(void)
{
    nconsole = 0;
}

void console_add(const char *text, int error)
{
    if (nconsole == CONSOLE_LINES) {
        memmove(&console[0], &console[1], sizeof(console[0]) * (CONSOLE_LINES - 1));
        nconsole--;
    }
    str_copy(console[nconsole].text, text, sizeof(console[0].text));
    console[nconsole].error = (uint8_t)error;
    nconsole++;
    changed |= RUN_CONSOLE;
}

void stage_reset(void)
{
    gfx_fill_rect(&stage, 0, 0, STAGE_W, STAGE_H, 0xffffff);
    pen = 0x1e88e5;
}

/* --- errors -------------------------------------------------------------------------------- */

static int fail(int b, const char *a, const char *quoted, const char *c)
{
    char msg[56];
    int n = 0;
    for (const char *p = a; p && *p && n < 50; p++)
        msg[n++] = *p;
    if (quoted) {
        msg[n++] = '"';
        for (const char *p = quoted; *p && n < 40; p++)
            msg[n++] = *p;
        msg[n++] = '"';
    }
    for (const char *p = c; p && *p && n < 54; p++)
        msg[n++] = *p;
    msg[n] = '\0';
    console_add(msg, 1);
    error_block = b;
    changed |= RUN_ERROR;
    k86_logf("blocksmith: error: %s", msg);
    return -1;
}

/* --- values -------------------------------------------------------------------------------- */

static int find_var(const char *name)
{
    for (int i = 0; i < nvars; i++)
        if (str_eq(name, var_names[i]))
            return i;
    return -1;
}

static void text_of(const struct value *v, char *out)
{
    if (v->text)
        str_copy(out, v->s, TEXT_MAX);
    else
        format_int(v->n, out);
}

static int num_of(int b, const struct value *v, int32_t *out)
{
    if (!v->text) {
        *out = v->n;
        return 0;
    }
    if (parse_int(v->s, out) == 0)
        return 0;
    return fail(b, "", v->s, " isn't a whole number");
}

static uint32_t rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static int eval(int b, struct value *out, int depth);

/* Input i of block b: the value in it, or what's typed there. */
static int arg(int b, int i, struct value *out, int depth)
{
    struct block *k = &blocks[b];
    if (k->arg[i] != NO)
        return eval(k->arg[i], out, depth + 1);
    char kind = block_slot_kind(k->op, i);
    if (kind == 'n') {
        out->text = 0;
        if (parse_int(k->lit[i], &out->n))
            return fail(b, "", k->lit[i], " isn't a whole number");
        return 0;
    }
    if (kind == 'b') {
        out->text = 0;
        out->n = 0;                     /* an empty yes/no input: no */
        return 0;
    }
    out->text = 1;
    str_copy(out->s, k->lit[i], TEXT_MAX);
    return 0;
}

static int arg_num(int b, int i, int32_t *out, int depth)
{
    struct value v;
    return arg(b, i, &v, depth) ? -1 : num_of(b, &v, out);
}

static int arg_bool(int b, int i, int *out, int depth)
{
    struct value v;
    if (arg(b, i, &v, depth))
        return -1;
    *out = v.text ? v.s[0] != '\0' : v.n != 0;
    return 0;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int same_text(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b))
        a++, b++;
    return lower(*a) == lower(*b);
}

static int key_index(const char *name)
{
    for (int i = 0; i < menu_count('k'); i++)
        if (str_eq(name, menu_item('k', i)))
            return i;
    return -1;
}

static int eval(int b, struct value *out, int depth)
{
    struct block *k = &blocks[b];
    int32_t x, y;
    if (depth > MAX_EVAL)
        return fail(b, "Too many blocks inside each other", 0, 0);
    out->text = 0;
    out->n = 0;
    switch (k->op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_LT: case OP_GT: case OP_RANDOM:
        if (arg_num(b, 0, &x, depth) || arg_num(b, 1, &y, depth))
            return -1;
        switch (k->op) {
        case OP_ADD: out->n = (int32_t)((uint32_t)x + (uint32_t)y); break;
        case OP_SUB: out->n = (int32_t)((uint32_t)x - (uint32_t)y); break;
        case OP_MUL: out->n = (int32_t)((int64_t)x * y); break;
        case OP_DIV:
        case OP_MOD:
            if (!y)
                return fail(b, "Can't divide by zero", 0, 0);
            if (y == -1)
                out->n = k->op == OP_DIV ? (int32_t)(0u - (uint32_t)x) : 0;
            else if (k->op == OP_DIV)
                out->n = x / y;
            else
                out->n = (x % y + y) % y;   /* like Scratch: the sign of the divisor */
            break;
        case OP_LT: out->n = x < y; break;
        case OP_GT: out->n = x > y; break;
        default: {                          /* OP_RANDOM */
            if (x > y) {
                int32_t t = x;
                x = y;
                y = t;
            }
            uint32_t span = (uint32_t)((int64_t)y - x + 1);
            out->n = span ? (int32_t)((int64_t)x + rnd() % span) : (int32_t)rnd();
            break;
        }
        }
        return 0;
    case OP_EQ: {
        struct value a, c;
        char ta[TEXT_MAX], tc[TEXT_MAX];
        if (arg(b, 0, &a, depth) || arg(b, 1, &c, depth))
            return -1;
        text_of(&a, ta);
        text_of(&c, tc);
        if (parse_int(ta, &x) == 0 && parse_int(tc, &y) == 0)
            out->n = x == y;                /* "5" = "05" */
        else
            out->n = same_text(ta, tc);     /* ignoring case, like Scratch */
        return 0;
    }
    case OP_AND: case OP_OR: {
        int p, q;
        if (arg_bool(b, 0, &p, depth) || arg_bool(b, 1, &q, depth))
            return -1;
        out->n = k->op == OP_AND ? p && q : p || q;
        return 0;
    }
    case OP_NOT: {
        int p;
        if (arg_bool(b, 0, &p, depth))
            return -1;
        out->n = !p;
        return 0;
    }
    case OP_JOIN: {
        struct value a, c;
        char ta[TEXT_MAX], tc[TEXT_MAX];
        if (arg(b, 0, &a, depth) || arg(b, 1, &c, depth))
            return -1;
        text_of(&a, ta);
        text_of(&c, tc);
        out->text = 1;
        int n = 0;
        for (const char *p = ta; *p && n < TEXT_MAX - 1; p++)
            out->s[n++] = *p;
        for (const char *p = tc; *p && n < TEXT_MAX - 1; p++)
            out->s[n++] = *p;
        out->s[n] = '\0';
        return 0;
    }
    case OP_VAR: {
        int v = find_var(k->lit[0]);
        if (v < 0)
            return fail(b, k->lit[0][0] ? "There's no variable " : "Choose a variable", k->lit[0][0] ? k->lit[0] : 0, 0);
        *out = vars[v];
        return 0;
    }
    case OP_MOUSEX: out->n = mouse_x; return 0;
    case OP_MOUSEY: out->n = mouse_y; return 0;
    case OP_MOUSEDOWN: out->n = mouse_down; return 0;
    case OP_TIMER: out->n = (int32_t)(k86_uptime_ms() - t0); return 0;
    case OP_KEYDOWN: {
        int i = key_index(k->lit[0]);
        if (i == NKEYS_MENU - 1) {
            for (int j = 0; j < NKEYS_MENU - 1; j++)
                out->n |= keys_down[j];
        } else if (i >= 0) {
            out->n = keys_down[i];
        }
        return 0;
    }
    default:
        return fail(b, "This block doesn't give a value", 0, 0);
    }
}

/* --- steps ---------------------------------------------------------------------------------- */

enum { STEP_OK, STEP_YIELD, STEP_DONE, STEP_ERROR };

static int push(struct thread *t, int b, int32_t count)
{
    if (t->depth == MAX_DEPTH)
        return fail(b, "Too many loops and ifs inside each other", 0, 0);
    t->f[t->depth++] = (struct frame){ (int16_t)b, count };
    return 0;
}

/* The end of a time round a loop: carry on, or pause until the next tick. */
static int loop_end(void)
{
    return drew ? STEP_YIELD : STEP_OK;
}

static uint32_t clamp255(int32_t v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
}

static int set_var(int b, const char *name, const struct value *v)
{
    int i = find_var(name);
    if (i < 0)
        return fail(b, name[0] ? "There's no variable " : "Choose a variable", name[0] ? name : 0, 0);
    vars[i] = *v;
    return 0;
}

static int step(struct thread *t)
{
    if (t->pc == NO) {                      /* the end of a stack: back to the loop or if it's in */
        if (!t->depth)
            return STEP_DONE;
        struct frame *f = &t->f[t->depth - 1];
        int b = f->block;
        struct block *k = &blocks[b];
        if (!k->used)
            return STEP_DONE;
        if (k->op == OP_REPEAT && --f->count > 0) {
            t->pc = k->sub[0];
            return loop_end();
        }
        if (k->op == OP_FOREVER) {
            t->pc = k->sub[0];
            return loop_end();
        }
        if (k->op == OP_UNTIL) {
            int c;
            if (arg_bool(b, 0, &c, 0))
                return STEP_ERROR;
            if (!c) {
                t->pc = k->sub[0];
                return loop_end();
            }
        }
        t->depth--;
        t->pc = k->next;
        return STEP_OK;
    }

    int b = t->pc;
    struct block *k = &blocks[b];
    if (!k->used || k->palette)
        return STEP_DONE;
    int32_t a[NARGS] = { 0 };
    struct value v;
    t->pc = k->next;
    switch (k->op) {
    case OP_RUN: case OP_KEY: case OP_CLICK:
        return STEP_OK;
    case OP_WAIT:
        if (arg_num(b, 0, &a[0], 0))
            return STEP_ERROR;
        t->wake = k86_uptime_ms() + (uint32_t)(a[0] < 0 ? 0 : a[0] > 3600000 ? 3600000 : a[0]);
        return STEP_YIELD;
    case OP_REPEAT:
        if (arg_num(b, 0, &a[0], 0))
            return STEP_ERROR;
        if (a[0] <= 0)
            return STEP_OK;
        if (push(t, b, a[0]))
            return STEP_ERROR;
        t->pc = k->sub[0];
        return STEP_OK;
    case OP_FOREVER:
        if (push(t, b, 0))
            return STEP_ERROR;
        t->pc = k->sub[0];
        return STEP_OK;
    case OP_IF: case OP_IFELSE: case OP_UNTIL: {
        int c;
        if (arg_bool(b, 0, &c, 0))
            return STEP_ERROR;
        if (k->op == OP_UNTIL && c)
            return STEP_OK;
        if (k->op == OP_IF && !c)
            return STEP_OK;
        if (push(t, b, 0))
            return STEP_ERROR;
        t->pc = k->op == OP_IFELSE && !c ? k->sub[1] : k->sub[0];
        return STEP_OK;
    }
    case OP_STOP:
        stop_all = 1;
        return STEP_DONE;
    case OP_CLEAR: case OP_COLOUR: {
        uint32_t rgb;
        if (colour_value(k->lit[0], &rgb))
            return fail(b, "", k->lit[0], " isn't a colour"), STEP_ERROR;
        if (k->op == OP_CLEAR)
            gfx_fill_rect(&stage, 0, 0, STAGE_W, STAGE_H, rgb);
        else
            pen = rgb;
        drew = 1;
        return STEP_OK;
    }
    case OP_RGB:
        for (int i = 0; i < 3; i++)
            if (arg_num(b, i, &a[i], 0))
                return STEP_ERROR;
        pen = clamp255(a[0]) << 16 | clamp255(a[1]) << 8 | clamp255(a[2]);
        return STEP_OK;
    case OP_CIRCLE: case OP_RECT: case OP_LINE: {
        int n = k->op == OP_CIRCLE ? 3 : 4;
        for (int i = 0; i < n; i++) {
            if (arg_num(b, i, &a[i], 0))
                return STEP_ERROR;
            if (a[i] < -10000 || a[i] > 10000)   /* keep the drawing maths in range */
                a[i] = a[i] < 0 ? -10000 : 10000;
        }
        if (k->op == OP_CIRCLE && a[2] > 0)
            gfx_fill_circle(&stage, a[0], a[1], a[2], pen);
        else if (k->op == OP_RECT)
            gfx_fill_rect(&stage, a[0], a[1], a[2], a[3], pen);
        else if (k->op == OP_LINE)
            gfx_line(&stage, a[0], a[1], a[2], a[3], pen);
        drew = 1;
        return STEP_OK;
    }
    case OP_WRITE: {
        char text[TEXT_MAX];
        if (arg(b, 0, &v, 0) || arg_num(b, 1, &a[1], 0) || arg_num(b, 2, &a[2], 0))
            return STEP_ERROR;
        text_of(&v, text);
        if (a[1] > -1000 && a[1] < 1000 && a[2] > -1000 && a[2] < 1000)
            text_draw(&stage, a[1], a[2], text, TEXT_BODY, pen);
        drew = 1;
        return STEP_OK;
    }
    case OP_PRINT: {
        char text[TEXT_MAX];
        if (arg(b, 0, &v, 0))
            return STEP_ERROR;
        text_of(&v, text);
        console_add(text, 0);
        k86_logf("blocksmith: print %s", text);
        return STEP_OK;
    }
    case OP_SET:
        if (arg(b, 1, &v, 0))
            return STEP_ERROR;
        if (v.text && parse_int(v.s, &a[0]) == 0) {   /* "5" typed in: the number 5 */
            v.text = 0;
            v.n = a[0];
        }
        return set_var(b, k->lit[0], &v) ? STEP_ERROR : STEP_OK;
    case OP_CHANGE: {
        int i = find_var(k->lit[0]);
        if (arg_num(b, 1, &a[1], 0))
            return STEP_ERROR;
        if (i < 0)
            return fail(b, k->lit[0][0] ? "There's no variable " : "Choose a variable",
                        k->lit[0][0] ? k->lit[0] : 0, 0), STEP_ERROR;
        if (num_of(b, &vars[i], &a[0]))
            return STEP_ERROR;
        vars[i].text = 0;
        vars[i].n = (int32_t)((uint32_t)a[0] + (uint32_t)a[1]);
        return STEP_OK;
    }
    default:                                /* a value on its own: show it, like Scratch */
        if (eval(b, &v, 0))
            return STEP_ERROR;
        {
            char text[TEXT_MAX];
            text_of(&v, text);
            console_add(text, 0);
        }
        return STEP_OK;
    }
}

/* --- threads ------------------------------------------------------------------------------- */

static int start(int top)
{
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].used && threads[i].top == top)
            return 0;                       /* already running: leave it be */
    for (int i = 0; i < MAX_THREADS; i++)
        if (!threads[i].used) {
            threads[i] = (struct thread){ 1, (int16_t)top, (int16_t)top, 0, { { 0, 0 } }, 0 };
            return 0;
        }
    return -1;
}

static int has_events(void)
{
    for (int i = 0; i < norder; i++) {
        int op = blocks[order[i]].op;
        if (op == OP_KEY || op == OP_CLICK)
            return 1;
    }
    return 0;
}

int run_active(void)
{
    return mode;
}

int run_threads(void)
{
    int n = 0;
    for (int i = 0; i < MAX_THREADS; i++)
        n += threads[i].used;
    return n;
}

static void begin(void)
{
    if (!mode) {
        t0 = k86_uptime_ms();
        seed = t0 * 2654435761u | 1;
    }
    mode = 1;
    error_block = NO;
}

void run_start(void)
{
    run_stop();
    stage_reset();
    console_clear();
    for (int i = 0; i < MAX_VARS; i++)
        vars[i] = (struct value){ 0, 0, { 0 } };
    begin();
    for (int i = 0; i < norder; i++)
        if (blocks[order[i]].op == OP_RUN)
            start(order[i]);
    changed |= RUN_STAGE | RUN_CONSOLE;
    k86_log("blocksmith: run");
}

void run_script(int top)
{
    begin();
    start(top);
}

void run_stop(void)
{
    memset(threads, 0, sizeof(threads));
    memset(keys_down, 0, sizeof(keys_down));
    mode = 0;
}

void run_event_key(int key_index)
{
    if (!mode)
        return;
    for (int i = 0; i < norder; i++) {
        struct block *k = &blocks[order[i]];
        if (k->op != OP_KEY)
            continue;
        int want = 0;
        for (int j = 0; j < menu_count('k'); j++)
            if (str_eq(k->lit[0], menu_item('k', j)))
                want = j;
        if (want == key_index || want == NKEYS_MENU - 1)
            start(order[i]);
    }
}

void run_event_click(void)
{
    if (!mode)
        return;
    for (int i = 0; i < norder; i++)
        if (blocks[order[i]].op == OP_CLICK)
            start(order[i]);
}

void run_set_key(int i, int down)
{
    if (i >= 0 && i < NKEYS_MENU)
        keys_down[i] = (uint8_t)down;
}

void run_set_mouse(int x, int y, int down)
{
    mouse_x = x;
    mouse_y = y;
    mouse_down = down;
}

void run_forget_block(int b)
{
    (void)b;
    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &threads[i];
        if (!t->used)
            continue;
        int gone = !blocks[t->top].used || (t->pc != NO && !blocks[t->pc].used);
        for (int d = 0; d < t->depth; d++)
            gone |= !blocks[t->f[d].block].used;
        if (gone)
            t->used = 0;
    }
}

int run_tick(void)
{
    if (!mode)
        return 0;
    uint32_t now = k86_uptime_ms(), start_ms = now;
    drew = 0;
    stop_all = 0;
    static int turn;                        /* who goes first: each in turn, so none starves */
    turn = (turn + 1) % MAX_THREADS;
    for (int j = 0; j < MAX_THREADS && !stop_all; j++) {
        struct thread *t = &threads[(turn + j) % MAX_THREADS];
        if (!t->used || (int32_t)(now - t->wake) < 0)
            continue;
        for (int n = 0;; n++) {
            int r = step(t);
            if (r == STEP_DONE || r == STEP_ERROR) {
                t->used = 0;
                if (r == STEP_ERROR)
                    stop_all = 1;
                break;
            }
            if (r == STEP_YIELD || stop_all)
                break;
            if ((n & 127) == 127 && k86_uptime_ms() - start_ms > TICK_MS)
                break;                      /* long enough: the rest next tick */
        }
        if (k86_uptime_ms() - start_ms > TICK_MS)
            break;
    }
    if (drew)
        changed |= RUN_STAGE;
    if (stop_all || (!run_threads() && !has_events())) {
        run_stop();
        changed |= RUN_ENDED;
    }
    int c = changed;
    changed = 0;
    return c;
}
