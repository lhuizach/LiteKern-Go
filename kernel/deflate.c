/* LiteKern X — DEFLATE compression (see kernel/deflate.h). */
#include "kernel/deflate.h"
#include "kernel/errno.h"
#include "kernel/string.h"

#define WINDOW      32768
#define HASH_BITS   15
#define HASH_SIZE   (1 << HASH_BITS)
#define MAX_CHAIN   32          /* candidates tried for each match */
#define GOOD_ENOUGH 64          /* a match this long: stop looking */
#define MIN_MATCH   3
#define MAX_MATCH   258
#define BLOCK_SYMS  (2 * 32768 - 2)     /* symbols in a block (each 4 bytes) */
#define MATCH_FLAG  0x80000000u

/* The caller's working memory, carved up. */
struct work {
    int32_t head[HASH_SIZE];    /* the latest position of each hash */
    int32_t prev[WINDOW];       /* the one before it, by position % WINDOW */
    uint32_t sym[BLOCK_SYMS + 2];   /* literals, or MATCH_FLAG | (dist - 1) << 9 | len */
};
_Static_assert(sizeof(struct work) <= DEFLATE_WORK, "DEFLATE_WORK is too small");

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
                                       59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                       5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                        513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385,
                                        24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
                                        10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t cl_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static int len_code(int len)
{
    int c = 28;
    while (len_base[c] > len)
        c--;
    return c;
}

static int dist_code(int dist)
{
    int c = 29;
    while (dist_base[c] > dist)
        c--;
    return c;
}

/* --- bits ------------------------------------------------------------------------------ */

struct bits {
    uint8_t *p, *end;
    uint32_t acc;
    int n, full;
};

static void put(struct bits *b, uint32_t v, int n)
{
    b->acc |= v << b->n;
    b->n += n;
    while (b->n >= 8) {
        if (b->p < b->end)
            *b->p++ = (uint8_t)b->acc;
        else
            b->full = 1;
        b->acc >>= 8;
        b->n -= 8;
    }
}

static void flush_bits(struct bits *b)
{
    if (b->n)
        put(b, 0, 8 - b->n);
}

/* --- Huffman codes ---------------------------------------------------------------------- */

/* Code lengths (at most maxbits) for symbols with these frequencies: a plain
 * Huffman tree; if it's too deep, the frequencies are flattened and it's
 * made again. Symbols that never occur get 0. At least two get a length, so
 * the code is always complete. */
static void lengths(const uint32_t *freq_in, int n, int maxbits, uint8_t *len)
{
    uint32_t freq[288], weight[2 * 288];
    int16_t parent[2 * 288], sym_of[288];
    memcpy(freq, freq_in, sizeof(freq[0]) * (size_t)n);
    int used = 0;
    for (int i = 0; i < n; i++)
        used += freq[i] != 0;
    for (int i = 0; used < 2 && i < n; i++)
        if (!freq[i])
            freq[i] = 1, used++;            /* a second code for the lone one */
    for (;;) {
        int m = 0;
        for (int i = 0; i < n; i++)
            if (freq[i]) {
                sym_of[m] = (int16_t)i;
                weight[m] = freq[i];
                parent[m] = -1;
                m++;
            }
        /* Join the two lightest free nodes until one is left (n is small). */
        int nodes = m;
        for (int joins = 0; joins < m - 1; joins++) {
            int a = -1, b = -1;
            for (int i = 0; i < nodes; i++) {
                if (parent[i] != -1)
                    continue;
                if (a < 0 || weight[i] < weight[a])
                    b = a, a = i;
                else if (b < 0 || weight[i] < weight[b])
                    b = i;
            }
            weight[nodes] = weight[a] + weight[b];
            parent[nodes] = -1;
            parent[a] = parent[b] = (int16_t)nodes;
            nodes++;
        }
        int deepest = 0;
        memset(len, 0, (size_t)n);
        for (int i = 0; i < m; i++) {
            int d = 0;
            for (int k = i; parent[k] != -1; k = parent[k])
                d++;
            len[sym_of[i]] = (uint8_t)d;
            if (d > deepest)
                deepest = d;
        }
        if (deepest <= maxbits)
            return;
        for (int i = 0; i < n; i++)
            if (freq[i])
                freq[i] = (freq[i] >> 1) | 1;
    }
}

/* Canonical codes for the lengths (RFC 1951 3.2.2), bit-reversed for put(). */
static void codes(const uint8_t *len, int n, uint16_t *code)
{
    uint16_t count[16] = { 0 }, next[16];
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    uint16_t c = 0;
    for (int b = 1; b < 16; b++) {
        c = (uint16_t)((c + count[b - 1]) << 1);
        next[b] = c;
    }
    for (int i = 0; i < n; i++) {
        if (!len[i])
            continue;
        uint16_t v = next[len[i]]++, r = 0;
        for (int k = 0; k < len[i]; k++)
            r = (uint16_t)(r << 1 | ((v >> k) & 1));
        code[i] = r;
    }
}

/* --- blocks ------------------------------------------------------------------------------- */

static void block(struct bits *b, const uint32_t *sym, int nsym, int last)
{
    uint32_t lf[286] = { 0 }, df[30] = { 0 }, cf[19] = { 0 };
    for (int i = 0; i < nsym; i++) {
        uint32_t s = sym[i];
        if (s & MATCH_FLAG) {
            lf[257 + len_code((int)(s & 511))]++;
            df[dist_code((int)((s >> 9) & 0xffff) + 1)]++;
        } else {
            lf[s]++;
        }
    }
    lf[256] = 1;                            /* end of block */
    uint8_t ll[286], dl[30], cl[19];
    uint16_t lc[286], dc[30], cc[19];
    lengths(lf, 286, 15, ll);
    lengths(df, 30, 15, dl);
    codes(ll, 286, lc);
    codes(dl, 30, dc);
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !ll[hlit - 1])
        hlit--;
    while (hdist > 1 && !dl[hdist - 1])
        hdist--;

    /* The two tables' lengths, run-length coded with 16, 17 and 18. */
    uint8_t all[286 + 30], rle[286 + 30], extra[286 + 30];
    int na = 0, nr = 0;
    for (int i = 0; i < hlit; i++)
        all[na++] = ll[i];
    for (int i = 0; i < hdist; i++)
        all[na++] = dl[i];
    for (int i = 0; i < na;) {
        int run = 1;
        while (i + run < na && all[i + run] == all[i])
            run++;
        if (all[i] == 0 && run >= 3) {
            int k = run > 138 ? 138 : run;
            rle[nr] = k >= 11 ? 18 : 17;
            extra[nr++] = (uint8_t)(k >= 11 ? k - 11 : k - 3);
            i += k;
        } else if (all[i] != 0 && run >= 4) {
            rle[nr] = all[i];
            extra[nr++] = 0;
            int k = run - 1 > 6 ? 6 : run - 1;
            rle[nr] = 16;
            extra[nr++] = (uint8_t)(k - 3);
            i += 1 + k;
        } else {
            rle[nr] = all[i];
            extra[nr++] = 0;
            i++;
        }
    }
    for (int i = 0; i < nr; i++)
        cf[rle[i]]++;
    lengths(cf, 19, 7, cl);
    codes(cl, 19, cc);
    int hclen = 19;
    while (hclen > 4 && !cl[cl_order[hclen - 1]])
        hclen--;

    put(b, (uint32_t)last, 1);
    put(b, 2, 2);                           /* dynamic Huffman codes */
    put(b, (uint32_t)(hlit - 257), 5);
    put(b, (uint32_t)(hdist - 1), 5);
    put(b, (uint32_t)(hclen - 4), 4);
    for (int i = 0; i < hclen; i++)
        put(b, cl[cl_order[i]], 3);
    for (int i = 0; i < nr; i++) {
        put(b, cc[rle[i]], cl[rle[i]]);
        if (rle[i] == 16)
            put(b, extra[i], 2);
        else if (rle[i] == 17)
            put(b, extra[i], 3);
        else if (rle[i] == 18)
            put(b, extra[i], 7);
    }
    for (int i = 0; i < nsym; i++) {
        uint32_t s = sym[i];
        if (!(s & MATCH_FLAG)) {
            put(b, lc[s], ll[s]);
            continue;
        }
        int len = (int)(s & 511), dist = (int)((s >> 9) & 0xffff) + 1;
        int c = len_code(len), d = dist_code(dist);
        put(b, lc[257 + c], ll[257 + c]);
        if (len_extra[c])
            put(b, (uint32_t)(len - len_base[c]), len_extra[c]);
        put(b, dc[d], dl[d]);
        if (dist_extra[d])
            put(b, (uint32_t)(dist - dist_base[d]), dist_extra[d]);
    }
    put(b, lc[256], ll[256]);
}

/* --- LZ77 -------------------------------------------------------------------------------------- */

static uint32_t hash3(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u >> (32 - HASH_BITS);
}

int deflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, void *work_mem)
{
    struct work *w = work_mem;
    struct bits b = { out, out + out_len, 0, 0, 0 };
    for (int i = 0; i < HASH_SIZE; i++)
        w->head[i] = -1;
    int nsym = 0;
    size_t i = 0;
    while (i < in_len) {
        int best = 0, best_dist = 0;
        if (i + MIN_MATCH <= in_len) {
            uint32_t h = hash3(in + i);
            int32_t cand = w->head[h];
            w->prev[i % WINDOW] = cand;
            w->head[h] = (int32_t)i;
            size_t most = in_len - i < MAX_MATCH ? in_len - i : MAX_MATCH;
            for (int chain = 0; cand >= 0 && i - (size_t)cand <= WINDOW && chain < MAX_CHAIN; chain++) {
                const uint8_t *p = in + cand, *q = in + i;
                if (p[best] == q[best] && p[0] == q[0]) {
                    size_t n = 0;
                    while (n < most && p[n] == q[n])
                        n++;
                    if ((int)n > best) {
                        best = (int)n;
                        best_dist = (int)(i - (size_t)cand);
                        if (best >= GOOD_ENOUGH || n == most)
                            break;
                    }
                }
                int32_t next = w->prev[cand % WINDOW];
                if (next >= cand)
                    break;                  /* an old, overwritten link */
                cand = next;
            }
        }
        if (best >= MIN_MATCH) {
            w->sym[nsym++] = MATCH_FLAG | (uint32_t)(best_dist - 1) << 9 | (uint32_t)best;
            for (size_t k = i + 1; k < i + (size_t)best && k + MIN_MATCH <= in_len; k++) {
                uint32_t h = hash3(in + k);     /* the positions skipped: findable later */
                w->prev[k % WINDOW] = w->head[h];
                w->head[h] = (int32_t)k;
            }
            i += (size_t)best;
        } else {
            w->sym[nsym++] = in[i++];
        }
        if (nsym >= BLOCK_SYMS) {
            block(&b, w->sym, nsym, 0);
            nsym = 0;
            if (b.full)
                return -ENOSPC;
        }
    }
    block(&b, w->sym, nsym, 1);
    flush_bits(&b);
    return b.full ? -ENOSPC : (int)(b.p - out);
}
