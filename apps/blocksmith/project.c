/* LiteKern X — Blocksmith: projects (see blocksmith.h).
 *
 * A project is a text file, /Blocksmith/<name>.blocks, on the first FAT32
 * volume that can be written:
 *
 *   blocksmith 1
 *   var <name>                                     one per variable
 *   block <id> <op> <x> <y> <next> <sub0> <sub1> <arg0..3> <inputs, tab-separated>
 *   top <id>                                       the scripts, back to front
 *
 * Ids are the file's own (0..n-1), -1 for none; ops by name (blocks.c). */
#include "apps/blocksmith/blocksmith.h"
#include "kernel/errno.h"
#include "kernel/string.h"
#include "sdk/kern86.h"

#define DIR       "/Blocksmith"
#define EXT       ".blocks"
#define FILE_MAX  (64 * 1024)

char project_name[48];
int project_dirty;

static char file[FILE_MAX];

/* --- the volume ------------------------------------------------------------------------------- */

static int volume(const char **why)
{
    static struct k86_volume v[8];
    int n = k86_volumes(v, 8);
    for (int i = 0; i < n && i < 8; i++)
        if (v[i].supported && !v[i].read_only)
            return i;
    *why = "There's no disk Blocksmith can save to.";
    return -1;
}

static void file_name(const char *name, char *out)
{
    str_copy(out, name, 48);
    int n = (int)strlen(out);
    str_copy(out + n, EXT, 64 - n);
}

/* --- starting points ------------------------------------------------------------------------- */

static void clear_all(void)
{
    run_stop();
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (!blocks[i].palette)
            blocks[i].used = 0;
    norder = 0;
    nvars = 0;
    error_block = NO;
}

void project_new_empty(void)
{
    clear_all();
    str_copy(var_names[0], "score", VAR_MAX);
    nvars = 1;
    project_name[0] = '\0';
    project_dirty = 0;
}

/* The starter's blocks: a block with what's typed in its inputs. */
static int mk(int op, const char *a, const char *b)
{
    int k = block_new(op, 0);
    if (k == NO)
        return NO;
    if (a)
        str_copy(blocks[k].lit[0], a, LIT_MAX);
    if (b)
        str_copy(blocks[k].lit[1], b, LIT_MAX);
    return k;
}

/* Join a stack: each block follows the one before. */
static int chain(const int *b, int n)
{
    for (int i = 1; i < n; i++)
        if (b[i] != NO && b[i - 1] != NO)
            block_attach(b[i], b[i - 1], PS_NEXT);
    return b[0];
}

static int var(const char *name)
{
    return mk(OP_VAR, name, 0);
}

/* "if x > hi then set dx to v" */
static int bounce(int op, const char *edge, const char *v)
{
    int c = mk(op, 0, edge), x = var("x"), body = mk(OP_SET, "dx", v), i = mk(OP_IF, 0, 0);
    block_attach(x, c, PS_ARG0);
    block_attach(c, i, PS_ARG0);
    block_attach(body, i, PS_SUB0);
    return i;
}

void project_starter(void)
{
    clear_all();
    str_copy(var_names[0], "x", VAR_MAX);
    str_copy(var_names[1], "dx", VAR_MAX);
    nvars = 2;

    /* A ball that bounces from side to side. */
    int circle = mk(OP_CIRCLE, 0, "120"), change = mk(OP_CHANGE, "x", 0);
    block_attach(var("x"), circle, PS_ARG0);
    str_copy(blocks[circle].lit[2], "24", LIT_MAX);
    block_attach(var("dx"), change, PS_ARG0 + 1);
    int loop[] = { mk(OP_CLEAR, "white", 0), mk(OP_COLOUR, "blue", 0), circle, change,
                   bounce(OP_GT, "296", "-4"), bounce(OP_LT, "24", "4") };
    int forever = mk(OP_FOREVER, 0, 0);
    block_attach(chain(loop, 6), forever, PS_SUB0);
    int run[] = { mk(OP_RUN, 0, 0), mk(OP_PRINT, "Hello from Blocksmith!", 0), mk(OP_SET, "x", "40"),
                  mk(OP_SET, "dx", "4"), forever };
    int top = chain(run, 5);
    blocks[top].x = 24;
    blocks[top].y = 28;
    order_add(top);
    layout_script(top);
    int below = blocks[top].y + blocks[top].sh + 40;

    /* Clicking the stage says where. */
    int join = mk(OP_JOIN, "You clicked at x = ", 0), print = mk(OP_PRINT, 0, 0);
    block_attach(mk(OP_MOUSEX, 0, 0), join, PS_ARG0 + 1);
    block_attach(join, print, PS_ARG0);
    int click[] = { mk(OP_CLICK, 0, 0), print };
    top = chain(click, 2);
    blocks[top].x = 24;
    blocks[top].y = (int16_t)below;
    order_add(top);

    project_name[0] = '\0';
    project_dirty = 0;
    layout_all();
}

/* --- saving ------------------------------------------------------------------------------------ */

struct out {
    char *p, *end;
    int full;
};

static void put(struct out *o, const char *s)
{
    while (*s) {
        if (o->p == o->end) {
            o->full = 1;
            return;
        }
        *o->p++ = *s++;
    }
}

static void put_int(struct out *o, int v)
{
    char t[12];
    format_int(v, t);
    put(o, t);
}

int project_save(const char *name, const char **why)
{
    int vol = volume(why);
    if (vol < 0)
        return -ENODEV;
    static int16_t id[MAX_BLOCKS];
    int n = 0;
    for (int i = 0; i < MAX_BLOCKS; i++)
        id[i] = blocks[i].used && !blocks[i].palette ? (int16_t)n++ : NO;
    struct out o = { file, file + FILE_MAX, 0 };
    put(&o, "blocksmith 1\n");
    for (int i = 0; i < nvars; i++) {
        put(&o, "var ");
        put(&o, var_names[i]);
        put(&o, "\n");
    }
    for (int i = 0; i < MAX_BLOCKS; i++) {
        struct block *k = &blocks[i];
        if (id[i] == NO)
            continue;
        int16_t links[] = { k->next, k->sub[0], k->sub[1], k->arg[0], k->arg[1], k->arg[2], k->arg[3] };
        put(&o, "block ");
        put_int(&o, id[i]);
        put(&o, " ");
        put(&o, ops[k->op].name);
        put(&o, " ");
        put_int(&o, k->x);
        put(&o, " ");
        put_int(&o, k->y);
        for (int j = 0; j < 7; j++) {
            put(&o, " ");
            put_int(&o, links[j] == NO ? -1 : id[links[j]]);
        }
        put(&o, " ");
        for (int j = 0; j < NARGS; j++) {
            if (j)
                put(&o, "\t");
            put(&o, k->lit[j]);
        }
        put(&o, "\n");
    }
    for (int i = 0; i < norder; i++) {
        put(&o, "top ");
        put_int(&o, id[order[i]]);
        put(&o, "\n");
    }
    if (o.full) {
        *why = "The project is too big to save.";
        return -ENOSPC;
    }
    int err = k86_create(vol, "/", "Blocksmith", 1);
    if (err && err != -EEXIST) {
        *why = "Couldn't make the Blocksmith folder.";
        return err;
    }
    char fname[64];
    file_name(name, fname);
    err = k86_write(vol, DIR, fname, file, (uint32_t)(o.p - file));
    if (err) {
        *why = err == -ENOSPC ? "The disk is full." : err == -EINVAL ? "That name can't be used for a file."
             : "Couldn't save the project.";
        return err;
    }
    str_copy(project_name, name, sizeof(project_name));
    project_dirty = 0;
    k86_logf("blocksmith: saved %s (%d blocks)", fname, n);
    return 0;
}

/* --- opening --------------------------------------------------------------------------------- */

static const char *word(const char *p, char *out, int size)
{
    while (*p == ' ')
        p++;
    int n = 0;
    while (*p && *p != ' ' && *p != '\n') {
        if (n < size - 1)
            out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return p;
}

static const char *number(const char *p, int *v, int *ok)
{
    char t[16];
    int32_t n = 0;
    p = word(p, t, sizeof(t));
    if (parse_int(t, &n) || !t[0])
        *ok = 0;
    *v = (int)n;
    return p;
}

static int find_op(const char *name)
{
    for (int i = 0; i < OPS; i++)
        if (str_eq(name, ops[i].name))
            return i;
    return -1;
}

static int16_t map[MAX_BLOCKS];
static int16_t links[MAX_BLOCKS][7];

static void mark(int b, uint8_t *seen)
{
    for (; b != NO && !seen[b]; b = blocks[b].next) {
        seen[b] = 1;
        for (int i = 0; i < NARGS; i++)
            mark(blocks[b].arg[i], seen);
        mark(blocks[b].sub[0], seen);
        mark(blocks[b].sub[1], seen);
    }
}

int project_open(const char *name, const char **why)
{
    int vol = volume(why);
    if (vol < 0)
        return -ENODEV;
    char fname[64];
    file_name(name, fname);
    int len = k86_read(vol, DIR, fname, file, FILE_MAX - 1);
    if (len < 0) {
        *why = "Couldn't open the project.";
        return len;
    }
    file[len] = '\0';
    if (len < 13 || memcmp(file, "blocksmith 1\n", 13))
        goto damaged;

    clear_all();
    int count = 0, ok = 1;
    for (int i = 0; i < MAX_BLOCKS; i++)
        map[i] = NO;
    for (const char *p = file; *p && ok; ) {
        char kw[16];
        p = word(p, kw, sizeof(kw));
        if (str_eq(kw, "var") && nvars < MAX_VARS) {
            word(p, var_names[nvars++], VAR_MAX);
        } else if (str_eq(kw, "block")) {
            char opname[16];
            int fid, x, y;
            p = number(p, &fid, &ok);
            p = word(p, opname, sizeof(opname));
            p = number(p, &x, &ok);
            p = number(p, &y, &ok);
            int op = find_op(opname);
            int b = op >= 0 ? block_new(op, 0) : NO;
            if (fid < 0 || fid >= MAX_BLOCKS || b == NO || map[fid] != NO) {
                ok = 0;
                break;
            }
            map[fid] = (int16_t)b;
            blocks[b].x = (int16_t)x;
            blocks[b].y = (int16_t)y;
            for (int j = 0; j < 7; j++) {
                int v;
                p = number(p, &v, &ok);
                links[b][j] = (int16_t)v;
            }
            if (*p == ' ')
                p++;
            for (int j = 0; j < NARGS; j++) {
                int n = 0;
                while (*p && *p != '\t' && *p != '\n') {
                    if (n < LIT_MAX - 1 && *p >= ' ' && *p <= '~')
                        blocks[b].lit[j][n++] = *p;
                    p++;
                }
                blocks[b].lit[j][n] = '\0';
                if (*p == '\t')
                    p++;
            }
            count++;
        } else if (str_eq(kw, "top")) {
            int fid;
            p = number(p, &fid, &ok);
            if (ok && fid >= 0 && fid < MAX_BLOCKS && map[fid] != NO)
                order_add(map[fid]);
        }
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }
    if (!ok)
        goto bad;

    /* Link: each block's parent is the one block that points to it. */
    for (int b = 0; b < MAX_BLOCKS; b++) {
        if (!blocks[b].used || blocks[b].palette)
            continue;
        int op = blocks[b].op, sh = ops[op].shape;
        for (int j = 0; j < 7; j++) {
            int f = links[b][j];
            if (f == -1)
                continue;
            int c = f >= 0 && f < MAX_BLOCKS ? map[f] : NO;
            int pslot = j == 0 ? PS_NEXT : j <= 2 ? PS_SUB0 + j - 1 : PS_ARG0 + j - 3;
            int fits = c != NO && blocks[c].parent == NO && c != b &&
                       (j == 0 ? block_has_next(b) && block_has_prev(c)
                        : j <= 2 ? (j == 1 ? sh == SH_C || sh == SH_ELSE || sh == SH_C_CAP : sh == SH_ELSE) &&
                                       block_has_prev(c)
                                 : block_slot_kind(op, j - 3) && block_is_value(c));
            if (!fits)
                goto bad;
            block_attach(c, b, pslot);
        }
    }
    /* Keep only what the scripts reach (a damaged file can't loop forever). */
    static uint8_t seen[MAX_BLOCKS];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < norder; i++) {
        if (blocks[order[i]].parent != NO)
            goto bad;
        mark(order[i], seen);
    }
    for (int b = 0; b < MAX_BLOCKS; b++)
        if (blocks[b].used && !blocks[b].palette && !seen[b])
            blocks[b].used = 0;
    str_copy(project_name, name, sizeof(project_name));
    project_dirty = 0;
    layout_all();
    k86_logf("blocksmith: opened %s (%d blocks)", fname, count);
    return 0;

bad:
    project_new_empty();
damaged:
    *why = "This project file is damaged.";
    return -EINVAL;
}

int project_list(char names[][48], int max, const char **why)
{
    static struct k86_dirent d[64];
    int vol = volume(why);
    if (vol < 0)
        return -ENODEV;
    int n = k86_list(vol, DIR, d, 64), out = 0;
    if (n < 0)
        return 0;                           /* no folder yet: no projects */
    int el = (int)strlen(EXT);
    for (int i = 0; i < n && i < 64 && out < max; i++) {
        int len = (int)strlen(d[i].name);
        if (d[i].is_dir || len <= el || len - el >= 48)
            continue;
        int match = 1;
        for (int j = 0; j < el; j++) {
            char c = d[i].name[len - el + j];
            match &= (c >= 'A' && c <= 'Z' ? c + 32 : c) == EXT[j];
        }
        if (!match)
            continue;
        str_copy(names[out], d[i].name, len - el + 1);
        out++;
    }
    return out;
}
