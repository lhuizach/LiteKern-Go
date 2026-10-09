/* LiteKern X — the desktop wallpaper (Phase 2, extended in Phase 3 §5).
 *
 * Wallpapers are in the ramdisk as wallpapers/<name>.lkxw (packed at build
 * time by tools/wallpaper-pack.py from assets/wallpapers/<name>.png), with
 * an optional <name>-light used by the light style. The one shown is
 * unpacked into a buffer the size of the screen: drawn 1:1 and centred, any
 * space around it (the VMs' 1024x768) filled with its edge colours
 * (docs/ASSET-PROMPTS.md §8). Without one, the theme's plain desktop colour
 * is used. */
#ifndef LKX_WALLPAPER_H
#define LKX_WALLPAPER_H

#include <stdint.h>
#include "kernel/gfx.h"

/* Make the buffer and show `name` (needs the screen and the ramdisk).
 * Logs what it did; 0, or a negative error (the plain colour is used). */
int wallpaper_init(const char *name);

/* Show another one (0, -ENOENT, -EINVAL if damaged); the old one stays on
 * failure. wallpaper_refresh() reloads the current one, e.g. after the
 * style changed (for its -light variant). */
int wallpaper_set(const char *name);
int wallpaper_refresh(void);

/* Show a picture an app gave (0x00RRGGBB, w x h, up to 4096 x 4096), scaled
 * to fill the screen with its middle kept. It's then the wallpaper
 * WALLPAPER_CUSTOM until another is chosen. 0, -EINVAL or -ENODEV. */
#define WALLPAPER_CUSTOM "Custom"
int wallpaper_set_image(const uint32_t *px, int w, int h, int stride);

/* The custom wallpaper as an .lkxw file (filtered and deflated, as the
 * build packs the others): `len` bytes at physical `phys`, in a block of
 * `bytes` to give back with wallpaper_pack_free(). 0, -ENOENT or -ENOMEM. */
int wallpaper_pack(uint32_t *phys, uint32_t *bytes, uint32_t *len);
void wallpaper_pack_free(uint32_t phys, uint32_t bytes);

/* Show such a file as the custom wallpaper (scaled if the screen is
 * another size). 0, or -EINVAL if it's damaged. */
int wallpaper_set_packed(const void *data, uint32_t size);

/* For the lock screen (kernel/lockscreen.h), which has a picture of its own:
 *   render         a preset (or "" for the plain colour) drawn into dst
 *   render_packed  an .lkxw file, scaled to fill dst
 *   pack_surface   any screen-sized picture packed as an .lkxw (as wallpaper_pack)
 *   scale_cover    a picture scaled to fill dst, the middle kept */
int wallpaper_render(const char *name, struct gfx_surface *dst);
int wallpaper_render_packed(const void *data, uint32_t size, struct gfx_surface *dst);
int wallpaper_pack_surface(const struct gfx_surface *src, uint32_t *phys, uint32_t *bytes, uint32_t *len);
void wallpaper_scale_cover(const uint32_t *px, int w, int h, int stride, struct gfx_surface *dst);
const char *wallpaper_current(void);       /* "" for the plain colour */

/* The wallpapers there are (light variants not listed separately). */
int wallpaper_list(char names[][32], int max);

/* A w x h preview of one, 0x00RRGGBB, for the Settings app. */
int wallpaper_thumb(const char *name, uint32_t *out, int w, int h);

/* Draw the background into dst, which shows the screen from (ox, oy),
 * darkened by `dim` (0..255, 0 = not at all). */
void wallpaper_draw(struct gfx_surface *dst, int ox, int oy, uint32_t dim);

/* The background colour at the screen's bottom-right corner (for tests). */
uint32_t wallpaper_corner(void);

#endif
