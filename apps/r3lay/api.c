/* R3lay: Supabase. The same project, tables and row rules as the web app, over
 * its public REST and auth endpoints.
 *
 * The project's URL and public key come from r3lay.conf (never in git): next
 * to the program, or wherever R3LAY_CONF says. Only the refresh token is kept
 * after signing in, never the password. New messages are found by polling for
 * rows newer than the last one seen. */
/* Cutting a long list of ids short is fine here: a poll just misses a group. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "r3lay.h"
#include "http.h"
#include "json.h"
#include "sdk/kern86.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHAT_POLL_S 3
#define LIST_POLL_S 8
#define SESSION_FILE "r3lay.session"
#define URL_MAX 3000
#define RETRIES 5

struct conv convs[CONV_MAX];
int nconvs, list_ready;
struct msg msgs[MSG_MAX];
int nmsgs, chat_ready;
struct person found[FOUND_MAX];
int nfound;
char me_id[ID_LEN], me_name[40];
int offline;

static char base_url[200], api_key[1024];
static char access_token[4096], refresh_token[512];
static time_t expires_at;
static int refreshing, signed_in;

static struct conv fresh[CONV_MAX];     /* the list while it's being fetched */
static int nfresh, list_busy;
static char newest_direct[WHEN_LEN], newest_group[WHEN_LEN];
static int polling_list;

static struct conv chat;                /* the open chat */
static int chat_open_now, chat_gen, chat_polling;
static char chat_last[WHEN_LEN];        /* the newest row from the server */

static struct person names[256];
static int nnames;

static void copy(char *to, const char *from, size_t room)
{
    size_t n = strlen(from) < room ? strlen(from) : room - 1;
    memcpy(to, from, n);
    to[n] = 0;
}

/* --- text and time ------------------------------------------------------------------ */

void show_text(const char *s, char *out, int room)
{
    /* U+00C0 .. U+00FF */
    static const char latin[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < room - 2) {
        unsigned c = *p;
        int len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        unsigned code = c;
        if (len == 2)
            code = (c & 31u) << 6 | (p[1] & 63u);
        else if (len == 3)
            code = (c & 15u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u);
        else if (len == 4)
            code = 0x10000;                 /* emoji and the like */
        for (int i = 1; i < len; i++)
            if (!p[i])
                goto done;                  /* cut short */
        p += len;
        if (code == '\n')
            out[n++] = '\n';
        else if (code == '\t')
            out[n++] = ' ';
        else if (code >= 32 && code < 127)
            out[n++] = (char)code;
        else if (code >= 0xc0 && code <= 0xff)
            out[n++] = latin[code - 0xc0];
        else if (code == 0xb7 || code == 0x2013 || code == 0x2014)
            out[n++] = '-';
        else if (code == 0x2018 || code == 0x2019)
            out[n++] = '\'';
        else if (code == 0x201c || code == 0x201d)
            out[n++] = '"';
        else if (code == 0x2026)
            out[n++] = (char)0x83;
    }
done:
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    int skip = 0;
    while (out[skip] == ' ')
        skip++;
    if (skip)
        memmove(out, out + skip, (size_t)(n - skip + 1));
    if (!out[0] && *s)
        copy(out, "[emoji]", (size_t)room);
}

static time_t when_time(const char *w)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    if (sscanf(w, "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6)
        return 0;
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    return timegm(&t);
}

/* How far the local time is ahead of UTC, from the shell's clock (its time
 * zone is the phone's setting, not this process's), to the quarter hour. */
static long zone_seconds(void)
{
    static long zone;
    static time_t worked_out;
    time_t now = time(0);
    if (now - worked_out > 60) {
        struct k86_time k;
        worked_out = now;
        if (k86_time(&k) == 0) {
            struct tm t;
            memset(&t, 0, sizeof(t));
            t.tm_year = k.year - 1900;
            t.tm_mon = k.month - 1;
            t.tm_mday = k.day;
            t.tm_hour = k.hour;
            t.tm_min = k.minute;
            t.tm_sec = k.second;
            long diff = (long)(timegm(&t) - now);
            zone = (diff + (diff < 0 ? -450 : 450)) / 900 * 900;
        }
    }
    return zone;
}

static const char months[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

void when_short(const char *when, char *out, int room)
{
    time_t t = when_time(when) + zone_seconds(), now = time(0) + zone_seconds();
    struct tm a, b;
    out[0] = 0;
    if (t < 86400)
        return;
    gmtime_r(&t, &a);
    gmtime_r(&now, &b);
    if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year)
        snprintf(out, (size_t)room, "%02d:%02d", a.tm_hour, a.tm_min);
    else
        snprintf(out, (size_t)room, "%d %s", a.tm_mday, months[a.tm_mon]);
}

void when_day(const char *when, char *out, int room)
{
    time_t t = when_time(when) + zone_seconds(), now = time(0) + zone_seconds();
    long days = (long)(now / 86400 - t / 86400);
    struct tm a;
    gmtime_r(&t, &a);
    if (days == 0)
        copy(out, "Today", (size_t)room);
    else if (days == 1)
        copy(out, "Yesterday", (size_t)room);
    else
        snprintf(out, (size_t)room, "%d %s %d", a.tm_mday, months[a.tm_mon], a.tm_year + 1900);
}

static void now_iso(char *out, size_t room)
{
    time_t now = time(0);
    struct tm t;
    gmtime_r(&now, &t);
    strftime(out, room, "%Y-%m-%dT%H:%M:%SZ", &t);
}

/* A timestamp for a query: its '+' would read as a space. */
static void url_when(const char *when, char *out, size_t room)
{
    size_t n = 0;
    for (; *when && n + 4 < room; when++) {
        if (*when == '+') {
            memcpy(out + n, "%2B", 3);
            n += 3;
        } else {
            out[n++] = *when;
        }
    }
    out[n] = 0;
}

static void new_uuid(char *out)
{
    uint8_t b[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, 16, f) != 16)
        for (int i = 0; i < 16; i++)
            b[i] = (uint8_t)rand();
    if (f)
        fclose(f);
    b[6] = (uint8_t)((b[6] & 15) | 0x40);
    b[8] = (uint8_t)((b[8] & 63) | 0x80);
    snprintf(out, ID_LEN, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/* --- the configuration and the session ---------------------------------------------- */

int api_setup(void)
{
    const char *path = getenv("R3LAY_CONF");
    char line[1200];
    FILE *f = fopen(path ? path : "/usr/lib/litekern/apps/r3lay/r3lay.conf", "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "url=", 4))
            copy(base_url, line + 4, sizeof(base_url));
        else if (!strncmp(line, "key=", 4))
            copy(api_key, line + 4, sizeof(api_key));
    }
    fclose(f);
    if (!base_url[0] || !api_key[0] || http_init())
        return -1;
    return 0;
}

static int home_dir(char *path)
{
    return k86_home(path);
}

static void session_save(void)
{
    char path[K86_PATH_MAX], text[700];
    int vol = home_dir(path);
    if (vol < 0)
        return;
    int n = snprintf(text, sizeof(text), "%s\n%s\n", me_id, refresh_token);
    k86_write(vol, path, SESSION_FILE, text, (uint32_t)n);
}

static void session_forget(void)
{
    char path[K86_PATH_MAX];
    int vol = home_dir(path);
    if (vol >= 0)
        k86_delete(vol, path, SESSION_FILE);
    access_token[0] = refresh_token[0] = me_id[0] = me_name[0] = 0;
    expires_at = 0;
    signed_in = 0;
    nconvs = list_ready = 0;
}

/* --- requests ----------------------------------------------------------------------- */

static int call(const char *method, const char *path, const char *prefer, const char *body, http_done done, void *ctx)
{
    static char key[1100], auth[4200], pref[100], url[URL_MAX];
    const char *h[5] = { key, auth, "Content-Type: application/json", 0, 0 };
    snprintf(key, sizeof(key), "apikey: %s", api_key);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", access_token[0] ? access_token : api_key);
    if (prefer) {
        snprintf(pref, sizeof(pref), "Prefer: %s", prefer);
        h[3] = pref;
    }
    snprintf(url, sizeof(url), "%s%s", base_url, path);
    return http_request(method, url, h, body, done, ctx);
}

/* After every answer: 1 if it's good to use. */
static int answered(long status)
{
    int was = offline;
    offline = status == 0;
    if (was != offline)
        ui_changed();
    if (status == 401)
        expires_at = 0;                     /* the token's gone stale: api_tick renews it */
    return status >= 200 && status < 300;
}

static void on_ignored(void *ctx, long status, char *body, size_t len)
{
    (void)ctx;
    (void)body;
    (void)len;
    answered(status);
}

/* --- signing in --------------------------------------------------------------------- */

static void start(void);

static int take_tokens(char *body)
{
    struct json *j = json_parse(body);
    const struct json *user = json_get(j, "user"), *exp = json_get(j, "expires_in");
    const char *a = json_str(j, "access_token"), *r = json_str(j, "refresh_token");
    int ok = a[0] && r[0] && strlen(a) < sizeof(access_token) && strlen(r) < sizeof(refresh_token);
    if (ok) {
        strcpy(access_token, a);
        strcpy(refresh_token, r);
        expires_at = time(0) + (exp && exp->text ? atol(exp->text) : 3600);
        if (user)
            copy(me_id, json_str(user, "id"), sizeof(me_id));
        session_save();
    }
    json_free(j);
    return ok;
}

static void on_sign_in(void *ctx, long status, char *body, size_t len)
{
    (void)ctx;
    (void)len;
    answered(status);
    if (status == 200 && take_tokens(body))
        start();
    else if (status == 0)
        ui_sign_in_failed("Can't reach R3lay. Is Wi-Fi on?");
    else if (status == 400 || status == 401)
        ui_sign_in_failed("Wrong email or password");
    else
        ui_sign_in_failed("Couldn't sign in. Try again later");
}

void api_sign_in(const char *email, const char *password)
{
    char body[600], e[200], p[200];
    if (json_quote(email, e, sizeof(e)) < 0 || json_quote(password, p, sizeof(p)) < 0)
        return;
    snprintf(body, sizeof(body), "{\"email\":%s,\"password\":%s}", e, p);
    access_token[0] = 0;
    if (call("POST", "/auth/v1/token?grant_type=password", 0, body, on_sign_in, 0))
        ui_sign_in_failed("Couldn't start. Try again");
    memset(body, 0, sizeof(body));
    memset(p, 0, sizeof(p));
}

static void on_refresh(void *ctx, long status, char *body, size_t len)
{
    (void)len;
    refreshing = 0;
    answered(status);
    if (status == 200 && take_tokens(body)) {
        if (ctx)
            start();
    } else if (status == 400 || status == 401) {    /* signed out elsewhere, or revoked */
        session_forget();
        ui_signed_out("Please sign in again");
    } else if (ctx) {
        /* No network yet: the app still opens, and keeps trying. */
        signed_in = 1;
        expires_at = 0;
        ui_signed_in();
    }
}

static void renew(void *first)
{
    char body[600], r[560];
    if (refreshing || json_quote(refresh_token, r, sizeof(r)) < 0)
        return;
    snprintf(body, sizeof(body), "{\"refresh_token\":%s}", r);
    refreshing = 1;
    access_token[0] = 0;
    if (call("POST", "/auth/v1/token?grant_type=refresh_token", 0, body, on_refresh, first))
        refreshing = 0;
}

int api_resume(void)
{
    char path[K86_PATH_MAX], text[700];
    int vol = home_dir(path), n;
    if (vol < 0 || (n = k86_read(vol, path, SESSION_FILE, text, sizeof(text) - 1)) <= 0)
        return 0;
    text[n] = 0;
    char *nl = strchr(text, '\n');
    if (!nl)
        return 0;
    *nl = 0;
    copy(me_id, text, sizeof(me_id));
    copy(refresh_token, nl + 1, sizeof(refresh_token));
    refresh_token[strcspn(refresh_token, "\n")] = 0;
    if (!me_id[0] || !refresh_token[0])
        return 0;
    renew((void *)1);
    return 1;
}

void api_sign_out(void)
{
    call("POST", "/auth/v1/logout", 0, "{}", on_ignored, 0);
    api_chat_close();
    session_forget();
    ui_signed_out("");
}

/* --- people ------------------------------------------------------------------------- */

const char *api_name(const char *id)
{
    for (int i = 0; i < nnames; i++)
        if (!strcmp(names[i].id, id))
            return names[i].name;
    return "";
}

static void name_add(const char *id, const char *name)
{
    char shown[40];
    show_text(name, shown, sizeof(shown));
    for (int i = 0; i < nnames; i++)
        if (!strcmp(names[i].id, id)) {
            strcpy(names[i].name, shown);
            return;
        }
    if (nnames < (int)(sizeof(names) / sizeof(names[0]))) {
        copy(names[nnames].id, id, ID_LEN);
        strcpy(names[nnames].name, shown);
        nnames++;
    }
}

static void take_names(char *body)
{
    struct json *j = json_parse(body);
    if (j && j->type == JSON_ARRAY)
        for (const struct json *r = j->first; r; r = r->next)
            name_add(json_str(r, "id"), json_str(r, "username"));
    json_free(j);
}

static void on_names(void *ctx, long status, char *body, size_t len)
{
    (void)ctx;
    (void)len;
    if (answered(status)) {
        take_names(body);
        ui_changed();
    }
}

static void fetch_names(const char *const *ids, int n)
{
    char path[URL_MAX - 300];
    int at = snprintf(path, sizeof(path), "/rest/v1/profiles?select=id,username&id=in.("), any = 0;
    for (int i = 0; i < n && at + ID_LEN + 4 < (int)sizeof(path); i++) {
        if (api_name(ids[i])[0])
            continue;
        at += snprintf(path + at, sizeof(path) - (size_t)at, "%s%s", any ? "," : "", ids[i]);
        any = 1;
    }
    snprintf(path + at, sizeof(path) - (size_t)at, ")");
    if (any)
        call("GET", path, 0, 0, on_names, 0);
}

/* --- the chat list ------------------------------------------------------------------ */

static struct conv *fresh_conv(int group, const char *id)
{
    for (int i = 0; i < nfresh; i++)
        if (fresh[i].group == group && !strcmp(fresh[i].id, id))
            return &fresh[i];
    if (nfresh == CONV_MAX)
        return 0;
    struct conv *c = &fresh[nfresh++];
    memset(c, 0, sizeof(*c));
    c->group = group;
    copy(c->id, id, ID_LEN);
    return c;
}

static void preview_of(const struct json *row, char *out, int room)
{
    const char *text = json_str(row, "text_content"), *media = json_str(row, "media_type");
    int mine = !strcmp(json_str(row, "sender_id"), me_id), n = 0;
    if (mine)
        n = snprintf(out, (size_t)room, "You: ");
    if (text[0])
        show_text(text, out + n, room - n);
    else
        copy(out + n, !strcmp(media, "image") ? "Photo" : !strcmp(media, "video") ? "Video" :
                      !strcmp(media, "audio") ? "Voice message" : "Message", (size_t)(room - n));
    for (char *p = out; *p; p++)
        if (*p == '\n')
            *p = ' ';
}

static int conv_newer(const void *a, const void *b)
{
    return strcmp(((const struct conv *)b)->when, ((const struct conv *)a)->when);
}

static void list_done(void *ctx, long status, char *body, size_t len)
{
    (void)ctx;
    (void)len;
    if (answered(status))
        take_names(body);
    for (int i = 0; i < nfresh; i++)
        if (!fresh[i].group)
            copy(fresh[i].name, api_name(fresh[i].id)[0] ? api_name(fresh[i].id) : "Someone", sizeof(fresh[i].name));
    qsort(fresh, (size_t)nfresh, sizeof(fresh[0]), conv_newer);
    memcpy(convs, fresh, sizeof(convs));
    nconvs = nfresh;
    list_ready = 1;
    list_busy = 0;
    ui_changed();
}

static void list_names(void)
{
    char path[URL_MAX - 300];
    int at = snprintf(path, sizeof(path), "/rest/v1/profiles?select=id,username&id=in.("), any = 0;
    for (int i = 0; i < nfresh; i++) {
        if (fresh[i].group || at + ID_LEN + 4 >= (int)sizeof(path))
            continue;
        at += snprintf(path + at, sizeof(path) - (size_t)at, "%s%s", any ? "," : "", fresh[i].id);
        any = 1;
    }
    snprintf(path + at, sizeof(path) - (size_t)at, ")");
    if (!any || call("GET", path, 0, 0, list_done, 0))
        list_done(0, 200, (char *)"", 0);
}

static void on_group_rows(void *ctx, long status, char *body, size_t len)
{
    (void)ctx;
    (void)len;
    if (answered(status)) {
        struct json *j = json_parse(body);
        if (j && j->type == JSON_ARRAY)
            for (const struct json *r = j->first; r; r = r->next) {
                struct conv *c = fresh_conv(1, json_str(r, "group_id"));
                const char *when = json_str(r, "created_at");
                if (!c)
                    continue;
                if (!c->when[0]) {
                    copy(c->when, when, WHEN_LEN);
                    preview_of(r, c->preview, sizeof(c->preview));
                }
                if (strcmp(json_str(r, "sender_id"), me_id) && strcmp(when, c->read_at) > 0)
                    c->unread++;
                if (strcmp(when, newest_group) > 0)
                    copy(newest_group, when, WHEN_LEN);
            }
        json_free(j);
    }
    list_names();
}

static int group_ids(char *out, int room)
{
    int at = 0;
    for (int i = 0; i < nfresh; i++)
        if (fresh[i].group && at + ID_LEN + 2 < room)
            at += snprintf(out + at, (size_t)(room - at), "%s%s", at ? "," : "", fresh[i].id);
    return at;
}

static void on_group_reads(void *ctx, long status, char *body, size_t len)
{
    char path[URL_MAX - 300], ids[URL_MAX - 500];
    (void)ctx;
    (void)len;
    if (answered(status)) {
        struct json *j = json_parse(body);
        if (j && j->type == JSON_ARRAY)
            for (const struct json *r = j->first; r; r = r->next) {
                struct conv *c = fresh_conv(1, json_str(r, "group_id"));
                if (c)
                    copy(c->read_at, json_str(r, "last_read_at"), WHEN_LEN);
            }
        json_free(j);
    }
    if (!group_ids(ids, sizeof(ids))) {
        list_names();
        return;
    }
    snprintf(path, sizeof(path), "/rest/v1/group_messages?select=group_id,sender_id,text_content,media_type,created_at"
             "&group_id=in.(%s)&deleted_at=is.null&order=created_at.desc&limit=200", ids);
    if (call("GET", path, 0, 0, on_group_rows, 0))
        list_names();
}

static void on_groups(void *ctx, long status, char *body, size_t len)
{
    char path[200];
    (void)ctx;
    (void)len;
    if (answered(status)) {
        struct json *j = json_parse(body);
        if (j && j->type == JSON_ARRAY)
            for (const struct json *r = j->first; r; r = r->next) {
                struct conv *c = fresh_conv(1, json_str(r, "group_id"));
                if (c)
                    show_text(json_str(json_get(r, "groups"), "name"), c->name, sizeof(c->name));
            }
        json_free(j);
    }
    snprintf(path, sizeof(path), "/rest/v1/group_reads?select=group_id,last_read_at&user_id=eq.%s", me_id);
    if (call("GET", path, 0, 0, on_group_reads, 0))
        list_names();
}

static void on_direct(void *ctx, long status, char *body, size_t len)
{
    char path[240];
    (void)ctx;
    (void)len;
    if (!answered(status)) {                /* try again at the next poll */
        list_busy = 0;
        return;
    }
    nfresh = 0;
    struct json *j = json_parse(body);
    if (j && j->type == JSON_ARRAY)
        for (const struct json *r = j->first; r; r = r->next) {
            const char *sender = json_str(r, "sender_id"), *to = json_str(r, "recipient_id");
            const char *when = json_str(r, "created_at");
            struct conv *c = fresh_conv(0, strcmp(sender, me_id) ? sender : to);
            if (!c)
                continue;
            if (!c->when[0]) {
                copy(c->when, when, WHEN_LEN);
                preview_of(r, c->preview, sizeof(c->preview));
            }
            if (strcmp(sender, me_id) && json_null(r, "read_at"))
                c->unread++;
            if (strcmp(when, newest_direct) > 0)
                copy(newest_direct, when, WHEN_LEN);
        }
    json_free(j);
    snprintf(path, sizeof(path), "/rest/v1/group_members?select=group_id,groups(name)&user_id=eq.%s&banned=eq.false", me_id);
    if (call("GET", path, 0, 0, on_groups, 0))
        list_names();
}

void api_list_load(void)
{
    char path[400];
    if (list_busy)
        return;
    snprintf(path, sizeof(path), "/rest/v1/messages?select=sender_id,recipient_id,text_content,media_type,created_at,read_at"
             "&or=(sender_id.eq.%s,recipient_id.eq.%s)&deleted_at=is.null&order=created_at.desc&limit=300", me_id, me_id);
    if (!call("GET", path, 0, 0, on_direct, 0))
        list_busy = 1;
}

/* Anything new for the list? A note for it, unless that chat's open. */
static void on_list_poll(void *ctx, long status, char *body, size_t len)
{
    int group = ctx != 0, any = 0;
    (void)len;
    polling_list--;
    if (answered(status)) {
        struct json *j = json_parse(body);
        if (j && j->type == JSON_ARRAY)
            for (const struct json *r = j->first; r; r = r->next) {
                const char *sender = json_str(r, "sender_id"), *id = group ? json_str(r, "group_id") : sender;
                char text[100], title[60];
                any = 1;
                if (chat_open_now && !strcmp(chat.id, id))
                    continue;
                preview_of(r, text, sizeof(text));
                copy(title, api_name(sender)[0] ? api_name(sender) : "R3lay", sizeof(title));
                k86_notify(title, text);
            }
        json_free(j);
    }
    if (any)
        api_list_load();
}

static void list_poll(void)
{
    char path[URL_MAX - 300], when[WHEN_LEN + 8], ids[URL_MAX - 500];
    if (polling_list || list_busy || !list_ready)
        return;
    /* Nothing yet seen: only the first full load's rows count. */
    url_when(newest_direct[0] ? newest_direct : "2000-01-01T00:00:00Z", when, sizeof(when));
    snprintf(path, sizeof(path), "/rest/v1/messages?select=sender_id,text_content,media_type,created_at"
             "&recipient_id=eq.%s&created_at=gt.%s&deleted_at=is.null&order=created_at.asc&limit=20", me_id, when);
    if (!call("GET", path, 0, 0, on_list_poll, 0))
        polling_list++;
    int at = 0;
    for (int i = 0; i < nconvs; i++)
        if (convs[i].group && at + ID_LEN + 2 < (int)sizeof(ids))
            at += snprintf(ids + at, sizeof(ids) - (size_t)at, "%s%s", at ? "," : "", convs[i].id);
    if (at) {
        url_when(newest_group[0] ? newest_group : "2000-01-01T00:00:00Z", when, sizeof(when));
        snprintf(path, sizeof(path), "/rest/v1/group_messages?select=group_id,sender_id,text_content,media_type,created_at"
                 "&group_id=in.(%s)&sender_id=neq.%s&created_at=gt.%s&deleted_at=is.null&order=created_at.asc&limit=20",
                 ids, me_id, when);
        if (!call("GET", path, 0, 0, on_list_poll, (void *)1))
            polling_list++;
    }
}

/* --- a chat ------------------------------------------------------------------------- */

static void msg_free(struct msg *m)
{
    free(m->raw);
    free(m->shown);
    free(m->line);
    memset(m, 0, sizeof(*m));
}

static void msgs_clear(void)
{
    for (int i = 0; i < nmsgs; i++)
        msg_free(&msgs[i]);
    nmsgs = 0;
}

static struct msg *msg_room(void)
{
    if (nmsgs == MSG_MAX) {                 /* the oldest goes */
        msg_free(&msgs[0]);
        memmove(&msgs[0], &msgs[1], sizeof(msgs[0]) * (MSG_MAX - 1));
        nmsgs--;
    }
    struct msg *m = &msgs[nmsgs++];
    memset(m, 0, sizeof(*m));
    return m;
}

static char *shown_copy(const char *raw)
{
    size_t room = strlen(raw) + 8;
    char *out = malloc(room);
    if (out)
        show_text(raw, out, (int)room);
    return out;
}

/* A row from the server; 1 if it's new here. */
static int take_row(const struct json *r)
{
    const char *id = json_str(r, "id"), *ref = json_str(r, "client_ref"), *when = json_str(r, "created_at");
    if (strcmp(when, chat_last) > 0)
        copy(chat_last, when, WHEN_LEN);
    for (int i = 0; i < nmsgs; i++) {
        struct msg *m = &msgs[i];
        if (!strcmp(m->id, id))
            return 0;
        if (ref[0] && !strcmp(m->ref, ref)) {   /* ours, seen coming back: it arrived */
            copy(m->id, id, ID_LEN);
            copy(m->when, when, WHEN_LEN);
            m->state = MSG_OK;
            return 0;
        }
    }
    struct msg *m = msg_room();
    const char *text = json_str(r, "text_content"), *media = json_str(r, "media_type");
    copy(m->id, id, ID_LEN);
    copy(m->ref, ref, ID_LEN);
    copy(m->sender, json_str(r, "sender_id"), ID_LEN);
    copy(m->when, when, WHEN_LEN);
    m->mine = !strcmp(m->sender, me_id);
    m->read = !json_null(r, "read_at");
    if (!text[0])
        text = !strcmp(media, "image") ? "[Photo]" : !strcmp(media, "video") ? "[Video]" :
               !strcmp(media, "audio") ? "[Voice message]" : "[Message]";
    m->shown = shown_copy(text);
    return 1;
}

static void mark_read(void)
{
    char path[300], body[200], now[32];
    now_iso(now, sizeof(now));
    if (chat.group) {
        snprintf(body, sizeof(body), "{\"group_id\":\"%s\",\"user_id\":\"%s\",\"last_read_at\":\"%s\"}", chat.id, me_id, now);
        call("POST", "/rest/v1/group_reads?on_conflict=group_id,user_id", "resolution=merge-duplicates", body, on_ignored, 0);
    } else {
        snprintf(body, sizeof(body), "{\"read_at\":\"%s\"}", now);
        snprintf(path, sizeof(path), "/rest/v1/messages?recipient_id=eq.%s&sender_id=eq.%s&read_at=is.null", me_id, chat.id);
        call("PATCH", path, 0, body, on_ignored, 0);
    }
}

static void take_senders(void)
{
    const char *ids[MSG_MAX];
    for (int i = 0; i < nmsgs; i++)
        ids[i] = msgs[i].sender;
    fetch_names(ids, nmsgs);
}

static void on_rows(void *ctx, long status, char *body, size_t len)
{
    int first = (intptr_t)ctx < 0, gen = (int)(first ? -(intptr_t)ctx : (intptr_t)ctx), added = 0, unread = 0;
    (void)len;
    if (gen != chat_gen)
        return;                             /* that chat's been left */
    chat_polling = 0;
    if (!answered(status)) {
        if (first)
            ui_changed();
        return;
    }
    struct json *j = json_parse(body);
    if (j && j->type == JSON_ARRAY) {
        const struct json *order[MSG_MAX];
        int n = 0;
        for (const struct json *r = j->first; r && n < MSG_MAX; r = r->next)
            order[n++] = r;
        /* The first load comes newest first, polls oldest first. */
        for (int i = 0; i < n; i++) {
            const struct json *r = order[first ? n - 1 - i : i];
            int fresh_row = take_row(r);
            added += fresh_row;
            if (fresh_row && strcmp(json_str(r, "sender_id"), me_id))
                unread = 1;
        }
    }
    json_free(j);
    chat_ready = 1;
    if (added) {
        take_senders();
        ui_chat_grew();
    } else {
        ui_changed();
    }
    if (unread)
        mark_read();
}

static void on_ticks(void *ctx, long status, char *body, size_t len)
{
    (void)len;
    if ((intptr_t)ctx != chat_gen || !answered(status))
        return;
    struct json *j = json_parse(body);
    if (j && j->first) {
        const char *upto = json_str(j->first, "created_at");
        int changed = 0;
        for (int i = 0; i < nmsgs; i++)
            if (msgs[i].mine && !msgs[i].read && msgs[i].state == MSG_OK && strcmp(msgs[i].when, upto) <= 0) {
                msgs[i].read = 1;
                changed = 1;
            }
        if (changed)
            ui_changed();
    }
    json_free(j);
}

static void rows_path(char *path, size_t room, const char *extra, int newest_first, int limit)
{
    if (chat.group)
        snprintf(path, room, "/rest/v1/group_messages?select=id,client_ref,sender_id,text_content,media_type,created_at"
                 "&group_id=eq.%s&deleted_at=is.null%s&order=created_at.%s&limit=%d",
                 chat.id, extra, newest_first ? "desc" : "asc", limit);
    else
        snprintf(path, room, "/rest/v1/messages?select=id,client_ref,sender_id,text_content,media_type,created_at,read_at"
                 "&or=(and(sender_id.eq.%s,recipient_id.eq.%s),and(sender_id.eq.%s,recipient_id.eq.%s))"
                 "&deleted_at=is.null%s&order=created_at.%s&limit=%d",
                 me_id, chat.id, chat.id, me_id, extra, newest_first ? "desc" : "asc", limit);
}

void api_chat_open(const struct conv *c)
{
    char path[600];
    chat = *c;
    msgs_clear();
    chat_ready = 0;
    chat_open_now = 1;
    chat_polling = 0;
    chat_last[0] = 0;
    chat_gen++;
    rows_path(path, sizeof(path), "", 1, 80);
    call("GET", path, 0, 0, on_rows, (void *)(intptr_t)-chat_gen);
    mark_read();
}

void api_chat_close(void)
{
    chat_open_now = 0;
    chat_gen++;
    msgs_clear();
}

static void chat_poll(void)
{
    char path[700], extra[80], when[WHEN_LEN + 8];
    if (chat_polling || !chat_ready)
        return;
    url_when(chat_last[0] ? chat_last : "2000-01-01T00:00:00Z", when, sizeof(when));
    snprintf(extra, sizeof(extra), "&created_at=gt.%s", when);
    rows_path(path, sizeof(path), extra, 0, 40);
    if (!call("GET", path, 0, 0, on_rows, (void *)(intptr_t)chat_gen))
        chat_polling = 1;
    if (!chat.group) {
        snprintf(path, sizeof(path), "/rest/v1/messages?select=created_at&sender_id=eq.%s&recipient_id=eq.%s"
                 "&read_at=not.is.null&order=created_at.desc&limit=1", me_id, chat.id);
        call("GET", path, 0, 0, on_ticks, (void *)(intptr_t)chat_gen);
    }
}

/* --- sending ------------------------------------------------------------------------ */

struct sending {
    char ref[ID_LEN];
    int gen;
};

static void on_sent(void *ctx, long status, char *body, size_t len)
{
    struct sending *s = ctx;
    struct msg *m = 0;
    (void)len;
    if (s->gen == chat_gen)
        for (int i = 0; i < nmsgs; i++)
            if (!strcmp(msgs[i].ref, s->ref))
                m = &msgs[i];
    if (m) {
        if (answered(status)) {
            struct json *j = json_parse(body);
            if (j && j->first) {
                copy(m->id, json_str(j->first, "id"), ID_LEN);
                copy(m->when, json_str(j->first, "created_at"), WHEN_LEN);
                if (strcmp(m->when, chat_last) > 0)
                    copy(chat_last, m->when, WHEN_LEN);
            }
            json_free(j);
            m->state = MSG_OK;
        } else {
            m->state = MSG_FAILED;
            /* Refused (blocked, banned, muted): trying again won't help. A
             * lost answer or an old token will: api_tick tries again. */
            if (status >= 400 && status != 401 && status < 500)
                m->tries = RETRIES;
        }
        ui_changed();
    }
    free(s);
}

static void post_msg(struct msg *m)
{
    size_t room = strlen(m->raw) * 6 + 300;
    char *body = malloc(room), *quoted = malloc(room);
    struct sending *s = malloc(sizeof(*s));
    int ok = body && quoted && s && json_quote(m->raw, quoted, room) >= 0;
    if (ok) {
        strcpy(s->ref, m->ref);
        s->gen = chat_gen;
        if (chat.group)
            snprintf(body, room, "{\"client_ref\":\"%s\",\"group_id\":\"%s\",\"sender_id\":\"%s\",\"text_content\":%s}",
                     m->ref, chat.id, me_id, quoted);
        else
            snprintf(body, room, "{\"client_ref\":\"%s\",\"recipient_id\":\"%s\",\"sender_id\":\"%s\",\"text_content\":%s}",
                     m->ref, chat.id, me_id, quoted);
        m->state = MSG_SENDING;
        m->tries++;
        ok = !call("POST", chat.group ? "/rest/v1/group_messages" : "/rest/v1/messages",
                   "return=representation", body, on_sent, s);
    }
    if (!ok) {
        m->state = MSG_FAILED;
        free(s);
    }
    free(body);
    free(quoted);
}

void api_send(const char *text)
{
    struct msg *m = msg_room();
    m->raw = strdup(text);
    m->shown = shown_copy(text);
    copy(m->sender, me_id, ID_LEN);
    new_uuid(m->ref);
    now_iso(m->when, WHEN_LEN);
    m->mine = 1;
    post_msg(m);
    ui_chat_grew();
}

/* A send whose answer was lost may still have arrived, and nothing in the
 * database stops a second copy: so a message is sent again only after a poll
 * has come back without it. */
static void retry_sends(void)
{
    static int polls_since;
    if (++polls_since < 2)
        return;
    polls_since = 0;
    for (int i = 0; i < nmsgs; i++)
        if (msgs[i].state == MSG_FAILED && msgs[i].raw && msgs[i].tries < RETRIES)
            post_msg(&msgs[i]);
}

/* --- finding people ----------------------------------------------------------------- */

static void on_found(void *ctx, long status, char *body, size_t len)
{
    (void)ctx;
    (void)len;
    if (!answered(status))
        return;
    nfound = 0;
    struct json *j = json_parse(body);
    if (j && j->type == JSON_ARRAY)
        for (const struct json *r = j->first; r && nfound < FOUND_MAX; r = r->next) {
            const char *id = json_str(r, "id");
            if (!strcmp(id, me_id))
                continue;
            copy(found[nfound].id, id, ID_LEN);
            show_text(json_str(r, "username"), found[nfound].name, sizeof(found[nfound].name));
            name_add(id, found[nfound].name);
            nfound++;
        }
    json_free(j);
    ui_changed();
}

void api_find(const char *query)
{
    char path[400], q[160];
    int n = 0;
    for (const char *p = query; *p && n < (int)sizeof(q) - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')
            q[n++] = (char)c;
        else
            n += snprintf(q + n, sizeof(q) - (size_t)n, "%%%02X", c);
    }
    q[n] = 0;
    if (!n) {
        nfound = 0;
        ui_changed();
        return;
    }
    snprintf(path, sizeof(path), "/rest/v1/profiles?select=id,username&username=ilike.*%s*&order=username.asc&limit=20", q);
    call("GET", path, 0, 0, on_found, 0);
}

/* --- keeping going ------------------------------------------------------------------ */

static void on_me(void *ctx, long status, char *body, size_t len)
{
    (void)ctx;
    (void)len;
    if (answered(status)) {
        struct json *j = json_parse(body);
        if (j && j->first)
            show_text(json_str(j->first, "username"), me_name, sizeof(me_name));
        json_free(j);
        ui_changed();
    }
}

/* Signed in: who we are, the login the web app records, and the list. */
static void start(void)
{
    char path[160];
    signed_in = 1;
    snprintf(path, sizeof(path), "/rest/v1/profiles?select=username&id=eq.%s", me_id);
    call("GET", path, 0, 0, on_me, 0);
    call("POST", "/rest/v1/rpc/touch_login", 0, "{}", on_ignored, 0);
    api_list_load();
    ui_signed_in();
}

void api_tick(void)
{
    static time_t last_chat, last_list;
    time_t now = time(0);
    if (!signed_in)
        return;
    if (refreshing)
        return;
    if (now > expires_at - 60) {
        renew(0);
        return;
    }
    if (!me_name[0] || !list_ready) {       /* a start with no network: catch up */
        if (now - last_list >= LIST_POLL_S) {
            last_list = now;
            start();
        }
        return;
    }
    if (chat_open_now && now - last_chat >= CHAT_POLL_S) {
        last_chat = now;
        chat_poll();
        retry_sends();
    }
    if (now - last_list >= LIST_POLL_S) {
        last_list = now;
        list_poll();
    }
}
