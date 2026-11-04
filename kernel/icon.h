/* LiteKern X — app icons (Phase 2, the desktop).
 *
 * 112x112 PNGs (LiteKern Go: the home screen's size; made from the 1024 px
 * ones in assets/icons/source/) + assets/icons.json, turned into C at build
 * time by tools/icons2c.py (rules: docs/ASSET-PROMPTS.md §2). */
#ifndef LKX_ICON_H
#define LKX_ICON_H

#include <stdint.h>

struct icon {
    const char *name;
    int w, h;
    const uint32_t *px;     /* w*h pixels, 0xAARRGGBB */
};

/* Generated from assets/icons.json. */
extern const struct icon icons[];
extern const int icon_count;

/* NULL if there is no icon of that name. */
const struct icon *icon_find(const char *name);

#endif
