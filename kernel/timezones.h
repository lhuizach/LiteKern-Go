/* LiteKern X — the time zones Settings offers (the Linux port's phase L4),
 * shared by Settings' System page and the shell (platform/machine.c). Each is
 * a POSIX TZ rule: its offset and, where it has one, when summer time starts
 * and ends, so no time zone database is needed. A city stands for its zone.
 *
 * Saved by name (/LiteKern/machine.txt): new ones go anywhere, but names
 * stay. */
#ifndef LKX_TIMEZONES_H
#define LKX_TIMEZONES_H

struct lkx_zone {
    const char *name;
    const char *rule;
};

static const struct lkx_zone lkx_zones[] = {
    { "London",                 "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Dublin",                 "GMT0IST,M3.5.0/1,M10.5.0" },
    { "Lisbon",                 "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Paris, Brussels",        "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Berlin, Amsterdam",      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Madrid, Rome",           "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Warsaw, Prague",         "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Athens, Helsinki",       "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Istanbul",               "<+03>-3" },
    { "Moscow",                 "MSK-3" },
    { "Dubai",                  "<+04>-4" },
    { "India",                  "IST-5:30" },
    { "Bangkok, Jakarta",       "<+07>-7" },
    { "China, Singapore",       "CST-8" },
    { "Tokyo, Seoul",           "JST-9" },
    { "Sydney, Melbourne",      "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Auckland",               "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Honolulu",               "HST10" },
    { "Los Angeles, Vancouver", "PST8PDT,M3.2.0,M11.1.0" },
    { "Denver",                 "MST7MDT,M3.2.0,M11.1.0" },
    { "Chicago, Dallas","CST6CDT,M3.2.0,M11.1.0" },
    { "New York, Toronto",      "EST5EDT,M3.2.0,M11.1.0" },
    { "Sao Paulo",              "<-03>3" },
    { "UTC",                    "UTC0" },
};
#define LKX_ZONES ((int)(sizeof(lkx_zones) / sizeof(lkx_zones[0])))
#define LKX_ZONE_DEFAULT 0      /* London */

#endif
