/* LiteKern Go: the cover screen (kernel/panicscreen.h). */
#include "kernel/panicscreen.h"
#include "kernel/printk.h"
#include "kernel/string.h"

#define BLACK   0x000000u
#define GREY    0xaaaaaau
#define WHITE   0xffffffu
#define RED     0xff5555u
#define YELLOW  0xffff55u
#define GREEN   0x55ff55u
#define CYAN    0x55ffffu

#define COLS 88
#define TEST_MS 9000                    /* the memory test */
#define LINE_MS 230                     /* a line of the dump */
#define DUMP_LINES 64
#define WAIT_MS 7000                    /* "Rebooting in 5 seconds" and a bit more, then the test again */

static uint32_t t0, seed;

void panicscreen_start(uint32_t now)
{
    t0 = now;
    seed = now * 2654435761u | 1;
}

static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static void put(struct gfx_surface *s, int ox, int oy, int row, const char *text, uint32_t fg)
{
    char line[COLS + 1];
    int n = 0;
    for (; text[n] && n < COLS; n++)
        line[n] = text[n];
    line[n] = '\0';
    gfx_text(s, 4 - ox, row * 16 - oy, line, fg, BLACK);
}

/* One line of the dump, k from 0: the same every time for the same k. */
static void dump_line(int k, uint32_t stamp_ms, char *out, int max, uint32_t *colour)
{
    uint32_t s = seed + (uint32_t)k * 7919u, a, b;
    a = rnd(&s);
    b = rnd(&s);
    uint32_t sec = stamp_ms / 1000, us = stamp_ms % 1000 * 1000 + (a & 999);
    *colour = GREY;
    switch (k) {
    case 0:  ksnprintf(out, (uint32_t)max, "[%5u.%06u] mmc0: error -110 whilst initialising SD card", sec, us); break;
    case 1:  ksnprintf(out, (uint32_t)max, "[%5u.%06u] qcom-smd-rpm rpm: timeout waiting for RPM reply", sec, us); break;
    case 2:  ksnprintf(out, (uint32_t)max, "[%5u.%06u] msm_dsi 1a98000.dsi: [drm:dsi_host_transfer] *ERROR* DSI transfer timed out", sec, us);
             *colour = YELLOW; break;
    case 3:  ksnprintf(out, (uint32_t)max, "[%5u.%06u] EXT4-fs (mmcblk0p27): unable to read superblock", sec, us); *colour = YELLOW; break;
    case 4:  ksnprintf(out, (uint32_t)max, "[%5u.%06u] VFS: Cannot open root device \"mmcblk0p27\" or unknown-block(179,27): error -6", sec, us); break;
    case 5:  ksnprintf(out, (uint32_t)max, "[%5u.%06u] ------------[ cut here ]------------", sec, us); break;
    case 6:  ksnprintf(out, (uint32_t)max, "[%5u.%06u] WARNING: CPU: 2 PID: 214 at drivers/gpu/drm/msm/msm_gem.c:742 msm_gem_put_pages+0x4c/0x88", sec, us);
             *colour = YELLOW; break;
    case 7:  ksnprintf(out, (uint32_t)max, "[%5u.%06u] Unable to handle kernel NULL pointer dereference at virtual address 0000000000000008", sec, us);
             *colour = RED; break;
    case 8:  ksnprintf(out, (uint32_t)max, "[%5u.%06u] Mem abort info:", sec, us); break;
    case 9:  ksnprintf(out, (uint32_t)max, "[%5u.%06u]   ESR = 0x0000000096000005", sec, us); break;
    case 10: ksnprintf(out, (uint32_t)max, "[%5u.%06u]   EC = 0x25: DABT (current EL), IL = 32 bits", sec, us); break;
    case 11: ksnprintf(out, (uint32_t)max, "[%5u.%06u]   SET = 0, FnV = 0", sec, us); break;
    case 12: ksnprintf(out, (uint32_t)max, "[%5u.%06u]   EA = 0, S1PTW = 0", sec, us); break;
    case 13: ksnprintf(out, (uint32_t)max, "[%5u.%06u] Data abort info:", sec, us); break;
    case 14: ksnprintf(out, (uint32_t)max, "[%5u.%06u]   ISV = 0, ISS = 0x00000005, ISS2 = 0x00000000", sec, us); break;
    case 15: ksnprintf(out, (uint32_t)max, "[%5u.%06u] user pgtable: 4k pages, 48-bit VAs, pgdp=00000000%08x", sec, us, a & 0xfffff000u); break;
    case 16: ksnprintf(out, (uint32_t)max, "[%5u.%06u] Internal error: Oops: 0000000096000005 [#1] PREEMPT SMP", sec, us); *colour = RED; break;
    case 17: ksnprintf(out, (uint32_t)max, "[%5u.%06u] Modules linked in: snd_soc_msm8916_digital msm_dsi panel_samsung_s6e8aa0 wcn36xx", sec, us); break;
    case 18: ksnprintf(out, (uint32_t)max, "[%5u.%06u] CPU: 1 UID: 0 PID: 1 Comm: init Not tainted 6.12.1-msm8916 #1", sec, us); break;
    case 19: ksnprintf(out, (uint32_t)max, "[%5u.%06u] Hardware name: Samsung Galaxy A5 (2015) (DT)", sec, us); break;
    case 20: ksnprintf(out, (uint32_t)max, "[%5u.%06u] pstate: 60400005 (nZCv daif +PAN -UAO -TCO -DIT -SSBS BTYPE=--)", sec, us); break;
    case 21: ksnprintf(out, (uint32_t)max, "[%5u.%06u] pc : ffff8000%08x", sec, us, (a & 0x00fffffcu) | 0x80000000u); break;
    case 22: ksnprintf(out, (uint32_t)max, "[%5u.%06u] lr : ffff8000%08x", sec, us, (b & 0x00fffffcu) | 0x80000000u); break;
    case 23: ksnprintf(out, (uint32_t)max, "[%5u.%06u] sp : ffff8000%08x", sec, us, (a >> 4 & 0x00fffff0u) | 0x83000000u); break;
    case 24: ksnprintf(out, (uint32_t)max, "[%5u.%06u] x29: ffff800083c1bb90 x28: ffff0000c1ad2b00 x27: 0000000000000000", sec, us); break;
    case 25: ksnprintf(out, (uint32_t)max, "[%5u.%06u] x26: 0000000000000000 x25: ffff800080d6e1c8 x24: ffff0000c0a31840", sec, us); break;
    case 26: ksnprintf(out, (uint32_t)max, "[%5u.%06u] x2 : 0000000000000000 x1 : 0000000000000008 x0 : ffff0000c1ad2b00", sec, us); break;
    case 27: ksnprintf(out, (uint32_t)max, "[%5u.%06u] Call trace:", sec, us); *colour = WHITE; break;
    case 28: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  msm_gem_put_pages+0x4c/0x88", sec, us); break;
    case 29: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  msm_gem_free_object+0x68/0x120", sec, us); break;
    case 30: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  drm_gem_object_release_handle+0x58/0xa0", sec, us); break;
    case 31: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  idr_for_each+0x70/0xf8", sec, us); break;
    case 32: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  drm_gem_release+0x2c/0x48", sec, us); break;
    case 33: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  drm_file_free.part.0+0x1fc/0x2c0", sec, us); break;
    case 34: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  drm_release+0x9c/0x10c", sec, us); break;
    case 35: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  __fput+0xd4/0x2b8", sec, us); break;
    case 36: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  ____fput+0x18/0x28", sec, us); break;
    case 37: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  task_work_run+0x80/0xe4", sec, us); break;
    case 38: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  do_exit+0x2a8/0x9a0", sec, us); break;
    case 39: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  do_group_exit+0x34/0x90", sec, us); break;
    case 40: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  __arm64_sys_exit_group+0x18/0x20", sec, us); break;
    case 41: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  invoke_syscall+0x48/0x114", sec, us); break;
    case 42: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  el0_svc_common.constprop.0+0x40/0xe0", sec, us); break;
    case 43: ksnprintf(out, (uint32_t)max, "[%5u.%06u]  el0t_64_sync_handler+0x100/0x12c", sec, us); break;
    case 44: ksnprintf(out, (uint32_t)max, "[%5u.%06u] Code: f9400400 b4000080 f9400c00 d63f0000 (f9400400)", sec, us); break;
    case 45: ksnprintf(out, (uint32_t)max, "[%5u.%06u] ---[ end trace 0000000000000000 ]---", sec, us); break;
    case 46: ksnprintf(out, (uint32_t)max, "[%5u.%06u] Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b", sec, us);
             *colour = RED; break;
    case 47: ksnprintf(out, (uint32_t)max, "[%5u.%06u] SMP: stopping secondary CPUs", sec, us); break;
    case 48: ksnprintf(out, (uint32_t)max, "[%5u.%06u] Kernel Offset: disabled", sec, us); break;
    case 49: ksnprintf(out, (uint32_t)max, "[%5u.%06u] CPU features: 0x0,00000000,40000000,0000420b", sec, us); break;
    case 50: ksnprintf(out, (uint32_t)max, "[%5u.%06u] Memory Limit: none", sec, us); break;
    case 51: ksnprintf(out, (uint32_t)max, "[%5u.%06u] ---[ end Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b ]---", sec, us);
             *colour = RED; break;
    default: out[0] = '\0'; break;
    }
}

/* The memory test: a bar that fills, patterns that change, errors that pile up. */
static void draw_test(struct gfx_surface *s, int ox, int oy, uint32_t el, int rows)
{
    int pct = (int)(el * 100 / TEST_MS);
    pct = pct > 100 ? 100 : pct;
    char line[120];
    int r = 1;
    put(s, ox, oy, r++, "LiteKern MemCheck 2.3 - memory diagnostic", WHITE);
    put(s, ox, oy, r++, "CPU: ARM Cortex-A53 x4 @ 998 MHz     Board: Samsung Galaxy A5 (SM-A500FU)", GREY);
    put(s, ox, oy, r++, "Memory: 2048 MB LPDDR3 at 0x80000000 (2 channels, 32 bit)", GREY);
    r++;
    static const char *const tests[] = { "address walking 1s", "moving inversions (8 bit)", "moving inversions (random)",
                                         "block move, 64 moves", "modulo 20, random pattern" };
    int t = pct * 5 / 101;
    ksnprintf(line, sizeof(line), "Test %d of 5: %s", t + 1, tests[t]);
    put(s, ox, oy, r++, line, CYAN);
    char bar[64];
    int fill = pct * 50 / 100;
    for (int i = 0; i < 50; i++)
        bar[i] = i < fill ? '#' : '.';
    bar[50] = '\0';
    ksnprintf(line, sizeof(line), "[%s] %3d%%", bar, pct);
    put(s, ox, oy, r++, line, WHITE);
    r++;
    int errors = pct > 30 ? (pct - 30) * (pct - 28) / 9 : 0;
    ksnprintf(line, sizeof(line), "Pass 1 of 4     Errors: %d", errors);
    put(s, ox, oy, r++, line, errors ? RED : GREEN);
    r++;
    uint32_t sd = seed;
    int shown = errors < 40 ? errors : 40;
    for (int i = 0; i < shown && r < rows - 6; i++) {
        uint32_t a = rnd(&sd), b = rnd(&sd);
        uint32_t addr = 0x80000000u + (a & 0x7ffffffcu), want = (b & 1) ? 0xffffffffu : 0xaaaaaaaau, got = want ^ (1u << (a >> 27));
        ksnprintf(line, sizeof(line), "FAIL  address 0x%08x  expected 0x%08x  got 0x%08x  (bit %u)", addr, want, got, a >> 27);
        put(s, ox, oy, r++, line, RED);
    }
    if (pct >= 100) {
        r++;
        ksnprintf(line, sizeof(line), "TEST FAILED: %d errors in 2048 MB. The memory may be defective.", errors);
        put(s, ox, oy, r++, line, RED);
        put(s, ox, oy, r++, "Continuing to boot is not recommended.", YELLOW);
    }
}

void panicscreen_draw(struct gfx_surface *s, int ox, int oy, uint32_t now)
{
    gfx_fill_rect(s, 0, 0, s->w, s->h, BLACK);
    int rows = 1280 / 16;
    uint32_t cycle = TEST_MS + 1500 + DUMP_LINES * LINE_MS + WAIT_MS;
    uint32_t el = (now - t0) % cycle;
    if (el < TEST_MS + 1500) {
        draw_test(s, ox, oy, el < TEST_MS ? el : TEST_MS, rows);
        return;
    }
    el -= TEST_MS + 1500;
    int shown = (int)(el / LINE_MS);
    shown = shown > 52 ? 52 : shown;
    /* The lines wrapped to the screen's width, the last ones that fit. */
    static char wrapped[160][COLS + 1];
    static uint32_t wcolour[160];
    int nrows = 0;
    char line[200];
    for (int k = 0; k < shown && nrows < 150; k++) {
        uint32_t colour;
        dump_line(k, 11400 + (uint32_t)k * 170, line, sizeof(line), &colour);
        int len = (int)strlen(line), at = 0;
        while (at < len && nrows < 158) {
            int n = len - at < COLS - (at ? 2 : 0) ? len - at : COLS - (at ? 2 : 0), j = 0;
            if (at)
                wrapped[nrows][j++] = ' ', wrapped[nrows][j++] = ' ';
            for (int i = 0; i < n; i++)
                wrapped[nrows][j++] = line[at + i];
            wrapped[nrows][j] = '\0';
            wcolour[nrows++] = colour;
            at += n;
        }
    }
    int keep = rows - 4, from = nrows > keep ? nrows - keep : 0, r = 0;
    for (int k = from; k < nrows; k++)
        put(s, ox, oy, r++, wrapped[k], wcolour[k]);
    if (shown >= 52) {                              /* the dump is done: the countdown to a restart that never comes */
        int left = 5 - (int)((el - 52u * LINE_MS) / 1000);
        left = left < 0 ? 0 : left;
        ksnprintf(line, sizeof(line), "Rebooting in %d seconds..", left);
        put(s, ox, oy, r++, line, WHITE);
    }
    if ((now / 500) % 2 == 0)
        put(s, ox, oy, r, "_", WHITE);
}

void panicscreen_draw_restart(struct gfx_surface *s, int ox, int oy)
{
    gfx_fill_rect(s, 0, 0, s->w, s->h, BLACK);
    put(s, ox, oy, 1, "Restarting...", GREY);
}
