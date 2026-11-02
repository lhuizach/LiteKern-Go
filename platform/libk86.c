/* LiteKern X on Linux — an app's side of KERN86 (platform/kip.h). The apps are
 * built from their own sources, unchanged; sdk/kern86.h sends every call
 * here (LKX_LINUX), which passes it to lkx-shell over the socket it left on
 * fd 3, with the bytes behind its pointers.
 *
 * Two calls are answered here: SYS_EXIT (Linux's exit) and SYS_MEMORY (the
 * memory at K86_HEAP, which is this process's own). The canvas
 * (SYS_WINDOW_OPEN) is the window's pixels, shared: the shell sends its
 * memory and it's mapped here. */
#define _GNU_SOURCE                 /* MAP_FIXED_NOREPLACE */
#include "platform/kip.h"
#include "sdk/kern86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static void broken(void)
{
    /* The shell is gone (or sent nonsense): nothing to draw on, nobody to
     * ask. Leave, as the kernel would end us. */
    _exit(1);
}

static void send_all(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t k = write(KIP_FD, b, n);
        if (k <= 0)
            broken();
        b += k;
        n -= (size_t)k;
    }
}

static void recv_all(void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t k = read(KIP_FD, b, n);
        if (k <= 0)
            broken();
        b += k;
        n -= (size_t)k;
    }
}

/* The reply header, and the fd that may come with it. */
static int recv_reply(struct kip_reply *r)
{
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { r, sizeof(*r) };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                        .msg_controllen = sizeof(control) };
    ssize_t k = recvmsg(KIP_FD, &m, 0);
    if (k <= 0)
        broken();
    int fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(c), sizeof(fd));
    if ((size_t)k < sizeof(*r))
        recv_all((uint8_t *)r + k, sizeof(*r) - (size_t)k);
    return fd;
}

/* --- SYS_MEMORY: this process's own pages at K86_HEAP -------------------------------------- */

static uint32_t heap_bytes;
static int heap_reserved;

static int memory(uint32_t bytes)
{
    if (bytes > K86_HEAP_MAX)
        return -22;                         /* -EINVAL */
    bytes = (bytes + 4095) & ~4095u;
    if (!heap_reserved) {
        void *p = mmap((void *)K86_HEAP, K86_HEAP_MAX, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != (void *)K86_HEAP)
            return -12;                     /* -ENOMEM: something else is there */
        heap_reserved = 1;
    }
    if (bytes < heap_bytes) {               /* shrink: those pages go (and come back zeroed) */
        madvise((void *)((uintptr_t)K86_HEAP + bytes), heap_bytes - bytes, MADV_DONTNEED);
        mprotect((void *)((uintptr_t)K86_HEAP + bytes), heap_bytes - bytes, PROT_NONE);
    } else if (bytes > heap_bytes &&
               mprotect((void *)((uintptr_t)K86_HEAP + heap_bytes), bytes - heap_bytes, PROT_READ | PROT_WRITE)) {
        return -12;
    }
    heap_bytes = bytes;
    return 0;
}

/* --- the app's pointers, checked without touching them ------------------------------------- *
 * An app may pass any pointer (tests/apps/badcalls does, on purpose). The
 * kernel answers a bad one with -EFAULT, at the point its own checks reach
 * it; reading it here would crash the app instead. So each buffer is first
 * tried with process_vm_readv/writev on ourselves, which report a bad
 * address rather than fault; a bad one is sent as KIP_BAD and the shell
 * hands kernel/sys_app.c a pointer its checks refuse, in their own order. */

static size_t safe_read(void *dst, uintptr_t src, size_t n)
{
    struct iovec local = { dst, n }, remote = { (void *)src, n };
    ssize_t k = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return k < 0 ? 0 : (size_t)k;
}

/* Can all n bytes at p be read (and, if `write`, written)? */
static int usable(uintptr_t p, uint32_t n, int write)
{
    uint8_t chunk[4096];
    for (uint32_t done = 0; done < n;) {
        uint32_t k = n - done < sizeof(chunk) ? n - done : (uint32_t)sizeof(chunk);
        if (safe_read(chunk, p + done, k) != k)
            return 0;
        if (write) {                        /* the same bytes back: proves it's writable */
            struct iovec local = { chunk, k }, remote = { (void *)(p + done), k };
            if (process_vm_writev(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)k)
                return 0;
        }
        done += k;
    }
    return 1;
}

/* A string's length with its NUL (at most 4096); 0 if it can't be read. */
static uint32_t string_len(uintptr_t p)
{
    char chunk[64];
    for (uint32_t done = 0; done < 4096; done += sizeof(chunk)) {
        size_t k = safe_read(chunk, p + done, sizeof(chunk));
        if (!k && sizeof(chunk) > 1) {      /* the last page may end mid-chunk: byte by byte */
            k = 0;
            while (k < sizeof(chunk) && safe_read(chunk + k, p + done + k, 1) == 1)
                k++;
        }
        for (size_t i = 0; i < k; i++)
            if (!chunk[i])
                return done + (uint32_t)i + 1;
        if (k < sizeof(chunk))
            return 0;                       /* unreadable before its NUL: the kernel says -EFAULT */
    }
    return 4096;                            /* not terminated in time: the shell says -EINVAL */
}

/* --- every other call goes to the shell ---------------------------------------------------- */

int k86_linux_call(uint32_t nr, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    uint64_t a[KIP_ARGS] = { a1, a2, a3, a4, a5 };
    if (nr == SYS_EXIT) {
        fflush(0);
        exit((int)a1);
    }
    if (nr == SYS_MEMORY)
        return memory((uint32_t)a1);
    if (nr >= KIP_CALLS)
        return -38;                         /* -ENOSYS */

    const struct kip_call *c = &kip_calls[nr];
    struct kip_request req = { nr, { a1, a2, a3, a4, a5 }, { 0 } };
    for (int i = 0; i < KIP_ARGS; i++) {
        const struct kip_arg *g = &c->arg[i];
        if (g->kind == KIP_VAL || !a[i])
            continue;                       /* a value, or a NULL pointer: sent as is */
        if (g->kind == KIP_STR) {
            req.len[i] = string_len(a[i]);
            if (!req.len[i])
                req.len[i] = KIP_BAD;
            continue;
        }
        req.len[i] = kip_size(g, a);
        if (!usable(a[i], req.len[i], g->kind != KIP_IN))
            req.len[i] = KIP_BAD;
    }
    send_all(&req, sizeof(req));
    for (int i = 0; i < KIP_ARGS; i++) {
        uint8_t k = c->arg[i].kind;
        if (req.len[i] && req.len[i] != KIP_BAD && (k == KIP_STR || k == KIP_IN || k == KIP_INOUT))
            send_all((const void *)(uintptr_t)a[i], req.len[i]);
    }

    struct kip_reply r;
    int fd = recv_reply(&r);
    for (int i = 0; i < KIP_ARGS; i++) {
        if (!r.len[i])
            continue;
        if (r.len[i] > req.len[i] || !a[i])
            broken();
        recv_all((void *)(uintptr_t)a[i], r.len[i]);
    }
    if (r.ret == KIP_EXIT_NOW) {            /* told to close, and back for more: done */
        fflush(0);
        exit(0);
    }
    if (nr == SYS_WINDOW_OPEN && r.ret == 0) {
        /* The canvas: the window's own pixels, shared with the shell. */
        struct k86_window *w = (struct k86_window *)a1;
        size_t bytes = (size_t)w->stride * (size_t)(w->h > 0 ? w->h : 1) * 4;
        void *px = MAP_FAILED;
        if (fd >= 0) {
            off_t size = lseek(fd, 0, SEEK_END);
            if (size > 0 && (size_t)size > bytes)
                bytes = (size_t)size;
            px = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
        }
        if (px == MAP_FAILED)
            return -12;
        w->canvas = px;
    } else if (fd >= 0) {
        close(fd);
    }
    return r.ret;
}
