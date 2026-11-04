/* LiteKern X — KERN86 apps (Phase 2 §5): ported from v1's kern86/loader.c.
 *
 * An app is a folder in the ramdisk, apps/<name>/, with
 *   kerns.json   {"name": "Files", "icon": "files", "entry": "files.lkx"}
 *   <entry>      the program: a struct lkx_header (kernel/kern86_abi.h),
 *                then its image, loaded at K86_APP_BASE
 * and it runs in ring 3, talking to the kernel through int 0x80 (the SDK's
 * sdk/kern86.h). v1 read "KERNS.JSN" and "APP.BIN" from the FAT disk and
 * called into the kernel directly; here the ramdisk holds them and a crash
 * ends only the app. */
#ifndef LKX_LKX_H
#define LKX_LKX_H

#include "kernel/apps.h"
#include "kernel/idt.h"
#include "kernel/kern86_abi.h"
#include "kernel/wm.h"

/* Add every app found in the ramdisk to the app list (kernel/apps.h). */
void lkx_register_all(void);
/* An app from a kerns.json found elsewhere than the ramdisk (`manifest_name`:
 * "apps/<name>/kerns.json", its program beside it): the Linux build's own
 * apps, offered by plat_more_apps() (platform/lkx.c). */
void appreg_add(const char *manifest_name, const char *json);
void plat_more_apps(void);

/* Start the app in its window (already open). It runs whenever it has
 * something to do (lkx_schedule); when it ends, how is logged and the shell
 * is told (desktop_app_ended). 0 or a negative errno. */
int lkx_start(const struct app *a, struct wm_window *w);

/* Give every app that has something to do a turn: one just started, or one
 * waiting in SYS_WAIT_EVENT whose window has an event. Each runs until it
 * waits again (or ends). Returns how many ran. */
int lkx_schedule(void);

/* The running app's window (inside its system calls), or NULL. */
struct wm_window *lkx_window(void);
/* SYS_WAIT_EVENT: the next event now, or sleep until there is one. */
void lkx_wait_event(struct int_frame *f);
/* SYS_TIMER_SET for the running app: a K86_EVENT_TIMER every `ms` (0: off). */
int lkx_set_timer(uint32_t ms);
/* SYS_MEMORY for the running app: its memory at K86_HEAP becomes `bytes`. */
int lkx_set_memory(uint32_t bytes);
/* Is the app running? */
int lkx_running(const struct app *a);

/* LiteKern Go's Task Manager (SYS_TASKS, SYS_TASK_STOP; kern86_abi.h).
 * The kernel version has no memory of its own to report: -ENOSYS. */
int lkx_tasks(struct k86_task *out, int max, struct k86_meminfo *mem);
int lkx_task_stop(int32_t id);
int lkx_task_icon(int32_t id, uint32_t *out);  /* K86_ICON x K86_ICON pixels */

#endif
