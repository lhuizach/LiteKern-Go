/* LiteKern Go: following fingers on a touchscreen (platform/touch.c). */
#ifndef LKX_TOUCH_H
#define LKX_TOUCH_H

#define TOUCH_SLOTS 10

struct touch_slot {
    int id, pending_id, x, y;
    int at_x, at_y;     /* where the pointer last had it: x and y may be the next finger's */
    int fresh, gone, moved;
};

struct touch {
    struct touch_slot s[TOUCH_SLOTS];
    int slot;           /* the one the events are about */
    int active;         /* the one the pointer follows; -1 none */
};

/* down: the finger's on the glass at (x, y), in the device's units. */
typedef void (*touch_emit)(void *ctx, int x, int y, int down);

void touch_init(struct touch *t);
/* An ABS_MT_ event. */
void touch_abs(struct touch *t, int code, int value);
/* At SYN_REPORT: what those events mean for the pointer. */
void touch_sync(struct touch *t, touch_emit emit, void *ctx);

#endif
