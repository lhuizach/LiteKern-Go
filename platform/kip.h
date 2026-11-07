/* LiteKern X on Linux — KIP, how an app (its own Linux process) makes KERN86
 * calls to lkx-shell (phase L2). Shared by both sides:
 *   platform/libk86.c  the app's side: sdk/kern86.h's k86_call() ends up here
 *   platform/lkx.c     the shell's side: runs the call through kernel/sys_app.c
 *
 * In the kernel, a call's pointers point into the app's address space, which
 * the kernel can read. Here the app sends the bytes behind them: each call
 * is described below (kip_args), so the app knows which arguments are
 * strings or buffers and how big. The shell puts them in its own memory and
 * hands kernel/sys_app.c pointers to those copies, so that code, and every
 * check in it (paths, users, privacy), runs unchanged; then the output
 * buffers come back.
 *
 * Over a stream socket (the app's fd 3, LKX_FD):
 *   app -> shell   struct kip_request, then each IN/STR buffer's bytes in
 *                  argument order
 *   shell -> app   struct kip_reply, then each OUT buffer's bytes; for
 *                  SYS_WINDOW_OPEN the canvas's memory fd rides along
 *                  (SCM_RIGHTS)
 * SYS_WAIT_EVENT's reply comes when there's an event. */
#ifndef LKX_KIP_H
#define LKX_KIP_H

#include <stdint.h>
#include "kernel/kern86_abi.h"
#include "kernel/theme.h"

#define KIP_FD          3       /* the socket, in the app */
#define KIP_ARGS        5
#define KIP_EXIT_NOW    (-10000)    /* SYS_WAIT_EVENT after a close: the app ends (exit 0) */
#define KIP_BUF_MAX     (64u * 1024 * 1024)
#define KIP_BAD         0xffffffffu /* a buffer's length: the app can't read (or write) it */
#define KIP_BAD_PTR     0xfffff000u /* what the shell passes for one: refused by user_check() */

/* What an argument is. */
enum kip_kind {
    KIP_VAL,        /* a number */
    KIP_STR,        /* a NUL-terminated string in (NULL allowed) */
    KIP_IN,         /* bytes in: `size` of them */
    KIP_OUT,        /* bytes out */
    KIP_INOUT,      /* both */
};

/* How big a buffer is: a fixed size, times the value of another argument
 * (`count_arg`, 1-based; 0: just `size`), or w x h x 4 (wallpaper pixels). */
struct kip_arg {
    uint8_t kind;
    uint8_t count_arg, count_arg2;
    uint32_t size;
};

struct kip_call {
    struct kip_arg arg[KIP_ARGS];
};

struct kip_request {
    uint32_t nr;
    uint64_t a[KIP_ARGS];       /* 64 bits: an app's address on a 64-bit system */
    uint32_t len[KIP_ARGS];     /* each buffer's bytes (0 for values) */
};

struct kip_reply {
    int32_t ret;
    uint32_t len[KIP_ARGS];     /* each OUT buffer's bytes that follow */
    uint32_t has_fd;
};

#define V { KIP_VAL, 0, 0, 0 }
#define S { KIP_STR, 0, 0, 0 }
#define IN(n) { KIP_IN, 0, 0, (n) }
#define OUT(n) { KIP_OUT, 0, 0, (n) }
#define INOUT(n) { KIP_INOUT, 0, 0, (n) }

/* The KERN86 calls (kernel/kern86_abi.h), argument by argument. Calls the
 * app answers itself (SYS_EXIT, SYS_MEMORY) and the ones taking no pointers
 * need nothing special. */
static const struct kip_call kip_calls[] = {
    [SYS_DEBUG_WRITE]     = { { { KIP_IN, 2, 0, 1 }, V } },
    [SYS_WINDOW_OPEN]     = { { OUT(sizeof(struct k86_window)) } },
    [SYS_WINDOW_PRESENT]  = { { V, V, V, V } },
    [SYS_WINDOW_HEADER]   = { { IN(sizeof(struct k86_header)) } },
    [SYS_WAIT_EVENT]      = { { OUT(sizeof(struct k86_event)) } },
    [SYS_FONT]            = { { OUT(K86_FONT_BYTES) } },
    [SYS_FS_VOLUMES]      = { { { KIP_OUT, 2, 0, sizeof(struct k86_volume) }, V } },
    [SYS_FS_LIST]         = { { V, S, { KIP_OUT, 4, 0, sizeof(struct k86_dirent) }, V } },
    [SYS_FS_CREATE]       = { { V, S, S, V } },
    [SYS_FS_RENAME]       = { { V, S, S, S } },
    [SYS_FS_DELETE]       = { { V, S, S } },
    [SYS_THEME_GET]       = { { OUT(sizeof(struct theme)) } },
    [SYS_APPEARANCE]      = { { OUT(sizeof(struct k86_appearance)) } },
    [SYS_APPEARANCE_SET]  = { { V, V, S } },
    [SYS_WALLPAPER_THUMB] = { { S, { KIP_OUT, 3, 4, 4 }, V, V } },
    [SYS_FS_READ]         = { { V, S, S, { KIP_OUT, 5, 0, 1 }, V } },
    [SYS_SCREENSAVER_SET] = { { V, V } },
    [SYS_HW_PROBE]        = { { { KIP_OUT, 2, 0, sizeof(struct k86_probe_item) }, V, V } },
    [SYS_TIMER_SET]       = { { V } },
    [SYS_EC_READ]         = { { OUT(256) } },
    [SYS_FS_WRITE]        = { { V, S, S, { KIP_IN, 5, 0, 1 }, V } },
    [SYS_WALLPAPER_IMAGE] = { { { KIP_IN, 4, 3, 4 }, V, V, V, V } },   /* stride x height x 4 */
    [SYS_LOCK]            = { { V } },
    [SYS_LOCK_SETTINGS]   = { { INOUT(sizeof(struct k86_lock)), V } },
    [SYS_USERS]           = { { OUT(sizeof(struct k86_users)) } },
    [SYS_USER_CHANGE]     = { { IN(sizeof(struct k86_user_change)) } },
    [SYS_HOME]            = { { OUT(K86_PATH_MAX) } },
    [SYS_UPTIME_MS]       = { { V } },
    [SYS_NET_STATUS]      = { { OUT(sizeof(struct k86_net)) } },
    [SYS_NET_SCAN]        = { { { KIP_OUT, 2, 0, sizeof(struct k86_wifi) }, V } },
    [SYS_NET_JOIN]        = { { S, S } },
    [SYS_NET_FORGET]      = { { S } },
    [SYS_MACHINE]         = { { OUT(sizeof(struct k86_machine)) } },
    [SYS_BRIGHTNESS_SET]  = { { V } },
    [SYS_TIMEZONE_SET]    = { { V } },
    [SYS_TOUCHPAD_SET]    = { { V } },
    [SYS_WINDOW_BACK]     = { { V } },
    [SYS_WINDOW_KEYBOARD] = { { V } },
    [SYS_NOTIFY]          = { { S, S } },
    [SYS_TIME]            = { { OUT(sizeof(struct k86_time)) } },
    [SYS_TASKS]           = { { { KIP_OUT, 2, 0, sizeof(struct k86_task) }, V, OUT(sizeof(struct k86_meminfo)) } },
    [SYS_TASK_STOP]       = { { V } },
    [SYS_TASK_ICON]       = { { V, OUT(K86_ICON * K86_ICON * 4) } },
    [SYS_APP_OPEN]        = { { S } },
};
#define KIP_CALLS (sizeof(kip_calls) / sizeof(kip_calls[0]))

#undef V
#undef S
#undef IN
#undef OUT
#undef INOUT

/* A buffer argument's size from the request's values; 0 when the
 * multiplication would overflow or pass KIP_BUF_MAX (the shell then refuses
 * the call, as the kernel refuses a bad pointer). */
static inline uint32_t kip_size(const struct kip_arg *g, const uint64_t *a)
{
    uint64_t n = g->size;
    if (g->count_arg)
        n *= a[g->count_arg - 1];
    if (g->count_arg2)
        n *= a[g->count_arg2 - 1];
    return n > KIP_BUF_MAX ? 0 : (uint32_t)n;
}

#endif
