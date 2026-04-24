/* pawnsave - decompress DDDA.sav and extract hired-pawn creator+gear state.
 *
 * DDDA.sav format:
 *   u32 version (21)
 *   u32 realSize            <- decompressed payload size
 *   u32 compressedSize
 *   u32 magic1 (860693325)
 *   u32 zero
 *   u32 magic2 (860700740)
 *   u32 crc32jam hash
 *   u32 magic3 (1079398965)
 *   [32 bytes header above]
 *   zlib-deflated XML of the save tree (~20 MB when inflated)
 *
 * XML layout we care about (top-level):
 *   <class type="cSAVE_DATA_CMC">   (x3 at the front: MainPawn, Hired1, Hired2)
 *       ...
 *       <class name="mArisenName" type="cName">
 *           ...
 *           <array name="( u8* )mEditName" type="u8" count="25">
 *               <u8 value="N"/> ... (ASCII codes, NUL-padded)
 *           </array>
 *           ...
 *       </class>
 *       ...
 *       <array name="mEquipItem" ...>12x cITEM_PARAM_DATA</array>
 *   </class>
 *
 * The late cSAVE_DATA_CMC copies at line 297720+ are checkpoint snapshots;
 * we scan in-order and take the first block matching each hired slot, so
 * the early (current-state) copies win.
 *
 * The save format was validated end-to-end by tools/save_to_archive.py. */

#include "pawnsave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls from the vendored easyzlib (third_party/easyzlib/easyzlib.c). */
extern int uncompress(unsigned char *dest, unsigned long *destLen,
                      const unsigned char *source, unsigned long sourceLen);

/* ===========================================================================
 * Minimal XML primitives, scoped to DDDA.sav's patterns.
 *
 * No DOM, no entity handling, no attribute-order tolerance beyond what the
 * save actually uses. These are composable enough to extend later (study
 * data lives in <array name="mStudyData.KillCnt" type="u32" count="72">…
 * see gear-persistence.md). Everything is substring search on bounded
 * spans, so each call is O(span_length). Within a ≤5 KB array body the
 * cost is trivial; scanning the full ~20 MB XML for the top-level array
 * anchor is a single memmem-style pass per query.
 * ===========================================================================
 */
typedef struct {
    const char *p;
    const char *end;
} xspan;

static const char *xfind(xspan in, const char *needle)
{
    size_t nlen = strlen(needle);
    if ((size_t)(in.end - in.p) < nlen) return NULL;
    const char *last = in.end - nlen;
    for (const char *p = in.p; p <= last; p++) {
        if (memcmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

/* Find a self-closing <TAG name="NAME" value="V"/> and parse V as a signed
 * long. Matches the exact `name="NAME" value="` substring, so order-of-
 * attributes or odd spacing would miss — that's fine for our fixed save
 * format. Unfindable fields return -1; caller checks the return. */
static int xml_get_long(xspan in, const char *name, long *out)
{
    char needle[96];
    int n = snprintf(needle, sizeof(needle), "name=\"%s\" value=\"", name);
    if (n <= 0 || n >= (int)sizeof(needle)) return -1;
    const char *hit = xfind(in, needle);
    if (!hit) return -1;
    const char *val = hit + n;
    const char *q = memchr(val, '"', (size_t)(in.end - val));
    if (!q) return -1;
    char buf[32];
    size_t len = (size_t)(q - val);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, val, len);
    buf[len] = 0;
    char *endp;
    long v = strtol(buf, &endp, 10);
    if (endp == buf) return -1;
    *out = v;
    return 0;
}

/* Iterate every <array name="NAME" ...>body</array>. Callback receives the
 * inner body span and the 0-based match index. Return non-zero from the
 * callback to stop iteration early. */
typedef int (*xml_array_cb)(xspan body, int index, void *ctx);

static int xml_for_each_array(xspan in, const char *name,
                              xml_array_cb cb, void *ctx)
{
    char needle[96];
    int n = snprintf(needle, sizeof(needle), "<array name=\"%s\"", name);
    if (n <= 0 || n >= (int)sizeof(needle)) return -1;

    xspan remaining = in;
    int idx = 0;
    while (1) {
        const char *start = xfind(remaining, needle);
        if (!start) return 0;
        /* Skip past the '>' that closes the <array ...> opening tag. */
        const char *gt = memchr(start + n, '>', (size_t)(in.end - (start + n)));
        if (!gt) return 0;
        const char *body_start = gt + 1;
        /* mEquipItem, mStudyData.* etc. don't nest <array> inside <array>
         * in this save, so the first </array> after body_start is ours. */
        xspan after = { body_start, in.end };
        const char *close = xfind(after, "</array>");
        if (!close) return 0;
        xspan body = { body_start, close };
        int r = cb(body, idx++, ctx);
        if (r) return r;
        remaining.p = close + 8;  /* len of "</array>" */
    }
}

/* Iterate every <class type="TYPE">body</class> inside a span. Matches only
 * the bare `<class type="...">` form — the name-prefixed variant
 * `<class name="..." type="...">` is intentionally skipped, which is what
 * lets us scope top-level cSAVE_DATA_CMC blocks without hitting the nested
 * mKaiouData / mKaiouPornData sub-classes. */
typedef int (*xml_class_cb)(xspan body, int index, void *ctx);

static int xml_for_each_class(xspan in, const char *type,
                              xml_class_cb cb, void *ctx)
{
    char needle[128];
    int n = snprintf(needle, sizeof(needle), "<class type=\"%s\">", type);
    if (n <= 0 || n >= (int)sizeof(needle)) return -1;

    xspan remaining = in;
    int idx = 0;
    while (1) {
        const char *start = xfind(remaining, needle);
        if (!start) return 0;
        const char *body_start = start + n;
        /* Nested <class> inside a cSAVE_DATA_CMC body (mArisenName, item
         * records, …) means we can't just find the first </class>. Walk
         * with a depth counter, recognising both the bare form and the
         * name-prefixed form as openings. */
        int depth = 1;
        const char *q = body_start;
        const char *close = NULL;
        while (q < in.end) {
            const char *lt = memchr(q, '<', (size_t)(in.end - q));
            if (!lt) break;
            if ((size_t)(in.end - lt) >= 8 && memcmp(lt, "</class>", 8) == 0) {
                if (--depth == 0) { close = lt; break; }
                q = lt + 8;
            } else if ((size_t)(in.end - lt) >= 12 &&
                       memcmp(lt, "<class name=", 12) == 0) {
                depth++;
                q = lt + 12;
            } else if ((size_t)(in.end - lt) >= 12 &&
                       memcmp(lt, "<class type=", 12) == 0) {
                depth++;
                q = lt + 12;
            } else {
                q = lt + 1;
            }
        }
        if (!close) return 0;
        xspan body = { body_start, close };
        int r = cb(body, idx++, ctx);
        if (r) return r;
        remaining.p = close + 8;  /* len of "</class>" */
    }
}

/* Fill `out[0..count-1]` with values from `<array name="NAME" type="u32" count="N">`
 * by walking its inner `<u32 value="N"/>` children in order. Missing entries
 * stay zero (callers should pre-zero the buffer if they care). Returns 0 on
 * success — including when the array is absent — so a pawn that simply has
 * no knowledge yet doesn't fail the parse. */
static int xml_get_u32_array(xspan in, const char *name, uint32_t *out, int count)
{
    char opener[96];
    int n = snprintf(opener, sizeof(opener),
                     "<array name=\"%s\" type=\"u32\"", name);
    if (n <= 0 || n >= (int)sizeof(opener)) return -1;
    const char *start = xfind(in, opener);
    if (!start) return 0;                          /* absent → leave out[] as caller initialised */
    const char *gt = memchr(start + n, '>', (size_t)(in.end - (start + n)));
    if (!gt) return 0;
    xspan body = { gt + 1, in.end };
    const char *close = xfind(body, "</array>");
    if (!close) return 0;
    body.end = close;

    const char *p = body.p;
    const char *tag = "<u32 value=\"";
    size_t tlen = strlen(tag);
    int idx = 0;
    while (idx < count) {
        xspan rem = { p, body.end };
        const char *hit = xfind(rem, tag);
        if (!hit) break;
        const char *v = hit + tlen;
        const char *q = memchr(v, '"', (size_t)(body.end - v));
        if (!q) break;
        char buf[16];
        size_t len = (size_t)(q - v);
        if (len == 0 || len >= sizeof(buf)) break;
        memcpy(buf, v, len);
        buf[len] = 0;
        out[idx++] = (uint32_t)strtoul(buf, NULL, 10);
        p = q + 1;
    }
    return 0;
}

/* Read a <class name="NAME" type="cName">…</class> sub-block: the
 * `mEditName` u8 array inside it holds the ASCII-coded name, NUL-padded to
 * 25 bytes. Writes up to out_cap-1 chars + NUL into out; returns 0 on
 * success, -1 if the class or inner array wasn't found. */
static int xml_get_cname(xspan in, const char *name, char *out, size_t out_cap)
{
    if (out_cap == 0) return -1;
    out[0] = 0;

    char opener[96];
    int n = snprintf(opener, sizeof(opener),
                     "<class name=\"%s\" type=\"cName\">", name);
    if (n <= 0 || n >= (int)sizeof(opener)) return -1;
    const char *start = xfind(in, opener);
    if (!start) return -1;
    const char *body_start = start + n;
    xspan after = { body_start, in.end };
    const char *close = xfind(after, "</class>");
    if (!close) return -1;
    xspan body = { body_start, close };

    /* Locate the u8 array. The name attribute contains literal parens, so
     * we match the whole opening-tag substring. */
    const char *arr = xfind(body, "<array name=\"( u8* )mEditName\"");
    if (!arr) return -1;
    const char *gt = memchr(arr, '>', (size_t)(body.end - arr));
    if (!gt) return -1;
    xspan arr_body = { gt + 1, body.end };
    const char *arr_end = xfind(arr_body, "</array>");
    if (!arr_end) return -1;
    arr_body.end = arr_end;

    /* Walk the <u8 value="N"/> entries in order, decoding each into a byte. */
    size_t w = 0;
    const char *p = arr_body.p;
    const char *tag = "<u8 value=\"";
    size_t tlen = strlen(tag);
    while (w + 1 < out_cap) {
        xspan rem = { p, arr_body.end };
        const char *hit = xfind(rem, tag);
        if (!hit) break;
        const char *v = hit + tlen;
        const char *q = memchr(v, '"', (size_t)(arr_body.end - v));
        if (!q) break;
        char buf[8];
        size_t len = (size_t)(q - v);
        if (len == 0 || len >= sizeof(buf)) break;
        memcpy(buf, v, len);
        buf[len] = 0;
        int c = atoi(buf);
        if (c == 0) break;                            /* C-string NUL terminator */
        if (c < 0 || c > 127) break;                  /* ASCII-only by construction */
        out[w++] = (char)c;
        p = q + 1;
    }
    out[w] = 0;
    return 0;
}

/* ===========================================================================
 * Gear-specific extraction.
 * ===========================================================================
 */
static int parse_gear_record(xspan body, struct sav_gear_record *rec)
{
    long v;
    if (xml_get_long(body, "data.mNum",          &v) < 0) return -1;
    rec->mNum = (int16_t)v;
    if (xml_get_long(body, "data.mItemNo",       &v) < 0) return -1;
    rec->mItemNo = (int16_t)v;
    if (xml_get_long(body, "data.mFlag",         &v) < 0) return -1;
    rec->mFlag = (uint32_t)v;
    if (xml_get_long(body, "data.mChgNum",       &v) < 0) return -1;
    rec->mChgNum = (uint16_t)v;
    if (xml_get_long(body, "data.mDay1",         &v) < 0) return -1;
    rec->mDay1 = (uint16_t)v;
    if (xml_get_long(body, "data.mDay2",         &v) < 0) return -1;
    rec->mDay2 = (uint16_t)v;
    if (xml_get_long(body, "data.mDay3",         &v) < 0) return -1;
    rec->mDay3 = (uint16_t)v;
    if (xml_get_long(body, "data.mMutationPool", &v) < 0) return -1;
    rec->mMutationPool = (int8_t)v;
    if (xml_get_long(body, "data.mOwnerId",      &v) < 0) return -1;
    rec->mOwnerId = (int8_t)v;
    if (xml_get_long(body, "data.mKey",          &v) < 0) return -1;
    rec->mKey = (uint32_t)v;
    return 0;
}

struct class_iter_ctx {
    struct sav_gear_record recs[12];
    int count;
};

static int class_cb(xspan body, int idx, void *ctxp)
{
    struct class_iter_ctx *c = ctxp;
    if (idx >= 12) return 1;                      /* stop — more than a full array */
    if (parse_gear_record(body, &c->recs[idx]) < 0) {
        /* Malformed record: treat as empty. */
        memset(&c->recs[idx], 0, sizeof(c->recs[idx]));
        c->recs[idx].mItemNo = -1;
    }
    c->count = idx + 1;
    return 0;
}

struct array_gear_ctx {
    struct sav_gear_record *recs;                 /* points into c->recs */
    int have_gear;
};

static int array_gear_cb(xspan body, int idx, void *ctxp)
{
    (void)idx;
    struct array_gear_ctx *c = ctxp;
    struct class_iter_ctx inner;
    memset(&inner, 0, sizeof(inner));
    for (int i = 0; i < 12; i++) inner.recs[i].mItemNo = -1;

    xml_for_each_class(body, "sItemManager::cITEM_PARAM_DATA", class_cb, &inner);
    if (inner.count < 12) return 0;               /* skip short arrays */

    memcpy(c->recs, inner.recs, sizeof(inner.recs));
    c->have_gear = 1;
    return 1;                                     /* first mEquipItem only */
}

/* ===========================================================================
 * Top-level cSAVE_DATA_CMC iteration.
 * ===========================================================================
 */
struct cmc_iter_ctx {
    struct pawnsave_hired_info *out;              /* two-entry array */
};

static int cmc_cb(xspan body, int idx, void *ctxp)
{
    (void)idx;
    struct cmc_iter_ctx *c = ctxp;

    /* Parse the mEquipItem array first; without at least one non-empty
     * slot we can't determine ownership and have nothing to write back. */
    struct sav_gear_record recs[12];
    for (int i = 0; i < 12; i++) { memset(&recs[i], 0, sizeof(recs[i])); recs[i].mItemNo = -1; }
    struct array_gear_ctx gctx = { .recs = recs, .have_gear = 0 };
    xml_for_each_array(body, "mEquipItem", array_gear_cb, &gctx);
    if (!gctx.have_gear) return 0;

    int owner = -1;
    for (int i = 0; i < 12; i++) {
        if (recs[i].mItemNo != -1) { owner = recs[i].mOwnerId; break; }
    }
    if (owner < 2 || owner > 3) return 0;         /* not a hired slot */
    int slot = owner - 2;
    if (c->out[slot].present) return 0;           /* first block wins (current state) */

    c->out[slot].present = 1;
    memcpy(c->out[slot].gear, recs, sizeof(recs));
    xml_get_cname(body, "mArisenName",
                  c->out[slot].creator_name,
                  sizeof(c->out[slot].creator_name));

    /* mStudyFlag / mLocalStudyFlag: 322-entry u32 arrays that round-trip
     * verbatim between save XML and the .pawn archive's two `0x142`-tagged
     * structs. Together they cover every knowledge category — monsters,
     * vocations, areas, quests — so copying them whole preserves the lot. */
    xml_get_u32_array(body, "mStudyFlag",      c->out[slot].study_flag,       322);
    xml_get_u32_array(body, "mLocalStudyFlag", c->out[slot].local_study_flag, 322);

    /* Stop once both slots have been filled. */
    return (c->out[0].present && c->out[1].present) ? 1 : 0;
}

/* ===========================================================================
 * Public entry point.
 * ===========================================================================
 */
int pawnsave_read_hired(const void *save_bytes, int32_t save_len,
                        struct pawnsave_hired_info out[2])
{
    if (!save_bytes || save_len < 32 || !out) return -1;

    const uint8_t *b = save_bytes;
    uint32_t real_size, comp_size;
    memcpy(&real_size, b + 4, 4);
    memcpy(&comp_size, b + 8, 4);
    if (comp_size == 0 || comp_size > (uint32_t)(save_len - 32)) return -1;
    if (real_size == 0 || real_size > 64u * 1024u * 1024u) return -1;  /* sanity */

    /* ~20 MB decompressed, well within the DLL's address space. Freed before return. */
    uint8_t *xml = (uint8_t *)malloc(real_size);
    if (!xml) return -2;
    unsigned long out_len = real_size;
    int rc = uncompress(xml, &out_len, b + 32, comp_size);
    if (rc != 0 || out_len != real_size) {
        free(xml);
        return -2;
    }

    for (int i = 0; i < 2; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        for (int k = 0; k < 12; k++) out[i].gear[k].mItemNo = -1;
    }

    xspan whole = { (const char *)xml, (const char *)xml + out_len };
    struct cmc_iter_ctx ctx = { .out = out };
    xml_for_each_class(whole, "cSAVE_DATA_CMC", cmc_cb, &ctx);

    free(xml);
    return 0;
}
