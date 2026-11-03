/*
 * lowzag_mem.c — buffer in, pixels out (see lowzag.h).
 *
 * Copyright (c) 2026 Tom (defensiblelogic.com)
 * SPDX-License-Identifier: MIT
 *
 * A separate translation unit on purpose.  This is the only file in
 * lowzag that calls malloc and free: src/lowzag.c compiled -O2
 * -fno-stack-protector has exactly two undefined symbols, memcpy and
 * memset, and a freestanding or embedded consumer keeps that property
 * by not linking this one (-DLOWZAG_BUILD_MEM_API=OFF, or simply never
 * referencing lowzag_decode_mem, since an unreferenced object is not
 * pulled out of a static archive).
 *
 * Everything here is a thin adapter over lowzag_decode(): a reader that
 * walks the input buffer, a row sink that writes into the output
 * buffer, malloc/free as the allocator.  No decoding logic and no
 * sizing arithmetic — the dimensions come from lowzag_estimate(), which
 * runs the decoder's own geometry code.
 *
 * The caller's lowzag_cfg_t carries everything about WHAT to decode —
 * scale, target, crop, cancel, mem_limit, state, the marker callback,
 * allow_partial — and this file overwrites only the four callbacks and
 * their contexts.  That is why it takes a cfg rather than the parameter
 * list it had through 1.1.1: every knob added to the config would
 * otherwise have to be added here as another argument, or be unreachable
 * from this entry point.
 *
 * lowzag_exif_orientation() is also here.  It needs no allocation and no
 * libc, so it could live in src/lowzag.c — but that file's undefined
 * symbol set is a documented property, checked in CI, and a helper for a
 * metadata format has no business being the thing that puts it at risk.
 * It is here because this is where things that are not the decoder go.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lowzag.h"

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} mem_src_t;

static int mem_read(void *ctx, uint8_t *buf, size_t len)
{
    mem_src_t *s = (mem_src_t *)ctx;
    size_t n = s->len - s->pos;

    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;           /* the callback returns an int */
    }
    if (n > len) {
        n = len;
    }
    if (n) {
        memcpy(buf, s->data + s->pos, n);
        s->pos += n;
    }
    return (int)n;                       /* 0 is end of input */
}

typedef struct {
    uint8_t *out;
    int      w, h;
    int      refused;                    /* a row did not fit the buffer */
} mem_dst_t;

static void mem_row(void *ctx, int y, int w, const uint8_t *rgb)
{
    mem_dst_t *d = (mem_dst_t *)ctx;

    /* lowzag_decode() delivers y in 0..out_h-1 with w == out_w, and the
     * buffer was sized from lowzag_estimate() for exactly that.  Checked
     * anyway: this is the one place where a wrong row index would run
     * off a caller's buffer, and the test costs nothing next to the
     * memcpy.  Dropping the row keeps the write in bounds; @refused
     * turns that into an error return rather than a short image handed
     * back as LOWZAG_OK, which is the worse of the two failures. */
    if (y < 0 || y >= d->h || w != d->w) {
        d->refused = 1;
        return;
    }
    memcpy(d->out + (size_t)y * (size_t)d->w * 3, rgb, (size_t)w * 3);
}

static void *mem_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void mem_free(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

/*
 * The struct_size rule, which must be the same one src/lowzag.c applies
 * (ss_bad() there, and the reasoning is written out at that copy).  It
 * cannot be shared, because that one is static to a translation unit
 * whose whole point is that it references nothing outside itself — so it
 * is repeated here and the two must be kept in step.
 *
 * It has to be applied BEFORE the memcpy below and not left to
 * lowzag_decode(): the copy is what would splice half a pointer into the
 * local config, and by the time lowzag_decode() saw the result it would
 * be carrying a struct_size this function had already written.
 */
static int mem_ss_bad(size_t given, size_t known)
{
    return given < sizeof(size_t) || given > known ||
           given % sizeof(size_t) != 0;
}

lowzag_err_t lowzag_decode_mem(const lowzag_cfg_t *user_cfg,
                               const uint8_t *LOWZAG_RESTRICT jpeg, size_t len,
                               uint8_t *LOWZAG_RESTRICT out, size_t out_size,
                               lowzag_info_t *info)
{
    lowzag_cfg_t      cfg;
    lowzag_estimate_t est;
    mem_src_t         src;
    mem_dst_t         dst;

    /* @cfg says what the decode should do; this function supplies how it
     * talks to the world.  Copying it rather than mutating the caller's
     * is what lets read/row/alloc/free be overwritten below, and it is
     * also where the struct_size rules are applied — lowzag_decode()
     * would apply them again to this copy, and agree. */
    if (!user_cfg) {
        return LOWZAG_ERR_INVALID_ARG;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    if (mem_ss_bad(user_cfg->struct_size, sizeof cfg)) {
        return LOWZAG_ERR_VERSION;
    }
    memcpy(&cfg, user_cfg, user_cfg->struct_size);
    cfg.struct_size = sizeof cfg;

    if (info) {
        if (mem_ss_bad(info->struct_size, sizeof *info)) {
            return LOWZAG_ERR_VERSION;
        }
        size_t ss = info->struct_size;
        memset(info, 0, ss);
        info->struct_size = ss;
    }
    if (!jpeg) {
        return LOWZAG_ERR_INVALID_ARG;
    }
    /* NULL with a nonzero size is a caller bug rather than an idiom:
     * they think they have a buffer.  NULL with 0 is the size query, and
     * it goes down the same path a real buffer that turns out to be too
     * small does. */
    if (!out && out_size != 0) {
        return LOWZAG_ERR_INVALID_ARG;
    }

    /* Ask what this decode will produce before starting one, because
     * the output buffer has to be big enough for all of it and there is
     * no way to stop half way through.  Same cfg, so the scale, the crop
     * and the dimensions are the ones the decode below will use. */
    est.struct_size = sizeof est;
    lowzag_err_t e = lowzag_estimate(&cfg, jpeg, len, &est);
    if (e != LOWZAG_OK) {
        return e;
    }
    if (info) {
        /* Enough for the caller to size the buffer and try again.
         * peak_alloc stays 0: nothing has been allocated. */
        lowzag_info_t tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.struct_size  = info->struct_size;
        tmp.src_w        = est.src_w;
        tmp.src_h        = est.src_h;
        tmp.out_w        = est.out_w;
        tmp.out_h        = est.out_h;
        tmp.crop_x       = est.crop_x;
        tmp.crop_y       = est.crop_y;
        tmp.crop_w       = est.crop_w;
        tmp.crop_h       = est.crop_h;
        tmp.scale        = est.scale;
        tmp.components   = est.components;
        tmp.chroma_steps = est.chroma_steps;
        memcpy(info, &tmp, info->struct_size < sizeof tmp ? info->struct_size
                                                          : sizeof tmp);
    }

    /* Cannot overflow: both dimensions are capped well below the point
     * where a size_t of any width would wrap (see LOWZAG_MAX_DIM in
     * src/lowzag.c, which lowzag_estimate() has already enforced). */
    if (!out || out_size < (size_t)est.out_w * (size_t)est.out_h * 3) {
        return LOWZAG_ERR_BUFFER_TOO_SMALL;
    }

    src.data    = jpeg;
    src.len     = len;
    src.pos     = 0;
    dst.out     = out;
    dst.w       = est.out_w;
    dst.h       = est.out_h;
    dst.refused = 0;

    cfg.read      = mem_read;
    cfg.read_ctx  = &src;
    cfg.row       = mem_row;
    cfg.row_ctx   = &dst;
    cfg.alloc     = mem_alloc;
    cfg.free      = mem_free;
    cfg.alloc_ctx = NULL;

    lowzag_err_t r = lowzag_decode(&cfg, info);
    /* Unreachable: lowzag_estimate() and lowzag_decode() run the same
     * geometry code over the same header, so the rows always match the
     * buffer.  If that ever stopped being true, the caller would get a
     * partly written buffer with LOWZAG_OK, so say so instead.
     * LOWZAG_PARTIAL is included because it too promises a full buffer —
     * every row arrives, from fewer scans. */
    if ((r == LOWZAG_OK || r == LOWZAG_PARTIAL) && dst.refused) {
        return LOWZAG_ERR_FORMAT;
    }
    return r;
}

/* ── EXIF orientation ───────────────────────────────────────────────────────
 *
 * A captured APP1 payload in, 1..8 or 0 out.  See lowzag.h for what the
 * caller owes and what 0 covers.
 *
 * The input is attacker-controlled and will be fuzzed, so the rules are
 * the boring ones: nothing is allocated, nothing recurses, no sub-IFD is
 * followed, every read is bounded by @len before it happens, and the
 * entry loop is bounded by a count that is itself checked against the
 * buffer.  A file that disagrees with itself anywhere gets 0.
 *
 * Layout, for anyone checking the arithmetic:
 *
 *   0    "Exif\0\0"                        6 bytes
 *   6    "II" or "MM"                      TIFF header starts here, and
 *   8    42, in that byte order            every offset below is
 *   10   offset of IFD0 from byte 6        relative to byte 6
 *   ...
 *   IFD0: entry count (2), then count x 12 bytes of
 *         tag (2), type (2), value count (4), value or offset (4)
 *
 * Tag 0x0112 is Orientation: type 3 (SHORT), count 1, so the value is in
 * the first two bytes of the value field and no offset is followed.
 */

#define EXIF_HDR      6u             /* "Exif\0\0"                        */
#define EXIF_TIFF_MIN 8u             /* byte order, magic, IFD0 offset    */
#define EXIF_ORIENT   0x0112u
#define EXIF_TYPE_SHORT 3u
#define EXIF_MAX_ENTRIES 4096u       /* IFD0 in a real file has tens      */

static unsigned exif_u16(const uint8_t *p, int big)
{
    return big ? ((unsigned)p[0] << 8) | p[1]
               : ((unsigned)p[1] << 8) | p[0];
}

static uint32_t exif_u32(const uint8_t *p, int big)
{
    return big ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8)  |  (uint32_t)p[3]
               : ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
                 ((uint32_t)p[1] << 8)  |  (uint32_t)p[0];
}

int lowzag_exif_orientation(const uint8_t *app1, size_t len)
{
    if (!app1 || len < EXIF_HDR + EXIF_TIFF_MIN) {
        return 0;
    }
    /* "Exif\0" then one pad byte, which the specification says is 0 and
     * which some encoders fill with something else.  The five that carry
     * meaning are checked; the sixth is skipped either way. */
    if (memcmp(app1, "Exif\0", 5) != 0) {
        return 0;                    /* XMP also lives in APP1 */
    }

    const uint8_t *tiff = app1 + EXIF_HDR;
    size_t         tlen = len - EXIF_HDR;
    int            big;

    if (tiff[0] == 'M' && tiff[1] == 'M') {
        big = 1;
    } else if (tiff[0] == 'I' && tiff[1] == 'I') {
        big = 0;
    } else {
        return 0;
    }
    if (exif_u16(tiff + 2, big) != 42u) {
        return 0;
    }

    uint32_t ifd = exif_u32(tiff + 4, big);
    /* Everything below is bounded by tlen, which is a size_t derived
     * from the caller's length; ifd is 32 bits out of the file.  Compare
     * in uint64_t so the check cannot wrap on a 32-bit target, where
     * ifd + 2 is exactly the kind of sum that would. */
    if ((uint64_t)ifd + 2u > (uint64_t)tlen) {
        return 0;
    }
    unsigned n = exif_u16(tiff + ifd, big);
    if (n == 0u || n > EXIF_MAX_ENTRIES) {
        return 0;
    }
    if ((uint64_t)ifd + 2u + (uint64_t)n * 12u > (uint64_t)tlen) {
        return 0;                    /* the count outruns the payload */
    }

    const uint8_t *e = tiff + ifd + 2u;
    for (unsigned i = 0; i < n; i++, e += 12) {
        if (exif_u16(e, big) != EXIF_ORIENT) {
            continue;
        }
        if (exif_u16(e + 2, big) != EXIF_TYPE_SHORT ||
            exif_u32(e + 4, big) != 1u) {
            return 0;                /* the tag, but not as a SHORT of 1 */
        }
        unsigned v = exif_u16(e + 8, big);
        return (v >= 1u && v <= 8u) ? (int)v : 0;
    }
    return 0;                        /* no orientation tag in IFD0 */
}
