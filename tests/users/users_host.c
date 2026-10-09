/* LiteKern X — kernel/users.c and kernel/sha256.c on Linux (tests/users/test-users.sh).
 *
 * The disk (kernel/settings.h's files) is a buffer here; the log is stdout.
 * Prints "users: ok|FAIL <what>" lines, and the users.txt it ends with
 * (after "file:") so the script can check its hashes with Python's hashlib. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/sha256.h"
#include "kernel/users.h"

/* --- what users.c needs from the kernel ------------------------------------------------------ */

static char disk[4096];
static int disk_len = -1;                   /* -1: no users.txt */

int settings_write_file(const char *name, const void *data, uint32_t len)
{
    (void)name;
    memcpy(disk, data, len);
    disk_len = (int)len;
    return 0;
}

int settings_read_file(const char *name, void *buf, uint32_t len)
{
    (void)name;
    if (disk_len < 0)
        return -ENOENT;
    if ((uint32_t)disk_len > len)
        return -EINVAL;
    memcpy(buf, disk, (size_t)disk_len);
    return disk_len;
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("log: ");
    vprintf(fmt, ap);
    va_end(ap);
}

uint32_t uptime_ms(void)
{
    static uint32_t t;
    return t += 7;
}

/* --- the tests ------------------------------------------------------------------------------------ */

static int failed;

static void check(int ok, const char *what)
{
    printf("users: %s %s\n", ok ? "ok  " : "FAIL", what);
    failed += !ok;
}

static void hexout(const uint8_t *d, int n, char *out)
{
    for (int i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", d[i]);
}

int main(void)
{
    /* SHA-256: FIPS 180-4's examples, and a million 'a's. */
    char h[65];
    uint8_t d[32];
    sha256("abc", 3, d);
    hexout(d, 32, h);
    check(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of \"abc\"");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two, strlen(two), d);
    hexout(d, 32, h);
    check(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "SHA-256 of the 448-bit message");
    sha256("", 0, d);
    hexout(d, 32, h);
    check(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256 of nothing");
    static char million[1000000];
    memset(million, 'a', sizeof(million));
    struct sha256 c;
    sha256_init(&c);
    for (int i = 0; i < 1000000; i += 999)              /* in odd-sized pieces */
        sha256_add(&c, million + i, i + 999 <= 1000000 ? 999 : (size_t)(1000000 - i));
    sha256_done(&c, d);
    hexout(d, 32, h);
    check(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "SHA-256 of a million 'a's, in pieces");

    /* A new stick: one admin, no password, no login needed. */
    users_load();
    check(users_count() == 1 && users_get(0)->admin && !users_get(0)->has_password && !strcmp(users_get(0)->name, "User"),
          "no users.txt: one admin, \"User\", no password");
    check(!users_need_login() && users_current() == 0, "...and nobody has to log in");
    check(users_check(0, "anything"), "without a password, anything does");

    /* Changes. */
    check(users_set_password(0, "hunter2") == 0 && users_get(0)->has_password, "set a password");
    check(users_check(0, "hunter2") && !users_check(0, "hunter3") && !users_check(0, ""), "it's checked exactly");
    check(users_add("Ann", "", 0) == 0 && users_add("Ben Jones", "pass word", 1) == 0, "add two users (one with a space)");
    check(users_add("Ann", "", 0) == -EEXIST, "a taken name is refused");
    check(users_add("", "", 0) == -EINVAL && users_add(" Ann", "", 0) == -EINVAL &&
              users_add("a/b", "", 0) == -EINVAL && users_add("abcdefghijklmnopqrstu", "", 0) == -EINVAL,
          "bad names are refused (empty, a leading space, a slash, 21 characters)");
    check(users_rename(1, "Annie") == 0 && users_find("Annie") == 1 && users_find("Ann") < 0, "rename");
    check(users_set_admin(0, 0) == 0 && users_set_admin(2, 0) == -EBUSY, "the last admin stays one");
    check(users_set_admin(0, 1) == 0, "make an admin again");
    check(users_remove(0) == -EBUSY, "you can't remove yourself");
    check(users_need_login(), "with three users, a login is needed");

    /* What it saved, read again (a restart). */
    static char saved[4096];
    memcpy(saved, disk, (size_t)disk_len);
    saved[disk_len] = '\0';
    users_load();
    check(users_count() == 3 && users_current() == -1, "read back: three users, nobody logged in yet");
    check(users_check(0, "hunter2") && users_check(2, "pass word") && users_check(1, "x"),
          "...with their passwords (and none)");
    check(users_get(0)->admin && !users_get(1)->admin && users_get(2)->admin, "...and who's an admin");
    users_set_current(2);
    check(users_remove(1) == 0 && users_count() == 2 && users_current() == 1, "remove one (who's logged in moves down)");

    /* A damaged file: the bad lines are skipped, not trusted. */
    const char *bad = "user admin zz0000 00 - Bad hex\nuser root 3584e4 00000000000000000000000000000000 - Wrong role\n"
                      "user standard 3584e4 00000000000000000000000000000000 - Fine\n";
    settings_write_file("users.txt", bad, (uint32_t)strlen(bad));
    users_load();
    check(users_count() == 1 && !strcmp(users_get(0)->name, "Fine") && users_get(0)->admin,
          "a damaged users.txt: only good lines (and there's always an admin)");

    printf("file:\n%s", saved);
    printf("users: %s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}
