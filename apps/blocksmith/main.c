/* LiteKern X — Blocksmith: the window (see blocksmith.h).
 *
 *   +-----------+--------------------------+----------------+
 *   | Events    |                          |  +----------+  |
 *   | Control.. |  the workspace: scripts  |  |  stage   |  |
 *   |-----------|  (drag the background    |  +----------+  |
 *   | the       |   to look around)        |  Console       |
 *   | category's|                          |  ...           |
 *   | blocks    |                          |                |
 *   +-----------+--------------------------+----------------+
 *
 * Drag a block from the palette into the workspace, and onto other blocks
 * to snap it there; drag it back onto the palette to delete it. Click a
 * typed input to edit it, a menu input to choose, a script to run just it.
 * Keyboard: Ctrl+Enter or F5 runs, Esc stops, Ctrl+S saves. */
#include "apps/blocksmith/blocksmith.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"

#define PAL_W       272
#define SIDE_W      344
#define CHIP_H      30
#define PAL_TOP     120         /* the category chips, above the palette's blocks */
#define SNAP_RANGE  28
#define DRAG_START  4           /* pixels the pointer moves before a press drags */
#define MARGIN      10          /* around a script: tabs, hat tops, outlines */
#define TICK_MS     33
#define ITEM_H      30          /* a menu's rows */
#define MAX_TPL     40
#define MAX_PROJECTS 32

enum { BTN_NEW = 1, BTN_OPEN, BTN_SAVE, BTN_STOP, BTN_RUN };

static struct k86_window win;
static struct gfx_surface canvas;
static struct gfx_rect pal, ws, side, stage_r, cons_r;

/* The palette. */
static int cat_sel;
static int16_t tpl[MAX_TPL];
static int ntpl, pal_scroll, pal_h;
static struct wg_button newvar;         /* Variables: "New variable" (palette coordinates) */

/* The workspace's view: the plane's point at its top left. */
static int scroll_x, scroll_y;

/* The pointer. */
enum { P_IDLE, P_PRESSED, P_DRAG, P_PAN, P_PAN_PAL };
static int pstate, press_x, press_y, press_block = NO, press_slot, press_tpl, press_scroll_x,
           press_scroll_y, press_in_stage;
static int drag = NO, grab_dx, grab_dy;
static struct snap snap;

/* Typing into an input. */
static int edit_block = NO, edit_slot;
static char edit_old[LIT_MAX];

/* A menu input's menu. */
static int menu_block = NO, menu_slot, menu_hover = -1;
static char menu_kind;
static struct gfx_rect menu_r;

/* Dialogs. */
enum { M_NONE, M_DIALOG, M_OPEN };
enum { D_NEWVAR, D_SAVE, D_NEW, D_INFO };
static int modal, dialog_kind;
static struct wg_dialog dialog;
static char projects[MAX_PROJECTS][48];
static int nprojects;
static struct wg_list open_list;
static struct wg_button open_buttons[2];
static struct gfx_rect open_card;

static const struct theme *T(void) { return theme_get(); }

static struct gfx_rect R(int x, int y, int w, int h) { return (struct gfx_rect){ x, y, w, h }; }

/* --- layout ----------------------------------------------------------------------------------- */

static void layout(void)
{
    int sw = canvas.w < 900 ? canvas.w * SIDE_W / 900 : SIDE_W;
    pal = R(0, 0, PAL_W, canvas.h);
    side = R(canvas.w - sw, 0, sw, canvas.h);
    ws = R(PAL_W, 0, side.x - PAL_W, canvas.h);
    stage_r = R(side.x + (side.w - STAGE_W) / 2, 12, STAGE_W, STAGE_H);
    cons_r = R(side.x + 12, stage_r.y + STAGE_H + 44, side.w - 24, canvas.h - (stage_r.y + STAGE_H + 44) - 12);
}

static void set_header(void)
{
    struct k86_header h;
    memset(&h, 0, sizeof(h));
    str_copy(h.title, project_name[0] ? project_name : "Blocksmith", sizeof(h.title));
    static const struct { int side, id; const char *label; } b[] = {
        { 0, BTN_NEW, "New" }, { 0, BTN_OPEN, "Open" },
        { 1, BTN_RUN, "Run" }, { 1, BTN_STOP, "Stop" }, { 1, BTN_SAVE, "Save" },
    };
    h.nbuttons = 5;
    for (int i = 0; i < 5; i++) {
        h.buttons[i].side = b[i].side;
        h.buttons[i].id = b[i].id;
        str_copy(h.buttons[i].label, b[i].label, sizeof(h.buttons[i].label));
    }
    k86_header(&h);
}

/* --- the palette ------------------------------------------------------------------------------ */

static void build_palette(void)
{
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (blocks[i].palette)
            blocks[i].used = 0;
    ntpl = 0;
    int y = 18;
    if (cat_sel == CAT_VARS) {
        newvar = (struct wg_button){ .r = { 14, 12, wg_button_width("New variable"), 34 },
                                     .label = "New variable" };
        y = 64;
    }
    for (int op = 0; op < OPS && ntpl < MAX_TPL; op++) {
        if (ops[op].cat != cat_sel)
            continue;
        int copies = op == OP_VAR ? nvars : 1;
        for (int c = 0; c < copies && ntpl < MAX_TPL; c++) {
            int b = block_new(op, 1);
            if (b == NO)
                break;
            if (op == OP_VAR)
                str_copy(blocks[b].lit[0], var_names[c], LIT_MAX);
            if (ops[op].shape == SH_HAT)
                y += 8;
            layout_move(b, 14, y);
            y += blocks[b].sh + 14;
            tpl[ntpl++] = (int16_t)b;
        }
    }
    pal_h = y;
    int most = pal_h - (pal.h - PAL_TOP);
    if (pal_scroll > most)
        pal_scroll = most > 0 ? most : 0;
}

static struct gfx_rect chip_rect(int i)
{
    int w = (pal.w - 26) / 2;
    return R(pal.x + 10 + (i % 2) * (w + 6), pal.y + 10 + (i / 2) * (CHIP_H + 6), w, CHIP_H);
}

static struct gfx_rect pal_list(void)
{
    return R(pal.x, pal.y + PAL_TOP, pal.w - 1, pal.h - PAL_TOP);   /* not the border */
}

static void draw_palette(struct gfx_rect clip)
{
    struct gfx_surface s = gfx_sub(&canvas, clip);
    gfx_fill_rect(&s, 0, 0, s.w, s.h, T()->window_bg);
    for (int i = 0; i < CATS; i++) {
        struct gfx_rect c = chip_rect(i);
        int x = c.x - clip.x, y = c.y - clip.y;
        if (i == cat_sel)
            gfx_fill_round_rect(&s, x, y, c.w, c.h, 8, T()->row_selected);
        gfx_fill_circle(&s, x + 15, y + c.h / 2, 7, cat_colour(i));
        text_draw(&s, x + 30, y + (c.h - text_height(TEXT_BODY)) / 2, cat_names[i],
                  i == cat_sel ? TEXT_BOLD : TEXT_BODY, T()->fg);
    }
    gfx_fill_rect(&s, pal.x - clip.x, pal.y + PAL_TOP - 1 - clip.y, pal.w, 1, T()->border);
    gfx_fill_rect(&s, pal.x + pal.w - 1 - clip.x, 0, 1, s.h, T()->border);

    struct gfx_rect lr = gfx_rect_intersect(clip, pal_list());
    if (gfx_rect_empty(lr))
        return;
    struct gfx_surface l = gfx_sub(&canvas, lr);
    int ox = lr.x - pal.x, oy = lr.y - (pal.y + PAL_TOP) + pal_scroll;
    if (cat_sel == CAT_VARS) {
        struct wg_button b = newvar;
        b.r.x -= ox;
        b.r.y -= oy;
        wg_button_draw(&l, &b);
        newvar.dirty = 0;
    }
    for (int i = 0; i < ntpl; i++) {
        struct block *k = &blocks[tpl[i]];
        if (k->y - 10 - oy > l.h || k->y + k->sh + 10 - oy < 0)
            continue;
        draw_block_only(&l, ox, oy, tpl[i]);
    }
}

/* --- the workspace -------------------------------------------------------------------------- */

static struct gfx_rect script_box(int top)          /* in canvas coordinates */
{
    struct block *k = &blocks[top];
    return R(ws.x + k->x - scroll_x - MARGIN, ws.y + k->y - scroll_y - MARGIN, k->sw + 2 * MARGIN,
             k->sh + 2 * MARGIN);
}

static void caret(struct gfx_surface *s, int ox, int oy)
{
    struct block *k = &blocks[edit_block];
    const char *t = k->lit[edit_slot];
    int tw = text_width(t, TEXT_BODY), w = k->slot_w[edit_slot];
    int x = k->x + k->slot_x[edit_slot] - ox, y = k->y + k->slot_y[edit_slot] - oy;
    int cx = block_slot_kind(k->op, edit_slot) == 'n' ? x + (w - tw) / 2 + tw + 1 : x + 6 + tw + 1;
    gfx_rect_outline(s, x - 2, y - 2, w + 4, k->slot_h[edit_slot] + 4, T()->accent_bg);
    gfx_fill_rect(s, cx, y + 3, 2, k->slot_h[edit_slot] - 6, 0x1b1330);
}

static void draw_ws(struct gfx_rect clip)
{
    struct gfx_surface s = gfx_sub(&canvas, clip);
    int ox = clip.x - ws.x + scroll_x, oy = clip.y - ws.y + scroll_y;
    gfx_fill_rect(&s, 0, 0, s.w, s.h, T()->view_bg);
    /* Blockly's dotted grid. */
    uint32_t dot = gfx_mix(T()->ink, T()->view_bg, 28);
    for (int gy = (oy / 24) * 24; gy - oy < s.h; gy += 24)
        for (int gx = (ox / 24) * 24; gx - ox < s.w; gx += 24)
            gfx_fill_rect(&s, gx - ox, gy - oy, 2, 2, dot);
    if (!norder) {
        const char *hint = "Drag blocks here from the left";
        int tw = text_width(hint, TEXT_BODY);
        text_draw(&s, ws.x + (ws.w - tw) / 2 - clip.x, ws.y + ws.h / 2 - 10 - clip.y, hint, TEXT_BODY,
                  T()->fg_dim);
    }
    for (int i = 0; i < norder; i++) {
        int top = order[i];
        if (top == drag)
            continue;
        struct gfx_rect b = gfx_rect_intersect(script_box(top), clip);
        if (gfx_rect_empty(b))
            continue;
        draw_script(&s, ox, oy, top);
    }
    if (pstate == P_DRAG && snap.ok)
        gfx_fill_round_rect(&s, snap.x - ox, snap.y - oy, snap.w, snap.h, 2,
                            snap.pslot >= PS_ARG0 ? T()->accent : T()->fg);
    if (pstate == P_DRAG && snap.ok && snap.pslot >= PS_ARG0)
        gfx_rect_outline(&s, snap.x - ox - 2, snap.y - oy - 2, snap.w + 4, snap.h + 4, T()->accent);
    if (edit_block != NO)
        caret(&s, ox, oy);
}

/* --- the stage and the console ----------------------------------------------------------- */

static void draw_side(struct gfx_rect clip)
{
    struct gfx_surface s = gfx_sub(&canvas, clip);
    const struct theme *t = T();
    gfx_fill_rect(&s, 0, 0, s.w, s.h, t->window_bg);
    gfx_fill_rect(&s, side.x - clip.x, 0, 1, s.h, t->border);
    int sx = stage_r.x - clip.x, sy = stage_r.y - clip.y;
    gfx_fill_rect(&s, sx - 1, sy - 1, STAGE_W + 2, STAGE_H + 2, t->border);
    gfx_blit(&s, sx, sy, &stage, 0, 0, STAGE_W, STAGE_H);

    int ly = stage_r.y + STAGE_H + 14 - clip.y;
    const char *state = run_active() ? "Running" : "Stopped";
    text_draw(&s, cons_r.x - clip.x, ly, "Console", TEXT_BOLD, t->fg);
    int w = text_width(state, TEXT_SMALL);
    if (run_active())
        gfx_fill_circle(&s, cons_r.x + cons_r.w - w - 12 - clip.x, ly + 9, 4, t->accent);
    text_draw(&s, cons_r.x + cons_r.w - w - clip.x, ly + 2, state, TEXT_SMALL,
              run_active() ? t->accent : t->fg_dim);

    int cx = cons_r.x - clip.x, cy = cons_r.y - clip.y;
    gfx_fill_round_rect(&s, cx, cy, cons_r.w, cons_r.h, 8, t->view_bg);
    int lh = text_height(TEXT_BODY), fits = (cons_r.h - 12) / lh;
    if (!nconsole)
        text_draw(&s, cx + 10, cy + 6, "Print blocks write here.", TEXT_BODY, t->fg_dim);
    int first = nconsole > fits ? nconsole - fits : 0;
    for (int i = first; i < nconsole; i++)
        text_draw_fit(&s, cx + 10, cy + 6 + (i - first) * lh, console[i].text, cons_r.w - 20, TEXT_BODY,
                      console[i].error ? t->destructive : t->fg);
}

/* --- the menu --------------------------------------------------------------------------------- */

static int menu_items(void)
{
    return menu_count(menu_kind) + (menu_kind == 'v');      /* variables: and "New variable" */
}

static const char *menu_label(int i)
{
    return i < menu_count(menu_kind) ? menu_item(menu_kind, i) : "New variable" TEXT_ELLIPSIS;
}

static void draw_menu(struct gfx_rect clip)
{
    struct gfx_rect r = gfx_rect_intersect(clip, menu_r);
    if (gfx_rect_empty(r))
        return;
    struct gfx_surface s = gfx_sub(&canvas, clip);
    int x = menu_r.x - clip.x, y = menu_r.y - clip.y;
    gfx_shadow(&s, x, y, menu_r.w, menu_r.h, 10, 10, 70);
    gfx_fill_round_rect(&s, x, y, menu_r.w, menu_r.h, 10, T()->dialog_bg);
    for (int i = 0; i < menu_items(); i++) {
        int iy = y + 6 + i * ITEM_H, tx = x + 14;
        if (i == menu_hover)
            gfx_fill_round_rect(&s, x + 6, iy, menu_r.w - 12, ITEM_H, 6, T()->row_hover);
        uint32_t rgb;
        if (menu_kind == 'c' && !colour_value(menu_label(i), &rgb)) {
            gfx_fill_circle(&s, tx + 8, iy + ITEM_H / 2, 8, T()->border);
            gfx_fill_circle(&s, tx + 8, iy + ITEM_H / 2, 7, rgb);
            tx += 26;
        }
        text_draw(&s, tx, iy + (ITEM_H - text_height(TEXT_BODY)) / 2, menu_label(i), TEXT_BODY, T()->fg);
    }
}

/* --- redrawing -------------------------------------------------------------------------------- */

static void draw_dialogs(void);

static void redraw(struct gfx_rect r)
{
    r = gfx_rect_intersect(r, R(0, 0, canvas.w, canvas.h));
    if (gfx_rect_empty(r))
        return;
    struct gfx_rect p = gfx_rect_intersect(r, pal), w = gfx_rect_intersect(r, ws),
                    sd = gfx_rect_intersect(r, side);
    if (!gfx_rect_empty(p))
        draw_palette(p);
    if (!gfx_rect_empty(w))
        draw_ws(w);
    if (!gfx_rect_empty(sd))
        draw_side(sd);
    if (drag != NO) {
        struct gfx_surface s = gfx_sub(&canvas, r);
        draw_script(&s, r.x - ws.x + scroll_x, r.y - ws.y + scroll_y, drag);
    }
    if (menu_block != NO)
        draw_menu(r);
    k86_present(r.x, r.y, r.w, r.h);
}

static void redraw_all(void)
{
    if (modal) {
        draw_dialogs();
        return;
    }
    redraw(R(0, 0, canvas.w, canvas.h));
}

static void redraw_ws(void)
{
    redraw(ws);
}

/* --- dialogs ------------------------------------------------------------------------------------ */

static void open_row(void *ctx, int i, struct wg_row *out)
{
    (void)ctx;
    *out = (struct wg_row){ projects[i], 0, WG_ICON_FILE };
}

static void draw_open(int whole)
{
    const struct theme *t = T();
    if (whole) {
        redraw(R(0, 0, canvas.w, canvas.h));
        gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 110);
    }
    struct gfx_rect c = open_card;
    gfx_fill_round_rect(&canvas, c.x, c.y, c.w, c.h, 14, t->dialog_bg);
    text_draw(&canvas, c.x + 24, c.y + 20, "Open a project", TEXT_HEADING, t->fg);
    if (nprojects)
        wg_list_draw(&canvas, &open_list);
    else
        text_draw(&canvas, c.x + 24, c.y + 70, "No saved projects yet.", TEXT_BODY, t->fg_dim);
    for (int i = 0; i < 2; i++)
        wg_button_draw(&canvas, &open_buttons[i]);
    if (whole)
        k86_present(0, 0, canvas.w, canvas.h);
    else
        k86_present(c.x, c.y, c.w, c.h);
}

static void draw_dialogs(void)
{
    if (modal == M_DIALOG) {
        redraw(R(0, 0, canvas.w, canvas.h));
        wg_dialog_layout(&dialog, canvas.w, canvas.h);
        wg_dialog_show(&canvas, &dialog);
        k86_present(0, 0, canvas.w, canvas.h);
    } else if (modal == M_OPEN) {
        draw_open(1);
    }
}

static void show_dialog(int kind, const char *title, const char *body, const char *action,
                        enum wg_button_style style, int entry, const char *text)
{
    dialog_kind = kind;
    wg_dialog_init(&dialog, title, body, action, style, entry);
    if (entry && text)
        wg_entry_set(&dialog.entry, text);
    modal = M_DIALOG;
    draw_dialogs();
}

static void info(const char *title, const char *body)
{
    show_dialog(D_INFO, title, body, "OK", WG_BUTTON_SUGGESTED, 0, 0);
}

static void close_modal(void)
{
    modal = M_NONE;
    redraw_all();
}

static void show_open(void)
{
    const char *why = 0;
    nprojects = project_list(projects, MAX_PROJECTS, &why);
    if (nprojects < 0) {
        info("Can't open projects", why);
        return;
    }
    int w = 380, h = 400;
    open_card = R((canvas.w - w) / 2, (canvas.h - h) / 2, w, h);
    open_list = (struct wg_list){ .r = { open_card.x + 24, open_card.y + 62, w - 48, h - 62 - 70 },
                                  .count = nprojects, .selected = nprojects ? 0 : -1, .hover = -1,
                                  .row = open_row, .under = T()->dialog_bg, .last_click_row = -1 };
    int bw = (w - 48 - 12) / 2, by = open_card.y + h - 24 - 36;
    open_buttons[0] = (struct wg_button){ .r = { open_card.x + 24, by, bw, 36 }, .label = "Cancel",
                                          .under = T()->dialog_bg };
    open_buttons[1] = (struct wg_button){ .r = { open_card.x + 24 + bw + 12, by, bw, 36 }, .label = "Open",
                                          .style = WG_BUTTON_SUGGESTED, .under = T()->dialog_bg,
                                          .disabled = !nprojects };
    modal = M_OPEN;
    draw_open(1);
}

static void load_project(const char *name)
{
    const char *why = 0;
    if (project_open(name, &why)) {
        build_palette();
        close_modal();
        info("Can't open the project", why);
        return;
    }
    scroll_x = scroll_y = 0;
    console_clear();
    stage_reset();
    build_palette();
    set_header();
    close_modal();
}

static int bad_char(char c)
{
    return c < ' ' || c > '~' || strchr("/\\:*?\"<>|", c);
}

static void do_save(const char *name)
{
    const char *why = 0;
    if (!name[0] || name[0] == ' ' || name[0] == '.') {
        wg_dialog_set_body(&dialog, "Give the project a name.", 1);
        draw_dialogs();
        return;
    }
    for (const char *p = name; *p; p++)
        if (bad_char(*p)) {
            wg_dialog_set_body(&dialog, "Names can't have / \\ : * ? \" < > |", 1);
            draw_dialogs();
            return;
        }
    if (project_save(name, &why)) {
        wg_dialog_set_body(&dialog, why, 1);
        draw_dialogs();
        return;
    }
    set_header();
    close_modal();
}

static void save(void)
{
    if (!project_name[0]) {
        show_dialog(D_SAVE, "Save the project", "Its name:", "Save", WG_BUTTON_SUGGESTED, 1, "");
        return;
    }
    const char *why = 0;
    if (project_save(project_name, &why))
        info("Couldn't save", why);
}

static int add_var(const char *name)
{
    if (!name[0])
        return wg_dialog_set_body(&dialog, "Give it a name.", 1), -1;
    if ((int)strlen(name) >= VAR_MAX)
        return wg_dialog_set_body(&dialog, "That name is too long.", 1), -1;
    for (int i = 0; i < nvars; i++)
        if (str_eq(name, var_names[i]))
            return wg_dialog_set_body(&dialog, "There's already a variable with that name.", 1), -1;
    if (nvars == MAX_VARS)
        return wg_dialog_set_body(&dialog, "That's as many variables as a project can have.", 1), -1;
    str_copy(var_names[nvars++], name, VAR_MAX);
    project_dirty = 1;
    return 0;
}

static void dialog_chose(int button)
{
    if (button == 0) {                      /* Cancel */
        menu_block = NO;
        close_modal();
        return;
    }
    switch (dialog_kind) {
    case D_NEWVAR:
        if (add_var(dialog.entry.text)) {
            draw_dialogs();
            return;
        }
        if (menu_block != NO) {             /* chosen from an input's menu: put it there */
            str_copy(blocks[menu_block].lit[menu_slot], var_names[nvars - 1], LIT_MAX);
            layout_script(block_top(menu_block));
            menu_block = NO;
        }
        build_palette();
        close_modal();
        break;
    case D_SAVE:
        do_save(dialog.entry.text);
        break;
    case D_NEW:
        project_new_empty();
        build_palette();
        set_header();
        scroll_x = scroll_y = 0;
        console_clear();
        stage_reset();
        close_modal();
        break;
    default:
        close_modal();
        break;
    }
}

/* --- editing an input ---------------------------------------------------------------------- */

static void end_edit(int keep)
{
    if (edit_block == NO)
        return;
    if (!keep)
        str_copy(blocks[edit_block].lit[edit_slot], edit_old, LIT_MAX);
    int top = block_top(edit_block);
    edit_block = NO;
    layout_script(top);
    redraw_ws();
}

static void begin_edit(int b, int slot)
{
    edit_block = b;
    edit_slot = slot;
    str_copy(edit_old, blocks[b].lit[slot], LIT_MAX);
    redraw_ws();
}

static void edit_key(const struct key_event *k)
{
    char *t = blocks[edit_block].lit[edit_slot];
    int n = (int)strlen(t), kind = block_slot_kind(blocks[edit_block].op, edit_slot);
    if (k->key == KEY_ENTER || k->key == KEY_TAB) {
        end_edit(1);
        return;
    }
    if (k->key == KEY_ESC) {
        end_edit(0);
        return;
    }
    if (k->key == KEY_BACKSPACE && n)
        t[n - 1] = '\0';
    else if (k->ascii >= ' ' && k->ascii <= '~' && n < LIT_MAX - 1 && !(k->mods & MOD_CTRL)) {
        if (kind == 'n' && !((k->ascii >= '0' && k->ascii <= '9') || (k->ascii == '-' && !n)))
            return;                         /* numbers: digits, and a minus first */
        t[n] = (char)k->ascii;
        t[n + 1] = '\0';
    } else {
        return;
    }
    project_dirty = 1;
    struct gfx_rect before = script_box(block_top(edit_block));
    layout_script(block_top(edit_block));
    redraw(gfx_rect_union(before, script_box(block_top(edit_block))));
}

/* --- menus ------------------------------------------------------------------------------------- */

static void open_menu(int b, int slot)
{
    struct block *k = &blocks[b];
    menu_block = b;
    menu_slot = slot;
    menu_kind = block_slot_kind(k->op, slot);
    menu_hover = -1;
    int w = 120;
    for (int i = 0; i < menu_items(); i++) {
        int iw = text_width(menu_label(i), TEXT_BODY) + 28 + (menu_kind == 'c' ? 26 : 0);
        w = iw > w ? iw : w;
    }
    int h = menu_items() * ITEM_H + 12;
    int x = ws.x + k->x + k->slot_x[slot] - scroll_x, y = ws.y + k->y + k->slot_y[slot] + k->slot_h[slot] + 4 - scroll_y;
    if (y + h > canvas.h - 4)
        y = canvas.h - 4 - h;
    if (y < 4)
        y = 4;
    if (x + w > canvas.w - 4)
        x = canvas.w - 4 - w;
    menu_r = R(x, y, w, h);
    redraw(R(menu_r.x - 12, menu_r.y - 12, menu_r.w + 24, menu_r.h + 24));
}

static void close_menu(void)
{
    if (menu_block == NO)
        return;
    struct gfx_rect r = R(menu_r.x - 12, menu_r.y - 12, menu_r.w + 24, menu_r.h + 24);
    menu_block = NO;
    redraw(r);
}

static int menu_at(int x, int y)
{
    if (x < menu_r.x || x >= menu_r.x + menu_r.w || y < menu_r.y + 6)
        return -1;
    int i = (y - menu_r.y - 6) / ITEM_H;
    return i < menu_items() ? i : -1;
}

static void menu_choose(int i)
{
    if (menu_kind == 'v' && i == menu_count('v')) {
        struct gfx_rect r = R(menu_r.x - 12, menu_r.y - 12, menu_r.w + 24, menu_r.h + 24);
        int b = menu_block;
        menu_block = NO;
        redraw(r);
        menu_block = b;                     /* the dialog puts the new one in this input */
        show_dialog(D_NEWVAR, "New variable", "Its name:", "Add", WG_BUTTON_SUGGESTED, 1, "");
        return;
    }
    str_copy(blocks[menu_block].lit[menu_slot], menu_item(menu_kind, i), LIT_MAX);
    project_dirty = 1;
    int top = blocks[menu_block].palette ? NO : block_top(menu_block);
    menu_block = NO;
    if (top != NO)
        layout_script(top);
    redraw_all();
}

/* --- the pointer ------------------------------------------------------------------------------- */

static int in(struct gfx_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* The script under (x, y) in the workspace (canvas coordinates), front first. */
static int ws_hit(int x, int y, int *slot)
{
    for (int i = norder - 1; i >= 0; i--) {
        int b = hit_script(order[i], x - ws.x + scroll_x, y - ws.y + scroll_y, slot);
        if (b != NO)
            return b;
    }
    return NO;
}

static int pal_hit(int x, int y)
{
    int px = x - pal.x, py = y - (pal.y + PAL_TOP) + pal_scroll, slot;
    for (int i = 0; i < ntpl; i++) {
        int b = hit_script(tpl[i], px, py, &slot);
        if (b != NO)
            return tpl[i];
    }
    return NO;
}

static struct gfx_rect drag_box(void)
{
    struct gfx_rect r = script_box(drag);
    if (snap.ok)
        r = gfx_rect_union(r, R(ws.x + snap.x - scroll_x - 4, ws.y + snap.y - scroll_y - 4, snap.w + 8, snap.h + 8));
    return r;
}

static void start_drag(int x, int y)
{
    if (press_tpl) {
        int c = block_copy_tree(press_block, 0);
        if (c == NO) {
            pstate = P_IDLE;
            info("Too many blocks", "That's as many blocks as a project can have.");
            return;
        }
        struct block *t = &blocks[press_block];
        grab_dx = press_x - (pal.x + t->x);
        grab_dy = press_y - (pal.y + PAL_TOP + t->y - pal_scroll);
        order_add(c);
        drag = c;
    } else {
        int b = press_block, old_top = block_top(b);
        block_detach(b);                    /* it and what follows it */
        if (old_top != b)
            layout_script(old_top);
        drag = b;
        order_front(drag);
        grab_dx = press_x - (ws.x + blocks[drag].x - scroll_x);
        grab_dy = press_y - (ws.y + blocks[drag].y - scroll_y);
    }
    pstate = P_DRAG;
    snap.ok = 0;
    layout_move(drag, x - grab_dx - ws.x + scroll_x, y - grab_dy - ws.y + scroll_y);
    redraw_all();
}

static void drag_to(int x, int y)
{
    struct gfx_rect before = drag_box();
    layout_move(drag, x - grab_dx - ws.x + scroll_x, y - grab_dy - ws.y + scroll_y);
    snap = x >= ws.x ? find_snap(drag, SNAP_RANGE) : (struct snap){ 0 };
    redraw(gfx_rect_union(before, drag_box()));
}

static void drop(int x)
{
    int d = drag;
    drag = NO;
    pstate = P_IDLE;
    if (x < ws.x) {                         /* back onto the palette: deleted */
        order_remove(d);
        block_free_tree(d);
        run_forget_block(d);
    } else if (snap.ok) {
        apply_snap(d, &snap);
    } else {
        int bx = blocks[d].x < 0 ? 0 : blocks[d].x, by = blocks[d].y < 8 ? 8 : blocks[d].y;
        layout_move(d, bx, by);
    }
    snap.ok = 0;
    project_dirty = 1;
    layout_all();
    redraw_all();
}

static void click(void)
{
    if (press_tpl) {
        return;
    }
    int b = press_block;
    if (press_slot >= 0) {
        char kind = block_slot_kind(blocks[b].op, press_slot);
        if (kind == 'n' || kind == 's')
            begin_edit(b, press_slot);
        else if (kind == 'v' || kind == 'k' || kind == 'c')
            open_menu(b, press_slot);
        return;
    }
    run_script(block_top(b));               /* a click on a script runs it */
    k86_timer_set(TICK_MS);
    redraw(side);
}

static void stage_mouse(const struct wg_pointer *p)
{
    int down = p->held && press_in_stage;
    run_set_mouse(p->x - stage_r.x, p->y - stage_r.y, down);
}

static void pointer_modal(const struct wg_pointer *p)
{
    if (modal == M_DIALOG) {
        int c = wg_dialog_pointer(&dialog, p);
        if (c != WG_DIALOG_NONE)
            dialog_chose(c);
        else if (dialog.dirty) {
            wg_dialog_draw(&canvas, &dialog);
            k86_present(dialog.r.x, dialog.r.y, dialog.r.w, dialog.r.h);
        }
        return;
    }
    enum wg_list_result lr = nprojects ? wg_list_pointer(&open_list, p) : WG_LIST_NONE;
    if (lr == WG_LIST_ACTIVATE) {
        load_project(projects[open_list.selected]);
        return;
    }
    if (wg_button_pointer(&open_buttons[0], p)) {
        close_modal();
        return;
    }
    if (wg_button_pointer(&open_buttons[1], p) && open_list.selected >= 0) {
        load_project(projects[open_list.selected]);
        return;
    }
    if (open_list.dirty || open_buttons[0].dirty || open_buttons[1].dirty)
        draw_open(0);
}

static void pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    if (modal) {
        pointer_modal(&p);
        return;
    }
    stage_mouse(&p);

    if (menu_block != NO) {
        int i = menu_at(p.x, p.y);
        if (p.down) {
            if (i >= 0)
                menu_choose(i);
            else
                close_menu();
            return;
        }
        if (i != menu_hover) {
            menu_hover = i;
            redraw(menu_r);
        }
        return;
    }

    if (p.down) {
        if (edit_block != NO) {
            int slot, b = ws_hit(p.x, p.y, &slot);
            if (b == edit_block && slot == edit_slot)
                return;                     /* a click in the input being typed in */
            end_edit(1);
        }
        press_x = p.x;
        press_y = p.y;
        press_in_stage = in(stage_r, p.x, p.y);
        press_tpl = 0;
        press_block = NO;
        pstate = P_IDLE;
        if (press_in_stage) {
            run_event_click();
            stage_mouse(&p);
            return;
        }
        if (in(pal, p.x, p.y)) {
            for (int i = 0; i < CATS; i++)
                if (in(chip_rect(i), p.x, p.y)) {
                    if (cat_sel != i) {
                        cat_sel = i;
                        pal_scroll = 0;
                        build_palette();
                        redraw(pal);
                    }
                    return;
                }
            if (!in(pal_list(), p.x, p.y))
                return;
            struct wg_pointer lp = p;
            lp.x -= pal.x;
            lp.y -= pal.y + PAL_TOP - pal_scroll;
            if (cat_sel == CAT_VARS && wg_button_pointer(&newvar, &lp))
                return;
            press_block = pal_hit(p.x, p.y);
            press_tpl = press_block != NO;
            pstate = press_tpl ? P_PRESSED : P_PAN_PAL;
            press_scroll_y = pal_scroll;
            if (cat_sel == CAT_VARS && newvar.dirty)
                redraw(pal_list());
            return;
        }
        if (in(ws, p.x, p.y)) {
            press_block = ws_hit(p.x, p.y, &press_slot);
            pstate = press_block != NO ? P_PRESSED : P_PAN;
            press_scroll_x = scroll_x;
            press_scroll_y = scroll_y;
        }
        return;
    }

    if (p.held) {
        int dx = p.x - press_x, dy = p.y - press_y;
        switch (pstate) {
        case P_PRESSED:
            if (dx * dx + dy * dy >= DRAG_START * DRAG_START)
                start_drag(p.x, p.y);
            break;
        case P_DRAG:
            drag_to(p.x, p.y);
            break;
        case P_PAN: {
            int nx = press_scroll_x - dx, ny = press_scroll_y - dy;
            nx = nx < 0 ? 0 : nx > 4000 ? 4000 : nx;
            ny = ny < 0 ? 0 : ny > 8000 ? 8000 : ny;
            if (nx != scroll_x || ny != scroll_y) {
                scroll_x = nx;
                scroll_y = ny;
                redraw_ws();
            }
            break;
        }
        case P_PAN_PAL: {
            int most = pal_h - (pal.h - PAL_TOP), ny = press_scroll_y - dy;
            ny = ny > most ? most : ny;
            ny = ny < 0 ? 0 : ny;
            if (ny != pal_scroll) {
                pal_scroll = ny;
                redraw(pal_list());
            }
            break;
        }
        }
        return;
    }

    if (p.up) {
        if (cat_sel == CAT_VARS) {
            struct wg_pointer lp = p;
            lp.x -= pal.x;
            lp.y -= pal.y + PAL_TOP - pal_scroll;
            if (wg_button_pointer(&newvar, &lp)) {
                redraw(pal_list());
                menu_block = NO;
                show_dialog(D_NEWVAR, "New variable", "Its name:", "Add", WG_BUTTON_SUGGESTED, 1, "");
                pstate = P_IDLE;
                return;
            }
            if (newvar.dirty)
                redraw(pal_list());
        }
        if (pstate == P_DRAG)
            drop(p.x);
        else if (pstate == P_PRESSED)
            click();
        pstate = P_IDLE;
        return;
    }

    /* Just moving: the palette's button shows the pointer over it. */
    if (cat_sel == CAT_VARS) {
        struct wg_pointer lp = p;
        lp.x -= pal.x;
        lp.y -= pal.y + PAL_TOP - pal_scroll;
        wg_button_pointer(&newvar, &lp);
        if (newvar.dirty)
            redraw(pal_list());
    }
}

/* --- keys -------------------------------------------------------------------------------------- */

/* The key menu's index of a key (blocks.c's order), or -1. */
static int key_menu_index(const struct key_event *k)
{
    static const uint16_t codes[] = { KEY_SPACE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_ENTER,
                                      0x01e, 0x020, 0x01f, 0x011 };
    for (int i = 0; i < (int)(sizeof(codes) / sizeof(codes[0])); i++)
        if (k->key == codes[i])
            return i;
    return -1;
}

static void run(void)
{
    end_edit(1);
    if (project_name[0] && project_dirty) {     /* Run saves, as Blocksmith autosaves */
        const char *why = 0;
        project_save(project_name, &why);
    }
    run_start();
    k86_timer_set(TICK_MS);
    redraw_all();
}

static void stop(void)
{
    run_stop();
    k86_timer_set(0);
    redraw(side);
}

static void key(const struct key_event *k)
{
    if (modal == M_DIALOG) {
        if (!k->pressed)
            return;
        int c = wg_dialog_key(&dialog, k);
        if (c != WG_DIALOG_NONE)
            dialog_chose(c);
        else if (dialog.dirty) {
            wg_dialog_draw(&canvas, &dialog);
            k86_present(dialog.r.x, dialog.r.y, dialog.r.w, dialog.r.h);
        }
        return;
    }
    if (modal == M_OPEN) {
        if (!k->pressed)
            return;
        if (k->key == KEY_ESC) {
            close_modal();
            return;
        }
        enum wg_list_result r = nprojects ? wg_list_key(&open_list, k) : WG_LIST_NONE;
        if (r == WG_LIST_ACTIVATE)
            load_project(projects[open_list.selected]);
        else if (open_list.dirty)
            draw_open(0);
        return;
    }
    if (edit_block != NO) {
        if (k->pressed)
            edit_key(k);
        return;
    }
    if (menu_block != NO) {
        if (k->pressed && k->key == KEY_ESC)
            close_menu();
        return;
    }
    int ctrl = k->mods & MOD_CTRL;
    if (k->pressed && ((ctrl && k->key == KEY_ENTER) || k->key == KEY_F1 + 4)) {
        run();
        return;
    }
    if (k->pressed && ctrl && k->key == 0x01f) {    /* Ctrl+S */
        save();
        return;
    }
    if (k->pressed && k->key == KEY_ESC && run_active()) {
        stop();
        return;
    }
    int i = key_menu_index(k);
    if (i < 0 && k->pressed)
        i = -2;                             /* any other key: only "any" */
    if (i >= 0)
        run_set_key(i, k->pressed);
    if (k->pressed)
        run_event_key(i >= 0 ? i : 99);
}

/* --- the app ------------------------------------------------------------------------------------- */

static void header(int id)
{
    if (modal)
        return;
    end_edit(1);
    close_menu();
    switch (id) {
    case BTN_RUN:
        run();
        break;
    case BTN_STOP:
        stop();
        break;
    case BTN_SAVE:
        save();
        break;
    case BTN_OPEN:
        show_open();
        break;
    case BTN_NEW:
        if (project_dirty && norder)
            show_dialog(D_NEW, "Start a new project?", "Changes that aren't saved will be lost.",
                        "Discard", WG_BUTTON_DESTRUCTIVE, 0, 0);
        else
        {
            dialog_kind = D_NEW;
            dialog_chose(1);
        }
        break;
    }
}

static void tick(void)
{
    int c = run_tick();
    if (c & RUN_ENDED)
        k86_timer_set(0);
    if (modal)
        return;                             /* behind a dialog: shown when it closes */
    if (c & RUN_ERROR)
        redraw_all();
    else {
        if (c & RUN_STAGE)
            redraw(stage_r);
        if (c & (RUN_CONSOLE | RUN_ENDED))
            redraw(R(side.x, stage_r.y + STAGE_H + 1, side.w, canvas.h - stage_r.y - STAGE_H - 1));
    }
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    layout();
    stage_reset();
    project_starter();
    cat_sel = CAT_EVENTS;
    build_palette();
    set_header();
    redraw_all();
    k86_log("blocksmith: open");
    for (;;) {
        struct k86_event ev;
        /* The phone's Back: a dialog, an edit, a menu or a running script
         * to close first; then it closes Blocksmith (and saves). */
        k86_back_wanted(modal != M_NONE || edit_block != NO || menu_block != NO || run_active());
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER: pointer(&ev); break;
        case K86_EVENT_KEY:     key(&ev.key); break;
        case K86_EVENT_HEADER:  header(ev.id); break;
        case K86_EVENT_TIMER:   tick(); break;
        case K86_EVENT_THEME:   redraw_all(); break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            layout();
            build_palette();
            if (modal == M_OPEN)
                close_modal();
            redraw_all();
            break;
        case K86_EVENT_CLOSE:
            if (project_name[0] && project_dirty) {
                const char *why = 0;
                project_save(project_name, &why);
            }
            return 0;
        default:
            break;
        }
    }
}
