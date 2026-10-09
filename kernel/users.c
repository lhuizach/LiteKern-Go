/* LiteKern X — users (see kernel/users.h). */
#include "kernel/users.h"
#include "kernel/errno.h"
#include "kernel/io.h"
#include "kernel/printk.h"
#include "kernel/settings.h"
#include "kernel/sha256.h"
#include "kernel/string.h"
#include "kernel/timing.h"

#define FILE_NAME "users.txt"
#define TEXT_MAX  (USERS_MAX * 160 + 128)

static struct user users[USERS_MAX];
static int count, current = -1;

/* Avatar colours, given in turn (GNOME's palette). */
static const uint32_t palette[] = { 0x3584e4, 0x2ec27e, 0xe66100, 0x9141ac, 0xc01c28, 0x1c71d8, 0x986a44,
                                    0x26a269 };

/* --- hashing -------------------------------------------------------------------------------- */

static void hash_password(const uint8_t salt[16], const char *pw, uint8_t out[32])
{
    size_t n = strlen(pw);
    struct sha256 c;
    sha256_init(&c);
    sha256_add(&c, salt, 16);
    sha256_add(&c, pw, n);
    sha256_done(&c, out);
    for (int r = 1; r < PASS_ROUNDS; r++) {
        sha256_init(&c);
        sha256_add(&c, out, 32);
        sha256_add(&c, salt, 16);
        sha256_add(&c, pw, n);
        sha256_done(&c, out);
    }
}

/* A fresh salt: the TSC, the uptime and a counter, hashed. Not a strong
 * random source (the kernel has none), but enough that two users with the
 * same password don't share a hash. */
static void new_salt(uint8_t out[16])
{
    static uint32_t n;
    struct { uint64_t tsc; uint32_t up, n; } seed = { rdtsc(), uptime_ms(), ++n };
    uint8_t h[32];
    sha256(&seed, sizeof(seed), h);
    memcpy(out, h, 16);
}

static int same_hash(const uint8_t *a, const uint8_t *b)
{
    uint8_t d = 0;
    for (int i = 0; i < 32; i++)
        d |= a[i] ^ b[i];                   /* every byte, whatever the first differs in */
    return d == 0;
}

/* --- the file ------------------------------------------------------------------------------- */

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int unhex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
        if (b < 0)
            return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return 0;
}

static char *hex(char *p, const uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) {
        *p++ = "0123456789abcdef"[d[i] >> 4];
        *p++ = "0123456789abcdef"[d[i] & 15];
    }
    return p;
}

static char *put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static void save(void)
{
    static char text[TEXT_MAX];
    char *p = put(text, "# LiteKern X's users: role, avatar colour, salt, password hash (or -), name.\n"
                        "# Written by the system; the passwords themselves aren't kept (kernel/users.h).\n");
    for (int i = 0; i < count; i++) {
        struct user *u = &users[i];
        uint8_t rgb[3] = { (uint8_t)(u->colour >> 16), (uint8_t)(u->colour >> 8), (uint8_t)u->colour };
        p = put(p, u->admin ? "user admin " : "user standard ");
        p = hex(p, rgb, 3);
        *p++ = ' ';
        p = hex(p, u->salt, 16);
        *p++ = ' ';
        p = u->has_password ? hex(p, u->hash, 32) : put(p, "-");
        *p++ = ' ';
        p = put(p, u->name);
        *p++ = '\n';
    }
    int err = settings_write_file(FILE_NAME, text, (uint32_t)(p - text));
    if (err && err != -ENODEV)
        kprintf("users: couldn't save (%d)\n", err);
    else if (!err)
        kprintf("users: saved (%d)\n", count);
}

static void defaults(void)
{
    count = 1;
    memset(&users[0], 0, sizeof(users[0]));
    memcpy(users[0].name, "User", 5);
    users[0].admin = 1;
    users[0].colour = palette[0];
    new_salt(users[0].salt);
}

const char *users_bad_name(const char *name)
{
    int n = (int)strlen(name);
    if (!n || name[0] == ' ' || name[n - 1] == ' ')
        return n ? "Names can't start or end with a space." : "Type a name.";
    if (n >= USER_NAME_MAX)
        return "Names can be 20 characters at most.";
    for (int i = 0; i < n; i++)
        if (name[i] < ' ' || name[i] > '~' || strchr("/\\:*?\"<>|", name[i]))
            return "Names can't use / \\ : * ? \" < > | or symbols beyond ASCII.";
    return 0;
}

void users_load(void)
{
    static char text[TEXT_MAX + 1];
    int n = settings_read_file(FILE_NAME, text, TEXT_MAX);
    count = 0;
    if (n > 0) {
        text[n] = '\0';
        for (char *line = text; *line && count < USERS_MAX;) {
            char *end = line;
            while (*end && *end != '\n')
                end++;
            char *next = *end ? end + 1 : end;
            *end = '\0';
            if (end > line && end[-1] == '\r')
                end[-1] = '\0';
            /* "user <role> <rgb> <salt> <hash|-> <name>" */
            struct user u;
            memset(&u, 0, sizeof(u));
            char *f[6];
            int k = 0;
            for (char *p = line; k < 6 && *p;) {
                f[k++] = p;
                if (k == 6)
                    break;
                while (*p && *p != ' ')
                    p++;
                if (*p)
                    *p++ = '\0';
            }
            uint8_t rgb[3];
            int ok = k == 6 && !strcmp(f[0], "user") && (!strcmp(f[1], "admin") || !strcmp(f[1], "standard")) &&
                     strlen(f[2]) == 6 && !unhex(f[2], rgb, 3) && strlen(f[3]) == 32 && !unhex(f[3], u.salt, 16) &&
                     (!strcmp(f[4], "-") || (strlen(f[4]) == 64 && !unhex(f[4], u.hash, 32))) &&
                     !users_bad_name(f[5]) && users_find(f[5]) < 0;
            if (ok) {
                u.admin = f[1][0] == 'a';
                u.colour = (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
                u.has_password = strcmp(f[4], "-") != 0;
                memcpy(u.name, f[5], strlen(f[5]) + 1);
                users[count++] = u;
            }
            line = next;
        }
    }
    int admins = 0;
    for (int i = 0; i < count; i++)
        admins += users[i].admin;
    if (!count) {
        defaults();
        kprintf("users: %s: one admin, \"User\", no password\n",
                n == -ENOENT ? "none saved yet" : n < 0 ? "users.txt unreadable" : "users.txt has no users");
    } else {
        if (!admins)
            users[0].admin = 1;             /* there's always one */
        kprintf("users: %d (", count);
        for (int i = 0; i < count; i++)
            kprintf("%s%s%s%s", i ? ", " : "", users[i].name, users[i].admin ? " admin" : "",
                    users[i].has_password ? " password" : "");
        kprintf(")\n");
    }
    current = users_need_login() ? -1 : 0;
}

/* --- reading -------------------------------------------------------------------------------- */

int users_count(void)
{
    return count;
}

const struct user *users_get(int i)
{
    return i >= 0 && i < count ? &users[i] : 0;
}

int users_find(const char *name)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(users[i].name, name))
            return i;
    return -1;
}

int users_current(void)
{
    return current;
}

void users_set_current(int i)
{
    current = i >= 0 && i < count ? i : -1;
    if (current >= 0)
        kprintf("users: %s logged in\n", users[current].name);
}

int users_need_login(void)
{
    return count > 1 || (count == 1 && users[0].has_password);
}

int users_check(int i, const char *password)
{
    if (i < 0 || i >= count)
        return 0;
    if (!users[i].has_password)
        return 1;
    uint8_t h[32];
    hash_password(users[i].salt, password, h);
    return same_hash(h, users[i].hash);
}

/* --- changing ---------------------------------------------------------------------------------- */

static int bad_password(const char *pw)
{
    int n = (int)strlen(pw);
    if (n >= PASSWORD_MAX)
        return 1;
    for (int i = 0; i < n; i++)
        if (pw[i] < ' ' || pw[i] > '~')
            return 1;
    return 0;
}

static void set_password(struct user *u, const char *pw)
{
    new_salt(u->salt);
    u->has_password = pw[0] != '\0';
    if (u->has_password)
        hash_password(u->salt, pw, u->hash);
    else
        memset(u->hash, 0, sizeof(u->hash));
}

int users_add(const char *name, const char *password, int admin)
{
    if (users_bad_name(name) || bad_password(password))
        return -EINVAL;
    if (users_find(name) >= 0)
        return -EEXIST;
    if (count == USERS_MAX)
        return -ENOSPC;
    struct user *u = &users[count];
    memset(u, 0, sizeof(*u));
    memcpy(u->name, name, strlen(name) + 1);
    u->admin = !!admin;
    u->colour = palette[count % (int)(sizeof(palette) / sizeof(palette[0]))];
    set_password(u, password);
    count++;
    kprintf("users: added %s%s%s\n", name, admin ? " (admin)" : "", u->has_password ? ", with a password" : "");
    save();
    return 0;
}

int users_set_password(int i, const char *password)
{
    if (i < 0 || i >= count || bad_password(password))
        return -EINVAL;
    set_password(&users[i], password);
    kprintf("users: %s %s\n", users[i].name, users[i].has_password ? "has a new password" : "has no password now");
    save();
    return 0;
}

int users_rename(int i, const char *name)
{
    if (i < 0 || i >= count || users_bad_name(name))
        return -EINVAL;
    int other = users_find(name);
    if (other >= 0 && other != i)
        return -EEXIST;
    kprintf("users: %s renamed %s\n", users[i].name, name);
    memset(users[i].name, 0, sizeof(users[i].name));
    memcpy(users[i].name, name, strlen(name) + 1);
    save();
    return 0;
}

static int admins(void)
{
    int n = 0;
    for (int i = 0; i < count; i++)
        n += users[i].admin;
    return n;
}

int users_set_admin(int i, int admin)
{
    if (i < 0 || i >= count)
        return -EINVAL;
    if (!admin && users[i].admin && admins() == 1)
        return -EBUSY;                      /* the last admin */
    users[i].admin = !!admin;
    kprintf("users: %s is %s\n", users[i].name, admin ? "an admin" : "a standard user");
    save();
    return 0;
}

int users_remove(int i)
{
    if (i < 0 || i >= count)
        return -EINVAL;
    if (i == current || (users[i].admin && admins() == 1))
        return -EBUSY;
    kprintf("users: removed %s\n", users[i].name);
    memmove(&users[i], &users[i + 1], sizeof(users[0]) * (size_t)(count - i - 1));
    count--;
    if (current > i)
        current--;
    save();
    return 0;
}
