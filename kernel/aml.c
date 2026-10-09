/* LiteKern X — reading AML declarations (kernel/aml.h). Opcodes and
 * encodings: ACPI 6.x, chapter 20 (AML grammar). */
#include "kernel/aml.h"
#include "kernel/string.h"

#define NAME_OP         0x08
#define BYTE_PREFIX     0x0a
#define WORD_PREFIX     0x0b
#define DWORD_PREFIX    0x0c
#define STRING_PREFIX   0x0d
#define QWORD_PREFIX    0x0e
#define SCOPE_OP        0x10
#define BUFFER_OP       0x11
#define PACKAGE_OP      0x12
#define METHOD_OP       0x14
#define RETURN_OP       0xa4
#define ONES_OP         0xff
#define DUAL_NAME       0x2e
#define MULTI_NAME      0x2f
#define EXT_OP          0x5b
#define EXT_REGION      0x80
#define EXT_FIELD       0x81
#define EXT_DEVICE      0x82
#define EXT_PROCESSOR   0x83
#define EXT_THERMAL     0x85
#define EXT_INDEX_FIELD 0x86

void acpi_body(const struct acpi_sdt *t, const uint8_t **p, const uint8_t **end)
{
    *p = (const uint8_t *)t + sizeof(*t);
    *end = (const uint8_t *)t + t->length;
}

int aml_int(const uint8_t **p, const uint8_t *end, uint32_t *v)
{
    const uint8_t *q = *p;
    if (q >= end)
        return -1;
    switch (*q) {
    case 0x00: *v = 0; *p = q + 1; return 0;                    /* ZeroOp */
    case 0x01: *v = 1; *p = q + 1; return 0;                    /* OneOp */
    case ONES_OP: *v = 0xffffffffu; *p = q + 1; return 0;
    case BYTE_PREFIX:
        if (q + 2 > end) return -1;
        *v = q[1]; *p = q + 2; return 0;
    case WORD_PREFIX:
        if (q + 3 > end) return -1;
        *v = q[1] | q[2] << 8; *p = q + 3; return 0;
    case DWORD_PREFIX:
    case QWORD_PREFIX: {
        const uint8_t *next = q + (*q == DWORD_PREFIX ? 5 : 9);
        if (next > end) return -1;
        *v = q[1] | q[2] << 8 | (uint32_t)q[3] << 16 | (uint32_t)q[4] << 24;
        *p = next;
        return 0;
    }
    default:
        return -1;
    }
}

/* PkgLength: the lead byte's top two bits say how many bytes follow. The
 * length counts from the PkgLength itself. Advances *p past it. */
static int pkglen(const uint8_t **p, const uint8_t *end, uint32_t *len)
{
    const uint8_t *q = *p;
    if (q >= end)
        return -1;
    int n = *q >> 6;
    if (q + 1 + n > end)
        return -1;
    uint32_t v;
    if (!n) {
        v = *q & 0x3f;
    } else {
        if (*q & 0x30)
            return -1;                  /* must be zero with following bytes */
        v = *q & 0x0f;
        for (int i = 0; i < n; i++)
            v |= (uint32_t)q[1 + i] << (4 + 8 * i);
    }
    *len = v;
    *p = q + 1 + n;
    return 0;
}

static int lead_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || c == '_';
}

static int name_char(uint8_t c)
{
    return lead_char(c) || (c >= '0' && c <= '9');
}

static int is_seg(const uint8_t *q, const uint8_t *end)
{
    return q + 4 <= end && lead_char(q[0]) && name_char(q[1]) && name_char(q[2]) && name_char(q[3]);
}

/* A segment for people: "EC0_" -> "EC0", "_SB_" -> "_SB". */
static int put_seg(char *out, int o, int max, const uint8_t *seg)
{
    int n = 4;
    while (n > 1 && seg[n - 1] == '_')
        n--;
    for (int i = 0; i < n && o < max - 1; i++)
        out[o++] = (char)seg[i];
    return o;
}

/* NameString: prefixes, then one, two (DualNamePrefix) or n segments, or
 * NullName. path: printable; last: the final segment, raw. */
static int namestring(const uint8_t **p, const uint8_t *end, char *path, int max, char *last)
{
    const uint8_t *q = *p;
    int o = 0;
    if (q < end && *q == '\\') {
        path[o++] = '\\';
        q++;
    } else {
        while (q < end && *q == '^' && o < max - 2) {
            path[o++] = '^';
            q++;
        }
    }
    if (q >= end)
        return -1;
    int segs;
    if (*q == DUAL_NAME) {
        segs = 2;
        q++;
    } else if (*q == MULTI_NAME) {
        if (q + 2 > end)
            return -1;
        segs = q[1];
        q += 2;
    } else if (*q == 0) {
        segs = 0;
        q++;
    } else {
        segs = 1;
    }
    if (segs > 16 || q + 4 * segs > end)
        return -1;
    last[0] = '\0';
    for (int i = 0; i < segs; i++, q += 4) {
        if (!is_seg(q, end))
            return -1;
        if (i && o < max - 1)
            path[o++] = '.';
        o = put_seg(path, o, max, q);
        memcpy(last, q, 4);
        last[4] = '\0';
    }
    path[o] = '\0';
    *p = q;
    return 0;
}

/* --- scopes ----------------------------------------------------------------- */

/* EISAID("PNP0C09") is stored as a DWord: three 5-bit letters, four hex digits. */
static void eisaid(uint32_t v, char *out)
{
    uint32_t id = (v & 0xff) << 24 | (v >> 8 & 0xff) << 16 | (v >> 16 & 0xff) << 8 | v >> 24;
    out[0] = (char)('@' + (id >> 26 & 0x1f));
    out[1] = (char)('@' + (id >> 21 & 0x1f));
    out[2] = (char)('@' + (id >> 16 & 0x1f));
    for (int i = 0; i < 4; i++)
        out[3 + i] = "0123456789ABCDEF"[id >> (12 - 4 * i) & 0xf];
    out[7] = '\0';
}

/* Name(_HID, ...) at q (pointing at "_HID"): its value. */
static int hid_value(const uint8_t *q, const uint8_t *end, char *out, int max)
{
    q += 4;
    if (q + 5 <= end && *q == DWORD_PREFIX) {
        eisaid(q[1] | q[2] << 8 | (uint32_t)q[3] << 16 | (uint32_t)q[4] << 24, out);
        return 0;
    }
    if (q < end && *q == STRING_PREFIX) {
        int o = 0;
        for (q++; q < end && *q && o < max - 1; q++) {
            if (*q < 0x20 || *q > 0x7e)
                return -1;
            out[o++] = (char)*q;
        }
        out[o] = '\0';
        return o ? 0 : -1;
    }
    return -1;
}

int aml_scopes(const uint8_t *p, const uint8_t *end, struct aml_scope *out, int max)
{
    int n = 0;
    for (const uint8_t *q = p; q + 6 < end; q++) {
        int kind;
        const uint8_t *r;
        if (q[0] == EXT_OP && q[1] == EXT_DEVICE)
            kind = AML_DEVICE, r = q + 2;
        else if (q[0] == EXT_OP && q[1] == EXT_THERMAL)
            kind = AML_THERMAL, r = q + 2;
        else if (q[0] == EXT_OP && q[1] == EXT_PROCESSOR)
            kind = AML_PROCESSOR, r = q + 2;
        else if (q[0] == SCOPE_OP)
            kind = AML_SCOPE, r = q + 1;
        else
            continue;
        const uint8_t *body = r;
        uint32_t len;
        if (pkglen(&body, end, &len) || len < 2 || r + len > end)
            continue;
        char path[40], last[5];
        const uint8_t *after = body;
        if (namestring(&after, r + len, path, sizeof(path), last) || !last[0])
            continue;
        /* Scope() is one byte, so data can look like it: only take the
         * usual ones, which name the root or one of its children. */
        if (kind == AML_SCOPE && path[0] != '\\' && last[0] != '_')
            continue;
        if (n < max) {
            struct aml_scope *s = &out[n];
            memset(s, 0, sizeof(*s));
            s->kind = (uint8_t)kind;
            memcpy(s->name, last, 5);
            memcpy(s->path, path, sizeof(path));
            s->start = after;
            s->end = r + len;
            s->parent = -1;
        }
        n++;
    }
    int filled = n < max ? n : max;
    /* Parents: the smallest other scope around each one. */
    for (int i = 0; i < filled; i++) {
        int best = -1;
        for (int j = 0; j < filled; j++) {
            if (j == i || out[j].start > out[i].start || out[j].end < out[i].end)
                continue;
            if (out[j].start == out[i].start && out[j].end == out[i].end)
                continue;
            if (best < 0 || out[j].end - out[j].start < out[best].end - out[best].start)
                best = j;
        }
        out[i].parent = best;
    }
    /* _HIDs: each belongs to the innermost scope around it. */
    for (const uint8_t *q = p + 1; q + 5 < end; q++) {
        if (q[-1] != NAME_OP || memcmp(q, "_HID", 4))
            continue;
        int best = -1;
        for (int j = 0; j < filled; j++)
            if (out[j].start <= q && q < out[j].end &&
                (best < 0 || out[j].end - out[j].start < out[best].end - out[best].start))
                best = j;
        if (best >= 0 && !out[best].hid[0])
            hid_value(q, end, out[best].hid, sizeof(out[best].hid));
    }
    return n;
}

void aml_path(const struct aml_scope *scopes, int n, int i, char *out, int max)
{
    int chain[16], depth = 0;
    for (int k = i; k >= 0 && k < n && depth < 16; k = scopes[k].parent) {
        chain[depth++] = k;
        if (scopes[k].path[0] == '\\')
            break;                      /* absolute: nothing above counts */
    }
    int o = 0;
    out[0] = '\0';
    /* Scope(_SB) at the top of a table is the root's child: "\_SB". */
    if (depth && scopes[chain[depth - 1]].path[0] != '\\' && scopes[chain[depth - 1]].path[0] != '^' &&
        max > 1)
        out[o++] = '\\';
    for (int d = depth - 1; d >= 0; d--) {
        const char *s = scopes[chain[d]].path;
        if (o > 0 && out[o - 1] != '\\' && o < max - 1)
            out[o++] = '.';
        while (*s && o < max - 1)
            out[o++] = *s++;
        out[o] = '\0';
    }
}

/* --- regions and fields -------------------------------------------------------- */

const char *aml_space_name(uint8_t space)
{
    static const char *const names[] = { "SystemMemory", "SystemIO", "PCI_Config",
                                         "EmbeddedControl", "SMBus", "SystemCMOS",
                                         "PciBarTarget", "IPMI", "GPIO", "GenericSerialBus", "PCC" };
    if (space < sizeof(names) / sizeof(names[0]))
        return names[space];
    return space >= 0x80 ? "OEM" : "unknown";
}

int aml_regions(const uint8_t *p, const uint8_t *end, struct aml_region *out, int max)
{
    int n = 0;
    for (const uint8_t *q = p; q + 8 < end; q++) {
        if (q[0] != EXT_OP || q[1] != EXT_REGION)
            continue;
        const uint8_t *r = q + 2;
        char path[40], last[5];
        if (namestring(&r, end, path, sizeof(path), last) || !last[0] || r >= end)
            continue;
        uint8_t space = *r++;
        if (space > 0x0a && space < 0x80)
            continue;
        struct aml_region reg = { .space = space };
        memcpy(reg.name, last, 5);
        if (aml_int(&r, end, &reg.offset) || aml_int(&r, end, &reg.len))
            reg.dynamic = 1;
        if (n < max)
            out[n] = reg;
        n++;
    }
    return n;
}

/* A FieldList from q to fend; each named element becomes a field. */
static int field_list(const uint8_t *q, const uint8_t *fend, const char *region, int indexed,
                      struct aml_field *out, int n, int max)
{
    uint32_t bit = 0, w;
    while (q < fend) {
        if (*q == 0x00) {                       /* ReservedField: skip bits (Offset()) */
            q++;
            if (pkglen(&q, fend, &w))
                break;
            bit += w;
        } else if (*q == 0x01) {                /* AccessField */
            q += 3;
        } else if (*q == 0x03) {                /* ExtendedAccessField */
            q += 4;
        } else if (is_seg(q, fend)) {           /* NamedField */
            const uint8_t *name = q;
            q += 4;
            if (pkglen(&q, fend, &w) || w > 0x10000)
                break;
            if (n < max) {
                struct aml_field *f = &out[n];
                memcpy(f->name, name, 4);
                f->name[4] = '\0';
                memcpy(f->region, region, 5);
                f->indexed = (uint8_t)indexed;
                f->bit = bit;
                f->width = w;
            }
            n++;
            bit += w;
        } else {
            break;                              /* ConnectField or something odd: stop */
        }
    }
    return n;
}

int aml_fields(const uint8_t *p, const uint8_t *end, const char *region, struct aml_field *out,
               int max)
{
    int n = 0;
    for (const uint8_t *q = p; q + 8 < end; q++) {
        if (q[0] != EXT_OP || (q[1] != EXT_FIELD && q[1] != EXT_INDEX_FIELD))
            continue;
        int indexed = q[1] == EXT_INDEX_FIELD;
        const uint8_t *r = q + 2, *pk = r;
        uint32_t len;
        if (pkglen(&r, end, &len) || pk + len > end || len < 6)
            continue;
        const uint8_t *fend = pk + len;
        char path[40], last[5], data[5];
        if (namestring(&r, fend, path, sizeof(path), last) || !last[0])
            continue;
        if (indexed && (namestring(&r, fend, path, sizeof(path), data) || !data[0]))
            continue;
        if (r >= fend || (*r & 0x0f) > 5)       /* FieldFlags: access type 0-5 */
            continue;
        r++;
        if (region && (indexed || memcmp(last, region, 4)))
            continue;
        n = field_list(r, fend, last, indexed, out, n, max);
    }
    return n;
}

/* --- constants -------------------------------------------------------------------- */

int aml_const(const uint8_t *p, const uint8_t *end, const char *seg, uint32_t *v)
{
    for (const uint8_t *q = p + 1; q + 5 < end; q++) {
        if (memcmp(q, seg, 4))
            continue;
        const uint8_t *r = q + 4;
        if (q[-1] == NAME_OP && !aml_int(&r, end, v))
            return 0;
        /* Method(seg, flags) { Return (n) }: MethodOp PkgLength seg flags ReturnOp n */
        for (int back = 2; back <= 5 && q - back >= p; back++) {
            if (q[-back] != METHOD_OP)
                continue;
            const uint8_t *m = q - back + 1;
            uint32_t len;
            if (pkglen(&m, end, &len) || m != q)
                continue;
            r = q + 5;
            if (r < end && *r == RETURN_OP) {
                r++;
                if (!aml_int(&r, end, v))
                    return 0;
            }
        }
    }
    return -1;
}

int aml_pstates(const uint8_t *p, const uint8_t *end, char *name, struct aml_pstate *out, int max)
{
    for (const uint8_t *q = p; q + 8 < end; q++) {
        if (q[0] != NAME_OP || !is_seg(q + 1, end) || q[5] != PACKAGE_OP)
            continue;
        const uint8_t *r = q + 6, *pk = r;
        uint32_t len;
        if (pkglen(&r, end, &len) || pk + len > end || r >= pk + len)
            continue;
        const uint8_t *pend = pk + len;
        int count = *r++, n = 0, ok = count > 0;
        for (int i = 0; ok && i < count; i++) {
            const uint8_t *sp = r + 1;
            uint32_t slen, v[6];
            if (r >= pend || *r != PACKAGE_OP || pkglen(&sp, pend, &slen) || sp >= pend || *sp != 6) {
                ok = 0;
                break;
            }
            const uint8_t *e = sp + 1;
            for (int k = 0; k < 6 && ok; k++)
                ok = !aml_int(&e, pend, &v[k]);
            if (!ok || v[0] < 100 || v[0] > 10000) {
                ok = 0;
                break;
            }
            if (n < max)
                out[n] = (struct aml_pstate){ v[0], v[1], v[4], v[5] };
            n++;
            r = e;
        }
        if (ok) {
            memcpy(name, q + 1, 4);
            name[4] = '\0';
            return n;
        }
    }
    return 0;
}

int aml_crs_io(const uint8_t *p, const uint8_t *end, uint16_t *ports, int max)
{
    for (const uint8_t *q = p + 1; q + 8 < end; q++) {
        if (q[-1] != NAME_OP || memcmp(q, "_CRS", 4) || q[4] != BUFFER_OP)
            continue;
        const uint8_t *r = q + 5, *pk = r;
        uint32_t len, size;
        if (pkglen(&r, end, &len) || pk + len > end || aml_int(&r, pk + len, &size))
            return 0;
        const uint8_t *bend = r + size < pk + len ? r + size : pk + len;
        int n = 0;
        while (r < bend) {
            uint8_t tag = *r;
            if (tag & 0x80) {                   /* large item: 16-bit length */
                if (r + 3 > bend)
                    break;
                r += 3 + (r[1] | r[2] << 8);
                continue;
            }
            uint8_t type = tag >> 3 & 0x0f, ilen = tag & 7;
            if (type == 0x0f)                   /* End Tag */
                break;
            if (type == 0x08 && ilen == 7 && r + 8 <= bend) {   /* IO(Decode16, min, max, align, len) */
                if (n < max)
                    ports[n] = (uint16_t)(r[2] | r[3] << 8);
                n++;
            }
            r += 1 + ilen;
        }
        return n;
    }
    return 0;
}
