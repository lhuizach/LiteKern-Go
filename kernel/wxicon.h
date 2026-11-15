/* LiteKern Go: weather pictures, for the Weather app and the home screen's Weather widget. Drawn from
 * shapes (a sun, a cloud, rain, snow...) into any surface, at any size. Header only. */
#ifndef LKX_WXICON_H
#define LKX_WXICON_H

#include "kernel/gfx.h"

enum { WX_CLEAR, WX_PARTLY, WX_CLOUD, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM };

/* From an open-meteo / WMO weather code. */
static inline int wx_category(int code)
{
    if (code == 0)
        return WX_CLEAR;
    if (code <= 2)
        return WX_PARTLY;
    if (code == 3)
        return WX_CLOUD;
    if (code == 45 || code == 48)
        return WX_FOG;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
        return WX_RAIN;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        return WX_SNOW;
    if (code >= 95)
        return WX_STORM;
    return WX_CLOUD;
}

static inline const char *wx_text(int cat)
{
    static const char *const t[] = { "Clear", "Partly cloudy", "Cloudy", "Foggy", "Rain", "Snow", "Thunderstorm" };
    return t[cat < 0 || cat > WX_STORM ? 0 : cat];
}

static inline void wx_cloud(struct gfx_surface *s, int cx, int cy, int r, uint32_t col)
{
    gfx_fill_circle(s, cx - r * 45 / 100, cy + r * 8 / 100, r * 38 / 100, col);
    gfx_fill_circle(s, cx + r * 2 / 100, cy - r * 18 / 100, r * 50 / 100, col);
    gfx_fill_circle(s, cx + r * 48 / 100, cy + r * 10 / 100, r * 34 / 100, col);
    gfx_fill_rect(s, cx - r * 45 / 100, cy + r * 8 / 100, r * 93 / 100, r * 36 / 100, col);
}

static inline void wx_sun(struct gfx_surface *s, int cx, int cy, int r, uint32_t col)
{
    static const int dx[8] = { 0, 724, 1024, 724, 0, -724, -1024, -724 };
    static const int dy[8] = { -1024, -724, 0, 724, 1024, 724, 0, -724 };
    gfx_fill_circle(s, cx, cy, r * 52 / 100, col);
    for (int k = 0; k < 8; k++)
        gfx_thick_line(s, cx + dx[k] * r * 70 / 100 / 1024, cy + dy[k] * r * 70 / 100 / 1024, cx + dx[k] * r / 1024,
                       cy + dy[k] * r / 1024, r / 9 > 2 ? r / 9 : 2, col);
}

/* A picture about 2r wide, its middle at (cx, cy). night: a moon where there'd be a sun. */
static inline void wx_draw(struct gfx_surface *s, int cx, int cy, int r, int cat, int night)
{
    const uint32_t sun = 0xffc933, moon = 0xdfe6f2, white = 0xf2f5fa, grey = 0x9aa4b4, rain = 0x58a6ff;
    uint32_t body = night ? moon : sun;
    if (cat == WX_CLEAR) {
        if (night) {
            gfx_fill_circle(s, cx, cy, r * 60 / 100, moon);
            gfx_fill_circle(s, cx + r * 30 / 100, cy - r * 20 / 100, r * 50 / 100, 0x1d1d20);
        } else {
            wx_sun(s, cx, cy, r, body);
        }
    } else if (cat == WX_PARTLY) {
        if (night) {
            gfx_fill_circle(s, cx - r * 25 / 100, cy - r * 25 / 100, r * 40 / 100, moon);
        } else {
            wx_sun(s, cx - r * 28 / 100, cy - r * 30 / 100, r * 62 / 100, body);
        }
        wx_cloud(s, cx + r * 8 / 100, cy + r * 14 / 100, r * 80 / 100, white);
    } else if (cat == WX_CLOUD) {
        wx_cloud(s, cx - r * 18 / 100, cy - r * 18 / 100, r * 62 / 100, grey);
        wx_cloud(s, cx + r * 5 / 100, cy + r * 8 / 100, r * 82 / 100, white);
    } else if (cat == WX_FOG) {
        wx_cloud(s, cx, cy - r * 22 / 100, r * 80 / 100, grey);
        for (int k = 0; k < 3; k++)
            gfx_fill_round_rect(s, cx - r * (70 - k * 10) / 100, cy + r * (34 + k * 22) / 100, r * (140 - k * 20) / 100,
                                r / 10 > 2 ? r / 10 : 2, r / 20, white);
    } else if (cat == WX_RAIN) {
        wx_cloud(s, cx, cy - r * 22 / 100, r * 82 / 100, grey);
        for (int k = 0; k < 3; k++)
            gfx_thick_line(s, cx + r * (k * 36 - 40) / 100, cy + r * 40 / 100, cx + r * (k * 36 - 52) / 100,
                           cy + r * 78 / 100, r / 9 > 2 ? r / 9 : 2, rain);
    } else if (cat == WX_SNOW) {
        wx_cloud(s, cx, cy - r * 22 / 100, r * 82 / 100, grey);
        for (int k = 0; k < 3; k++)
            gfx_fill_circle(s, cx + r * (k * 38 - 38) / 100, cy + r * (50 + (k % 2) * 22) / 100, r / 10 > 2 ? r / 10 : 2, white);
    } else {
        wx_cloud(s, cx, cy - r * 22 / 100, r * 82 / 100, 0x6c7688);
        int bx = cx + r * 5 / 100, by = cy + r * 22 / 100, w = r / 7 > 2 ? r / 7 : 2;     /* the bolt */
        gfx_thick_line(s, bx + r * 12 / 100, by, bx - r * 10 / 100, by + r * 30 / 100, w, 0xffd23f);
        gfx_thick_line(s, bx - r * 10 / 100, by + r * 30 / 100, bx + r * 12 / 100, by + r * 30 / 100, w, 0xffd23f);
        gfx_thick_line(s, bx + r * 12 / 100, by + r * 30 / 100, bx - r * 6 / 100, by + r * 62 / 100, w, 0xffd23f);
    }
}

#endif
