/* LiteKern X on Linux — the x11 backend: a window on the PC. It speaks the X11
 * protocol to the X server itself (WSL's WSLg has one, $DISPLAY=:0), so no
 * library is needed: a socket, a window, PutImage for the pixels, and the
 * key, button, motion and expose events back.
 *
 * Key codes are Linux evdev codes + 8 (what X servers on Linux use), which
 * platform/devices.c turns into the kernel's key codes: the layout (AZERTY)
 * is LiteKern X's own, not the PC's. The desktop draws its own pointer, so
 * X's is hidden over the window. */
#include "platform/platform.h"
#include "kernel/input.h"
#include "kernel/printk.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int fd = -1, win_w, win_h;
static uint32_t id_base, id_mask, next_id = 1, root, window, gc;
static uint32_t max_req_bytes;
static uint8_t out[1 << 20];
static size_t out_len;
static uint8_t inbuf[32 * 256];
static size_t in_len;
static uint8_t buttons;

/* --- bytes ------------------------------------------------------------------------------------- */

static uint32_t new_id(void)
{
    return id_base | (next_id++ & id_mask);
}

static void lost(void)
{
    kprintf("x11: the window was closed\n");
    plat_exit(0);
}

static void write_all(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t k = write(fd, b, n);
        if (k <= 0)
            lost();
        b += k;
        n -= (size_t)k;
    }
}

static void flush(void)
{
    write_all(out, out_len);
    out_len = 0;
}

static void put(const void *p, size_t n)
{
    if (out_len + n > sizeof(out))
        flush();
    if (n > sizeof(out)) {
        write_all(p, n);
        return;
    }
    memcpy(out + out_len, p, n);
    out_len += n;
}

static void put8(uint8_t v) { put(&v, 1); }
static void put16(uint16_t v) { put(&v, 2); }           /* we said little-endian ('l') */
static void put32(uint32_t v) { put(&v, 4); }

static void request(uint8_t opcode, uint8_t data, uint16_t words)
{
    put8(opcode);
    put8(data);
    put16(words);
}

static int read_all(void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t k = read(fd, b, n);
        if (k <= 0)
            return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* --- connecting -------------------------------------------------------------------------------- */

/* The MIT-MAGIC-COOKIE-1 for display `num` from ~/.Xauthority, if there is one. */
static int cookie(int num, uint8_t *out_cookie)
{
    char path[512];
    const char *xa = getenv("XAUTHORITY"), *home = getenv("HOME");
    if (xa)
        snprintf(path, sizeof(path), "%s", xa);
    else if (home)
        snprintf(path, sizeof(path), "%s/.Xauthority", home);
    else
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    char want[16];
    snprintf(want, sizeof(want), "%d", num);
    for (size_t at = 0; at + 2 <= n;) {
        at += 2;                                    /* family */
        const uint8_t *field[4];
        size_t len[4];
        for (int k = 0; k < 4; k++) {
            if (at + 2 > n)
                return 0;
            len[k] = (size_t)(buf[at] << 8 | buf[at + 1]);
            field[k] = buf + at + 2;
            at += 2 + len[k];
            if (at > n)
                return 0;
        }
        /* address, display number, name, data */
        if (len[1] == strlen(want) && !memcmp(field[1], want, len[1]) && len[2] == 18 &&
            !memcmp(field[2], "MIT-MAGIC-COOKIE-1", 18) && len[3] == 16) {
            memcpy(out_cookie, field[3], 16);
            return 1;
        }
    }
    return 0;
}

static int x_open(int w, int h)
{
    const char *display = getenv("DISPLAY");
    int num = 0;
    if (!display || sscanf(display, ":%d", &num) != 1) {
        fprintf(stderr, "x11: $DISPLAY isn't a local display (\":0\")\n");
        return -1;
    }
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "/tmp/.X11-unix/X%d", num);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "x11: can't connect to %s\n", sa.sun_path);
        return -1;
    }

    uint8_t key[16];
    int auth = cookie(num, key);
    put8('l'), put8(0), put16(11), put16(0);
    put16(auth ? 18 : 0), put16(auth ? 16 : 0), put16(0);
    if (auth) {
        put("MIT-MAGIC-COOKIE-1\0\0", 20);
        put(key, 16);
    }
    flush();
    uint8_t head[8];
    if (read_all(head, 8)) {
        fprintf(stderr, "x11: the server hung up\n");
        return -1;
    }
    size_t extra = (size_t)get16(head + 6) * 4;
    uint8_t *d = malloc(extra + 1);
    if (!d || read_all(d, extra)) {
        free(d);
        return -1;
    }
    if (head[0] != 1) {
        d[extra] = '\0';
        fprintf(stderr, "x11: refused: %.*s\n", (int)head[1], (const char *)d);
        free(d);
        return -1;
    }
    id_base = get32(d + 4);
    id_mask = get32(d + 8);
    uint16_t vendor_len = get16(d + 16);
    max_req_bytes = (uint32_t)get16(d + 18) * 4;
    uint8_t nformats = d[21];
    const uint8_t *fmt = d + 32 + ((vendor_len + 3) & ~3u);
    int bpp24 = 0;
    for (int i = 0; i < nformats; i++)
        if (fmt[i * 8] == 24)
            bpp24 = fmt[i * 8 + 1];
    const uint8_t *screen = fmt + 8 * nformats;
    root = get32(screen);
    uint8_t depth = screen[38];
    free(d);
    if (depth != 24 || bpp24 != 32) {
        fprintf(stderr, "x11: needs a 24-bit screen with 32-bit pixels (got depth %u, %d bpp)\n", depth, bpp24);
        return -1;
    }

    win_w = w, win_h = h;
    window = new_id();
    request(1, 0, 8 + 2);                           /* CreateWindow */
    put32(window), put32(root);
    put16(0), put16(0), put16((uint16_t)w), put16((uint16_t)h), put16(0), put16(1);
    put32(0);                                       /* visual: the parent's */
    put32(0x2 | 0x800);                             /* background pixel, event mask */
    put32(0x202634);
    put32(1 << 0 | 1 << 1 | 1 << 2 | 1 << 3 | 1 << 6 | 1 << 15 | 1 << 21);
    /* The title, and a fixed size (WM_NORMAL_HINTS: min = max). */
    static const char title[] = "LiteKern X";
    request(18, 0, (uint16_t)(6 + (sizeof(title) - 1 + 3) / 4));      /* ChangeProperty */
    put32(window), put32(39), put32(31);            /* WM_NAME, STRING */
    put8(8), put8(0), put16(0), put32(sizeof(title) - 1);
    put(title, sizeof(title) - 1);
    put("\0\0\0", (4 - (sizeof(title) - 1) % 4) % 4);
    request(18, 0, 6 + 18);
    put32(window), put32(40), put32(41);            /* WM_NORMAL_HINTS, WM_SIZE_HINTS */
    put8(32), put8(0), put16(0), put32(18);
    uint32_t hints[18] = { 16 | 32, 0, 0, 0, 0, (uint32_t)w, (uint32_t)h, (uint32_t)w, (uint32_t)h };
    put(hints, sizeof(hints));
    gc = new_id();
    request(55, 0, 4);                              /* CreateGC */
    put32(gc), put32(window), put32(0);
    /* No X pointer over the window: an empty 1x1 cursor. */
    uint32_t pix = new_id(), cur = new_id();
    request(53, 1, 4);                              /* CreatePixmap, depth 1 */
    put32(pix), put32(window), put16(1), put16(1);
    request(93, 0, 8);                              /* CreateCursor */
    put32(cur), put32(pix), put32(pix);
    put16(0), put16(0), put16(0), put16(0), put16(0), put16(0), put16(0), put16(0);
    request(2, 0, 4);                               /* ChangeWindowAttributes: cursor */
    put32(window), put32(0x4000), put32(cur);
    request(8, 0, 2);                               /* MapWindow */
    put32(window);
    flush();
    kprintf("x11: a %dx%d window on %s\n", w, h, display);
    return 0;
}

/* --- drawing ----------------------------------------------------------------------------------- */

static void x_present(const uint32_t *px, int stride, int x, int y, int w, int h)
{
    if (fd < 0)
        return;
    int rows_per = (int)((max_req_bytes - 24) / ((uint32_t)w * 4));
    if (rows_per < 1)
        return;                                     /* wider than a request: not at these sizes */
    for (int row = 0; row < h; row += rows_per) {
        int n = h - row < rows_per ? h - row : rows_per;
        request(72, 2, (uint16_t)(6 + w * n));      /* PutImage, ZPixmap */
        put32(window), put32(gc);
        put16((uint16_t)w), put16((uint16_t)n), put16((uint16_t)x), put16((uint16_t)(y + row));
        put8(0), put8(24), put16(0);
        for (int r = 0; r < n; r++)
            put(px + (y + row + r) * stride + x, (size_t)w * 4);
    }
}

/* --- events ------------------------------------------------------------------------------------ */

static uint8_t button_bit(uint8_t b)
{
    return b == 1 ? MOUSE_LEFT : b == 2 ? MOUSE_MIDDLE : b == 3 ? MOUSE_RIGHT : 0;
}

static void event(const uint8_t *e)
{
    int16_t ex = (int16_t)get16(e + 24), ey = (int16_t)get16(e + 26);
    switch (e[0] & 0x7f) {
    case 0:                                         /* an error */
        kprintf("x11: error %u from request %u\n", e[1], e[10]);
        break;
    case 2:                                         /* KeyPress */
    case 3:                                         /* KeyRelease */
        if (e[1] >= 8)
            plat_key(plat_evdev_key(e[1] - 8u), (e[0] & 0x7f) == 2);
        break;
    case 4:                                         /* ButtonPress */
        buttons |= button_bit(e[1]);
        plat_pointer(ex, ey, buttons);
        break;
    case 5:                                         /* ButtonRelease */
        buttons &= (uint8_t)~button_bit(e[1]);
        plat_pointer(ex, ey, buttons);
        break;
    case 6:                                         /* MotionNotify */
        plat_pointer(ex, ey, buttons);
        break;
    case 12: {                                      /* Expose: show that part again */
        int w, h;
        const uint32_t *px = plat_framebuffer(&w, &h);
        int x = get16(e + 8), y = get16(e + 10), ew = get16(e + 12), eh = get16(e + 14);
        if (px && x < w && y < h)
            x_present(px, w, x, y, x + ew > w ? w - x : ew, y + eh > h ? h - y : eh);
        break;
    }
    }
}

/* LKX_X11_VERIFY=1 (tests, debugging): once the desktop is up, read a few
 * pixels back from the X server (GetImage) and log whether they're the
 * framebuffer's: the window really shows what the desktop drew, in the right
 * colours. */
static void verify(void)
{
    int w, h;
    const uint32_t *px = plat_framebuffer(&w, &h);
    const int at[][2] = { { w / 2, 15 }, { w - 1, h - 1 }, { w / 4, h / 2 }, { w / 2, h - 40 } };
    int good = 0;
    for (int i = 0; i < 4; i++) {
        request(73, 2, 5);                          /* GetImage, ZPixmap */
        put32(window), put16((uint16_t)at[i][0]), put16((uint16_t)at[i][1]), put16(1), put16(1);
        put32(0xffffffffu);
        flush();
        uint8_t r[32];
        for (;;) {                                  /* events before the reply: handled */
            if (read_all(r, 32))
                lost();
            if (r[0] == 1)
                break;
            event(r);
        }
        size_t extra = (size_t)get32(r + 4) * 4;
        uint8_t data[64] = { 0 };
        if (extra > sizeof(data) || read_all(data, extra))
            lost();
        uint32_t server = get32(data) & 0xffffff, ours = px[at[i][1] * w + at[i][0]] & 0xffffff;
        good += server == ours;
        kprintf("x11: pixel (%d, %d): window %06x, framebuffer %06x\n", at[i][0], at[i][1], server, ours);
    }
    kprintf("x11: the window shows the framebuffer: %s\n", good == 4 ? "yes" : "NO");
}

static int x_fd(void)
{
    flush();
    return fd;
}

static int x_due(void)
{
    return -1;
}

/* Whatever events have come (never waits). */
static void x_pump(void)
{
    static int verified;
    if (!verified && plat_ready() && getenv("LKX_X11_VERIFY")) {
        verified = 1;
        verify();
    }
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0)
        return;
    ssize_t k = read(fd, inbuf + in_len, sizeof(inbuf) - in_len);
    if (k <= 0)
        lost();
    in_len += (size_t)k;
    size_t at = 0;
    while (in_len - at >= 32) {
        const uint8_t *e = inbuf + at;
        size_t len = 32;
        if (e[0] == 1 || (e[0] & 0x7f) == 35)       /* a reply or a generic event: longer */
            len += (size_t)get32(e + 4) * 4;
        if (in_len - at < len)
            break;
        if (e[0] != 1)
            event(e);
        at += len;
    }
    memmove(inbuf, inbuf + at, in_len - at);
    in_len -= at;
}

static void x_close(void)
{
    if (fd >= 0) {
        if (out_len && write(fd, out, out_len) < 0)
            out_len = 0;                            /* gone already: nothing to tidy */
        close(fd);
        fd = -1;
    }
}

const struct backend backend_x11 = { "x11", x_open, x_present, x_fd, x_due, x_pump, x_close };
