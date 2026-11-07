/* R3lay on LiteKern Go: the owner's messaging app, as a native client. Chats,
 * groups and text only so far (docs/R3LAY-PLAN.md); nobody can ring this
 * account (profiles.calls_disabled), so there's no calling to handle.
 *
 * Pages: sign in, chats, a chat, find a person, me. Everything's redrawn when
 * something changes. Log lines ("user: r3lay: ...") are for the tests, and
 * never carry anything typed. */
/* Cutting a long list of ids short is fine here: a poll just misses a group. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "r3lay.h"
#include "http.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T() theme_get()
#define ROW_H 132
#define AVATAR_R 44
#define COMPOSE_H 108
#define SIDE 24
#define BUBBLE_MAX 540
#define BUBBLE_PAD_X 22
#define BUBBLE_PAD_Y 14
#define BUBBLE_R 30
#define GAP 12
#define LINE_GAP 4
#define TAP_SLOP 24
#define ADD_ID 1
#define ME_ID 2
#define BACK_ID 3
#define SAVE_ID 4
#define FIND_DELAY_MS 400
#define GLIDE_MS 16

enum page { P_NOT_SET_UP, P_WAIT, P_LOGIN, P_CHATS, P_CHAT, P_FIND, P_ME, P_BIO };

static struct k86_window win;
static struct gfx_surface canvas;
static enum page page = P_WAIT;

static struct wg_entry email, password, finder;
static struct wg_button sign_in_btn, sign_out_btn;
static char sign_in_error[80];
static int signing_in;

static struct conv open_conv;
static char draft[TEXT_MAX];
static int draft_len, typing;
static int content_h, stick_bottom = 1;
static uint32_t find_due;

/* Dragging a list by the finger. */
static int scroll, press_y, press_scroll, pressed, dragged;
static int vel, last_y;                 /* px a frame x 256 */
static uint32_t last_move;
static int glide;

static void draw(void);

/* --- what the pages share ----------------------------------------------------------- */

static int view_top(void)
{
    return page == P_FIND ? 136 : 0;
}

static int view_bottom(void)
{
    return page == P_CHAT ? canvas.h - COMPOSE_H : canvas.h;
}

static int scroll_max(void)
{
    int m = content_h - (view_bottom() - view_top());
    return m > 0 ? m : 0;
}

static void scroll_to(int y)
{
    int m = scroll_max();
    scroll = y < 0 ? 0 : y > m ? m : y;
    if (scroll == 0 || scroll == m)
        vel = 0;
    stick_bottom = scroll >= m - 8;
}

static void header(void)
{
    struct k86_header h;
    memset(&h, 0, sizeof(h));
    switch (page) {
    case P_CHAT:  snprintf(h.title, sizeof(h.title), "%s", open_conv.name); break;
    case P_FIND:  strcpy(h.title, "New chat"); break;
    case P_ME:    strcpy(h.title, "Me"); break;
    case P_BIO:   strcpy(h.title, "Bio"); break;
    default:      strcpy(h.title, "R3lay"); break;
    }
    if (page == P_CHAT || page == P_FIND || page == P_ME || page == P_BIO) {
        h.nbuttons = 1;
        h.buttons[0].side = 0;
        h.buttons[0].icon = 1;
        h.buttons[0].id = BACK_ID;
        if (page == P_BIO) {
            h.nbuttons = 2;
            h.buttons[1].side = 1;
            strcpy(h.buttons[1].label, "Save");
            h.buttons[1].id = SAVE_ID;
        }
    } else if (page == P_CHATS) {
        h.nbuttons = 2;
        h.buttons[0].side = 1;
        h.buttons[0].icon = 2;
        h.buttons[0].id = ADD_ID;
        h.buttons[1].side = 0;
        strcpy(h.buttons[1].label, "Me");
        h.buttons[1].id = ME_ID;
    }
    k86_header(&h);
}

static uint32_t avatar_colour(const char *name)
{
    static const uint32_t set[] = { 0x3584e4, 0x33d17a, 0xf6d32d, 0xff7800, 0xe01b24, 0x9141ac, 0x2190a4 };
    uint32_t h = 7;
    for (; *name; name++)
        h = h * 31 + (uint8_t)*name;
    return set[h % (sizeof(set) / sizeof(set[0]))];
}

/* Their picture in a circle, once it's here. */
static int photo_avatar(int cx, int cy, int r, const char *id)
{
    const uint32_t *px = id && id[0] ? api_avatar(id) : 0;
    if (!px)
        return 0;
    for (int y = -r; y < r; y++) {
        int py = cy + y;
        if (py < 0 || py >= canvas.h)
            continue;
        uint32_t *row = canvas.px + (size_t)py * (size_t)canvas.stride;
        const uint32_t *src = px + (size_t)((y + r) * AVATAR_SIZE / (2 * r)) * AVATAR_SIZE;
        for (int x = -r; x < r; x++)
            if (x * x + y * y < r * r && cx + x >= 0 && cx + x < canvas.w)
                row[cx + x] = src[(x + r) * AVATAR_SIZE / (2 * r)];
    }
    return 1;
}

static void avatar(int cx, int cy, int r, const char *id, const char *name)
{
    if (photo_avatar(cx, cy, r, id))
        return;
    char letter[2] = { name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0 };
    gfx_fill_circle(&canvas, cx, cy, r, avatar_colour(name));
    if (letter[0])
        text_draw(&canvas, cx - text_width(letter, TEXT_HEADING) / 2, cy - text_height(TEXT_HEADING) / 2, letter,
                  TEXT_HEADING, 0xffffff);
}

static void centred_note(int y, const char *text)
{
    text_draw(&canvas, (canvas.w - text_width(text, TEXT_BODY)) / 2, y, text, TEXT_BODY, T()->fg_dim);
}

/* --- signing in --------------------------------------------------------------------- */

static void login_layout(void)
{
    int w = canvas.w - 2 * 48, y = 330, h = T()->row_h;
    uint32_t bg = T()->window_bg;
    email.r = (struct gfx_rect){ 48, y, w, h };
    password.r = (struct gfx_rect){ 48, y + h + 24, w, h };
    sign_in_btn.r = (struct gfx_rect){ 48, y + 2 * h + 72, w, h };
    sign_in_btn.label = signing_in ? "Signing in" TEXT_ELLIPSIS : "Sign in";
    sign_in_btn.style = WG_BUTTON_SUGGESTED;
    sign_in_btn.disabled = signing_in;
    email.under = password.under = sign_in_btn.under = bg;
    password.secret = 1;
}

static void login_draw(void)
{
    login_layout();
    wg_label_centred(&canvas, canvas.w / 2, 160, "R3lay", WG_TEXT_HEADING);
    centred_note(226, "Sign in to your account");
    wg_entry_draw(&canvas, &email);
    wg_entry_draw(&canvas, &password);
    if (!email.len)
        text_draw(&canvas, email.r.x + 10, email.r.y + (email.r.h - text_height(TEXT_BODY)) / 2, "Email", TEXT_BODY, T()->fg_dim);
    if (!password.len)
        text_draw(&canvas, password.r.x + 10, password.r.y + (password.r.h - text_height(TEXT_BODY)) / 2, "Password", TEXT_BODY, T()->fg_dim);
    wg_button_draw(&canvas, &sign_in_btn);
    if (sign_in_error[0])
        wg_label_centred(&canvas, canvas.w / 2, sign_in_btn.r.y + sign_in_btn.r.h + 28, sign_in_error, WG_TEXT_ERROR);
}

static void sign_in(void)
{
    if (!email.len || !password.len || signing_in)
        return;
    signing_in = 1;
    sign_in_error[0] = 0;
    email.focused = password.focused = 0;
    k86_log("r3lay: signing in");
    api_sign_in(email.text, password.text);
    draw();
}

void ui_sign_in_failed(const char *why)
{
    signing_in = 0;
    snprintf(sign_in_error, sizeof(sign_in_error), "%s", why);
    k86_logf("r3lay: sign in failed: %s", why);
    wg_entry_set(&password, "");
    password.focused = 1;
    if (page == P_LOGIN)
        draw();
}

static void show_page(enum page p)
{
    page = p;
    scroll = 0;
    glide = 0;
    typing = 0;
    pressed = 0;
    stick_bottom = 1;
    header();
    draw();
}

void ui_signed_in(void)
{
    signing_in = 0;
    wg_entry_set(&password, "");
    k86_log("r3lay: signed in");
    show_page(P_CHATS);
}

void ui_signed_out(const char *why)
{
    snprintf(sign_in_error, sizeof(sign_in_error), "%s", why);
    k86_log("r3lay: signed out");
    show_page(P_LOGIN);
}

void ui_changed(void)
{
    draw();
}

/* --- the chat list ------------------------------------------------------------------ */

static void chats_draw(void)
{
    const struct theme *t = T();
    int top = view_top(), bottom = view_bottom();
    content_h = nconvs * ROW_H;
    if (!list_ready) {
        centred_note(420, offline ? "Can't reach R3lay" : "Loading" TEXT_ELLIPSIS);
        return;
    }
    if (!nconvs) {
        centred_note(420, "No chats yet. Tap + to start one");
        return;
    }
    for (int i = 0; i < nconvs; i++) {
        const struct conv *c = &convs[i];
        int y = top + i * ROW_H - scroll, right = canvas.w - SIDE;
        char when[16];
        if (y + ROW_H <= top || y >= bottom)
            continue;
        avatar(SIDE + AVATAR_R, y + ROW_H / 2, AVATAR_R, c->group ? 0 : c->id, c->name);
        when_short(c->when, when, sizeof(when));
        int tw = text_width(when, TEXT_SMALL), tx = SIDE + 2 * AVATAR_R + 24;
        text_draw(&canvas, right - tw, y + 28, when, TEXT_SMALL, c->unread ? t->accent : t->fg_dim);
        text_draw_fit(&canvas, tx, y + 24, c->name, right - tw - 16 - tx, TEXT_BOLD, t->fg);
        int badge = 0;
        if (c->unread) {
            char n[8];
            snprintf(n, sizeof(n), "%d", c->unread > 99 ? 99 : c->unread);
            badge = text_width(n, TEXT_SMALL) + 24;
            if (badge < 40)
                badge = 40;
            gfx_fill_round_rect(&canvas, right - badge, y + 72, badge, 40, 20, t->accent_bg);
            text_draw(&canvas, right - badge + (badge - text_width(n, TEXT_SMALL)) / 2,
                      y + 72 + (40 - text_height(TEXT_SMALL)) / 2, n, TEXT_SMALL, t->accent_fg);
        }
        text_draw_fit(&canvas, tx, y + 72, c->preview, right - tx - badge - 16, TEXT_BODY, t->fg_dim);
        gfx_fill_rect(&canvas, tx, y + ROW_H - 1, right - tx, 1, t->border);
    }
}

static void open_chat(const struct conv *c)
{
    open_conv = *c;
    draft_len = 0;
    draft[0] = 0;
    api_chat_open(c);
    k86_log("r3lay: chat open");
    show_page(P_CHAT);
}

/* --- a chat ------------------------------------------------------------------------- */

/* Where the lines of m start, wrapped at spaces to fit `room` pixels. */
static void wrap(struct msg *m, int room)
{
    const char *s = m->shown ? m->shown : "";
    int len = (int)strlen(s), cap = 8, n = 1;
    int *line = malloc((size_t)(cap + 1) * sizeof(int)), start = 0, width = 0, widest = 0, space = -1;
    if (!line)
        return;
    line[0] = 0;
    for (int i = 0; i <= len; i++) {
        int newline = s[i] == '\n', end = !s[i], w = end || newline ? 0 : text_width_n(s + i, 1, TEXT_BODY);
        if (!end && !newline && width + w <= room) {
            if (s[i] == ' ')
                space = i;
            width += w;
            continue;
        }
        int stop = i;
        if (!end && !newline && space > start)  /* too wide: back to the last space */
            stop = space;
        int shown = text_width_n(s + start, stop - start, TEXT_BODY);
        if (shown > widest)
            widest = shown;
        if (end)
            break;
        start = newline || stop != i ? stop + 1 : stop;
        i = start - 1;
        width = 0;
        space = -1;
        if (n == cap) {
            cap *= 2;
            int *more = realloc(line, (size_t)(cap + 1) * sizeof(int));
            if (!more)
                break;
            line = more;
        }
        line[n++] = start;
    }
    line[n] = len + 1;
    free(m->line);
    m->line = line;
    m->nlines = n;
    m->w = widest;
}

static int same_day(const struct msg *a, const struct msg *b)
{
    return !strncmp(a->when, b->when, 10);
}

/* Heights, and where each message goes. */
static void chat_layout(void)
{
    int lh = text_height(TEXT_BODY) + LINE_GAP, y = 16, room = canvas.w * 3 / 4 - 2 * BUBBLE_PAD_X;
    if (room > BUBBLE_MAX)
        room = BUBBLE_MAX;
    for (int i = 0; i < nmsgs; i++) {
        struct msg *m = &msgs[i], *prev = i ? &msgs[i - 1] : 0;
        if (!m->line)
            wrap(m, room);
        m->day_above = !prev || !same_day(prev, m);
        m->name_above = open_conv.group && !m->mine && (m->day_above || strcmp(prev->sender, m->sender));
        if (m->day_above)
            y += 64;
        m->y = y;
        m->h = m->nlines * lh + 2 * BUBBLE_PAD_Y + text_height(TEXT_SMALL) + (m->name_above ? text_height(TEXT_SMALL) + 4 : 0);
        y += m->h + GAP;
    }
    content_h = y + 8;
}

static void tick_mark(int x, int y, uint32_t colour)
{
    gfx_thick_line(&canvas, x, y + 8, x + 6, y + 14, 3, colour);
    gfx_thick_line(&canvas, x + 6, y + 14, x + 16, y, 3, colour);
}

static void bubble_draw(const struct msg *m, int top)
{
    const struct theme *t = T();
    char when[16];
    const char *note = m->state == MSG_SENDING ? "Sending" TEXT_ELLIPSIS : m->state == MSG_FAILED ?
                       (m->tries >= 5 ? "Not sent" : "Trying again") : when;
    when_short(m->when, when, sizeof(when));
    int note_w = text_width(note, TEXT_SMALL) + (m->mine && m->state == MSG_OK ? 36 : 0);
    const char *name = m->name_above ? api_name(m->sender) : "";
    int inner = m->w > note_w ? m->w : note_w, name_w = name[0] ? text_width(name, TEXT_SMALL) : 0;
    if (name_w > inner)
        inner = name_w;
    int w = inner + 2 * BUBBLE_PAD_X, x = m->mine ? canvas.w - SIDE - w : SIDE, y = top + m->y;
    uint32_t bg = m->state == MSG_FAILED ? t->destructive_bg : m->mine ? t->accent_bg : t->button_bg;
    uint32_t fg = m->mine || m->state == MSG_FAILED ? t->accent_fg : t->fg;
    gfx_fill_round_rect(&canvas, x, y, w, m->h, BUBBLE_R, bg);
    int ty = y + BUBBLE_PAD_Y, tx = x + BUBBLE_PAD_X, lh = text_height(TEXT_BODY) + LINE_GAP;
    if (m->name_above) {
        text_draw(&canvas, tx, ty, name[0] ? name : "Someone", TEXT_SMALL, t->accent);
        ty += text_height(TEXT_SMALL) + 4;
    }
    for (int i = 0; i < m->nlines; i++) {
        int from = m->line[i], to = m->line[i + 1] - 1;
        if (i + 1 < m->nlines && m->shown[to] != ' ' && m->shown[to] != '\n')
            to++;                           /* a word cut in two: nothing was skipped */
        if (ty + lh > 0 && ty < canvas.h)
            text_draw_n(&canvas, tx, ty, m->shown + from, to - from, TEXT_BODY, fg);
        ty += lh;
    }
    uint32_t dim = gfx_mix(fg, bg, 170);
    int nx = x + w - BUBBLE_PAD_X - note_w;
    text_draw(&canvas, nx, ty, note, TEXT_SMALL, dim);
    if (m->mine && m->state == MSG_OK) {
        int cx = nx + note_w - 22, cy = ty + 6;
        tick_mark(cx - (m->read ? 8 : 0), cy, dim);
        if (m->read)
            tick_mark(cx + 4, cy, dim);
    }
}

static struct gfx_rect field_rect(void)
{
    return (struct gfx_rect){ SIDE, canvas.h - COMPOSE_H + 16, canvas.w - 2 * SIDE - 88, COMPOSE_H - 32 };
}

static struct gfx_rect send_rect(void)
{
    return (struct gfx_rect){ canvas.w - SIDE - 76, canvas.h - COMPOSE_H + 16, 76, COMPOSE_H - 32 };
}

static void compose_draw(void)
{
    const struct theme *t = T();
    struct gfx_rect f = field_rect(), s = send_rect();
    int ty = f.y + (f.h - text_height(TEXT_BODY)) / 2, room = f.w - 56;
    gfx_fill_rect(&canvas, 0, canvas.h - COMPOSE_H, canvas.w, COMPOSE_H, t->headerbar_bg);
    gfx_fill_rect(&canvas, 0, canvas.h - COMPOSE_H, canvas.w, 1, t->headerbar_border);
    gfx_fill_round_rect(&canvas, f.x, f.y, f.w, f.h, f.h / 2, t->button_bg);
    if (draft_len) {
        int start = 0;                      /* the end of a long draft shows */
        while (text_width(draft + start, TEXT_BODY) > room)
            start++;
        int w = text_draw(&canvas, f.x + 28, ty, draft + start, TEXT_BODY, t->fg);
        if (typing)
            gfx_fill_rect(&canvas, f.x + 30 + w, ty + 2, 2, text_height(TEXT_BODY) - 4, t->accent);
    } else {
        if (typing)
            gfx_fill_rect(&canvas, f.x + 28, ty + 2, 2, text_height(TEXT_BODY) - 4, t->accent);
        text_draw(&canvas, f.x + 34, ty, "Message", TEXT_BODY, t->fg_dim);
    }
    gfx_fill_circle(&canvas, s.x + s.w / 2, s.y + s.h / 2, 38, draft_len ? t->accent_bg : t->button_bg);
    uint32_t arrow = draft_len ? t->accent_fg : t->fg_dim;
    int cx = s.x + s.w / 2, cy = s.y + s.h / 2;
    gfx_thick_line(&canvas, cx - 14, cy, cx + 14, cy, 4, arrow);
    gfx_thick_line(&canvas, cx + 2, cy - 12, cx + 14, cy, 4, arrow);
    gfx_thick_line(&canvas, cx + 2, cy + 12, cx + 14, cy, 4, arrow);
}

static void chat_draw(void)
{
    int bottom = view_bottom();
    chat_layout();
    if (stick_bottom)
        scroll = scroll_max();
    if (scroll > scroll_max())
        scroll = scroll_max();
    if (!chat_ready && !nmsgs) {
        centred_note(420, offline ? "Can't reach R3lay" : "Loading" TEXT_ELLIPSIS);
    } else if (!nmsgs) {
        centred_note(420, "No messages yet. Say hi");
    } else {
        for (int i = 0; i < nmsgs; i++) {
            const struct msg *m = &msgs[i];
            int y = m->y - scroll;
            if (y + m->h <= -64 || y >= bottom)
                continue;
            if (m->day_above) {
                char day[24];
                when_day(m->when, day, sizeof(day));
                int w = text_width(day, TEXT_SMALL) + 40;
                gfx_fill_round_rect(&canvas, (canvas.w - w) / 2, y - 56, w, 40, 20, T()->button_bg);
                text_draw(&canvas, (canvas.w - w) / 2 + 20, y - 56 + (40 - text_height(TEXT_SMALL)) / 2, day,
                          TEXT_SMALL, T()->fg_dim);
            }
            bubble_draw(m, -scroll);
        }
    }
    compose_draw();
}

void ui_chat_grew(void)
{
    if (page == P_CHAT) {
        k86_logf("r3lay: %d messages", (uint32_t)nmsgs);
        draw();
    }
}

static void send_draft(void)
{
    if (!draft_len)
        return;
    stick_bottom = 1;
    api_send(draft);
    k86_log("r3lay: sent");
    draft_len = 0;
    draft[0] = 0;
    draw();
}

/* --- finding someone ---------------------------------------------------------------- */

static void find_draw(void)
{
    const struct theme *t = T();
    int top = view_top();
    finder.r = (struct gfx_rect){ SIDE, 32, canvas.w - 2 * SIDE, t->row_h };
    finder.under = t->window_bg;
    wg_entry_draw(&canvas, &finder);
    content_h = nfound * ROW_H;
    if (!finder.len)
        centred_note(top + 80, "Type a username");
    else if (!nfound)
        centred_note(top + 80, "Nobody found");
    for (int i = 0; i < nfound; i++) {
        int y = top + i * ROW_H - scroll;
        if (y + ROW_H <= top || y >= canvas.h)
            continue;
        avatar(SIDE + AVATAR_R, y + ROW_H / 2, AVATAR_R, found[i].id, found[i].name);
        text_draw_fit(&canvas, SIDE + 2 * AVATAR_R + 24, y + (ROW_H - text_height(TEXT_BOLD)) / 2, found[i].name,
                      canvas.w - 2 * SIDE - 2 * AVATAR_R - 24, TEXT_BOLD, t->fg);
    }
}

static void open_found(int i)
{
    struct conv c;
    memset(&c, 0, sizeof(c));
    memcpy(c.id, found[i].id, ID_LEN);
    strcpy(c.name, found[i].name);
    for (int k = 0; k < nconvs; k++)        /* an existing chat stays itself */
        if (!convs[k].group && !strcmp(convs[k].id, c.id))
            c = convs[k];
    open_chat(&c);
}

/* --- me ------------------------------------------------------------------------------ */

static struct gfx_rect bio_rect(void)
{
    return (struct gfx_rect){ SIDE, 350, canvas.w - 2 * SIDE, 104 };
}

static struct gfx_rect note_rect(void)
{
    return (struct gfx_rect){ SIDE, 470, canvas.w - 2 * SIDE, 92 };
}

/* Writing a bio: the words wrapped, with a cursor at the end. */
static char bio_draft[BIO_MAX];

static void bio_draw(void)
{
    const struct theme *t = T();
    int x = SIDE + 8, y = 32, room = canvas.w - 2 * x, lh = text_height(TEXT_BODY) + LINE_GAP, n = (int)strlen(bio_draft);
    int start = 0, width = 0, cursor_x = x, cursor_y = y;
    for (int i = 0; i <= n; i++) {
        int w = i < n ? text_width_n(bio_draft + i, 1, TEXT_BODY) : 0;
        if (i == n || width + w > room) {
            text_draw_n(&canvas, x, y, bio_draft + start, i - start, TEXT_BODY, t->fg);
            cursor_x = x + width;
            cursor_y = y;
            if (i == n)
                break;
            y += lh;
            start = i;
            width = 0;
        }
        width += w;
    }
    gfx_fill_rect(&canvas, cursor_x + 2, cursor_y + 2, 2, text_height(TEXT_BODY) - 4, t->accent);
    if (!n)
        text_draw(&canvas, x + 10, y, "Say something about yourself", TEXT_BODY, t->fg_dim);
}

static void me_draw(void)
{
    const struct theme *t = T();
    const char *name = me_name[0] ? me_name : "";
    struct gfx_rect bio = bio_rect(), note = note_rect();
    avatar(canvas.w / 2, 170, 88, me_id, name);
    wg_label_centred(&canvas, canvas.w / 2, 276, name, WG_TEXT_HEADING);

    gfx_fill_round_rect(&canvas, bio.x, bio.y, bio.w, bio.h, t->radius * 2, t->button_bg);
    text_draw(&canvas, bio.x + 24, bio.y + 12, "Bio", TEXT_SMALL, t->fg_dim);
    text_draw_fit(&canvas, bio.x + 24, bio.y + 12 + text_height(TEXT_SMALL), me_bio[0] ? me_bio : "Tap to write one",
                  bio.w - 48, TEXT_BODY, me_bio[0] ? t->fg : t->fg_dim);

    gfx_fill_round_rect(&canvas, note.x, note.y, note.w, note.h, t->radius * 2, t->button_bg);
    text_draw(&canvas, note.x + 24, note.y + (note.h - text_height(TEXT_BODY)) / 2, "Notifications", TEXT_BODY, t->fg);
    int sw = 92, sh = 52, sx = note.x + note.w - sw - 24, sy = note.y + (note.h - sh) / 2;
    gfx_fill_round_rect(&canvas, sx, sy, sw, sh, sh / 2, notifications_on ? t->accent_bg : t->button_active);
    gfx_fill_circle(&canvas, notifications_on ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, sh / 2 - 6, 0xffffff);

    sign_out_btn.r = (struct gfx_rect){ 48, note.y + note.h + 40, canvas.w - 96, t->row_h };
    sign_out_btn.label = "Sign out";
    sign_out_btn.style = WG_BUTTON_DESTRUCTIVE;
    sign_out_btn.under = t->window_bg;
    wg_button_draw(&canvas, &sign_out_btn);
}

/* --- drawing ------------------------------------------------------------------------- */

static void draw(void)
{
    const struct theme *t = T();
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, page == P_CHATS || page == P_CHAT ? t->view_bg : t->window_bg);
    switch (page) {
    case P_NOT_SET_UP: centred_note(420, "R3lay isn't set up on this phone"); break;
    case P_WAIT:       centred_note(420, "Loading" TEXT_ELLIPSIS); break;
    case P_LOGIN:      login_draw(); break;
    case P_CHATS:      chats_draw(); break;
    case P_CHAT:       chat_draw(); break;
    case P_FIND:       find_draw(); break;
    case P_ME:         me_draw(); break;
    case P_BIO:        bio_draw(); break;
    }
    if (offline && (page == P_CHATS || page == P_CHAT) && (page == P_CHAT ? nmsgs : nconvs)) {
        gfx_fill_rect(&canvas, 0, 0, canvas.w, 44, t->destructive_bg);
        text_draw(&canvas, SIDE, (44 - text_height(TEXT_SMALL)) / 2, "No connection", TEXT_SMALL, t->accent_fg);
    }
    k86_present(0, 0, canvas.w, canvas.h);
}

/* --- input --------------------------------------------------------------------------- */

static int inside(struct gfx_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void tap(int x, int y)
{
    if (page == P_CHATS) {
        int i = (y + scroll) / ROW_H;
        if (list_ready && i >= 0 && i < nconvs)
            open_chat(&convs[i]);
    } else if (page == P_FIND && y >= view_top()) {
        int i = (y - view_top() + scroll) / ROW_H;
        if (i >= 0 && i < nfound)
            open_found(i);
    } else if (page == P_CHAT && typing) {
        typing = 0;
        draw();
    }
    (void)x;
}

/* A finger on a scrolling page. 1 if it was dealt with here. */
static int list_pointer(const struct wg_pointer *p, uint32_t now)
{
    if (p->y >= view_bottom() || p->y < view_top()) {
        if (!pressed)
            return 0;
    }
    if (p->down) {
        pressed = 1;
        dragged = 0;
        glide = 0;
        vel = 0;
        press_y = last_y = p->y;
        press_scroll = scroll;
        last_move = now;
        return 1;
    }
    if (!pressed)
        return 0;
    if (p->held) {
        if (!dragged && abs(p->y - press_y) > TAP_SLOP)
            dragged = 1;
        if (dragged) {
            uint32_t dt = now - last_move ? now - last_move : 1;
            vel = (last_y - p->y) * 256 * GLIDE_MS / (int)dt;
            last_y = p->y;
            last_move = now;
            scroll_to(press_scroll + press_y - p->y);
            draw();
        }
        return 1;
    }
    pressed = 0;
    if (dragged) {
        glide = now - last_move < 80 && abs(vel) > 256;
    } else {
        tap(p->x, p->y);
    }
    return 1;
}

static void pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    switch (page) {
    case P_LOGIN: {
        if (signing_in)
            break;
        wg_entry_pointer(&email, &p);
        wg_entry_pointer(&password, &p);
        if (p.down) {
            email.focused = inside(email.r, p.x, p.y);
            password.focused = inside(password.r, p.x, p.y);
        }
        if (wg_button_pointer(&sign_in_btn, &p))
            sign_in();
        else
            draw();
        break;
    }
    case P_CHATS:
        list_pointer(&p, ev->time_ms);
        break;
    case P_CHAT:
        if (p.up && !pressed) {
            if (inside(send_rect(), p.x, p.y)) {
                send_draft();
            } else if (inside(field_rect(), p.x, p.y) && !typing) {
                typing = 1;
                stick_bottom = 1;
                draw();
            }
            break;
        }
        list_pointer(&p, ev->time_ms);
        break;
    case P_FIND:
        if (p.down && inside(finder.r, p.x, p.y)) {
            wg_entry_pointer(&finder, &p);
            finder.focused = 1;
            draw();
            break;
        }
        list_pointer(&p, ev->time_ms);
        break;
    case P_ME:
        if (p.up && inside(bio_rect(), p.x, p.y)) {
            snprintf(bio_draft, sizeof(bio_draft), "%s", me_bio);
            show_page(P_BIO);
        } else if (p.up && inside(note_rect(), p.x, p.y)) {
            api_set_notifications(!notifications_on);
            k86_logf("r3lay: notifications %s", notifications_on ? "on" : "off");
            draw();
        } else if (wg_button_pointer(&sign_out_btn, &p)) {
            api_sign_out();
        } else {
            draw();
        }
        break;
    default:
        break;
    }
}

static void go_back(void)
{
    if (page == P_BIO) {
        show_page(P_ME);
        return;
    }
    if (page == P_CHAT)
        api_chat_close();
    if (page == P_CHAT || page == P_FIND || page == P_ME) {
        api_list_load();
        show_page(P_CHATS);
    }
}

static void key(const struct key_event *k)
{
    if (!k->pressed)
        return;
    if (k->key == KEY_ESC) {
        if (page == P_CHAT && typing) {
            typing = 0;
            draw();
        } else if (page == P_LOGIN) {
            email.focused = password.focused = 0;
            draw();
        } else if (page == P_FIND && finder.focused) {
            finder.focused = 0;
            draw();
        } else {
            go_back();
        }
        return;
    }
    if (page == P_LOGIN) {
        struct wg_entry *e = password.focused ? &password : email.focused ? &email : 0;
        if (!e)
            return;
        if (k->key == KEY_TAB || (wg_entry_key(e, k) == WG_ENTRY_ACTIVATE && e == &email)) {
            email.focused = e != &email;
            password.focused = e == &email;
        } else if (e == &password && k->key == KEY_ENTER) {
            sign_in();
            return;
        }
        draw();
    } else if (page == P_FIND && finder.focused) {
        if (wg_entry_key(&finder, k) == WG_ENTRY_CHANGED)
            find_due = k86_uptime_ms() + FIND_DELAY_MS;
        draw();
    } else if (page == P_BIO) {
        int n = (int)strlen(bio_draft);
        if (k->key == KEY_BACKSPACE) {
            if (n)
                bio_draft[n - 1] = 0;
        } else if (((k->ascii >= 32 && k->ascii < 127) || k->ascii >= 0xa0) && n < BIO_MAX - 1) {
            bio_draft[n] = (char)k->ascii;
            bio_draft[n + 1] = 0;
        }
        draw();
    } else if (page == P_CHAT && typing) {
        if (k->key == KEY_ENTER) {
            send_draft();
        } else if (k->key == KEY_BACKSPACE) {
            if (draft_len)
                draft[--draft_len] = 0;
            draw();
        } else if (((k->ascii >= 32 && k->ascii < 127) || k->ascii >= 0xa0) && draft_len < TEXT_MAX - 1) {
            draft[draft_len++] = (char)k->ascii;
            draft[draft_len] = 0;
            draw();
        }
    }
}

static void wheel(const struct k86_event *ev)
{
    if (page == P_CHATS || page == P_CHAT || page == P_FIND) {
        scroll_to(scroll + ev->id * 120);
        draw();
    }
}

static void timer(void)
{
    static uint32_t last_tick;
    uint32_t now = k86_uptime_ms();
    http_pump();
    if (now - last_tick >= 1000) {
        last_tick = now;
        api_tick();
    }
    if (find_due && now >= find_due) {
        find_due = 0;
        api_find(finder.text);
    }
    if (glide) {
        scroll_to(scroll + vel / 256);
        vel = vel * 15 / 16;
        if (abs(vel) < 200)
            glide = 0;
        draw();
    }
}

static void timer_follow(void)
{
    static uint32_t was;
    uint32_t want = glide ? GLIDE_MS : (http_pump() || find_due) ? 50 : 500;
    if (want != was) {
        was = want;
        k86_timer_set(want);
    }
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    header();
    draw();
    if (api_setup()) {
        k86_log("r3lay: not set up");
        page = P_NOT_SET_UP;
        draw();
    } else if (!api_resume()) {
        email.focused = 1;
        show_page(P_LOGIN);
    }
    k86_log("r3lay: open");

    for (;;) {
        struct k86_event ev;
        int want_keys = (page == P_LOGIN && (email.focused || password.focused)) ||
                        (page == P_FIND && finder.focused) || (page == P_CHAT && typing) || page == P_BIO;
        k86_back_wanted(page == P_CHAT || page == P_FIND || page == P_ME || page == P_BIO ||
                        (want_keys && page != P_LOGIN));
        k86_keyboard_wanted(want_keys);
        timer_follow();
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER: pointer(&ev); break;
        case K86_EVENT_SCROLL:  wheel(&ev); break;
        case K86_EVENT_KEY:     key(&ev.key); break;
        case K86_EVENT_TIMER:   timer(); break;
        case K86_EVENT_HEADER:
            if (ev.id == BACK_ID) {
                go_back();
            } else if (ev.id == ADD_ID) {
                wg_entry_set(&finder, "");
                finder.focused = 1;
                nfound = 0;
                show_page(P_FIND);
            } else if (ev.id == ME_ID) {
                show_page(P_ME);
            } else if (ev.id == SAVE_ID) {
                api_save_bio(bio_draft);
                k86_log("r3lay: bio saved");
                show_page(P_ME);
            }
            break;
        case K86_EVENT_THEME:   draw(); break;
        case K86_EVENT_RESIZE:
            if (ev.x != canvas.w)
                for (int i = 0; i < nmsgs; i++) {   /* wrapped for the old width */
                    free(msgs[i].line);
                    msgs[i].line = 0;
                }
            canvas.w = ev.x;
            canvas.h = ev.y;
            draw();
            break;
        case K86_EVENT_CLOSE:   return 0;
        default:                break;
        }
    }
}
