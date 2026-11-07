/* R3lay: a small JSON reader. Parses a reply in place into a tree. */
#ifndef R3LAY_JSON_H
#define R3LAY_JSON_H

#include <stddef.h>

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json {
    enum json_type type;
    const char *key;            /* in an object */
    const char *text;           /* a string (UTF-8); a number or bool as written */
    struct json *first, *next;  /* an array's or object's items */
};

/* NULL if it isn't JSON. text is changed, and must outlive the tree. */
struct json *json_parse(char *text);
void json_free(struct json *j);

const struct json *json_get(const struct json *object, const char *key);
/* A string member; "" if it's missing or null. */
const char *json_str(const struct json *object, const char *key);
int json_null(const struct json *object, const char *key);      /* missing counts */

/* s as a JSON string with its quotes, into out; the length, or -1 if it won't fit. */
int json_quote(const char *s, char *out, size_t room);

#endif
