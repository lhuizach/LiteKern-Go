#include "json.h"

#include <stdlib.h>
#include <string.h>

#define DEPTH_MAX 32

static char *skip(char *p)
{
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
        p++;
    return p;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            return -1;
        v = v * 16 + d;
    }
    return v;
}

static char *put_utf8(char *o, unsigned c)
{
    if (c < 0x80) {
        *o++ = (char)c;
    } else if (c < 0x800) {
        *o++ = (char)(0xc0 | c >> 6);
        *o++ = (char)(0x80 | (c & 63));
    } else if (c < 0x10000) {
        *o++ = (char)(0xe0 | c >> 12);
        *o++ = (char)(0x80 | (c >> 6 & 63));
        *o++ = (char)(0x80 | (c & 63));
    } else {
        *o++ = (char)(0xf0 | c >> 18);
        *o++ = (char)(0x80 | (c >> 12 & 63));
        *o++ = (char)(0x80 | (c >> 6 & 63));
        *o++ = (char)(0x80 | (c & 63));
    }
    return o;
}

/* *pp at the opening quote. Decodes in place (never longer than the source);
 * *pp ends after the closing quote. NULL if it's cut short. */
static char *string(char **pp)
{
    char *p = *pp + 1, *start = p, *o = p;
    while (*p != '"') {
        if (!*p)
            return 0;
        if (*p != '\\') {
            *o++ = *p++;
            continue;
        }
        p++;
        switch (*p) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            int c = hex4(p + 1);
            if (c < 0)
                return 0;
            p += 4;
            if (c >= 0xd800 && c < 0xdc00 && p[1] == '\\' && p[2] == 'u') {
                int low = hex4(p + 3);
                if (low >= 0xdc00 && low < 0xe000) {
                    c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                    p += 6;
                }
            }
            o = put_utf8(o, (unsigned)c);
            break;
        }
        case 0:
            return 0;
        default:
            *o++ = *p;
        }
        p++;
    }
    *o = 0;
    *pp = p + 1;
    return start;
}

static struct json *value(char **pp, int depth);

static int items(struct json *j, char **pp, int depth, char close)
{
    char *p = skip(*pp + 1);
    struct json **tail = &j->first;
    if (*p == close) {
        *pp = p + 1;
        return 0;
    }
    for (;;) {
        char *key = 0;
        if (close == '}') {
            if (*p != '"' || !(key = string(&p)))
                return -1;
            p = skip(p);
            if (*p != ':')
                return -1;
            p = skip(p + 1);
        }
        struct json *v = value(&p, depth + 1);
        if (!v)
            return -1;
        v->key = key;
        *tail = v;
        tail = &v->next;
        p = skip(p);
        if (*p == close) {
            *pp = p + 1;
            return 0;
        }
        if (*p != ',')
            return -1;
        p = skip(p + 1);
    }
}

static struct json *value(char **pp, int depth)
{
    char *p = skip(*pp);
    struct json *j = calloc(1, sizeof(*j));
    if (!j || depth > DEPTH_MAX)
        goto bad;
    if (*p == '{' || *p == '[') {
        j->type = *p == '{' ? JSON_OBJECT : JSON_ARRAY;
        if (items(j, &p, depth, *p == '{' ? '}' : ']'))
            goto bad;
    } else if (*p == '"') {
        j->type = JSON_STRING;
        if (!(j->text = string(&p)))
            goto bad;
    } else if (!strncmp(p, "null", 4)) {
        p += 4;
    } else if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
        j->type = JSON_BOOL;
        j->text = *p == 't' ? "true" : "false";
        p += *p == 't' ? 4 : 5;
    } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
        /* Not NUL-terminated in place (a ',' may follow); strtol and the like
         * stop reading by themselves. */
        j->type = JSON_NUMBER;
        j->text = p;
        while (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9'))
            p++;
    } else {
        goto bad;
    }
    *pp = p;
    return j;
bad:
    json_free(j);
    return 0;
}

struct json *json_parse(char *text)
{
    char *p = text;
    struct json *j = value(&p, 0);
    if (j && *skip(p)) {
        json_free(j);
        return 0;
    }
    return j;
}

void json_free(struct json *j)
{
    while (j) {
        struct json *next = j->next;
        json_free(j->first);
        free(j);
        j = next;
    }
}

const struct json *json_get(const struct json *o, const char *key)
{
    if (o && o->type == JSON_OBJECT)
        for (const struct json *k = o->first; k; k = k->next)
            if (!strcmp(k->key, key))
                return k;
    return 0;
}

const char *json_str(const struct json *o, const char *key)
{
    const struct json *k = json_get(o, key);
    return k && k->type == JSON_STRING ? k->text : "";
}

int json_null(const struct json *o, const char *key)
{
    const struct json *k = json_get(o, key);
    return !k || k->type == JSON_NULL;
}

int json_quote(const char *s, char *out, size_t room)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    if (room < 3)
        return -1;
    out[n++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (n + 8 > room)
            return -1;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c == '\n') {
            out[n++] = '\\';
            out[n++] = 'n';
        } else if (c < 0x20) {
            memcpy(out + n, "\\u00", 4);
            n += 4;
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        } else {
            out[n++] = (char)c;
        }
    }
    out[n++] = '"';
    out[n] = 0;
    return (int)n;
}
