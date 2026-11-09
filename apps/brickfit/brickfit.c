#include <stdint.h>
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"

#define BOARD_SIDE 8
#define OFFER_COUNT 3
#define CLEAR_TICKS 6
#define RETURN_TICKS 6
#define ANIM_MS 28
#define BEST_FILE "brickfit-best.dat"

#define BIT(x, y) ((uint16_t)(1u << ((y) * 4 + (x))))

struct shape {
    uint8_t w, h;
    uint16_t bits;
};

struct piece {
    int8_t shape;
    int8_t colour;
};

static const struct shape shapes[] = {
    { 1, 1, BIT(0, 0) },
    { 2, 1, BIT(0, 0) | BIT(1, 0) },
    { 1, 2, BIT(0, 0) | BIT(0, 1) },
    { 3, 1, BIT(0, 0) | BIT(1, 0) | BIT(2, 0) },
    { 1, 3, BIT(0, 0) | BIT(0, 1) | BIT(0, 2) },
    { 2, 2, BIT(0, 0) | BIT(1, 0) | BIT(0, 1) | BIT(1, 1) },
    { 4, 1, BIT(0, 0) | BIT(1, 0) | BIT(2, 0) | BIT(3, 0) },
    { 1, 4, BIT(0, 0) | BIT(0, 1) | BIT(0, 2) | BIT(0, 3) },
    { 2, 2, BIT(0, 0) | BIT(0, 1) | BIT(1, 1) },
    { 2, 3, BIT(0, 0) | BIT(0, 1) | BIT(0, 2) | BIT(1, 2) },
    { 2, 3, BIT(1, 0) | BIT(1, 1) | BIT(0, 2) | BIT(1, 2) },
    { 3, 2, BIT(1, 0) | BIT(0, 1) | BIT(1, 1) | BIT(2, 1) },
    { 3, 2, BIT(1, 0) | BIT(2, 0) | BIT(0, 1) | BIT(1, 1) },
    { 3, 2, BIT(0, 0) | BIT(1, 0) | BIT(1, 1) | BIT(2, 1) },
    { 3, 2, BIT(2, 0) | BIT(0, 1) | BIT(1, 1) | BIT(2, 1) },
    { 3, 3, BIT(0, 0) | BIT(1, 0) | BIT(2, 0) | BIT(0, 1) | BIT(0, 2) },
    { 3, 2, BIT(0, 0) | BIT(2, 0) | BIT(0, 1) | BIT(1, 1) | BIT(2, 1) },
    { 3, 3, BIT(1, 0) | BIT(0, 1) | BIT(1, 1) | BIT(2, 1) | BIT(1, 2) },
    { 2, 2, BIT(0, 0) | BIT(1, 1) },
    { 3, 3, BIT(1, 0) | BIT(0, 1) | BIT(1, 1) | BIT(2, 1) | BIT(1, 2) }
};
#define SHAPE_COUNT ((int)(sizeof(shapes) / sizeof(shapes[0])))

static const uint32_t block_colours[] = {
    0x53b8a6, 0xe4a452, 0x718fd1, 0xd47483, 0x9b80c5, 0x69ad79
};
#define COLOUR_COUNT ((int)(sizeof(block_colours) / sizeof(block_colours[0])))

static struct k86_window win;
static struct gfx_surface canvas;
static int8_t board[BOARD_SIDE][BOARD_SIDE];
static struct piece offers[OFFER_COUNT];
static uint32_t score, best_score;
static int combo;
static uint32_t rng_state;

static struct gfx_rect header_r, board_r, hint_r, tray_r, slots[OFFER_COUNT];
static int margin, gap, cell, cell_gap;

enum animation { ANIM_NONE, ANIM_CLEAR, ANIM_RETURN };
static enum animation anim;
static int anim_tick;
static uint8_t clear_rows, clear_cols;
static int game_over;

static int dragging, active_slot;
static int grab_x, grab_y;
static int candidate_x, candidate_y;
static int candidate_inside, candidate_valid;
static uint8_t preview_rows, preview_cols;
static int drag_x, drag_y;

static int return_from_x, return_from_y, return_to_x, return_to_y;
static int return_from_unit, return_to_unit;
static int return_x, return_y, return_unit;
static int again_pressed;
static struct gfx_rect again_r, panel_r;

static int shape_has(const struct shape *s, int x, int y)
{
    return (s->bits & BIT(x, y)) != 0;
}

static uint32_t random_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static void format_number(uint32_t value, char *out)
{
    char reversed[12];
    int n = 0;
    do {
        reversed[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && n < (int)sizeof(reversed));
    for (int i = 0; i < n; i++)
        out[i] = reversed[n - i - 1];
    out[n] = 0;
}

static void append_text(char *dst, const char *src)
{
    while (*dst)
        dst++;
    while (*src)
        *dst++ = *src++;
    *dst = 0;
}

static void load_best(void)
{
    char home[K86_PATH_MAX];
    uint32_t stored = 0;
    int vol = k86_home(home);
    if (vol < 0)
        return;
    if (k86_read(vol, home, BEST_FILE, &stored, sizeof(stored)) == (int)sizeof(stored))
        best_score = stored;
}

static void save_best(void)
{
    char home[K86_PATH_MAX];
    int vol = k86_home(home);
    if (vol >= 0)
        (void)k86_write(vol, home, BEST_FILE, &best_score, sizeof(best_score));
}

static void choose_offers(void)
{
    for (int i = 0; i < OFFER_COUNT; i++) {
        offers[i].shape = (int8_t)(random_next() % SHAPE_COUNT);
        offers[i].colour = (int8_t)(random_next() % COLOUR_COUNT);
    }
}

static int all_offers_used(void)
{
    for (int i = 0; i < OFFER_COUNT; i++)
        if (offers[i].shape >= 0)
            return 0;
    return 1;
}

static int can_place(int shape_id, int ax, int ay)
{
    if (shape_id < 0 || shape_id >= SHAPE_COUNT)
        return 0;
    const struct shape *s = &shapes[shape_id];
    if (ax < 0 || ay < 0 || ax + s->w > BOARD_SIDE || ay + s->h > BOARD_SIDE)
        return 0;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            if (shape_has(s, x, y) && board[ay + y][ax + x] >= 0)
                return 0;
    return 1;
}

static int any_offer_fits(void)
{
    for (int i = 0; i < OFFER_COUNT; i++) {
        int shape_id = offers[i].shape;
        if (shape_id < 0)
            continue;
        for (int y = 0; y < BOARD_SIDE; y++)
            for (int x = 0; x < BOARD_SIDE; x++)
                if (can_place(shape_id, x, y))
                    return 1;
    }
    return 0;
}

static void settle_turn(void)
{
    if (all_offers_used())
        choose_offers();
    if (!any_offer_fits()) {
        game_over = 1;
        k86_logf("brickfit: game over, score %u", score);
    }
}

static void layout(void)
{
    int w = canvas.w, h = canvas.h;
    margin = w / 30;
    if (margin < 12)
        margin = 12;
    if (margin > 28)
        margin = 28;
    gap = h / 22;
    if (gap < 30)
        gap = 30;
    if (gap > 36)
        gap = 36;
    int header_h = h / 11;
    if (header_h < 104)
        header_h = 104;
    if (header_h > 132)
        header_h = 132;
    int tray_h = h / 5;
    if (tray_h < 120)
        tray_h = 120;
    if (tray_h > 250)
        tray_h = 250;
    int board_size = w - 2 * margin;
    int by_height = h - header_h - tray_h - 3 * gap;
    if (board_size > by_height)
        board_size = by_height;
    if (board_size < 8 * 20)
        board_size = 8 * 20;
    board_size = board_size / BOARD_SIDE * BOARD_SIDE;
    cell = board_size / BOARD_SIDE;
    cell_gap = cell / 18;
    if (cell_gap < 2)
        cell_gap = 2;
    if (cell_gap > 6)
        cell_gap = 6;

    int group_h = header_h + gap + board_size + gap + tray_h;
    int top = (h - group_h) / 2;
    if (top < 0)
        top = 0;
    header_r = (struct gfx_rect){ 0, top, w, header_h };
    board_r = (struct gfx_rect){ (w - board_size) / 2, top + header_h + gap, board_size, board_size };
    hint_r = (struct gfx_rect){ board_r.x, board_r.y + board_size, board_size, gap };
    tray_r = (struct gfx_rect){ board_r.x, hint_r.y + gap, board_size, tray_h };

    int slot_gap = board_size / 30;
    if (slot_gap < 8)
        slot_gap = 8;
    int slot_w = (board_size - 2 * slot_gap) / OFFER_COUNT;
    for (int i = 0; i < OFFER_COUNT; i++)
        slots[i] = (struct gfx_rect){ board_r.x + i * (slot_w + slot_gap), tray_r.y + 4,
                                      slot_w, tray_r.h - 8 };

    int panel_w = w - 2 * margin;
    if (panel_w > 520)
        panel_w = 520;
    int panel_h = h - 2 * margin;
    if (panel_h > 340)
        panel_h = 340;
    if (panel_h < 300)
        panel_h = 300;
    panel_r = (struct gfx_rect){ (w - panel_w) / 2, (h - panel_h) / 2, panel_w, panel_h };
    int bw = panel_w - 48;
    if (bw > 340)
        bw = 340;
    again_r = (struct gfx_rect){ panel_r.x + (panel_w - bw) / 2, panel_r.y + panel_h - 88, bw, 62 };
}

static struct gfx_rect preview_geometry(int slot, int *unit_out)
{
    struct gfx_rect card = slots[slot];
    int shape_id = offers[slot].shape;
    if (shape_id < 0) {
        *unit_out = 0;
        return (struct gfx_rect){ card.x, card.y, 0, 0 };
    }
    const struct shape *s = &shapes[shape_id];
    int avail_w = card.w - 20;
    int avail_h = card.h - 42;
    int unit = avail_w / 4;
    if (unit > avail_h / 4)
        unit = avail_h / 4;
    if (unit < 10)
        unit = 10;
    int x = card.x + (card.w - s->w * unit) / 2;
    int y = card.y + 4 + (avail_h - s->h * unit) / 2;
    *unit_out = unit;
    return (struct gfx_rect){ x, y, s->w * unit, s->h * unit };
}

static void draw_block(int x, int y, int size, uint32_t colour, uint32_t under, int alpha)
{
    const struct theme *t = theme_get();
    int radius = size / 5;
    if (radius < 3)
        radius = 3;
    if (radius > 11)
        radius = 11;
    uint32_t face = gfx_mix(colour, under, alpha);
    if (alpha > 220 && size > 12)
        gfx_fill_round_rect(&canvas, x, y + 2, size, size, radius, gfx_mix(face, t->window_bg, 90));
    gfx_fill_round_rect(&canvas, x, y, size, size, radius, face);
    if (alpha > 220 && size > 18) {
        int inset = size / 8;
        int band = size / 9;
        if (band < 2)
            band = 2;
        gfx_fill_round_rect(&canvas, x + inset, y + inset, size - 2 * inset, band,
                            radius / 2, gfx_mix(face, 0xffffff, 52));
    } else {
        gfx_rect_outline(&canvas, x, y, size, size, gfx_mix(colour, under, 185));
    }
}

static void draw_shape(int x, int y, int unit, int shape_id, uint32_t colour,
                       uint32_t under, int alpha)
{
    if (shape_id < 0 || shape_id >= SHAPE_COUNT || unit <= 0)
        return;
    const struct shape *s = &shapes[shape_id];
    int pad = unit / 12;
    if (pad < 1)
        pad = 1;
    for (int sy = 0; sy < s->h; sy++)
        for (int sx = 0; sx < s->w; sx++)
            if (shape_has(s, sx, sy))
                draw_block(x + sx * unit + pad, y + sy * unit + pad, unit - 2 * pad,
                           colour, under, alpha);
}

static void draw_header(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, header_r.x, header_r.y, header_r.w, header_r.h, t->window_bg);
    text_draw(&canvas, margin, header_r.y, "Brickfit", TEXT_HEADING, t->fg);
    gfx_fill_round_rect(&canvas, margin, header_r.y + 45, 42, 4, 2, t->accent);

    int card_y = header_r.y + 52;
    int card_h = header_r.h - 52;
    int card_gap = 10;
    int card_w = (canvas.w - 2 * margin - card_gap) / 2;
    struct gfx_rect a = { margin, card_y, card_w, card_h };
    struct gfx_rect b = { margin + card_w + card_gap, card_y, card_w, card_h };
    gfx_fill_round_rect(&canvas, a.x, a.y, a.w, a.h, 12, t->view_bg);
    gfx_fill_round_rect(&canvas, b.x, b.y, b.w, b.h, 12, t->view_bg);
    gfx_rect_outline(&canvas, a.x, a.y, a.w, a.h, t->border);
    gfx_rect_outline(&canvas, b.x, b.y, b.w, b.h, t->border);
    char line[32] = "SCORE ";
    char number[12];
    format_number(score, number);
    append_text(line, number);
    text_draw_fit(&canvas, a.x + 12, a.y + (a.h - text_height(TEXT_SMALL)) / 2,
                  line, a.w - 24, TEXT_SMALL, t->fg);
    line[0] = 0;
    append_text(line, "BEST ");
    format_number(best_score, number);
    append_text(line, number);
    text_draw_fit(&canvas, b.x + 12, b.y + (b.h - text_height(TEXT_SMALL)) / 2,
                  line, b.w - 24, TEXT_SMALL, t->fg);
}

static int active_shape_id(void)
{
    if (active_slot < 0 || active_slot >= OFFER_COUNT)
        return -1;
    return offers[active_slot].shape;
}

static struct gfx_rect drag_rect(void)
{
    int shape_id = active_shape_id();
    if (shape_id < 0)
        return (struct gfx_rect){ 0, 0, 0, 0 };
    const struct shape *s = &shapes[shape_id];
    return (struct gfx_rect){ drag_x - 2, drag_y - 2, s->w * cell + 4, s->h * cell + 4 };
}

static struct gfx_rect return_rect(void)
{
    int shape_id = active_shape_id();
    if (shape_id < 0)
        return (struct gfx_rect){ 0, 0, 0, 0 };
    const struct shape *s = &shapes[shape_id];
    return (struct gfx_rect){ return_x - 2, return_y - 2, s->w * return_unit + 4, s->h * return_unit + 4 };
}

static void draw_board(void)
{
    const struct theme *t = theme_get();
    int radius = cell / 5;
    if (radius < 8)
        radius = 8;
    if (radius > 20)
        radius = 20;
    gfx_fill_rect(&canvas, board_r.x, board_r.y, board_r.w, board_r.h, t->window_bg);
    gfx_fill_round_rect(&canvas, board_r.x, board_r.y, board_r.w, board_r.h, radius, t->border);
    int tile_gap = cell_gap;
    uint32_t empty = gfx_mix(t->window_bg, t->view_bg, 72);
    for (int y = 0; y < BOARD_SIDE; y++) {
        for (int x = 0; x < BOARD_SIDE; x++) {
            int px = board_r.x + x * cell + tile_gap;
            int py = board_r.y + y * cell + tile_gap;
            int size = cell - 2 * tile_gap;
            gfx_fill_round_rect(&canvas, px, py, size, size, cell / 8, empty);
            int colour_id = board[y][x];
            if (colour_id >= 0) {
                int flashing = anim == ANIM_CLEAR &&
                               ((clear_rows & (1u << y)) || (clear_cols & (1u << x)));
                if (flashing) {
                    int fade = anim_tick * 38;
                    if (fade > 228)
                        fade = 228;
                    uint32_t faded = gfx_mix(block_colours[colour_id], t->window_bg, fade);
                    gfx_fill_round_rect(&canvas, px, py, size, size, cell / 8, faded);
                } else {
                    draw_block(px, py, size, block_colours[colour_id], empty, 255);
                }
            }
        }
    }

    if (dragging && candidate_inside) {
        int shape_id = active_shape_id();
        const struct shape *s = &shapes[shape_id];
        uint32_t ghost = candidate_valid ? block_colours[offers[active_slot].colour] : t->destructive;
        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++)
                if (shape_has(s, x, y)) {
                    int px = board_r.x + (candidate_x + x) * cell + tile_gap;
                    int py = board_r.y + (candidate_y + y) * cell + tile_gap;
                    draw_block(px, py, cell - 2 * tile_gap, ghost, empty, 105);
                }
    }

    if (dragging && candidate_valid) {
        for (int y = 0; y < BOARD_SIDE; y++)
            if (preview_rows & (1u << y))
                gfx_rect_outline(&canvas, board_r.x + 2, board_r.y + y * cell + 2,
                                 board_r.w - 4, cell - 4, t->accent);
        for (int x = 0; x < BOARD_SIDE; x++)
            if (preview_cols & (1u << x))
                gfx_rect_outline(&canvas, board_r.x + x * cell + 2, board_r.y + 2,
                                 cell - 4, board_r.h - 4, t->accent);
    }
}

static void make_hint(char *out)
{
    out[0] = 0;
    if (anim == ANIM_CLEAR) {
        append_text(out, "CLEARING LINES");
    } else if (dragging && candidate_valid && (preview_rows || preview_cols)) {
        int n = 0;
        for (int i = 0; i < BOARD_SIDE; i++) {
            n += !!(preview_rows & (1u << i));
            n += !!(preview_cols & (1u << i));
        }
        append_text(out, "CLEAR ");
        char number[12];
        format_number((uint32_t)n, number);
        append_text(out, number);
        append_text(out, n == 1 ? " LINE" : " LINES");
        if (combo > 0) {
            append_text(out, "  + COMBO x");
            format_number((uint32_t)(combo + 1), number);
            append_text(out, number);
        }
    } else if (dragging && candidate_valid) {
        append_text(out, "READY TO PLACE");
    } else if (dragging && candidate_inside) {
        append_text(out, "BLOCKED");
    } else if (dragging) {
        append_text(out, "DRAG TO GRID");
    } else if (combo > 1) {
        append_text(out, "COMBO x");
        char number[12];
        format_number((uint32_t)combo, number);
        append_text(out, number);
    } else {
        append_text(out, "ROWS + COLUMNS CLEAR");
    }
}

static void draw_hint(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, hint_r.x, hint_r.y, hint_r.w, hint_r.h, t->window_bg);
    char hint[48];
    make_hint(hint);
    uint32_t colour = dragging && candidate_valid && (preview_rows || preview_cols) ? t->accent : t->fg_dim;
    if (hint_r.h >= text_height(TEXT_SMALL) + 2) {
        int tw = text_width(hint, TEXT_SMALL);
        text_draw_fit(&canvas, hint_r.x + (hint_r.w - tw) / 2,
                      hint_r.y + (hint_r.h - text_height(TEXT_SMALL)) / 2,
                      hint, hint_r.w - 8, TEXT_SMALL, colour);
    } else {
        int tw = gfx_text_width(hint);
        gfx_text(&canvas, hint_r.x + (hint_r.w - tw) / 2,
                 hint_r.y + (hint_r.h - 16) / 2, hint, colour, GFX_TRANSPARENT);
    }
}

static void draw_tray(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, tray_r.x, tray_r.y, tray_r.w, tray_r.h, t->window_bg);
    for (int i = 0; i < OFFER_COUNT; i++) {
        struct gfx_rect r = slots[i];
        gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, 16, t->view_bg);
        gfx_rect_outline(&canvas, r.x, r.y, r.w, r.h,
                         (dragging || anim == ANIM_RETURN) && active_slot == i ? t->accent : t->border);
        if (offers[i].shape >= 0) {
            int unit;
            struct gfx_rect p = preview_geometry(i, &unit);
            int held = ((dragging || anim == ANIM_RETURN) && active_slot == i);
            uint32_t under = t->view_bg;
            int alpha = held ? 95 : 255;
            draw_shape(p.x, p.y, unit, offers[i].shape, block_colours[offers[i].colour], under, alpha);
        } else {
            const char *used = "READY";
            int tw = text_width(used, TEXT_SMALL);
            text_draw(&canvas, r.x + (r.w - tw) / 2, r.y + (r.h - text_height(TEXT_SMALL)) / 2,
                      used, TEXT_SMALL, t->fg_dim);
        }
        char label[12] = "BLOCK ";
        label[6] = (char)('1' + i);
        label[7] = 0;
        text_draw_fit(&canvas, r.x + 8, r.y + r.h - 31, label, r.w - 16, TEXT_SMALL, t->fg_dim);
    }
}

static void draw_drag_piece(void)
{
    int shape_id = active_shape_id();
    if (shape_id < 0)
        return;
    uint32_t under = theme_get()->window_bg;
    draw_shape(drag_x, drag_y, cell, shape_id, block_colours[offers[active_slot].colour], under, 255);
}

static void draw_return_piece(void)
{
    int shape_id = active_shape_id();
    if (shape_id < 0)
        return;
    draw_shape(return_x, return_y, return_unit, shape_id,
               block_colours[offers[active_slot].colour], theme_get()->window_bg, 255);
}

static int rect_hits(struct gfx_rect a, struct gfx_rect b)
{
    return !gfx_rect_empty(gfx_rect_intersect(a, b));
}

static void present_rect(struct gfx_rect r)
{
    struct gfx_rect bounds = { 0, 0, canvas.w, canvas.h };
    r = gfx_rect_intersect(r, bounds);
    if (!gfx_rect_empty(r))
        k86_present(r.x, r.y, r.w, r.h);
}

static void restore_under(struct gfx_rect r)
{
    const struct theme *t = theme_get();
    struct gfx_rect bounds = { 0, 0, canvas.w, canvas.h };
    r = gfx_rect_intersect(r, bounds);
    if (gfx_rect_empty(r))
        return;
    gfx_fill_rect(&canvas, r.x, r.y, r.w, r.h, t->window_bg);
    if (rect_hits(r, header_r))
        draw_header();
    if (rect_hits(r, board_r))
        draw_board();
    if (rect_hits(r, hint_r))
        draw_hint();
    if (rect_hits(r, tray_r))
        draw_tray();
    present_rect(r);
}

static void draw_game(void)
{
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, theme_get()->window_bg);
    draw_header();
    draw_board();
    draw_hint();
    draw_tray();
    k86_present(0, 0, canvas.w, canvas.h);
}

static void draw_modal(void)
{
    const struct theme *t = theme_get();
    gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 145);
    int radius = 22;
    gfx_fill_round_rect(&canvas, panel_r.x, panel_r.y, panel_r.w, panel_r.h, radius, t->dialog_bg);
    gfx_rect_outline(&canvas, panel_r.x, panel_r.y, panel_r.w, panel_r.h, t->border);
    const char *title = "GAME OVER";
    int tw = text_width(title, TEXT_HEADING);
    text_draw(&canvas, panel_r.x + (panel_r.w - tw) / 2, panel_r.y + 22, title, TEXT_HEADING, t->fg);

    char line[48] = "SCORE ";
    char number[12];
    format_number(score, number);
    append_text(line, number);
    tw = text_width(line, TEXT_BODY);
    text_draw(&canvas, panel_r.x + (panel_r.w - tw) / 2, panel_r.y + 90, line, TEXT_BODY, t->fg);
    line[0] = 0;
    append_text(line, "BEST ");
    format_number(best_score, number);
    append_text(line, number);
    tw = text_width(line, TEXT_SMALL);
    text_draw(&canvas, panel_r.x + (panel_r.w - tw) / 2, panel_r.y + 132, line, TEXT_SMALL, t->fg_dim);
    const char *message = "No offered block fits.";
    tw = text_width(message, TEXT_SMALL);
    text_draw(&canvas, panel_r.x + (panel_r.w - tw) / 2, panel_r.y + 176, message, TEXT_SMALL, t->fg_dim);

    uint32_t button_bg = again_pressed ? gfx_mix(t->accent_bg, t->fg, 38) : t->accent_bg;
    gfx_fill_round_rect(&canvas, again_r.x, again_r.y, again_r.w, again_r.h, 18, button_bg);
    const char *label = "Play Again";
    tw = text_width(label, TEXT_BOLD);
    text_draw(&canvas, again_r.x + (again_r.w - tw) / 2,
              again_r.y + (again_r.h - text_height(TEXT_BOLD)) / 2,
              label, TEXT_BOLD, t->accent_fg);
    k86_present(0, 0, canvas.w, canvas.h);
}

static void draw_all(void)
{
    draw_game();
    if (game_over)
        draw_modal();
}

static int round_cell(int pixels)
{
    if (pixels >= 0)
        return (pixels + cell / 2) / cell;
    return -((-pixels + cell / 2) / cell);
}

static void compute_preview_lines(int ax, int ay, int shape_id, uint8_t *rows, uint8_t *cols)
{
    const struct shape *s = &shapes[shape_id];
    *rows = 0;
    *cols = 0;
    for (int y = 0; y < BOARD_SIDE; y++) {
        int full = 1;
        for (int x = 0; x < BOARD_SIDE; x++) {
            int in_piece = x >= ax && x < ax + s->w && y >= ay && y < ay + s->h &&
                          shape_has(s, x - ax, y - ay);
            if (board[y][x] < 0 && !in_piece) {
                full = 0;
                break;
            }
        }
        if (full)
            *rows |= (uint8_t)(1u << y);
    }
    for (int x = 0; x < BOARD_SIDE; x++) {
        int full = 1;
        for (int y = 0; y < BOARD_SIDE; y++) {
            int in_piece = x >= ax && x < ax + s->w && y >= ay && y < ay + s->h &&
                          shape_has(s, x - ax, y - ay);
            if (board[y][x] < 0 && !in_piece) {
                full = 0;
                break;
            }
        }
        if (full)
            *cols |= (uint8_t)(1u << x);
    }
}

static void update_drag(int x, int y)
{
    int shape_id = active_shape_id();
    if (shape_id < 0)
        return;
    const struct shape *s = &shapes[shape_id];
    drag_x = x - grab_x;
    drag_y = y - grab_y - s->h * cell - cell / 3;
    candidate_x = round_cell(drag_x - board_r.x);
    candidate_y = round_cell(y - grab_y - board_r.y);
    candidate_inside = candidate_x >= 0 && candidate_y >= 0 &&
                       candidate_x + s->w <= BOARD_SIDE && candidate_y + s->h <= BOARD_SIDE;
    candidate_valid = candidate_inside && can_place(shape_id, candidate_x, candidate_y);
    preview_rows = preview_cols = 0;
    if (candidate_valid)
        compute_preview_lines(candidate_x, candidate_y, shape_id, &preview_rows, &preview_cols);
}

static int line_count(uint8_t rows, uint8_t cols)
{
    int n = 0;
    for (int i = 0; i < BOARD_SIDE; i++) {
        n += !!(rows & (1u << i));
        n += !!(cols & (1u << i));
    }
    return n;
}

static uint32_t add_score(uint32_t old, uint32_t amount)
{
    if (UINT32_MAX - old < amount)
        return UINT32_MAX;
    return old + amount;
}

static void place_active_piece(void)
{
    int shape_id = active_shape_id();
    int colour_id = offers[active_slot].colour;
    const struct shape *s = &shapes[shape_id];
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            if (shape_has(s, x, y))
                board[candidate_y + y][candidate_x + x] = (int8_t)colour_id;

    clear_rows = clear_cols = 0;
    for (int y = 0; y < BOARD_SIDE; y++) {
        int full = 1;
        for (int x = 0; x < BOARD_SIDE; x++)
            if (board[y][x] < 0) {
                full = 0;
                break;
            }
        if (full)
            clear_rows |= (uint8_t)(1u << y);
    }
    for (int x = 0; x < BOARD_SIDE; x++) {
        int full = 1;
        for (int y = 0; y < BOARD_SIDE; y++)
            if (board[y][x] < 0) {
                full = 0;
                break;
            }
        if (full)
            clear_cols |= (uint8_t)(1u << x);
    }

    int lines = line_count(clear_rows, clear_cols);
    if (lines) {
        combo++;
        int multiplier = combo > 5 ? 5 : combo;
        uint32_t base = (uint32_t)(lines * 100 + (lines > 1 ? lines * (lines - 1) * 50 : 0));
        score = add_score(score, base * (uint32_t)multiplier);
        k86_logf("brickfit: cleared %d lines, score %u", lines, score);
    } else {
        combo = 0;
    }
    offers[active_slot].shape = -1;
    active_slot = -1;

    if (score > best_score) {
        best_score = score;
        save_best();
    }
    if (lines) {
        anim = ANIM_CLEAR;
        anim_tick = 0;
        k86_timer_set(ANIM_MS);
    } else {
        anim = ANIM_NONE;
        settle_turn();
    }
}

static void finish_clear(void)
{
    for (int y = 0; y < BOARD_SIDE; y++)
        for (int x = 0; x < BOARD_SIDE; x++)
            if ((clear_rows & (1u << y)) || (clear_cols & (1u << x)))
                board[y][x] = -1;
    clear_rows = clear_cols = 0;
    anim = ANIM_NONE;
    anim_tick = 0;
    settle_turn();
}

static int point_in(struct gfx_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void start_drag(int slot, int x, int y)
{
    int unit;
    struct gfx_rect p = preview_geometry(slot, &unit);
    int shape_id = offers[slot].shape;
    if (shape_id < 0 || unit <= 0)
        return;
    const struct shape *s = &shapes[shape_id];
    int local_x = x - p.x;
    int local_y = y - p.y;
    int best_dist = 0x7fffffff;
    int selected_x = 0, selected_y = 0;
    for (int sy = 0; sy < s->h; sy++)
        for (int sx = 0; sx < s->w; sx++)
            if (shape_has(s, sx, sy)) {
                int dx = sx * unit + unit / 2 - local_x;
                int dy = sy * unit + unit / 2 - local_y;
                int d = dx * dx + dy * dy;
                if (d < best_dist) {
                    best_dist = d;
                    selected_x = sx;
                    selected_y = sy;
                }
            }
    active_slot = slot;
    grab_x = selected_x * unit + unit / 2;
    grab_y = selected_y * unit + unit / 2;
    dragging = 1;
    update_drag(x, y);
    draw_tray();
    present_rect(tray_r);
    draw_board();
    present_rect(board_r);
    draw_hint();
    present_rect(hint_r);
    draw_drag_piece();
    present_rect(drag_rect());
}

static void move_drag(int x, int y)
{
    struct gfx_rect old = drag_rect();
    update_drag(x, y);
    restore_under(old);
    draw_board();
    present_rect(board_r);
    draw_hint();
    present_rect(hint_r);
    draw_drag_piece();
    present_rect(drag_rect());
}

static void begin_return(void)
{
    int unit;
    struct gfx_rect p = preview_geometry(active_slot, &unit);
    struct gfx_rect old = drag_rect();
    return_from_x = drag_x;
    return_from_y = drag_y;
    return_to_x = p.x;
    return_to_y = p.y;
    return_from_unit = cell;
    return_to_unit = unit;
    return_x = return_from_x;
    return_y = return_from_y;
    return_unit = return_from_unit;
    anim_tick = 0;
    anim = ANIM_RETURN;
    dragging = 0;
    candidate_inside = candidate_valid = 0;
    preview_rows = preview_cols = 0;
    restore_under(old);
    draw_return_piece();
    present_rect(return_rect());
    k86_timer_set(ANIM_MS);
}

static void drop_piece(void)
{
    struct gfx_rect old = drag_rect();
    if (!candidate_valid) {
        begin_return();
        return;
    }
    dragging = 0;
    place_active_piece();
    if (game_over) {
        draw_all();
        draw_modal();
        return;
    }
    restore_under(old);
    draw_header();
    present_rect(header_r);
    draw_board();
    present_rect(board_r);
    draw_hint();
    present_rect(hint_r);
    draw_tray();
    present_rect(tray_r);
}

static void timer_event(void)
{
    if (anim == ANIM_CLEAR) {
        anim_tick++;
        if (anim_tick >= CLEAR_TICKS) {
            finish_clear();
            k86_timer_set(0);
            if (game_over) {
                draw_all();
                draw_modal();
            } else {
                draw_header();
                present_rect(header_r);
                draw_board();
                present_rect(board_r);
                draw_hint();
                present_rect(hint_r);
                draw_tray();
                present_rect(tray_r);
            }
        } else {
            draw_board();
            present_rect(board_r);
        }
    } else if (anim == ANIM_RETURN) {
        struct gfx_rect old = return_rect();
        anim_tick++;
        if (anim_tick >= RETURN_TICKS) {
            anim = ANIM_NONE;
            active_slot = -1;
            restore_under(old);
            draw_tray();
            present_rect(tray_r);
            k86_timer_set(0);
            return;
        }
        int step = anim_tick;
        return_x = return_from_x + (return_to_x - return_from_x) * step / RETURN_TICKS;
        return_y = return_from_y + (return_to_y - return_from_y) * step / RETURN_TICKS;
        return_unit = return_from_unit + (return_to_unit - return_from_unit) * step / RETURN_TICKS;
        restore_under(old);
        draw_return_piece();
        present_rect(return_rect());
    }
}

static void pointer_event(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    if (game_over) {
        if (p.down) {
            again_pressed = point_in(again_r, p.x, p.y);
            draw_modal();
        } else if (p.up) {
            int restart = again_pressed && point_in(again_r, p.x, p.y);
            again_pressed = 0;
            if (restart) {
                game_over = 0;
                memset(board, 0xff, sizeof(board));
                score = 0;
                combo = 0;
                anim = ANIM_NONE;
                active_slot = -1;
                dragging = 0;
                choose_offers();
                settle_turn();
                draw_all();
                if (game_over)
                    draw_modal();
            } else {
                draw_modal();
            }
        }
        return;
    }
    if (anim != ANIM_NONE)
        return;
    if (p.down && !dragging) {
        for (int i = 0; i < OFFER_COUNT; i++)
            if (point_in(slots[i], p.x, p.y) && offers[i].shape >= 0) {
                start_drag(i, p.x, p.y);
                return;
            }
    }
    if (dragging && p.held) {
        move_drag(p.x, p.y);
        return;
    }
    if (dragging && p.up) {
        move_drag(p.x, p.y);
        drop_piece();
    }
}

static void start_game(void)
{
    memset(board, 0xff, sizeof(board));
    score = 0;
    combo = 0;
    game_over = 0;
    anim = ANIM_NONE;
    anim_tick = 0;
    clear_rows = clear_cols = 0;
    dragging = 0;
    active_slot = -1;
    again_pressed = 0;
    choose_offers();
    settle_turn();
}

int main(void)
{
    struct k86_header header;
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    memset(&header, 0, sizeof(header));
    memcpy(header.title, "Brickfit", sizeof("Brickfit"));
    k86_header(&header);

    rng_state = k86_uptime_ms() ^ 0xa5293c71u;
    if (!rng_state)
        rng_state = 1;
    load_best();
    start_game();
    layout();
    draw_all();
    k86_log("brickfit: open");

    for (;;) {
        struct k86_event ev;
        if (k86_wait_event(&ev)) {
            k86_timer_set(0);
            return 1;
        }
        switch (ev.type) {
        case K86_EVENT_POINTER:
            pointer_event(&ev);
            break;
        case K86_EVENT_TIMER:
            timer_event();
            break;
        case K86_EVENT_THEME:
            draw_all();
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            dragging = 0;
            active_slot = -1;
            if (anim == ANIM_RETURN)
                anim = ANIM_NONE;
            layout();
            if (anim == ANIM_CLEAR)
                k86_timer_set(ANIM_MS);
            else
                k86_timer_set(0);
            draw_all();
            break;
        case K86_EVENT_CLOSE:
            k86_timer_set(0);
            return 0;
        default:
            break;
        }
    }
}
