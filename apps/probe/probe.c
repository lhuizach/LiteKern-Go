/* LiteKern X — Hardware Probe, a KERN86 app: stage (a) of the monitor and
 * fan control app (docs/MONITOR.md).
 *
 * Shows what the kernel's hardware probe (SYS_HW_PROBE, kernel/hwprobe.h)
 * found on this machine: a summary of what monitoring and fan control can
 * use, then the details by section (processor, firmware, ACPI, embedded
 * controller and its named fields, disk, power, devices). Each line has a
 * status dot: accent = there and read, amber = there but not usable yet,
 * grey = missing, red = there but failing.
 *
 * Nothing here writes to hardware. Finding which embedded-controller bytes
 * are the fan, the temperatures and the battery takes watching them change:
 *   - Read EC RAM: all 256 bytes once; each later read highlights what moved.
 *   - Watch: reads the EC every 2 s and keeps, per byte that ever changed,
 *     its lowest and highest value and how often it moved. It notes when the
 *     charger goes in or out. Everything is logged when it stops.
 *   - Stress: runs the CPU flat out (up to 4 minutes) so the fan has to
 *     react, with Watch on. It stops by itself if the EC's CPU temperature
 *     (byte 0x51) reaches 85 C or reads nonsense, or if the EC stops
 *     answering; the firmware keeps running the fan throughout.
 * The EC page's field names and the EC fields page (dense, three columns)
 * show current values from the latest read.
 *
 * Keys: Up/Down/PgUp/PgDn/Home/End scroll, Tab or 1-9, 0 change section,
 * R rescans, E reads EC RAM, W watches, S stresses. */
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"

#define MAX_ITEMS 640
#define SIDE_W 212
#define SIDE_ROW 36
#define FOOT_H 30
#define NAME_W 196
#define MARGIN 24
#define ROW_H 30
#define ROW_NOTE_H 48
#define HEX_STEP 25
#define BAR_W 14

#define DENSE_ROW_H 22
#define DENSE_COLS 3
#define WATCH_HEAD_H 146

#define SEC_WATCH K86_PROBE_SECTIONS
#define NSEC (K86_PROBE_SECTIONS + 1)

#define STRESS_MS (4 * 60 * 1000)
#define WATCH_MS 2000
#define STRESS_SAMPLE_MS 1000
#define STOP_TEMP_C 85
#define MAX_MISSES 8                /* EC reads failed in a row before Watch gives up */
#define EC_CPU_TEMP 0x51            /* found by comparing reads (docs/MONITOR.md) */

#define ID_RESCAN 1
#define ID_READ_EC 2
#define ID_WATCH 3
#define ID_STRESS 4

enum action { ACT_READ, ACT_WATCH, ACT_STRESS };

static struct k86_window win;
static struct gfx_surface canvas;

static struct k86_probe_item items[MAX_ITEMS];
static int nitems, total;
static int rows[MAX_ITEMS], nrows;          /* the entries in the section shown */
static int section, top;
static int side_hover = -1;
static int ec_ok_to_read;                   /* the user said yes, this session */
static int ec_reads;                        /* how many times EC RAM was read */
static uint8_t ec_now[256], ec_before[256], ec_changed[256];
static int ec_valid;                        /* ec_now holds a real read */
static int nchanged;

/* Watch */
static int watching, stressing;
static uint32_t watch_start, last_sample, stress_end;
static int w_samples, w_misses;
static uint8_t w_cur[256], w_min[256], w_max[256];
static uint16_t w_chg[256];
static int wlist[256], nw;                  /* bytes that have ever changed, by address */
static char w_event[4][44];
static int w_nevents;
static char w_status[96];

static struct wg_dialog dialog;
static int dialog_open;
static enum action pending;

static const char *const section_name[NSEC] = {
    "Summary", "Processor", "Firmware", "ACPI", "Embedded controller", "EC fields", "Disk",
    "Power", "Devices", "Watch",
};
static const char *const section_about[NSEC] = {
    "What monitoring and fan control can use on this machine",
    "The CPU's own sensor, clock and throttling registers",
    "The BIOS, SMBIOS and ACPI as the firmware describes them",
    "Tables, devices, thermal zones and regions the firmware declares",
    "The chip that runs the fan and battery gauge. Read-only",
    "Names the ACPI tables give to bytes of EC RAM, with their values now",
    "Drives, and their SMART health and temperature",
    "Battery, AC adapter, lid, ASUS controls, backlight, memory",
    "PCI devices, and whether LiteKern X drives them",
    "EC bytes that have changed since Watch started",
};

/* --- text helpers ------------------------------------------------------------- */

static char *put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = '\0';
    return p;
}

static char *put_num(char *p, int v)
{
    char d[12];
    int n = 0;
    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    do
        d[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        *p++ = d[--n];
    *p = '\0';
    return p;
}

static char *put_hex2(char *p, int v)
{
    static const char digits[] = "0123456789abcdef";
    *p++ = digits[v >> 4 & 15];
    *p++ = digits[v & 15];
    *p = '\0';
    return p;
}

static void num(char *out, int v)
{
    put_num(out, v);
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* An EC RAM row: section EC, named "0x00".."0xf0", 16 hex bytes. */
static int ec_row(const struct k86_probe_item *it, int *base)
{
    if (it->section != K86_PS_EC || it->name[0] != '0' || it->name[1] != 'x' || it->name[4])
        return 0;
    *base = hexval(it->name[2]) * 16 + hexval(it->name[3]);
    return *base >= 0;
}

/* The bytes of one row's value: "12 34 ...  9a ..." (two spaces mid-row). */
static int parse_hex(const char *s, uint8_t *out)
{
    int n = 0;
    while (*s && n < 16) {
        while (*s == ' ')
            s++;
        int hi = hexval(s[0]), lo = s[0] ? hexval(s[1]) : -1;
        if (hi < 0 || lo < 0)
            break;
        out[n++] = (uint8_t)(hi * 16 + lo);
        s += 2;
    }
    return n;
}

/* A field's value text: "0x61, 1 byte", "0x93, 2 bytes", "0x62 bit 3, 1 bit". */
static int parse_field(const char *v, int *addr, int *bit, int *width)
{
    if (v[0] != '0' || v[1] != 'x')
        return -1;
    int i = 2, a = 0, b = 0, w = 0;
    while (hexval(v[i]) >= 0)
        a = a * 16 + hexval(v[i++]);
    if (!memcmp(v + i, " bit ", 5)) {
        i += 5;
        while (v[i] >= '0' && v[i] <= '9')
            b = b * 10 + (v[i++] - '0');
    }
    if (v[i] != ',')
        return -1;
    i += 2;
    while (v[i] >= '0' && v[i] <= '9')
        w = w * 10 + (v[i++] - '0');
    *addr = a;
    *bit = b;
    *width = !memcmp(v + i, " byte", 5) ? w * 8 : w;
    return 0;
}

/* The field's current value from the latest EC read: 0 and *ok = 1 if it can be told. */
static int field_value(int addr, int bit, int width, int *ok)
{
    *ok = 0;
    if (!ec_valid || addr > 255 || bit + width > 16 || width < 1)
        return 0;
    uint32_t raw = ec_now[addr] | (addr < 255 ? ec_now[addr + 1] << 8 : 0);
    *ok = 1;
    return (int)(raw >> bit & ((1u << width) - 1));
}

/* --- probing ------------------------------------------------------------------- */

static int is_dense(void)
{
    return section == K86_PS_EC_FIELDS || section == SEC_WATCH;
}

static void select_section(int s)
{
    section = s;
    top = 0;
    nrows = 0;
    if (s == SEC_WATCH) {
        nrows = nw;
        return;
    }
    for (int i = 0; i < nitems; i++)
        if (items[i].section == section)
            rows[nrows++] = i;
}

static void probe(int read_ec)
{
    total = k86_hw_probe(items, MAX_ITEMS, read_ec ? K86_PROBE_EC_RAM : 0);
    nitems = total < 0 ? 0 : total > MAX_ITEMS ? MAX_ITEMS : total;
    int got = 0;
    for (int i = 0; i < nitems; i++) {
        int base;
        if (ec_row(&items[i], &base) && base + 16 <= 256)
            got += parse_hex(items[i].value, &ec_now[base]) == 16;
    }
    nchanged = 0;
    if (got == 16) {
        for (int i = 0; i < 256; i++) {
            ec_changed[i] = ec_reads > 0 && ec_now[i] != ec_before[i];
            nchanged += ec_changed[i];
        }
        memcpy(ec_before, ec_now, sizeof(ec_now));
        ec_valid = 1;
        ec_reads++;
        k86_logf("probe: EC RAM read %d, %d bytes changed", ec_reads, nchanged);
    }
    int keep_top = top;
    select_section(section);
    top = keep_top < nrows ? keep_top : 0;
}

static int have_ec(void)
{
    for (int i = 0; i < nitems; i++)
        if (items[i].section == K86_PS_EC && !strcmp(items[i].name, "Embedded controller"))
            return items[i].status == K86_PROBE_OK;
    return 0;
}

/* --- watch and stress ----------------------------------------------------------- */

static void push_event(const char *what)
{
    uint32_t s = (k86_uptime_ms() - watch_start) / 1000;
    char line[44], *p = line;
    p = put_num(p, (int)(s / 60));
    p = put(p, s % 60 < 10 ? ":0" : ":");
    p = put_num(p, (int)(s % 60));
    p = put(p, "  ");
    put(p, what);
    if (w_nevents == 4) {
        for (int i = 0; i < 3; i++)
            memcpy(w_event[i], w_event[i + 1], sizeof(w_event[i]));
        w_nevents = 3;
    }
    memcpy(w_event[w_nevents++], line, sizeof(line));
    k86_logf("probe: watch %s", line);
}

static void set_header(void)
{
    struct k86_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.title, "Hardware Probe", 15);
    h.nbuttons = 4;
    h.buttons[0] = (typeof(h.buttons[0])){ .side = 1, .id = ID_STRESS };
    h.buttons[1] = (typeof(h.buttons[0])){ .side = 1, .id = ID_WATCH };
    h.buttons[2] = (typeof(h.buttons[0])){ .side = 1, .id = ID_READ_EC };
    h.buttons[3] = (typeof(h.buttons[0])){ .side = 1, .id = ID_RESCAN };
    const char *s = stressing ? "Stop stress" : "Stress";
    const char *w = watching ? "Stop watch" : "Watch";
    memcpy(h.buttons[0].label, s, strlen(s) + 1);
    memcpy(h.buttons[1].label, w, strlen(w) + 1);
    memcpy(h.buttons[2].label, "Read EC RAM", 12);
    memcpy(h.buttons[3].label, "Rescan", 7);
    k86_header(&h);
}

static void update_timer(void)
{
    k86_timer_set(stressing ? 1 : watching ? WATCH_MS : 0);
}

static void rebuild_wlist(void)
{
    nw = 0;
    for (int i = 0; i < 256; i++)
        if (w_chg[i])
            wlist[nw++] = i;
    if (section == SEC_WATCH) {
        nrows = nw;
        if (top >= nrows)
            top = 0;
    }
}

static void stop_stress(const char *why)
{
    if (!stressing)
        return;
    stressing = 0;
    char *p = put(w_status, "Stress stopped: ");
    put(p, why);
    push_event(why);
    update_timer();
    set_header();
}

static void stop_watch(void)
{
    if (!watching)
        return;
    stop_stress("watch stopped");
    watching = 0;
    update_timer();
    set_header();
    for (int i = 0; i < nw; i++) {
        int a = wlist[i];
        k86_logf("probe: watch byte 0x%x now %u min %u max %u changes %u", a, w_cur[a], w_min[a],
                 w_max[a], w_chg[a]);
    }
    k86_logf("probe: watch ended after %u samples, %u bytes changed", w_samples, nw);
    char *p = put(w_status, "Stopped after ");
    p = put_num(p, w_samples);
    put(p, " samples");
}

static void sample(void)
{
    uint8_t b[256];
    last_sample = k86_uptime_ms();
    int err = k86_ec_read(b);
    if (err) {
        /* The EC is sometimes busy (the kernel already retried each byte).
         * A miss isn't the end; several in a row are. Stress, which can't
         * see the temperature without the EC, gives up sooner. */
        char msg[48];
        w_misses++;
        char *mp = put_num(put(msg, "EC read failed ("), err);
        mp = put_num(put(mp, "), "), w_misses);
        put(mp, " in a row");
        if (stressing && w_misses >= 3)
            stop_stress("EC not answering, stress stopped");
        if (w_misses >= MAX_MISSES) {
            stop_watch();
            put(put(w_status, "The EC stopped answering: "), msg);
        } else if (watching) {
            put(put(w_status, "EC busy, missed a read: "), msg);
        }
        return;
    }
    w_misses = 0;
    int ac_was = w_cur[0xd0] & 1, st_was = w_cur[0x3f];
    for (int i = 0; i < 256; i++) {
        if (!w_samples) {
            w_min[i] = w_max[i] = b[i];
        } else {
            if (b[i] != w_cur[i] && w_chg[i] < 65535)
                w_chg[i]++;
            if (b[i] < w_min[i])
                w_min[i] = b[i];
            if (b[i] > w_max[i])
                w_max[i] = b[i];
        }
    }
    memcpy(w_cur, b, sizeof(b));
    memcpy(ec_now, b, sizeof(b));
    ec_valid = 1;
    if (w_samples && (b[0xd0] & 1) != ac_was)
        push_event((b[0xd0] & 1) ? "charger plugged in (0xd0 bit 0)" : "charger unplugged (0xd0 bit 0)");
    if (w_samples && b[0x3f] != st_was)
        push_event(b[0x3f] == 2 ? "battery charging (0x3f = 2)" : b[0x3f] == 1
                   ? "battery discharging (0x3f = 1)" : "battery state 0x3f changed");
    w_samples++;
    rebuild_wlist();
    int t = b[EC_CPU_TEMP];
    if (stressing && (t >= STOP_TEMP_C || t < 5 || t > 125)) {
        char msg[40], *p = put_num(put(msg, "CPU at "), t);
        put(p, " C");
        stop_stress(msg);
    }
}

static void start_watch(void)
{
    watching = 1;
    w_samples = 0;
    nw = 0;
    w_nevents = 0;
    memset(w_chg, 0, sizeof(w_chg));
    watch_start = k86_uptime_ms();
    w_misses = 0;
    w_status[0] = '\0';
    update_timer();
    set_header();
    sample();
    select_section(SEC_WATCH);
    k86_log("probe: watch started");
}

static void start_stress(void)
{
    if (!watching)
        start_watch();
    stressing = 1;
    stress_end = k86_uptime_ms() + STRESS_MS;
    push_event("stress started (4 min, stops at 85 C)");
    update_timer();
    set_header();
    select_section(SEC_WATCH);
    k86_log("probe: stress started");
}

static volatile uint32_t sink;

/* Keep the CPU busy for about ms milliseconds: integer maths, with divisions
 * (slow on the Atom), calling the kernel now and then so the watchdog and the
 * clock both see the app is alive. */
static void burn(uint32_t ms)
{
    uint32_t t0 = k86_uptime_ms(), x = sink | 1;
    while (k86_uptime_ms() - t0 < ms) {
        for (int i = 0; i < 4000; i++) {
            x = x * 1664525u + 1013904223u;
            x ^= x >> 7;
            x = x / 3u + x % 7u + (x >> 11);
        }
    }
    sink = x;
}

/* A timer event: 1 if something on screen changed. */
static int tick(void)
{
    int changed = 0;
    uint32_t now = k86_uptime_ms();
    if (stressing) {
        if (now >= stress_end) {
            stop_stress("4 minutes done");
            sample();
            changed = 1;
        } else {
            burn(40);
        }
    }
    if (watching && now - last_sample >= (stressing ? STRESS_SAMPLE_MS : WATCH_MS - 100)) {
        sample();
        changed = 1;
        if (stressing) {
            uint32_t left = (stress_end - k86_uptime_ms()) / 1000;
            char *p = put(w_status, "Stress: ");
            p = put_num(p, (int)(left / 60));
            p = put(p, left % 60 < 10 ? ":0" : ":");
            p = put_num(p, (int)(left % 60));
            put(p, " left. Listen to the fan.");
        } else if (watching) {
            char *p = put(w_status, "Watching: ");
            p = put_num(p, w_samples);
            put(p, " samples, every 2 s");
        }
    }
    return changed;
}

/* --- drawing ------------------------------------------------------------------------ */

static uint32_t status_colour(int st)
{
    const struct theme *t = theme_get();
    switch (st) {
    case K86_PROBE_OK:      return t->accent;
    case K86_PROBE_PARTIAL: return t->warning;
    case K86_PROBE_MISSING: return gfx_mix(t->ink, t->view_bg, 70);
    case K86_PROBE_ERROR:   return t->destructive;
    default:                return 0xffffffffu;   /* no dot */
    }
}

static int row_height(const struct k86_probe_item *it)
{
    return it->note[0] ? ROW_NOTE_H : ROW_H;
}

static int list_top(void)
{
    return MARGIN + text_height(TEXT_HEADING) + 4 + text_height(TEXT_BODY) + 14;
}

static int list_bottom(void)
{
    return canvas.h - FOOT_H;
}

static int dense_top(void)
{
    return list_top() + (section == SEC_WATCH ? WATCH_HEAD_H : 0);
}

static int dense_cap(void)
{
    int lines = (list_bottom() - dense_top()) / DENSE_ROW_H;
    return (lines > 0 ? lines : 1) * DENSE_COLS;
}

/* How many entries from `from` fit in the list. */
static int fit_from(int from)
{
    if (is_dense()) {
        int left = nrows - from, cap = dense_cap();
        return left < cap ? left : cap;
    }
    int y = list_top(), n = 0;
    for (int i = from; i < nrows; i++) {
        y += row_height(&items[rows[i]]);
        if (y > list_bottom())
            break;
        n++;
    }
    return n;
}

static void draw_sidebar(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, 0, SIDE_W, canvas.h, t->window_bg);
    gfx_fill_rect(&canvas, SIDE_W - 1, 0, 1, canvas.h, t->border);
    int counts[NSEC] = { 0 }, worst[NSEC] = { 0 };
    for (int i = 0; i < nitems; i++) {
        counts[items[i].section]++;
        if (items[i].status == K86_PROBE_ERROR)
            worst[items[i].section] = 1;
    }
    counts[SEC_WATCH] = nw;
    for (int s = 0; s < NSEC; s++) {
        int y = 8 + s * SIDE_ROW;
        if (s == section)
            gfx_fill_round_rect(&canvas, 8, y, SIDE_W - 17, SIDE_ROW - 4, 6, t->row_selected);
        else if (s == side_hover)
            gfx_fill_round_rect(&canvas, 8, y, SIDE_W - 17, SIDE_ROW - 4, 6, t->row_hover);
        int ty = y + (SIDE_ROW - 4 - text_height(TEXT_BODY)) / 2;
        char n[8];
        num(n, counts[s]);
        int nw_ = text_width(n, TEXT_SMALL);
        text_draw_fit(&canvas, 20, ty, section_name[s], SIDE_W - 52 - nw_, TEXT_BODY,
                      s == SEC_WATCH && watching ? t->accent : t->fg);
        text_draw(&canvas, SIDE_W - 22 - nw_, ty + 1, n, TEXT_SMALL, worst[s] ? t->destructive : t->fg_dim);
    }
}

static void draw_hex(int x, int y, const struct k86_probe_item *it, int base)
{
    const struct theme *t = theme_get();
    uint8_t b[16];
    int n = parse_hex(it->value, b);
    for (int k = 0; k < n; k++) {
        static const char digits[] = "0123456789abcdef";
        char s[3] = { digits[b[k] >> 4], digits[b[k] & 15], 0 };
        int changed = ec_changed[base + k];
        int bx = x + k * HEX_STEP + (k >= 8 ? 10 : 0);
        if (changed)
            gfx_fill_round_rect(&canvas, bx - 3, y - 1, HEX_STEP - 3, text_height(TEXT_BODY) + 2, 4,
                                t->accent_bg);
        text_draw(&canvas, bx, y, s, TEXT_BODY, changed ? t->accent_fg : t->fg);
    }
}

/* One cell of the dense views. */
static void draw_cell(int x, int y, int w, int idx)
{
    const struct theme *t = theme_get();
    char s[48], *p;
    if (section == SEC_WATCH) {
        int a = wlist[idx];
        put_hex2(put(s, "0x"), a);
        text_draw(&canvas, x, y, s, TEXT_BOLD, t->fg);
        put_num(s, w_cur[a]);
        text_draw(&canvas, x + 48, y, s, TEXT_BODY, t->fg);
        p = put_num(s, w_min[a]);
        p = put(p, "-");
        put_num(p, w_max[a]);
        text_draw(&canvas, x + 96, y, s, TEXT_BODY, t->fg_dim);
        put_num(put(s, "x"), w_chg[a]);
        text_draw_fit(&canvas, x + 168, y, s, w - 170, TEXT_BODY, t->fg_dim);
        return;
    }
    const struct k86_probe_item *it = &items[rows[idx]];
    int addr, bit, width, ok = 0, v = 0;
    text_draw_fit(&canvas, x, y, it->name, 56, TEXT_BOLD, t->fg);
    if (parse_field(it->value, &addr, &bit, &width)) {
        text_draw_fit(&canvas, x + 60, y, it->value, w - 62, TEXT_BODY, t->fg_dim);
        return;
    }
    p = put_hex2(s, addr);
    if (bit) {
        p = put(p, ".");
        p = put_num(p, bit);
    }
    if (width > 8 && !bit) {
        p = put(p, " ");
        p = put_num(p, width / 8);
        put(p, "B");
    }
    text_draw(&canvas, x + 60, y, s, TEXT_BODY, t->fg_dim);
    v = field_value(addr, bit, width, &ok);
    if (ok) {
        put_num(s, v);
        text_draw(&canvas, x + 150, y, s, TEXT_BODY, t->fg);
    } else {
        text_draw(&canvas, x + 150, y, "-", TEXT_BODY, t->fg_dim);
    }
}

static void draw_watch_head(int x, int w)
{
    const struct theme *t = theme_get();
    int y = list_top(), lh = DENSE_ROW_H;
    char s[160], *p;
    text_draw_fit(&canvas, x, y, w_status[0] ? w_status : "Press Watch (or Stress) to start.", w,
                  TEXT_BOLD, stressing ? t->warning : t->fg);
    y += lh;
    if (w_samples) {
        int cur = (int16_t)(w_cur[0x2e] | w_cur[0x2f] << 8);
        p = put(s, "CPU ");
        p = put_num(p, w_cur[EC_CPU_TEMP]);
        p = put(p, " C   0x57: ");
        p = put_num(p, w_cur[0x57]);
        p = put(p, "   0x63: ");
        p = put_num(p, w_cur[0x63]);
        p = put(p, "   0x66: ");
        p = put_num(p, w_cur[0x66] << 8 | w_cur[0x67]);
        p = put(p, "   battery ");
        p = put_num(p, cur);
        p = put(p, " mA   ");
        put(p, (w_cur[0xd0] & 1) ? "charger in" : "on battery");
        text_draw_fit(&canvas, x, y, s, w, TEXT_BODY, t->fg);
    }
    y += lh;
    for (int i = 0; i < w_nevents; i++)
        text_draw_fit(&canvas, x, y + i * 18, w_event[i], w, TEXT_SMALL, t->fg_dim);
    /* Column heads for the table below. */
    if (nw) {
        int y2 = list_top() + WATCH_HEAD_H - 20, cw = w / DENSE_COLS;
        for (int c = 0; c < DENSE_COLS; c++) {
            int cx = x + c * cw;
            text_draw(&canvas, cx, y2, "byte", TEXT_SMALL, t->fg_dim);
            text_draw(&canvas, cx + 48, y2, "now", TEXT_SMALL, t->fg_dim);
            text_draw(&canvas, cx + 96, y2, "low-high", TEXT_SMALL, t->fg_dim);
            text_draw(&canvas, cx + 168, y2, "moves", TEXT_SMALL, t->fg_dim);
        }
    }
}

static void draw_content(void)
{
    const struct theme *t = theme_get();
    int x0 = SIDE_W, w = canvas.w - SIDE_W;
    gfx_fill_rect(&canvas, x0, 0, w, canvas.h, t->view_bg);
    int x = x0 + MARGIN;
    text_draw(&canvas, x, MARGIN, section_name[section], TEXT_HEADING, t->fg);
    text_draw_fit(&canvas, x, MARGIN + text_height(TEXT_HEADING) + 4, section_about[section],
                  w - 2 * MARGIN, TEXT_BODY, t->fg_dim);

    int y = list_top(), right = canvas.w - MARGIN - BAR_W;
    int vx = x + 20 + NAME_W + 12;
    int shown = 0;
    if (!nrows && section != SEC_WATCH)
        text_draw(&canvas, x, y + 8, nitems ? "Nothing in this section." : "The probe returned nothing.",
                  TEXT_BODY, t->fg_dim);
    if (section == SEC_WATCH)
        draw_watch_head(x, right - x);
    if (is_dense()) {
        int cw = (right - x) / DENSE_COLS, lines = dense_cap() / DENSE_COLS, ty0 = dense_top();
        shown = fit_from(top);
        if (section == K86_PS_EC_FIELDS && !ec_valid)
            text_draw(&canvas, x, ty0 - 4, "Values appear after Read EC RAM or Watch.", TEXT_SMALL,
                      t->fg_dim);
        for (int k = 0; k < shown; k++) {
            int col = k / lines, line = k % lines;
            draw_cell(x + col * cw, ty0 + line * DENSE_ROW_H + (section == K86_PS_EC_FIELDS ? 10 : 0),
                      cw - 8, top + k);
        }
    } else {
        for (int i = top; i < nrows; i++) {
            const struct k86_probe_item *it = &items[rows[i]];
            int h = row_height(it);
            if (y + h > list_bottom())
                break;
            gfx_fill_rect(&canvas, x, y + h - 1, right - x, 1, t->border);
            int ty = y + (ROW_H - text_height(TEXT_BODY)) / 2;
            uint32_t dot = status_colour(it->status);
            if (dot != 0xffffffffu)
                gfx_fill_circle(&canvas, x + 6, ty + text_height(TEXT_BODY) / 2, 5, dot);
            text_draw_fit(&canvas, x + 20, ty, it->name, NAME_W, TEXT_BOLD, t->fg);
            int base;
            if (ec_row(it, &base))
                draw_hex(vx, ty, it, base);
            else
                text_draw_fit(&canvas, vx, ty, it->value, right - vx, TEXT_BODY, t->fg);
            if (it->note[0])
                text_draw_fit(&canvas, vx, ty + text_height(TEXT_BODY) + 1, it->note, right - vx,
                              TEXT_SMALL, t->fg_dim);
            y += h;
            shown++;
        }
    }
    /* The scrollbar, when not everything fits. */
    if (shown < nrows) {
        int ty = dense_top(), th = list_bottom() - ty - 8;
        int bx = canvas.w - MARGIN - 6;
        gfx_fill_round_rect(&canvas, bx, ty, 6, th, 3, gfx_mix(t->ink, t->view_bg, 20));
        int kh = th * shown / nrows, ky = ty + th * top / nrows;
        gfx_fill_round_rect(&canvas, bx, ky, 6, kh < 24 ? 24 : kh, 3, gfx_mix(t->ink, t->view_bg, 110));
    }

    /* Footer: the promise, and what the last EC read showed. */
    int fy = canvas.h - FOOT_H;
    gfx_fill_rect(&canvas, x0, fy, w, 1, t->border);
    char foot[120];
    char *p = put(foot, "Read-only: nothing is written to the hardware.");
    if (ec_reads) {
        p = put(p, ec_reads == 1 ? "  EC RAM read once." : "  EC RAM read ");
        if (ec_reads > 1) {
            p = put_num(p, ec_reads);
            p = put(p, " times; ");
            p = put_num(p, nchanged);
            put(p, nchanged == 1 ? " byte changed since the last." : " bytes changed since the last.");
        }
    }
    /* Which build this is, at the right: when a fix doesn't seem to be there,
     * this says whether the machine is running it. */
    static const char build[] = "build " __DATE__ " " __TIME__;
    int bw = text_width(build, TEXT_SMALL);
    text_draw_fit(&canvas, x, fy + (FOOT_H - text_height(TEXT_SMALL)) / 2, foot,
                  w - 2 * MARGIN - bw - 16, TEXT_SMALL, t->fg_dim);
    text_draw(&canvas, canvas.w - MARGIN - bw, fy + (FOOT_H - text_height(TEXT_SMALL)) / 2, build,
              TEXT_SMALL, t->fg_dim);
}

static void draw(void)
{
    draw_sidebar();
    draw_content();
    if (dialog_open) {
        wg_dialog_layout(&dialog, canvas.w, canvas.h);
        wg_dialog_show(&canvas, &dialog);
    }
    k86_present(0, 0, canvas.w, canvas.h);
}

/* --- input --------------------------------------------------------------------------- */

static void scroll_to(int to)
{
    /* The furthest it scrolls: the first entry from which the rest all fit. */
    int last = 0;
    while (last < nrows && fit_from(last) < nrows - last)
        last++;
    if (to > last)
        to = last;
    if (to < 0)
        to = 0;
    if (to != top) {
        top = to;
        draw_content();
        k86_present(SIDE_W, 0, canvas.w - SIDE_W, canvas.h);
    }
}

static void run(enum action what)
{
    if (what == ACT_READ) {
        probe(1);
    } else if (what == ACT_WATCH) {
        if (watching)
            stop_watch();
        else
            start_watch();
    } else {
        if (stressing)
            stop_stress("stopped by you");
        else
            start_stress();
    }
    draw();
}

static void ask(enum action what)
{
    if (!have_ec()) {                   /* nothing to read: the EC page says why */
        select_section(K86_PS_EC);
        draw();
        return;
    }
    /* Already agreed once this session, or stopping something: no question. */
    if (ec_ok_to_read || (what == ACT_WATCH && watching) || (what == ACT_STRESS && stressing)) {
        run(what);
        return;
    }
    pending = what;
    if (what == ACT_READ)
        wg_dialog_init(&dialog, "Read the embedded controller?",
                       "Reads its 256 bytes of RAM, one at a time, with the ACPI read command. "
                       "Nothing is written to it. Read again later to see what changed.",
                       "Read", WG_BUTTON_SUGGESTED, 0);
    else if (what == ACT_WATCH)
        wg_dialog_init(&dialog, "Watch the embedded controller?",
                       "Reads its 256 bytes every 2 seconds and keeps the bytes that change. "
                       "Nothing is written to it.",
                       "Watch", WG_BUTTON_SUGGESTED, 0);
    else
        wg_dialog_init(&dialog, "Run the CPU flat out?",
                       "Up to 4 minutes, so the fan has to react. Stops by itself at 85 C. "
                       "Watches the EC every second; the firmware keeps controlling the fan.",
                       "Start", WG_BUTTON_SUGGESTED, 0);
    dialog_open = 1;
    draw();
}

static void dialog_done(int choice)
{
    dialog_open = 0;
    if (choice == 1) {
        ec_ok_to_read = 1;
        k86_log("probe: EC RAM reads allowed");
        run(pending);
        return;
    }
    draw();
}

static void pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    if (dialog_open) {
        int c = wg_dialog_pointer(&dialog, &p);
        if (c != WG_DIALOG_NONE)
            dialog_done(c);
        else if (dialog.dirty) {
            wg_dialog_draw(&canvas, &dialog);
            k86_present(dialog.r.x, dialog.r.y, dialog.r.w, dialog.r.h);
        }
        return;
    }
    int over = -1;
    if (p.x < SIDE_W && p.y >= 8) {
        int s = (p.y - 8) / SIDE_ROW;
        over = s < NSEC ? s : -1;
    }
    if (over != side_hover) {
        side_hover = over;
        draw_sidebar();
        k86_present(0, 0, SIDE_W, canvas.h);
    }
    if (!p.down)
        return;
    if (over >= 0 && over != section) {
        select_section(over);
        draw();
    } else if (p.x >= canvas.w - MARGIN - BAR_W && p.y >= dense_top() && p.y < list_bottom()) {
        /* The scrollbar's track: a page up or down. */
        int th = list_bottom() - dense_top() - 8, ky = dense_top() + (nrows ? th * top / nrows : 0);
        int page = fit_from(top);
        scroll_to(p.y < ky ? top - page : top + (page > 1 ? page : 1));
    }
}

static void key(const struct key_event *k)
{
    if (!k->pressed)
        return;
    if (dialog_open) {
        int c = wg_dialog_key(&dialog, k);
        if (c != WG_DIALOG_NONE)
            dialog_done(c);
        return;
    }
    int step = is_dense() ? DENSE_COLS : 1;
    int page = fit_from(top);
    switch (k->key) {
    case KEY_UP:       scroll_to(top - step); return;
    case KEY_DOWN:     scroll_to(top + step); return;
    case KEY_PAGEUP:   scroll_to(top - (page > 1 ? page : 1)); return;
    case KEY_PAGEDOWN: scroll_to(top + (page > 1 ? page : 1)); return;
    case KEY_HOME:     scroll_to(0); return;
    case KEY_END:      scroll_to(nrows); return;
    case KEY_TAB:
        select_section((section + ((k->mods & MOD_SHIFT) ? NSEC - 1 : 1)) % NSEC);
        draw();
        return;
    default:
        break;
    }
    if (k->ascii >= '1' && k->ascii <= '9') {
        select_section(k->ascii - '1');
        draw();
    } else if (k->ascii == '0') {
        select_section(SEC_WATCH);
        draw();
    } else if (k->ascii == 'r' || k->ascii == 'R') {
        probe(ec_ok_to_read);
        draw();
    } else if (k->ascii == 'e' || k->ascii == 'E') {
        ask(ACT_READ);
    } else if (k->ascii == 'w' || k->ascii == 'W') {
        ask(ACT_WATCH);
    } else if (k->ascii == 's' || k->ascii == 'S') {
        ask(ACT_STRESS);
    }
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    set_header();
    k86_log("probe: open");
    probe(0);
    k86_logf("probe: %u items", total);
    draw();

    for (;;) {
        struct k86_event ev;
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER: pointer(&ev); break;
        case K86_EVENT_KEY:     key(&ev.key); break;
        case K86_EVENT_THEME:   draw(); break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            scroll_to(top);
            draw();
            break;
        case K86_EVENT_TIMER:
            if (tick() && !dialog_open) {
                draw_content();
                k86_present(SIDE_W, 0, canvas.w - SIDE_W, canvas.h);
            }
            break;
        case K86_EVENT_HEADER:
            if (dialog_open)
                break;
            if (ev.id == ID_RESCAN) {
                probe(ec_ok_to_read);
                draw();
            } else if (ev.id == ID_READ_EC) {
                ask(ACT_READ);
            } else if (ev.id == ID_WATCH) {
                ask(ACT_WATCH);
            } else if (ev.id == ID_STRESS) {
                ask(ACT_STRESS);
            }
            break;
        case K86_EVENT_CLOSE:   return 0;
        default:                break;
        }
    }
}
