/* R3lay on LiteKern Go: what the app's two halves share. api.c talks to
 * Supabase and keeps the data; main.c draws it. No calls in this version. */
#ifndef R3LAY_H
#define R3LAY_H

#define ID_LEN   37                     /* a uuid and its NUL */
#define WHEN_LEN 36                     /* a timestamp as the server writes it */
#define CONV_MAX 64
#define MSG_MAX  240
#define FOUND_MAX 20
#define TEXT_MAX 2000                   /* the longest message we send */

struct conv {
    int group;
    char id[ID_LEN];                    /* the other person, or the group */
    char name[40];
    char preview[100];
    char when[WHEN_LEN];                /* "": nothing said yet */
    char read_at[WHEN_LEN];             /* groups */
    int unread;
};

enum msg_state { MSG_OK, MSG_SENDING, MSG_FAILED };

struct msg {
    char id[ID_LEN], ref[ID_LEN], sender[ID_LEN], when[WHEN_LEN];
    char *raw;                          /* as sent (UTF-8): for a retry */
    char *shown;                        /* what our font can draw */
    int mine, read, tries;
    enum msg_state state;
    /* main.c's layout */
    int y, h, w, nlines;
    int *line;                          /* where each line starts in shown; nlines + 1 */
    int day_above, name_above;
};

struct person {
    char id[ID_LEN], name[40];
};

extern struct conv convs[CONV_MAX];
extern int nconvs, list_ready;          /* list_ready: it's been fetched at least once */
extern struct msg msgs[MSG_MAX];
extern int nmsgs, chat_ready;
extern struct person found[FOUND_MAX];
extern int nfound;
extern char me_id[ID_LEN], me_name[40];
extern int offline;                     /* the last request got no answer */

/* api.c calls these (main.c). */
void ui_changed(void);                  /* the data's different: redraw */
void ui_chat_grew(void);                /* a message came or went out */
void ui_signed_in(void);
void ui_signed_out(const char *why);
void ui_sign_in_failed(const char *why);

/* 0, or -1 with no usable configuration. */
int api_setup(void);
/* There's a saved session: starts it (ui_signed_in, or ui_signed_out). */
int api_resume(void);
void api_sign_in(const char *email, const char *password);
void api_sign_out(void);
void api_list_load(void);
void api_chat_open(const struct conv *c);
void api_chat_close(void);
void api_send(const char *text);
void api_find(const char *query);
/* Once a second: tokens, polling, retries. */
void api_tick(void);

const char *api_name(const char *id);   /* a person's name, if known; else "" */

/* What our font can draw of UTF-8 text (kernel/text.h: ASCII and a few
 * symbols): accents come off, emoji are left out for now. */
void show_text(const char *utf8, char *out, int room);
/* A timestamp as "14:05" today, or "7 Nov". */
void when_short(const char *when, char *out, int room);
/* The day of one: "Today", "Yesterday", "7 Nov 2026". */
void when_day(const char *when, char *out, int room);

#endif
