/* R3lay: libcurl's multi interface, so a slow network never holds up the app.
 *
 * libcurl is loaded with dlopen: it's in every image (NetSurf needs it), but
 * the PC the tests are built on may have no headers for it. The few names and
 * numbers needed are below, as curl.h has them; they're fixed by its ABI. */
#include "http.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#define OPT_WRITEDATA      10001L
#define OPT_URL            10002L
#define OPT_POSTFIELDS     10015L
#define OPT_USERAGENT      10018L
#define OPT_HTTPHEADER     10023L
#define OPT_CUSTOMREQUEST  10036L
#define OPT_CAPATH         10097L
#define OPT_PRIVATE        10103L
#define OPT_WRITEFUNCTION  20011L
#define OPT_TIMEOUT        13L
#define OPT_CONNECTTIMEOUT 78L
#define OPT_NOSIGNAL       99L
#define INFO_RESPONSE_CODE 0x200002
#define INFO_PRIVATE       0x100015
#define MSG_DONE           1
#define GLOBAL_ALL         3L

struct curl_msg {
    int msg;
    void *easy;
    union {
        void *whatever;
        int result;
    } data;
};

static void *(*easy_init)(void);
static int (*easy_setopt)(void *, long, ...);
static int (*easy_getinfo)(void *, int, ...);
static void (*easy_cleanup)(void *);
static void *(*slist_append)(void *, const char *);
static void (*slist_free_all)(void *);
static void *(*multi_init)(void);
static int (*multi_add)(void *, void *);
static int (*multi_remove)(void *, void *);
static int (*multi_perform)(void *, int *);
static struct curl_msg *(*multi_info_read)(void *, int *);

static void *multi;

struct req {
    void *easy, *headers;
    char *body, *reply;
    size_t got, room;
    http_done done;
    void *ctx;
};

int http_init(void)
{
    static const char *const names[] = { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so" };
    void *lib = 0;
    int (*global_init)(long);
    for (unsigned i = 0; !lib && i < sizeof(names) / sizeof(names[0]); i++)
        lib = dlopen(names[i], RTLD_NOW);
    if (!lib)
        return -1;
#define LOAD(var, name) if (!(*(void **)&var = dlsym(lib, name))) return -1
    LOAD(global_init, "curl_global_init");
    LOAD(easy_init, "curl_easy_init");
    LOAD(easy_setopt, "curl_easy_setopt");
    LOAD(easy_getinfo, "curl_easy_getinfo");
    LOAD(easy_cleanup, "curl_easy_cleanup");
    LOAD(slist_append, "curl_slist_append");
    LOAD(slist_free_all, "curl_slist_free_all");
    LOAD(multi_init, "curl_multi_init");
    LOAD(multi_add, "curl_multi_add_handle");
    LOAD(multi_remove, "curl_multi_remove_handle");
    LOAD(multi_perform, "curl_multi_perform");
    LOAD(multi_info_read, "curl_multi_info_read");
#undef LOAD
    global_init(GLOBAL_ALL);
    multi = multi_init();
    return multi ? 0 : -1;
}

static size_t on_data(char *p, size_t size, size_t count, void *user)
{
    struct req *r = user;
    size_t n = size * count;
    if (r->got + n + 1 > r->room) {
        size_t room = r->room ? r->room * 2 : 16384;
        while (room < r->got + n + 1)
            room *= 2;
        if (room > 32u * 1024 * 1024)           /* far more than any reply here */
            return 0;
        char *bigger = realloc(r->reply, room);
        if (!bigger)
            return 0;
        r->reply = bigger;
        r->room = room;
    }
    memcpy(r->reply + r->got, p, n);
    r->got += n;
    r->reply[r->got] = 0;
    return n;
}

static void finish(struct req *r)
{
    multi_remove(multi, r->easy);
    easy_cleanup(r->easy);
    slist_free_all(r->headers);
    free(r->body);
    free(r->reply);
    free(r);
}

int http_request(const char *method, const char *url, const char *const *headers, const char *body,
                 http_done done, void *ctx)
{
    struct req *r = calloc(1, sizeof(*r));
    if (!r || !multi)
        goto no;
    r->easy = easy_init();
    if (!r->easy)
        goto no;
    r->done = done;
    r->ctx = ctx;
    for (; headers && *headers; headers++)
        r->headers = slist_append(r->headers, *headers);
    easy_setopt(r->easy, OPT_URL, url);
    easy_setopt(r->easy, OPT_CUSTOMREQUEST, method);
    easy_setopt(r->easy, OPT_HTTPHEADER, r->headers);
    easy_setopt(r->easy, OPT_WRITEFUNCTION, on_data);
    easy_setopt(r->easy, OPT_WRITEDATA, r);
    easy_setopt(r->easy, OPT_PRIVATE, r);
    easy_setopt(r->easy, OPT_NOSIGNAL, 1L);
    easy_setopt(r->easy, OPT_TIMEOUT, 20L);
    easy_setopt(r->easy, OPT_CONNECTTIMEOUT, 8L);
    easy_setopt(r->easy, OPT_USERAGENT, "R3lay-LiteKernGo/1.0");
    easy_setopt(r->easy, OPT_CAPATH, "/etc/ssl/certs");
    if (body) {
        r->body = strdup(body);                 /* libcurl keeps only the pointer */
        easy_setopt(r->easy, OPT_POSTFIELDS, r->body);
    }
    if (multi_add(multi, r->easy)) {
        easy_cleanup(r->easy);
        goto no;
    }
    return 0;
no:
    if (r) {
        if (r->headers)
            slist_free_all(r->headers);
        free(r->body);
        free(r);
    }
    return -1;
}

int http_pump(void)
{
    int running = 0, left = 0;
    struct curl_msg *m;
    if (!multi)
        return 0;                               /* not set up: nothing is going */
    multi_perform(multi, &running);
    while ((m = multi_info_read(multi, &left))) {
        struct req *r = 0;
        long status = 0;
        if (m->msg != MSG_DONE)
            continue;
        int result = m->data.result;
        easy_getinfo(m->easy, INFO_PRIVATE, &r);
        if (!result)
            easy_getinfo(m->easy, INFO_RESPONSE_CODE, &status);
        r->done(r->ctx, status, r->reply ? r->reply : (char *)"", r->got);
        finish(r);
        multi_perform(multi, &running);         /* the callback may have started more */
    }
    return running;
}
