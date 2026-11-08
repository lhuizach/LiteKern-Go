/* Camera: the rear sensor through V4L2 (cam.c) and turning its raw frames into
 * pictures (photo.c). Frames are 1280x720 packed RAW10, red first. */
#ifndef CAMERA_CAM_H
#define CAMERA_CAM_H

#include <stdint.h>

#define CAM_W 1280
#define CAM_H 720
/* The photo mode: the same window of the sensor as the live picture, with every 2 x 2 of
 * its pixels made one instead of the whole being scaled down. */
#define STILL_W 2080
#define STILL_H 1170
#define CAM_BLACK 64
#define CAM_WHITE 1023

/* Starts streaming. 0, or -1 if there's no camera. */
int cam_open(void);
void cam_close(void);
/* The newest frame not yet seen, or NULL; give it back with cam_release(). */
const uint8_t *cam_next(int *slot);
void cam_release(int slot);
int cam_stride(void);
/* A photo-sized frame (see cam.c); free() it. w, h and row_bytes say its size; settled() is called
 * once the photo mode is streaming, to put the lens back. NULL if it didn't work. */
uint8_t *cam_still(int *w, int *h, int *row_bytes, void (*settled)(void));
/* One step of auto-exposure from this frame. */
void cam_expose(const uint8_t *frame);

/* The white balance, 8.8 fixed point, from a frame. */
void photo_balance(const uint8_t *frame, int *red, int *blue);
/* A picture from a frame of w x h pixels and rows `stride` bytes apart, upright, as a PNG in
 * Pictures/Camera. Its name goes in name (64 bytes). 0, or -1. */
int photo_save(const uint8_t *frame, int w, int h, int stride, int red, int blue, char *name);
/* The last photo's little picture (THUMB x THUMB, 0x00RRGGBB), kept next to the
 * photos. 1 if there is one. */
#define THUMB 104
int photo_thumb(uint32_t *out);

/* The lens (focus.c): focus_open() is -1 if there's no motor. focus_at() starts a
 * sweep, sharpening around that point of the sensor's picture; focus_frame() each
 * frame moves it along; focus_active() while one runs. */
int focus_open(void);
void focus_close(void);
void focus_at(int x, int y);
int focus_active(void);
void focus_restore(void);          /* the lens back where it was, after the stream restarted */
void focus_frame(const uint8_t *frame);

/* The preview: frame rotated upright, 2 screen pixels to a Bayer cell, filling
 * out (0x00RRGGBB, w x h, rows `stride` pixels apart) from the middle. */
void photo_preview(const uint8_t *frame, int red, int blue, uint32_t *out, int w, int h, int stride);

#endif
