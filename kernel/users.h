/* LiteKern X — users (Phase 5 §5, stage 2).
 *
 * Up to USERS_MAX people share the machine, each with a name, an avatar
 * colour, an optional password, and whether they're an admin (admins add
 * and remove users; stage 3 lets them see everyone's files). Kept on the
 * boot disk, in /LiteKern/users.txt, one a line:
 *
 *   user admin|standard <rrggbb> <salt, 32 hex> <hash, 64 hex, or -> <name>
 *
 * Passwords are never kept as typed: the hash is SHA-256 of the salt and
 * the password, then of that, the salt and the password again, PASS_ROUNDS
 * times in all (kernel/sha256.h), so guessing is slow. The stick itself
 * isn't encrypted: a password keeps people off this machine, not out of
 * the stick.
 *
 * With no users.txt (a new stick) there's one admin, "User", without a
 * password: the machine starts straight to the desktop, as before users
 * existed. A login screen is needed only when there's more than one user,
 * or the one has a password. */
#ifndef LKX_USERS_H
#define LKX_USERS_H

#include <stdint.h>

#define USERS_MAX       8
#define USER_NAME_MAX   21              /* 20 characters, NUL included */
#define PASSWORD_MAX    64
#define PASS_ROUNDS     20000

struct user {
    char name[USER_NAME_MAX];
    int admin;
    uint32_t colour;                    /* the avatar's */
    int has_password;
    uint8_t salt[16], hash[32];
};

/* At boot, after the disks (settings_load()). Logs what it found. */
void users_load(void);

int users_count(void);
const struct user *users_get(int i);
int users_find(const char *name);       /* its index, or -1 */

/* Who's logged in: an index, or -1 while the login screen shows. */
int users_current(void);
void users_set_current(int i);

/* At boot: does someone have to log in? */
int users_need_login(void);

/* Is this their password? (Anything is, without one.) 1 or 0. */
int users_check(int i, const char *password);

/* Changes, each saved at once. 0, or -EINVAL (a bad name or password),
 * -EEXIST (that name is taken), -ENOSPC (USERS_MAX already), -EBUSY (the
 * last admin, or the one logged in, can't be removed or stop being admin). */
int users_add(const char *name, const char *password, int admin);
int users_set_password(int i, const char *password);    /* "": none */
int users_rename(int i, const char *name);
int users_set_admin(int i, int admin);
int users_remove(int i, int delete_files);     /* their home folder too, if asked */

/* NULL if a name can be used, else why not, for the user. */
const char *users_bad_name(const char *name);

#endif
