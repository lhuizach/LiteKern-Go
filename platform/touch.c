/* LiteKern Go: fingers on a touchscreen, as one pointer.
 *
 * Linux's single-touch view holds the "button" down until the last finger lifts, so
 * fast typing, where the next finger lands before the last one has left, came through
 * as one long press and lost keys. So fingers are followed here, slot by slot (the
 * kernel's multi-touch protocol B): a new finger lifts the pointer from the old one
 * and puts it down at the new place. Its old finger is then ignored until it lifts.
 * No hardware or system calls in here, so tests/linux/test-touch.c can drive it. */
#include "platform/touch.h"

#include <linux/input-event-codes.h>
#include <string.h>

void touch_init(struct touch *t)
{
    memset(t, 0, sizeof(*t));
    t->active = -1;
    for (int i = 0; i < TOUCH_SLOTS; i++)
        t->s[i].id = -1;
}

void touch_abs(struct touch *t, int code, int value)
{
    struct touch_slot *s;
    if (code == ABS_MT_SLOT) {
        t->slot = value >= 0 && value < TOUCH_SLOTS ? value : 0;
        return;
    }
    s = &t->s[t->slot];
    switch (code) {
    case ABS_MT_TRACKING_ID:
        if (value < 0) {
            s->gone = 1;
        } else {
            s->fresh = 1;
            s->pending_id = value;
        }
        break;
    case ABS_MT_POSITION_X:
        s->x = value;
        s->moved = 1;
        break;
    case ABS_MT_POSITION_Y:
        s->y = value;
        s->moved = 1;
        break;
    default:
        break;
    }
}

void touch_sync(struct touch *t, touch_emit emit, void *ctx)
{
    /* Lifted first: a slot can lift and land again within one report. */
    for (int i = 0; i < TOUCH_SLOTS; i++) {
        struct touch_slot *s = &t->s[i];
        if (!s->gone)
            continue;
        if (t->active == i) {
            emit(ctx, s->at_x, s->at_y, 0);
            t->active = -1;
        }
        s->gone = 0;
        s->id = -1;
    }
    for (int i = 0; i < TOUCH_SLOTS; i++) {
        struct touch_slot *s = &t->s[i];
        if (!s->fresh)
            continue;
        if (t->active >= 0)                     /* the old key is done with */
            emit(ctx, t->s[t->active].at_x, t->s[t->active].at_y, 0);
        s->fresh = 0;
        s->moved = 0;
        s->id = s->pending_id;
        t->active = i;
        s->at_x = s->x;
        s->at_y = s->y;
        emit(ctx, s->x, s->y, 1);
    }
    if (t->active >= 0 && t->s[t->active].moved) {
        struct touch_slot *s = &t->s[t->active];
        s->moved = 0;
        s->at_x = s->x;
        s->at_y = s->y;
        emit(ctx, s->x, s->y, 1);
    }
}
