/* R3lay: HTTPS through libcurl, without waiting. A request starts at once and
 * its callback runs from http_pump(), which the app calls from its timer. */
#ifndef R3LAY_HTTP_H
#define R3LAY_HTTP_H

#include <stddef.h>

/* status 0: no answer (no network, timed out). body is NUL-terminated and
 * only lasts for the call. */
typedef void (*http_done)(void *ctx, long status, char *body, size_t len);

/* 0, or -1 if libcurl can't be loaded. */
int http_init(void);
/* method: "GET", "POST", "PATCH". headers: NULL-ended, "Name: value". body may
 * be NULL. 0, or -1 if it couldn't start (the callback won't run). */
int http_request(const char *method, const char *url, const char *const *headers, const char *body,
                 http_done done, void *ctx);
/* Moves everything along, runs callbacks; how many requests are still going. */
int http_pump(void);

#endif
