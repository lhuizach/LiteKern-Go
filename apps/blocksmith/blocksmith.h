/* LiteKern X — Blocksmith, a KERN86 app: block coding, natively.
 *
 * The native version of the Blocksmith web app. Blocks
 * snap together into scripts, as in Scratch or Blockly, and run right here:
 * they draw on a 320 x 240 stage and print to a console. Apps have no
 * floating point, so numbers are whole numbers.
 *
 *   blocks.c    the blocks: what each one is, the pool, layout, drawing, hit tests
 *   run.c       running scripts (threads that take turns), the stage, the console
 *   project.c   saving and opening projects (/Blocksmith/<name>.blocks), the starter
 *   main.c      the window: palette, workspace, dragging, editing, dialogs */
#ifndef BLOCKSMITH_H
#define BLOCKSMITH_H

#include <stdint.h>
#include "kernel/gfx.h"

#define NO          (-1)
#define MAX_BLOCKS  480
#define NARGS       4
#define LIT_MAX     24          /* a value typed into a block, NUL included */
#define MAX_VARS    12
#define VAR_MAX     16          /* a variable's name, NUL included */
#define TEXT_MAX    64          /* a text value while running */

/* --- what a block is -------------------------------------------------------------- */

enum shape {
    SH_HAT,         /* starts a script: "when Run clicked" */
    SH_STACK,       /* one step */
    SH_C,           /* holds a stack: "repeat", "if" */
    SH_ELSE,        /* holds two: "if ... else" */
    SH_C_CAP,       /* holds a stack, nothing can follow: "forever" */
    SH_CAP,         /* nothing can follow: "stop" */
    SH_NUM,         /* a round value: numbers and text */
    SH_BOOL,        /* a pointed value: yes or no */
};

enum cat { CAT_EVENTS, CAT_CONTROL, CAT_STAGE, CAT_MATHS, CAT_VARS, CAT_SENSING, CATS };

/* Inputs in a block's spec: "%n" a number, "%s" text, "%b" a yes/no value,
 * "%v" a variable, "%k" a key, "%c" a colour (the last three are menus). */
enum op {
    OP_RUN, OP_KEY, OP_CLICK,
    OP_WAIT, OP_REPEAT, OP_FOREVER, OP_IF, OP_IFELSE, OP_UNTIL, OP_STOP,
    OP_CLEAR, OP_COLOUR, OP_RGB, OP_CIRCLE, OP_RECT, OP_LINE, OP_WRITE, OP_PRINT,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_RANDOM, OP_LT, OP_EQ, OP_GT,
    OP_AND, OP_OR, OP_NOT, OP_JOIN,
    OP_SET, OP_CHANGE, OP_VAR,
    OP_MOUSEX, OP_MOUSEY, OP_MOUSEDOWN, OP_KEYDOWN, OP_TIMER,
    OPS
};

struct op_def {
    const char *name;           /* in project files: never change one */
    const char *spec;           /* words and inputs, separated by spaces */
    uint8_t shape, cat;
    const char *defaults[NARGS];
};
extern const struct op_def ops[OPS];
extern const char *const cat_names[CATS];
uint32_t cat_colour(int cat);

/* Where a block hangs off its parent. */
enum { PS_TOP = -1, PS_NEXT = 0, PS_SUB0, PS_SUB1, PS_ARG0 };

struct block {
    uint8_t used, op, palette;  /* palette: one of the palette's templates */
    int8_t pslot;               /* PS_*: how it's attached to parent */
    int16_t parent, next, sub[2], arg[NARGS];
    int16_t x, y;               /* top left, in workspace (or palette) coordinates */
    int16_t w, h;               /* the block; C blocks: the whole shape */
    int16_t row_h;              /* its first row */
    int16_t mid_y, bot_y;       /* C blocks: where the "else" row and the bottom bar start */
    int16_t sw, sh;             /* a script's top block: the whole script's size */
    int16_t slot_x[NARGS], slot_y[NARGS], slot_w[NARGS], slot_h[NARGS];   /* from x, y */
    char lit[NARGS][LIT_MAX];   /* what's typed (or chosen) in each input */
};

extern struct block blocks[MAX_BLOCKS];

/* Scripts, back to front (the last one is drawn on top). */
extern int16_t order[MAX_BLOCKS];
extern int norder;

/* Variables: the project's names. */
extern char var_names[MAX_VARS][VAR_MAX];
extern int nvars;

/* --- blocks.c ----------------------------------------------------------------------- */
int block_new(int op, int palette);             /* NO if the pool is full */
void block_free_tree(int b);                    /* it, what's inside it, and what follows */
int block_copy_tree(int b, int palette);        /* a copy of it and its inputs (not what follows) */
int block_nargs(int op);
char block_slot_kind(int op, int i);            /* 'n', 's', 'b', 'v', 'k', 'c' */
int block_has_next(int b);                      /* something can follow it */
int block_has_prev(int b);                      /* it can follow something */
int block_is_value(int b);
int block_last(int b);                          /* the last block of the stack starting at b */
int block_top(int b);                           /* the script's top block */

void order_add(int b);
void order_remove(int b);
void order_front(int b);

/* Attach / detach. */
void block_detach(int b);                       /* b (and what follows) becomes a script */
void block_attach(int b, int parent, int pslot);

/* Layout: positions and sizes of everything in the script at top. */
void layout_script(int top);
void layout_all(void);
void layout_move(int top, int x, int y);        /* a script moved: place it again */

/* Draw a script (or a palette block) into s, which shows the plane from (ox, oy). */
void draw_script(struct gfx_surface *s, int ox, int oy, int top);
void draw_block_only(struct gfx_surface *s, int ox, int oy, int b);   /* one palette block */
/* A highlight: the block that failed. */
extern int error_block;

/* What's at (x, y) in the script at top: the block, and the input (-1: the block itself). */
int hit_script(int top, int x, int y, int *slot);

/* Menus. */
int menu_count(char kind);
const char *menu_item(char kind, int i);
int colour_value(const char *name, uint32_t *out);    /* 0 if it's a colour's name */

/* Where a dragged script can snap. */
struct snap {
    int ok;
    int target, pslot;          /* attach to target at pslot (PS_TOP: above the script target) */
    int x, y, w, h;             /* the marker to draw, in workspace coordinates */
};
struct snap find_snap(int drag, int range);
void apply_snap(int drag, const struct snap *s);

/* --- run.c ---------------------------------------------------------------------------- */
#define STAGE_W 320
#define STAGE_H 240
extern uint32_t stage_px[STAGE_W * STAGE_H];
extern struct gfx_surface stage;

struct console_line { char text[56]; uint8_t error; };
#define CONSOLE_LINES 48
extern struct console_line console[CONSOLE_LINES];
extern int nconsole;

int run_active(void);                   /* scripts running, or waiting for keys and clicks */
int run_threads(void);                  /* how many scripts are running now */
void run_start(void);                   /* Run: reset, start every "when Run clicked" */
void run_script(int top);               /* run one script (it was clicked) */
void run_stop(void);
void run_event_key(int key_index);      /* a key in the key menu went down */
void run_event_click(void);             /* the stage was clicked */
void run_set_key(int key_index, int down);
void run_set_mouse(int x, int y, int down);   /* stage coordinates */
/* One turn of every script. Returns what changed: RUN_STAGE, RUN_CONSOLE, RUN_ERROR. */
#define RUN_STAGE   1
#define RUN_CONSOLE 2
#define RUN_ERROR   4
#define RUN_ENDED   8
int run_tick(void);
void run_forget_block(int b);           /* b is being deleted: stop what's running it */
void console_clear(void);
void console_add(const char *text, int error);
void stage_reset(void);

/* --- project.c ---------------------------------------------------------------------------- */
extern char project_name[48];           /* "" until saved */
extern int project_dirty;
void project_new_empty(void);
void project_starter(void);
/* 0 or a negative errno; why: a message for the user. */
int project_save(const char *name, const char **why);
int project_open(const char *name, const char **why);
/* The saved projects' names (without ".blocks"). */
int project_list(char names[][48], int max, const char **why);

/* --- utilities (blocks.c) --------------------------------------------------------- */
void str_copy(char *dst, const char *src, int size);
int str_eq(const char *a, const char *b);
int parse_int(const char *s, int32_t *out);     /* 0 if s is a whole number ("" is 0) */
void format_int(int32_t v, char *out);

#endif
