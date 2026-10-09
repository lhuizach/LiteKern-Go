/* LiteKern X — Files: pictures (PNG, JPEG, BMP) decoded in the app.
 *
 * A damaged or hostile file can only make a decode fail (or, at worst, end
 * Files): every read is bounds checked, and the kernel never sees the
 * file's contents. Apps have no floating point, so the maths is integer. */
#ifndef FILES_IMAGE_H
#define FILES_IMAGE_H

#include <stdint.h>

struct image {
    uint32_t *px;               /* 0xAARRGGBB, straight alpha, w a row */
    int w, h;
    const char *format;         /* "PNG", "JPEG", "BMP" */
};

/* Where decoding gets memory: n bytes, zeroed, 16-byte aligned; 0 if none. */
typedef void *(*image_alloc_fn)(uint32_t n);

#define IMAGE_MAX_PIXELS (24u * 1024 * 1024)    /* 24 megapixels */

/* Called now and then while decoding (each row or so): a big picture takes
 * seconds on the EeePC, and an app must make a system call at least every
 * 10 s or the kernel stops it. May be NULL. */
extern void (*image_busy)(void);

/* Is this a picture Files can show (by its first bytes)? */
int image_recognise(const uint8_t *data, uint32_t len);

/* Decode a whole file. 0, or -1 with *why saying what's wrong, for the user. */
int image_decode(const uint8_t *data, uint32_t len, image_alloc_fn alloc, struct image *out,
                 const char **why);

#endif
