/* platform/touch.c by itself: bash tests/linux/test-touch.sh */
#include "platform/touch.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

static char got[256];

static void emit(void *ctx, int x, int y, int down)
{
    (void)ctx;
    char one[32];
    snprintf(one, sizeof(one), "%s%d,%d ", down ? "D" : "U", x, y);
    strcat(got, one);
}

static struct touch t;

static void land(int slot, int id, int x, int y)
{
    touch_abs(&t, ABS_MT_SLOT, slot);
    touch_abs(&t, ABS_MT_TRACKING_ID, id);
    touch_abs(&t, ABS_MT_POSITION_X, x);
    touch_abs(&t, ABS_MT_POSITION_Y, y);
}

static void lift(int slot)
{
    touch_abs(&t, ABS_MT_SLOT, slot);
    touch_abs(&t, ABS_MT_TRACKING_ID, -1);
}

static void move(int slot, int x, int y)
{
    touch_abs(&t, ABS_MT_SLOT, slot);
    touch_abs(&t, ABS_MT_POSITION_X, x);
    touch_abs(&t, ABS_MT_POSITION_Y, y);
}

static int check(const char *name, const char *want)
{
    touch_sync(&t, emit, 0);
    int ok = !strcmp(got, want);
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : ": got ", ok ? "" : got);
    got[0] = 0;
    return !ok;
}

int main(void)
{
    int bad = 0;
    touch_init(&t);
    land(0, 1, 10, 20);
    bad += check("a finger lands", "D10,20 ");
    move(0, 11, 21);
    bad += check("and moves", "D11,21 ");
    lift(0);
    bad += check("and lifts", "U11,21 ");

    land(0, 2, 10, 10);
    touch_sync(&t, emit, 0);
    got[0] = 0;
    land(1, 3, 50, 50);
    bad += check("a second finger before the first lifts: the first key ends", "U10,10 D50,50 ");
    lift(0);
    bad += check("the old finger lifting does nothing", "");
    move(1, 51, 51);
    bad += check("the new one still moves", "D51,51 ");
    lift(1);
    bad += check("the last one lifting ends it", "U51,51 ");

    land(0, 4, 1, 1);
    touch_sync(&t, emit, 0);
    got[0] = 0;
    lift(0);
    land(0, 5, 2, 2);
    bad += check("a slot lifts and lands in one report", "U1,1 D2,2 ");
    lift(0);
    touch_sync(&t, emit, 0);
    got[0] = 0;

    land(3, 6, 7, 7);
    land(4, 7, 8, 8);
    bad += check("two land in one report", "D7,7 U7,7 D8,8 ");

    /* where the fingers are, for a pinch: the pointer's and another */
    lift(3);
    lift(4);
    touch_sync(&t, emit, 0);
    got[0] = 0;
    {
        int x1, y1, x2, y2, n;
        n = touch_fingers(&t, &x1, &y1, &x2, &y2);
        printf("%s  no fingers: none counted\n", n == 0 ? "PASS" : "FAIL");
        bad += n != 0;
        land(0, 8, 100, 200);
        touch_sync(&t, emit, 0);
        land(1, 9, 300, 400);
        touch_sync(&t, emit, 0);
        got[0] = 0;
        move(0, 110, 210);
        touch_sync(&t, emit, 0);
        n = touch_fingers(&t, &x1, &y1, &x2, &y2);
        int ok = n == 2 && x1 == 300 && y1 == 400 && x2 == 110 && y2 == 210;
        printf("%s  two fingers: the pointer's (newest) and the other, which still moves (n %d, %d,%d and %d,%d)\n",
               ok ? "PASS" : "FAIL", n, x1, y1, x2, y2);
        bad += !ok;
        lift(1);
        touch_sync(&t, emit, 0);
        n = touch_fingers(&t, &x1, &y1, &x2, &y2);
        printf("%s  one lifts: one finger left\n", n == 1 ? "PASS" : "FAIL");
        bad += n != 1;
    }
    return bad;
}
