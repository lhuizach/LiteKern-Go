/* LiteKern X on Linux — running apps (kernel/lkx.h), phase L2. Each app is its
 * own Linux process, built from its own sources (platform/Makefile), with a
 * socket to this one on its fd 3 (platform/kip.h). The kernel's rules carry
 * over:
 *   - a call runs through kernel/sys_app.c, unchanged, with every check in it
 *   - SYS_WAIT_EVENT sleeps until the app's window has an event, or its
 *     timer is due; after K86_EVENT_CLOSE, the next one ends the app
 *   - an app that crashes is stopped, and the desktop carries on
 *   - an app that makes no call for 10 s (and isn't waiting) is stopped
 * and the log says the same things, so the tests read the same lines. */
#include "kernel/lkx.h"
#include "kernel/apps.h"
#include "kernel/desktop.h"
#include "kernel/errno.h"
#include "kernel/pmm.h"
#include "kernel/printk.h"
#include "kernel/sys_app.h"
#include "kernel/syscall.h"
#include "kernel/timing.h"
#include "kernel/user.h"
#include "kernel/wake.h"
#include "kernel/wm.h"
#include "platform/kip.h"
#include "platform/platform.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROC_MAX WM_MAX
#define WATCHDOG_MS 10000   /* an app that makes no call for this long is stopped */

struct proc {
    int used;
    const struct app *app;
    struct wm_window *win;
    pid_t pid;
    int sock;
    int waiting;            /* in SYS_WAIT_EVENT */
    int close_sent;         /* its window asked it to close: its next wait ends it */
    int canvas;             /* SYS_WINDOW_OPEN mapped its window's pixels */
    int hung;
    uint32_t last_call_ms;
    uint32_t timer_ms, timer_due;
    uint32_t free_before;
};

static struct proc procs[PROC_MAX];
static struct proc *running_proc;
static const char *app_dir = "build-linux";

/* --- the current call's buffers: what user_check() lets kernel/sys_app.c touch ---------------- */

static struct {
    uint8_t *p;
    uint32_t len;
} bufs[KIP_ARGS];
static int pending_fd = -1;         /* SYS_WINDOW_OPEN's canvas memory, for the reply */

int user_check(uint32_t ptr, uint32_t len, int write)
{
    (void)write;
    if (ptr == K86_CANVAS)
        return running_proc && running_proc->canvas ? 0 : -EFAULT;
    for (int i = 0; i < KIP_ARGS; i++)
        if (bufs[i].p && ptr >= (uint32_t)bufs[i].p && len <= bufs[i].len &&
            ptr - (uint32_t)bufs[i].p <= bufs[i].len - len)
            return 0;
    return -EFAULT;
}

int user_map_shared(uint32_t virt, uint32_t phys, uint32_t bytes)
{
    (void)bytes;
    int fd = plat_memory_fd(phys);
    if (virt != K86_CANVAS || fd < 0 || !running_proc)
        return -EINVAL;
    pending_fd = fd;
    running_proc->canvas = 1;
    return 0;
}

/* --- starting ---------------------------------------------------------------------------------- */

static int timer_due(const struct proc *p)
{
    return p->timer_ms && (int32_t)(uptime_ms() - p->timer_due) >= 0;
}

int lkx_start(const struct app *a, struct wm_window *w)
{
    /* "apps/files/files.lkx" -> build-linux/apps/files/files */
    char path[512];
    const char *dir = getenv("LKX_APPS");
    snprintf(path, sizeof(path), "%s/%s", dir ? dir : app_dir, a->lkx);
    size_t n = strlen(path);
    if (n > 4 && !strcmp(path + n - 4, ".lkx"))
        path[n - 4] = '\0';
    if (access(path, X_OK)) {
        kprintf("lkx: %s: program missing\n", a->name);
        return -ENOENT;
    }
    struct proc *p = 0;
    for (int i = 0; i < PROC_MAX && !p; i++)
        if (!procs[i].used)
            p = &procs[i];
    if (!p)
        return -ENOMEM;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv))
        return -ENOMEM;
    uint32_t free_before = pmm_free_frames();
    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]), close(sv[1]);
        return -ENOMEM;
    }
    if (!pid) {                                     /* the app */
        dup2(sv[1], KIP_FD);                        /* dup2 clears CLOEXEC on the copy */
        execl(path, path, (char *)0);
        _exit(127);
    }
    close(sv[1]);
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->app = a;
    p->win = w;
    p->pid = pid;
    p->sock = sv[0];
    p->last_call_ms = uptime_ms();
    p->free_before = free_before;
    return 0;
}

/* --- ending ------------------------------------------------------------------------------------ */

static void timers_changed(void)
{
    uint32_t fastest = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].used && procs[i].timer_ms && (!fastest || procs[i].timer_ms < fastest))
            fastest = procs[i].timer_ms;
    uint32_t hz = 0;
    if (fastest && fastest < 1000)
        for (hz = 2; hz < 64 && hz * fastest < 1000; hz *= 2)
            ;
    wake_rate(WAKE_APPS, hz);
}

/* The kernel's names for what stopped an app (kernel/lkx.c), from Linux's signals. */
static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "page fault";
    case SIGFPE:  return "divide error";
    case SIGILL:  return "invalid opcode";
    case SIGBUS:  return "general protection";
    case SIGTRAP: return "breakpoint";
    case SIGABRT: return "abort";
    default:      return "signal";
    }
}

static void ended(struct proc *p)
{
    const struct app *a = p->app;
    struct wm_window *w = p->win;
    int status = 0;
    close(p->sock);
    if (p->hung)
        kill(p->pid, SIGKILL);
    waitpid(p->pid, &status, 0);
    if (p->hung)
        kprintf("lkx: %s stopped responding and was stopped\n", a->name);
    else if (WIFSIGNALED(status))
        kprintf("lkx: %s crashed (%s) and was stopped; the system carries on\n", a->name,
                signal_name(WTERMSIG(status)));
    else if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        kprintf("lkx: %s couldn't start\n", a->name);
    else
        kprintf("lkx: %s exited (%d)\n", a->name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    kprintf("lkx: memory free %u KiB before, %u KiB after\n", p->free_before * 4, pmm_free_frames() * 4);
    p->used = 0;
    if (running_proc == p)
        running_proc = 0;
    timers_changed();
    desktop_app_ended(a, w);
}

/* --- the socket -------------------------------------------------------------------------------- */

static int recv_all(int fd, void *buf, size_t n)
{
    uint8_t *b = buf;
    while (n) {
        ssize_t k = read(fd, b, n);
        if (k <= 0)
            return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t n)
{
    const uint8_t *b = buf;
    while (n) {
        ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
        if (k <= 0)
            return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

/* The reply header (with the canvas's fd, if there is one), then the OUT buffers. */
static int reply(struct proc *p, struct kip_reply *r, int fd, const uint8_t *const *out)
{
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { r, sizeof(*r) };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    r->has_fd = fd >= 0;
    if (fd >= 0) {
        memset(control, 0, sizeof(control));
        m.msg_control = control;
        m.msg_controllen = sizeof(control);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    if (sendmsg(p->sock, &m, MSG_NOSIGNAL) != (ssize_t)sizeof(*r))
        return -1;
    for (int i = 0; i < KIP_ARGS; i++)
        if (r->len[i] && send_all(p->sock, out[i], r->len[i]))
            return -1;
    return 0;
}

/* p's next event, as SYS_WAIT_EVENT returns it: 1 if there was one. */
static int next_event(struct proc *p, struct k86_event *out)
{
    struct wm_event ev;
    if (!wm_poll_event(p->win, &ev)) {
        if (!timer_due(p))
            return 0;
        p->timer_due = uptime_ms() + p->timer_ms;
        *out = (struct k86_event){ .type = K86_EVENT_TIMER, .time_ms = uptime_ms() };
        return 1;
    }
    p->close_sent |= ev.type == WM_EVENT_CLOSE;
    *out = (struct k86_event){ .type = (int32_t)ev.type, .x = ev.x, .y = ev.y, .buttons = ev.buttons,
                               .changed = ev.changed, .id = ev.id, .time_ms = ev.time_ms, .key = ev.key };
    return 1;
}

static int reply_event(struct proc *p, int32_t ret, const struct k86_event *ev)
{
    struct kip_reply r = { ret, { ev ? (uint32_t)sizeof(*ev) : 0 }, 0 };
    const uint8_t *out[KIP_ARGS] = { (const uint8_t *)ev };
    return reply(p, &r, -1, out);
}

static void free_bufs(void)
{
    for (int i = 0; i < KIP_ARGS; i++) {
        free(bufs[i].p);
        bufs[i].p = 0;
        bufs[i].len = 0;
    }
}

/* One call from p. -1 if p has gone (or broke the protocol). */
static int handle(struct proc *p)
{
    struct kip_request req;
    if (recv_all(p->sock, &req, sizeof(req)))
        return -1;
    p->last_call_ms = uptime_ms();
    if (req.nr >= KIP_CALLS)
        return -1;
    const struct kip_call *c = &kip_calls[req.nr];
    uint32_t a[KIP_ARGS];
    memcpy(a, req.a, sizeof(a));
    for (int i = 0; i < KIP_ARGS; i++) {
        uint8_t kind = c->arg[i].kind;
        if (kind == KIP_VAL || !req.a[i])
            continue;
        if (req.len[i] == KIP_BAD) {        /* the app couldn't touch it: sys_app.c refuses it */
            a[i] = KIP_BAD_PTR;
            continue;
        }
        uint32_t want = kind == KIP_STR ? req.len[i] : kip_size(&c->arg[i], req.a);
        if (kind == KIP_STR ? (req.len[i] == 0 || req.len[i] > 4096) : req.len[i] != want) {
            free_bufs();
            return -1;
        }
        bufs[i].len = req.len[i];
        bufs[i].p = calloc(1, req.len[i] ? req.len[i] : 1);
        if (!bufs[i].p) {
            free_bufs();
            return -1;
        }
        if (kind != KIP_OUT && req.len[i] && recv_all(p->sock, bufs[i].p, req.len[i])) {
            free_bufs();
            return -1;
        }
        a[i] = (uint32_t)bufs[i].p;
    }

    running_proc = p;
    pending_fd = -1;
    int ret;
    switch (req.nr) {
    case SYS_DEBUG_WRITE:
        if (req.a[1] > DEBUG_WRITE_MAX || user_check(a[0], req.a[1], 0)) {
            ret = -EFAULT;
        } else {
            kprintf("user: ");
            kwrite((const char *)a[0], req.a[1]);
            kprintf("\n");
            ret = (int)req.a[1];
        }
        break;
    case SYS_UPTIME_MS:
        ret = (int)uptime_ms();
        break;
    case SYS_WAIT_EVENT: {
        free_bufs();
        struct k86_event ev;
        if (p->close_sent)
            return reply_event(p, KIP_EXIT_NOW, 0);     /* told to close, and back for more */
        if (wm_has_event(p->win) && next_event(p, &ev))
            return reply_event(p, 0, &ev);
        p->waiting = 1;                     /* lkx_schedule() answers when one comes */
        running_proc = 0;
        return 0;
    }
    default:
        ret = sys_app(req.nr, a[0], a[1], a[2], a[3], a[4]);
        break;
    }
    running_proc = 0;

    struct kip_reply r = { ret, { 0 }, 0 };
    const uint8_t *out[KIP_ARGS] = { 0 };
    for (int i = 0; i < KIP_ARGS; i++) {
        uint8_t kind = c->arg[i].kind;
        if ((kind == KIP_OUT || kind == KIP_INOUT) && bufs[i].p) {
            r.len[i] = bufs[i].len;
            out[i] = bufs[i].p;
        }
    }
    int err = reply(p, &r, pending_fd, out);
    pending_fd = -1;
    free_bufs();
    return err;
}

/* --- the loop's side --------------------------------------------------------------------------- */

int lkx_schedule(void)
{
    int ran = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *p = &procs[i];
        if (!p->used)
            continue;
        /* Its calls: answered one after another, up to a turn's worth, so
         * input and the desktop still get theirs. */
        for (int n = 0; n < 64 && p->used; n++) {
            struct pollfd pf = { p->sock, POLLIN, 0 };
            if (poll(&pf, 1, 0) <= 0)
                break;
            ran++;
            if (handle(p)) {
                ended(p);
                break;
            }
        }
        if (!p->used)
            continue;
        if (p->waiting && (wm_has_event(p->win) || timer_due(p))) {
            struct k86_event ev;
            if (next_event(p, &ev)) {
                p->waiting = 0;
                p->last_call_ms = uptime_ms();
                ran++;
                if (reply_event(p, 0, &ev))
                    ended(p);
            }
        } else if (!p->waiting && uptime_ms() - p->last_call_ms >= WATCHDOG_MS) {
            kprintf("user: no response for %u s; stopped\n", WATCHDOG_MS / 1000);
            p->hung = 1;
            ended(p);
        }
    }
    return ran;
}

int lkx_fds(struct pollfd *out, int max)
{
    int n = 0;
    for (int i = 0; i < PROC_MAX && n < max; i++)
        if (procs[i].used)
            out[n++] = (struct pollfd){ procs[i].sock, POLLIN, 0 };
    return n;
}

int lkx_next_ms(void)
{
    int best = -1;
    for (int i = 0; i < PROC_MAX; i++) {
        const struct proc *p = &procs[i];
        if (!p->used)
            continue;
        int ms = -1;
        if (p->waiting && p->timer_ms)
            ms = (int32_t)(p->timer_due - uptime_ms()) < 0 ? 0 : (int)(p->timer_due - uptime_ms());
        else if (!p->waiting)
            ms = 1000;                      /* the watchdog looks in once a second */
        if (ms >= 0 && (best < 0 || ms < best))
            best = ms;
    }
    return best;
}

struct wm_window *lkx_window(void)
{
    return running_proc ? running_proc->win : 0;
}

int lkx_set_timer(uint32_t ms)
{
    struct proc *p = running_proc;
    if (!p || ms > 3600000)
        return -EINVAL;
    p->timer_ms = ms;
    p->timer_due = uptime_ms() + ms;
    timers_changed();
    return 0;
}

int lkx_set_memory(uint32_t bytes)
{
    (void)bytes;
    return -ENOSYS;                         /* the app's own (platform/libk86.c) */
}

int lkx_running(const struct app *a)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].used && procs[i].app == a)
            return 1;
    return 0;
}
