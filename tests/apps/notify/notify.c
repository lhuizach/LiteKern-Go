/* LiteKern Go — a test app for notifications (SYS_NOTIFY): it posts two,
 * then waits like any app until it's closed. */
#include "sdk/kern86.h"

int main(void)
{
    struct k86_window w;
    k86_window_open(&w);
    k86_notify("Hello", "A first notification");
    k86_notify("Again", "And a second one");
    k86_log("notify: posted 2");
    for (;;) {
        struct k86_event ev;
        if (k86_wait_event(&ev) || ev.type == K86_EVENT_CLOSE)
            return 0;
    }
}
