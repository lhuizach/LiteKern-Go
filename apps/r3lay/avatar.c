/* R3lay: profile pictures. Fetched from their public URLs when first drawn, a couple
 * at a time, then kept in memory as squares for as long as the app runs. */
#include "r3lay.h"
#include "http.h"
#include "apps/files/image.h"

#include <stdlib.h>
#include <string.h>

#define AVATARS 48
#define AT_ONCE 2

enum state { AV_EMPTY, AV_FETCHING, AV_READY, AV_NONE };

struct avatar {
    char id[ID_LEN];
    uint32_t *px;                       /* AVATAR_SIZE square, 0x00RRGGBB */
    enum state state;
};

static struct avatar avatars[AVATARS];
static int fetching;

/* The decoder asks for memory piece by piece: all freed together afterwards. */
#define PIECES 64
static void *pieces[PIECES];
static int npieces;

static void *piece(uint32_t n)
{
    if (npieces == PIECES)
        return 0;
    void *p = aligned_alloc(16, (n + 15u) & ~15u);
    if (p) {
        memset(p, 0, n);
        pieces[npieces++] = p;
    }
    return p;
}

/* The middle square of a picture, averaged down. */
static uint32_t *square(const struct image *im)
{
    uint32_t *out = malloc(sizeof(uint32_t) * AVATAR_SIZE * AVATAR_SIZE);
    int side = im->w < im->h ? im->w : im->h, left = (im->w - side) / 2, top = (im->h - side) / 2;
    int step = side / AVATAR_SIZE > 0 ? side / AVATAR_SIZE : 1;
    if (!out)
        return 0;
    for (int y = 0; y < AVATAR_SIZE; y++)
        for (int x = 0; x < AVATAR_SIZE; x++) {
            int sx = left + x * side / AVATAR_SIZE, sy = top + y * side / AVATAR_SIZE;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int j = 0; j < step && sy + j < im->h; j++)
                for (int i = 0; i < step && sx + i < im->w; i++) {
                    uint32_t c = im->px[(sy + j) * im->w + sx + i], a = c >> 24;
                    r += (c >> 16 & 255) * a / 255;
                    g += (c >> 8 & 255) * a / 255;
                    b += (c & 255) * a / 255;
                    n++;
                }
            out[y * AVATAR_SIZE + x] = n ? (r / n) << 16 | (g / n) << 8 | (b / n) : 0;
        }
    return out;
}

static void on_picture(void *ctx, long status, char *body, size_t len)
{
    struct avatar *a = ctx;
    struct image im;
    const char *why = "";
    fetching--;
    a->state = AV_NONE;
    if (status == 200 && len && !image_decode((const uint8_t *)body, (uint32_t)len, piece, &im, &why)) {
        a->px = square(&im);
        if (a->px)
            a->state = AV_READY;
    }
    while (npieces)
        free(pieces[--npieces]);
    ui_changed();
}

const uint32_t *api_avatar(const char *id)
{
    struct avatar *a = 0;
    for (int i = 0; i < AVATARS && !a; i++)
        if (!strcmp(avatars[i].id, id))
            a = &avatars[i];
    for (int i = 0; i < AVATARS && !a; i++)
        if (!avatars[i].id[0]) {
            a = &avatars[i];
            strcpy(a->id, id);
        }
    if (!a)
        return 0;
    if (a->state == AV_EMPTY && fetching < AT_ONCE) {
        const char *url = api_avatar_url(id);
        if (url[0]) {
            if (http_request("GET", url, 0, 0, on_picture, a))
                a->state = AV_NONE;
            else {
                a->state = AV_FETCHING;
                fetching++;
            }
        }
    }
    return a->state == AV_READY ? a->px : 0;
}
