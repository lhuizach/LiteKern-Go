/* Music's Online tab: songs from Audius (audius.co), a free streaming service with no account and no key.
 * This file has the tab's screen (a search box, Trending, Genres and a list of results) and the helper
 * programs that talk to the internet (the app run as `music --api URL FILE` or `music --dl URL FILE`, with
 * no window), so a slow network never holds the screen. The playing is music.c's. */
#ifndef LKX_MUSIC_ONLINE_H
#define LKX_MUSIC_ONLINE_H

#include <stdint.h>
#include <sys/types.h>

#include "kernel/widget.h"

struct online_pick {
    char id[16];
    char title[64];
    char artist[40];
    int dur;                            /* seconds */
};

/* `music --api ...` and `music --dl ...`: run it from main first. -1: not one of those. */
int online_helper_main(int argc, char **argv);

void online_init(struct gfx_surface *canvas);
/* The tab fills the canvas from y = top down to y = bottom. */
void online_layout(int top, int bottom);
void online_draw(void);
/* A pointer event: 1 if the tab took it (draw again). */
int online_pointer(const struct wg_pointer *p);
int online_key(const struct key_event *k);
int online_scroll(int x, int y, int notches);
/* Every timer tick: 1 if something changed (draw again). online_busy() says whether to tick quickly. */
int online_tick(void);
int online_busy(void);
int online_wants_keyboard(void);
/* A song the finger picked since last asked: 1 and filled. */
int online_take_pick(struct online_pick *out);
/* The song before / after the one playing (by its id) in the results; 0 at the ends. */
int online_neighbour(const char *id, int dir, struct online_pick *out);

/* Starts the helper that downloads song `id` into `file`; its pid, or -1. */
pid_t online_download(const char *id, const char *file);

#endif
