/* LiteKern X — the Web app's downloads (LiteKernX-Roadmap/06, L6's last
 * piece). NetSurf's framebuffer front end has no download window, so this is
 * its download table: the file is gathered in memory as it arrives, then
 * saved in one go in the user's Downloads folder on the stick (the "lkx"
 * surface's lkx_save_download(): SYS_HOME, SYS_FS_CREATE, SYS_FS_WRITE). The
 * status bar says how it's going.
 *
 * linux/package/netsurf-lkx copies this into frontends/framebuffer/ and adds
 * it to the front end's table (gui.c). At most 32 MiB (SYS_FS_WRITE's limit):
 * a bigger file is refused at the start, or when it passes that. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libnsfb.h>
#include <libnsfb_event.h>

#include "utils/errors.h"
#include "desktop/download.h"
#include "netsurf/download.h"
#include "framebuffer/gui.h"
#include "framebuffer/fbtk.h"

#define DOWNLOAD_MAX (32u * 1024 * 1024)

/* In the "lkx" surface (platform/nsfb_lkx.c, linked in with libnsfb). */
int lkx_save_download(const char *name, const void *data, unsigned len, char *saved, int max);

struct gui_download_window {
    struct download_context *ctx;
    struct gui_window *gw;
    char name[64];
    char *data;
    unsigned len, size;
    unsigned long long total;
    unsigned shown_kb;
    int failed;
};

static void status(struct gui_download_window *dw, const char *text)
{
    if (dw->gw && dw->gw->status)
        fbtk_set_text(dw->gw->status, text);
}

static struct gui_download_window *lkx_download_create(struct download_context *ctx, struct gui_window *parent)
{
    struct gui_download_window *dw = calloc(1, sizeof(*dw));
    if (!dw)
        return NULL;
    dw->ctx = ctx;
    dw->gw = parent;
    dw->total = download_context_get_total_length(ctx);
    const char *n = download_context_get_filename(ctx);
    snprintf(dw->name, sizeof(dw->name), "%s", n && *n ? n : "download");
    for (char *p = dw->name; *p; p++)               /* what FAT32 names can't hold */
        if (strchr("/\\:*?\"<>|", *p) || (unsigned char)*p < 32)
            *p = '_';
    char text[128];
    if (dw->total > DOWNLOAD_MAX) {
        snprintf(text, sizeof(text), "%s is too big to download here (over 32 MB)", dw->name);
        status(dw, text);
        dw->failed = 1;
        return dw;
    }
    snprintf(text, sizeof(text), "Downloading %s...", dw->name);
    status(dw, text);
    return dw;
}

static nserror lkx_download_data(struct gui_download_window *dw, const char *data, unsigned int size)
{
    if (dw->failed)
        return NSERROR_SAVE_FAILED;
    if (dw->len + size > DOWNLOAD_MAX) {
        char text[128];
        snprintf(text, sizeof(text), "%s is too big to download here (over 32 MB)", dw->name);
        status(dw, text);
        dw->failed = 1;
        return NSERROR_SAVE_FAILED;
    }
    if (dw->len + size > dw->size) {
        unsigned grow = dw->size ? dw->size * 2 : 256 * 1024;
        while (grow < dw->len + size)
            grow *= 2;
        char *more = realloc(dw->data, grow);
        if (!more) {
            dw->failed = 1;
            return NSERROR_NOMEM;
        }
        dw->data = more;
        dw->size = grow;
    }
    memcpy(dw->data + dw->len, data, size);
    dw->len += size;
    if (dw->len / 1024 >= dw->shown_kb + 256) {     /* now and then: how far */
        char text[128];
        dw->shown_kb = dw->len / 1024;
        if (dw->total)
            snprintf(text, sizeof(text), "Downloading %s: %u%%", dw->name,
                     (unsigned)((unsigned long long)dw->len * 100 / dw->total));
        else
            snprintf(text, sizeof(text), "Downloading %s: %u KB", dw->name, dw->shown_kb);
        status(dw, text);
    }
    return NSERROR_OK;
}

static void lkx_download_error(struct gui_download_window *dw, const char *error_msg)
{
    char text[160];
    snprintf(text, sizeof(text), "%s didn't download: %s", dw->name, error_msg ? error_msg : "an error");
    status(dw, text);
    download_context_destroy(dw->ctx);      /* the download's over: ours to end */
    free(dw->data);
    free(dw);
}

static void lkx_download_done(struct gui_download_window *dw)
{
    if (!dw->failed) {
        char saved[300], text[400];
        status(dw, "Saving to the USB stick...");
        int err = lkx_save_download(dw->name, dw->data ? dw->data : "", dw->len, saved, sizeof(saved));
        if (err)
            snprintf(text, sizeof(text), "%s couldn't be saved (%d)", dw->name, err);
        else
            snprintf(text, sizeof(text), "Saved %s (%u KB)", saved, (dw->len + 1023) / 1024);
        status(dw, text);
    }
    download_context_destroy(dw->ctx);
    free(dw->data);
    free(dw);
}

struct gui_download_table lkx_download_table = {
    .create = lkx_download_create,
    .data = lkx_download_data,
    .error = lkx_download_error,
    .done = lkx_download_done,
};
