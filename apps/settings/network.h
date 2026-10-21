/* LiteKern X — Settings: the Network page (network.c). */
#ifndef SETTINGS_NETWORK_H
#define SETTINGS_NETWORK_H

#include "kernel/gfx.h"
#include "sdk/kern86.h"

int network_page_available(void);                   /* the machine has Ethernet or Wi-Fi */
void network_page_open(struct gfx_surface *canvas);  /* read the state, lay out, draw */
void network_page_draw(void);
void network_page_layout(void);                     /* the window was resized */
void network_page_tick(void);                       /* every 2 s while it shows: refresh */
void network_page_pointer(const struct k86_event *ev);
void network_page_scroll(const struct k86_event *ev);
void network_page_key(const struct key_event *k);

#endif
