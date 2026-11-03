/*
 * lowzag.c — band-limited progressive JPEG decoder (see lowzag.h).
 *
 * Copyright (c) 2026 Tom (defensiblelogic.com)
 * SPDX-License-Identifier: MIT
 *
 * Structure:
 *   1. buffered byte reader with pushback + entropy bit reader
 *      (0xFF stuffing, marker detection)
 *   2. Huffman tables with 8-bit lookahead
 *   3. marker parsing (DQT/DHT/SOF2/SOS/DRI)
 *   4. progressive scan decoding into band-limited coefficient storage:
 *      per block, the first K zigzag coefficients as int16 plus (when
 *      K < 64, or when the block lies outside the crop) a 64-bit nonzero
 *      bitmap.  The bitmap lets AC refinement scans consume their
 *      correction bits correctly for coefficients whose values we do not
 *      keep.  Every scan is entropy-decoded — a refinement scan's bit
 *      count depends on nonzero history across its whole band, so
 *      dropping one desynchronizes those after it — but only the kept
 *      prefix is dequantized and transformed.
 *   5. output: per MCU row, corner-truncated inverse DCT (a scaled
 *      bs-point orthonormal IDCT of the low bs x bs coefficients, the
 *      standard DCT-domain downsample), chroma replication, YCbCr to
 *      RGB888 rows.
 *
 * Kept zigzag prefix per scale: 1/1: 64, 1/2: 25, 1/4: 5, 1/8: 1 —
 * the shortest prefixes containing the bs x bs low corners.
 *
 * A crop is the other axis of the same idea: a block outside the
 * requested rectangle keeps no coefficients at all, only its 8-byte
 * nonzero map, and is never inverse-transformed.  It cannot skip the
 * entropy decode for the same reason a scan cannot be skipped.
 *
 * cfg.cancel is polled at five places, all of them outside the
 * per-block and per-pixel loops: the marker loop, the chunked clear of
 * the coefficient arrays, each block row of a DC scan, each block row of
 * an AC scan, and each MCU row of the output pass.  The polls sit in an
 * outer loop over block rows so that the MCU loop bodies keep the shape
 * they had before — a test inside the loop costs 17% on a 12 MP file,
 * the outer form costs about 1%.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lowzag.h"

/*
 * Keep the entropy scan loops out of the top-level function.
 *
 * Every static function here is called from exactly one place, so at -O2
 * the compiler folds the whole decoder into lowzag_decode() — about 8 KB
 * of code in 1.1.1.  The scan loops are the hot part of that and they
 * want every register they can get; anything else the same function has
 * to keep live costs them.  Adding the crop planning and the version
 * handling was enough to push the allocator over the edge: measured on a
 * 12 MP 4:2:0 file, the entropy pass went from 47.6 ms to 58.0 ms with
 * no change to the loops themselves — 15% of the whole decode at 1/2,
 * 1/4 and 1/8.  Splitting them back out recovers all of it and leaves
 * the decode a shade faster than 1.1.1 was.
 *
 * This is a hint and not a dependency: where the attribute does not
 * exist the macro is empty and the compiler decides for itself, and the
 * only thing at stake is speed.  Nothing in the test suite would notice
 * if it stopped working — decode times are not asserted anywhere — so
 * anyone reshaping this file should time it against the previous release
 * the way the 2.0.0 notes did.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define LZ_NOINLINE __attribute__((noinline))
#else
#  define LZ_NOINLINE
#endif

#define MAX_COMPS   3
#define BUF_SZ      4096
#define DCTSZ       8
#define LOWZAG_MAX_SCANS 100

/* Ceiling on the DC predictor.  A legal 8-bit stream keeps it inside
 * +/-2047; the limit here is deliberately far looser, so it never
 * rejects a real file, while still leaving headroom for the `<< Al`
 * (Al <= 13) that follows: 2^17 << 13 is 2^30, comfortably inside
 * int32.  Its purpose is to stop a malformed file from walking the
 * predictor into signed overflow, which is undefined behaviour. */
#define DC_PRED_MAX  (1 << 17)

/* This decoder assumes a 32-bit (or wider) int: the IDCT accumulators
 * and the bit buffer both need it.  Fail at compile time rather than
 * producing wrong pixels on a 16-bit target. */
typedef char lowzag_requires_32bit_int[(sizeof(int) >= 4) ? 1 : -1];

/*
 * Largest frame dimension accepted, in either axis.
 *
 * This is a memory-safety boundary, not a taste limit, and it is the
 * only thing standing between a malformed header and a heap overflow
 * where size_t is 32 bits — which is every target this decoder was
 * written for.  Worst case (4:4:4 at scale 1) the coefficient arrays
 * alone want 6*D*D bytes.  On a 32-bit size_t that expression wraps at
 * D = 26755; JPEG itself permits 65535, where it wraps from 25.7 GB to
 * about 2 MB, the ceiling check passes, a 2 MB buffer is handed back
 * and 25 GB gets written into it.
 *
 * 24000 leaves roughly 11% of headroom below the wrap.  Anyone raising
 * it must redo that arithmetic; the static assertion below fails if the
 * chosen value cannot be squared and scaled inside a size_t, so the
 * mistake is caught at compile time rather than in the field.
 */
#define LOWZAG_MAX_DIM  24000

/* Two divisions rather than 6*D*D, so the test itself cannot overflow
 * and stays an integer constant expression. */
typedef char lowzag_max_dim_fits_size_t[
    (LOWZAG_MAX_DIM <= (SIZE_MAX / 6u) / LOWZAG_MAX_DIM) ? 1 : -1];

/* Marks working state that a decode is currently using.  A caller who
 * reenters lowzag_decode() with the same cfg.state — most easily by
 * calling it from inside its own row callback — would otherwise have
 * the outer decode's arena pointers zeroed by the inner one's memset,
 * leaking every allocation and letting the outer call run on cleared
 * state and return LOWZAG_OK with a fraction of the image.  The header
 * documents that this is unsupported; the point of the sentinel is that
 * unsupported should fail loudly rather than silently.
 *
 * 64 bits so that a caller-supplied buffer holding this value by
 * chance before its first use is not worth worrying about. */
#define LZ_IN_USE  0x6C6F777A61675F31ULL   /* "lowzag_1" */

/* Alignment lz_t actually requires.  C99 has no _Alignof, so derive it
 * the classic way: the offset of a maximally-aligned union inside a
 * struct that starts with a char IS that union's alignment. */
struct lz_align_probe {
    char c;
    union { void *p; long l; double d; } u;
};
#define LZ_ALIGNMENT  offsetof(struct lz_align_probe, u)

/* Multiply with overflow detection.  LOWZAG_MAX_DIM already makes
 * overflow unreachable for any accepted image; this is the same
 * invariant enforced where the arithmetic actually happens, so that
 * raising the cap turns into a clean LOWZAG_ERR_NOMEM instead of a
 * buffer sized by a wrapped product. */
static bool size_mul(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

/* ── tables ─────────────────────────────────────────────────────────────────*/

static const uint8_t ZZ[64] = {      /* zigzag index -> natural index */
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/* Shortest zigzag prefix containing every natural position with
 * row < @rows and col < @cols — the coefficients a rows x cols reduced
 * IDCT reads, and therefore all we need to store. */
static int keep_for_block(int cols, int rows)
{
    int k_max = 0;
    for (int k = 0; k < 64; k++) {
        int nat = ZZ[k];
        if ((nat >> 3) < rows && (nat & 7) < cols) {
            k_max = k;
        }
    }
    return k_max + 1;
}

/* ── reader ─────────────────────────────────────────────────────────────────*/

typedef struct {
    const lowzag_cfg_t *cfg;
    uint8_t  buf[BUF_SZ];
    int      pos, fill;
    uint8_t  unget[4];
    int      unget_n;
    int      eof;
    int      io_err;
    size_t   nread;          /* bytes the read callback has handed over */
    /* entropy state */
    uint32_t bitbuf;
    int      bitcnt;
    int      marker;         /* nonzero: marker byte terminated entropy */
    /* Set the first time the bit reader fabricates a byte because the
     * input ended — everything decoded from here on is made up.  It is
     * what tells a partial decode where the real data stopped, and it is
     * why a format error raised after this point is reported as
     * truncation rather than as a malformed file. */
    int      pad;
} rd_t;

static int rd_byte(rd_t *r)
{
    if (r->unget_n > 0) {
        return r->unget[--r->unget_n];
    }
    if (r->pos >= r->fill) {
        if (r->eof) {
            return -1;
        }
        int n = r->cfg->read(r->cfg->read_ctx, r->buf, BUF_SZ);
        if (n < 0) {
            r->io_err = 1;
            r->eof = 1;
            return -1;
        }
        if (n == 0) {
            r->eof = 1;
            return -1;
        }
        /* Returning more than was asked for is a caller bug, but it is a
         * common one — reporting bytes available rather than bytes
         * written — and taking it at face value reads off the end of
         * this buffer.  Clamp rather than trust. */
        if (n > BUF_SZ) {
            n = BUF_SZ;
        }
        r->pos = 0;
        r->fill = n;
        r->nread += (size_t)n;
    }
    return r->buf[r->pos++];
}

static void rd_unget(rd_t *r, uint8_t b)
{
    if (r->unget_n < (int)sizeof r->unget) {
        r->unget[r->unget_n++] = b;
    }
}

static void bits_reset(rd_t *r)
{
    r->bitbuf = 0;
    r->bitcnt = 0;
    r->marker = 0;
}

/* Refill the bit accumulator.  Entropy data escapes 0xFF as 0xFF00 and
 * a real marker ends the segment, so bytes cannot simply be blitted in
 * — but a run containing no 0xFF can, which is the overwhelmingly
 * common case and worth a fast path: three bytes straight out of the
 * read buffer with no per-byte call. */
static void bits_need_slow(rd_t *r, int n)
{
    while (r->bitcnt < n) {
        uint32_t b = 0;
        if (!r->marker) {
            int c = rd_byte(r);
            if (c < 0) {
                r->marker = 0xD9;          /* EOF: pad; caller checks */
                r->pad = 1;
            } else if (c == 0xFF) {
                int c2 = rd_byte(r);
                if (c2 == 0x00) {
                    b = 0xFF;              /* stuffed literal */
                } else {
                    r->marker = (c2 < 0) ? 0xD9 : c2;
                    if (c2 < 0) {
                        r->pad = 1;
                    }
                }
            } else {
                b = (uint32_t)c;
            }
        }
        r->bitbuf = (r->bitbuf << 8) | b;
        r->bitcnt += 8;
    }
}

static inline void bits_need(rd_t *r, int n)
{
    if (r->bitcnt >= n) {
        return;
    }
    /* bitcnt <= 8 keeps the 24-bit load inside the 32-bit accumulator */
    if (r->bitcnt <= 8 && !r->marker && r->unget_n == 0 &&
        r->pos + 3 <= r->fill) {
        const uint8_t *q = r->buf + r->pos;
        if (q[0] != 0xFF && q[1] != 0xFF && q[2] != 0xFF) {
            r->bitbuf = (r->bitbuf << 24) | ((uint32_t)q[0] << 16) |
                        ((uint32_t)q[1] << 8) | (uint32_t)q[2];
            r->bitcnt += 24;
            r->pos += 3;
            if (r->bitcnt >= n) {
                return;
            }
        }
    }
    bits_need_slow(r, n);
}

static inline int get_bits(rd_t *r, int n)
{
    bits_need(r, n);
    r->bitcnt -= n;
    return (int)((r->bitbuf >> r->bitcnt) & ((1u << n) - 1));
}

static inline int get_bit(rd_t *r)
{
    bits_need(r, 1);
    r->bitcnt -= 1;
    return (int)((r->bitbuf >> r->bitcnt) & 1);
}

/* T.81 EXTEND: map an s-bit magnitude to its signed value. */
static int huff_extend(int v, int s)
{
    return (v < (1 << (s - 1))) ? v - (1 << s) + 1 : v;
}

/* ── Huffman ────────────────────────────────────────────────────────────────*/

typedef struct {
    int      defined;
    /* canonical decode */
    int32_t  maxcode[18];            /* -1 terminated per length        */
    int32_t  mincode[17];
    int      valptr[17];
    uint8_t  vals[256];
    /* 8-bit lookahead: (len << 8) | value, 0 = miss */
    uint16_t lut[256];
} huff_t;

static int huff_build(huff_t *h, const uint8_t counts[16],
                      const uint8_t *vals, int nvals)
{
    memset(h, 0, sizeof *h);
    memcpy(h->vals, vals, (size_t)nvals);

    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->valptr[len]  = k;
        h->mincode[len] = code;
        code += counts[len - 1];
        k    += counts[len - 1];
        h->maxcode[len] = code - 1;
        if (counts[len - 1] == 0) {
            h->maxcode[len] = -1;    /* no codes of this length */
        }
        if (code > (1 << len)) {
            return -1;               /* over-subscribed table */
        }
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;

    /* lookahead */
    code = 0;
    k = 0;
    for (int len = 1; len <= 8; len++) {
        for (int i = 0; i < counts[len - 1]; i++, k++, code++) {
            int lo = code << (8 - len);
            int hi = lo + (1 << (8 - len));
            for (int j = lo; j < hi; j++) {
                h->lut[j] = (uint16_t)((len << 8) | h->vals[k]);
            }
        }
        code <<= 1;
    }
    h->defined = 1;
    return 0;
}

static int huff_decode(rd_t *r, const huff_t *h)
{
    bits_need(r, 16);                /* longest code, plus the lookahead */
    int look = (int)((r->bitbuf >> (r->bitcnt - 8)) & 0xFF);
    uint16_t e = h->lut[look];
    if (e) {
        r->bitcnt -= (int)(e >> 8);
        return (int)(e & 0xFF);
    }
    int code = look;
    r->bitcnt -= 8;
    for (int len = 9; len <= 16; len++) {
        code = (code << 1) | get_bit(r);
        if (h->maxcode[len] >= 0 && code <= h->maxcode[len]) {
            return h->vals[h->valptr[len] + (code - h->mincode[len])];
        }
    }
    return -1;                       /* invalid code */
}

/* ── decoder state ──────────────────────────────────────────────────────────*/

typedef struct {
    int id, h, v, tq;
    int bw, bh;                      /* real block dims (non-interleaved) */
    int pad_bw, pad_bh;              /* MCU-padded dims (interleaved)     */
    int cbx, cby;                    /* first stored block, in blocks     */
    int cbw, cbh;                    /* stored block rectangle            */
    int16_t *coef;                   /* cbw*cbh blocks x K int16          */
    uint8_t *nz;                     /* 8 bytes/block bitmap, full grid,
                                        when K < 64 or the crop leaves
                                        blocks unstored                   */
    int bs_h, bs_v;                  /* this component's IDCT size, per
                                        axis: a 4:2:2 component needs a
                                        different reduction horizontally
                                        than vertically                   */
    int K;                           /* kept zigzag prefix covering them  */
    int out_w, out_h;                /* this component's decoded extent   */
    int dc_pred;
    /* Percentage of THIS component's blocks that a first DC pass (ah==0)
     * reached, or -1 before one has.  Per component rather than per
     * frame because the DC scans need not be interleaved: a file may
     * carry one DC scan per component, and a stream cut between them
     * leaves whole components with no coefficients at all.  See
     * note_dc_cov().  Set to -1 in parse_sof2(); the zero a memset
     * leaves would read as "reached, and reached nothing". */
    int dc_cov;
} comp_t;

typedef struct {
    const lowzag_cfg_t *cfg;
    rd_t     rd;
    uint16_t qt[4][64];              /* natural order */
    int      qt_def[4];
    huff_t   hdc[4], hac[4];
    int      W, H, ncomp;
    comp_t   comp[MAX_COMPS];
    int      hmax, vmax;
    int      mcux, mcuy;
    int      restart;                /* restart interval, MCUs           */
    int      scale;
    int      full_out_w, full_out_h; /* whole-frame output grid          */
    int      out_w, out_h;           /* delivered size (the crop's)      */
    int      ox0, oy0;               /* delivered origin, output pixels  */
    int      crop_x, crop_y;         /* delivered rectangle, source px   */
    int      crop_w, crop_h;
    int      rows_out;               /* rows handed to cfg.row so far    */
    uint32_t eobrun;
    size_t   allocated, peak;
    /* output strips */
    uint8_t *strip[MAX_COMPS];       /* per comp: pad_bw*bs x (v*bs)     */
    uint8_t *rowbuf;                 /* out_w * 3                        */
    /* Per-component source column for each output column.  Sampling
     * ratios are fixed for the frame, so this is a table lookup rather
     * than a divide in the pixel loop. */
    uint16_t *colmap[MAX_COMPS];
    /* YCbCr->RGB, one table lookup per channel (libjpeg's arrangement). */
    int32_t cr_r[256], cb_b[256], cr_g[256], cb_g[256];
    int      bs;
    int      chroma_steps;    /* chroma tiers given up to mem_limit */
    /* Short-stream bookkeeping.  partial caches cfg->allow_partial so
     * the scan loops test an int in the state they already have; every
     * branch it guards is off for a caller who did not ask, which is
     * what keeps 2.1.0's behaviour bit for bit.
     *
     * dc_cov is the percentage of the frame the first DC pass reached,
     * or -1 before any DC scan has run.  Where several DC scans cover
     * different components it is the worst of them, and a component no
     * first DC pass reached at all counts as 0 — see note_dc_cov(),
     * which is where that is worked out from comp[].dc_cov. */
    int      partial;
    int      scans_done;
    int      dc_cov;
    /* Where a block outside the crop sends its DC.  The DC scans store
     * unconditionally — a branch there would cost more than this 128
     * bytes — and nothing ever reads it back: emit_rows() never visits
     * an unstored block, and the refinement path's `stored` flag is
     * false for one, which is what gates every read. */
    int16_t  discard[DCTSZ * DCTSZ];
    /* Set while a decode owns this state, so a second decode handed
     * the same cfg.state is refused instead of memsetting the first
     * one's pointers out from under it.  See LZ_IN_USE. */
    uint64_t in_use;
} lz_t;

/*
 * The cancellation poll.  Optional, so a NULL cfg.cancel is a decode
 * that always runs to completion; the indirect call is the only cost
 * when one is supplied, and it happens a few thousand times for a 12 MP
 * file rather than a few million.
 */
static inline int lz_cancelled(const lz_t *p)
{
    return p->cfg->cancel && p->cfg->cancel(p->cfg->cancel_ctx);
}

/*
 * The input ended, and did so on its own rather than because the
 * caller's reader reported a failure.
 *
 * Past that point the parsers cannot tell a short file from a broken
 * one: a segment length that could not be read comes back as
 * LOWZAG_ERR_FORMAT, and so does a Huffman code decoded out of the zeros
 * the bit reader fabricates.  Both are the file ending.  Only a caller
 * who set allow_partial has this applied, and it decides which way an
 * ambiguity is resolved, never whether a complete file decodes.
 */
static int lz_ran_out(const lz_t *p)
{
    return p->rd.eof && !p->rd.io_err;
}

/* Clearing 36 MB of coefficient storage is a second or so on a
 * microcontroller with nothing in it that can be interrupted, so the
 * clear is chunked and polled between chunks.  64 KB measured: at 256 KB
 * the worst-case gap on a light file at 1/4 doubles for no speed gain. */
#define LZ_CLEAR_CHUNK  (64u * 1024u)

static lowzag_err_t lz_clear(lz_t *p, void *mem, size_t n)
{
    uint8_t *q = (uint8_t *)mem;

    while (n > 0) {
        size_t k = (n > LZ_CLEAR_CHUNK) ? (size_t)LZ_CLEAR_CHUNK : n;
        if (lz_cancelled(p)) {
            return LOWZAG_ERR_CANCELLED;
        }
        memset(q, 0, k);
        q += k;
        n -= k;
    }
    return LOWZAG_OK;
}

static void *lz_alloc(lz_t *p, size_t n)
{
    if (p->cfg->mem_limit && p->allocated + n > p->cfg->mem_limit) {
        return NULL;
    }
    void *m = p->cfg->alloc(p->cfg->alloc_ctx, n);
    if (m) {
        p->allocated += n;
        if (p->allocated > p->peak) {
            p->peak = p->allocated;
        }
    }
    return m;
}

/* ── bitmap helpers (nonzero history for unstored coefficients) ─────────────*/

static inline int nz_get(const uint8_t *nz, int k)
{
    return (nz[k >> 3] >> (k & 7)) & 1;
}

static inline void nz_set(uint8_t *nz, int k)
{
    nz[k >> 3] |= (uint8_t)(1 << (k & 7));
}

/* ── marker-level parsing ───────────────────────────────────────────────────*/

static int read_u16(rd_t *r)
{
    int a = rd_byte(r), b = rd_byte(r);
    if (a < 0 || b < 0) {
        return -1;
    }
    return (a << 8) | b;
}

/* Read the next marker byte (skipping fill bytes). -1 on EOF. */
static int next_marker(rd_t *r)
{
    int b;
    /* tolerate garbage between segments the way libjpeg does: scan
     * forward for FF */
    do {
        b = rd_byte(r);
        if (b < 0) {
            return -1;
        }
    } while (b != 0xFF);
    do {
        b = rd_byte(r);
        if (b < 0) {
            return -1;
        }
    } while (b == 0xFF);             /* fill bytes */
    return b;
}

static lowzag_err_t skip_segment(lz_t *p)
{
    int len = read_u16(&p->rd);
    if (len < 2) {
        return LOWZAG_ERR_FORMAT;
    }
    for (int i = 0; i < len - 2; i++) {
        if (rd_byte(&p->rd) < 0) {
            return LOWZAG_ERR_TRUNCATED;
        }
    }
    return LOWZAG_OK;
}

/* ── APPn passthrough ───────────────────────────────────────────────────────
 *
 * Hand an APPn payload to cfg.marker in chunks, buffering none of it.
 * The alternative designs were an allocation sized from the segment
 * header — which is a number in an attacker's file — and a
 * caller-supplied buffer with a cap, which is this with the copying done
 * twice and a truncation rule to explain.  Chunks put the ceiling where
 * the caller can apply their own judgement: @total is known before a
 * byte of payload is read, so refusing a segment costs nothing but the
 * read that would have happened anyway.
 *
 * The chunk buffer is a local rather than part of lz_t: it would
 * otherwise sit in the working state for every decode, including the
 * overwhelming majority that never attach a marker callback.
 *
 * Not inlined for the same reason the scan loops are not — this runs
 * once per segment and has no business holding registers the rest of
 * lowzag_decode() wants.
 */
#define LZ_APP_CHUNK  256

static LZ_NOINLINE lowzag_err_t deliver_app(lz_t *p, int m)
{
    uint8_t chunk[LZ_APP_CHUNK];
    int len = read_u16(&p->rd);

    if (len < 2) {
        return LOWZAG_ERR_FORMAT;
    }
    size_t total = (size_t)len - 2;
    size_t off = 0;
    int skipping = 0;

    /* do/while so an empty segment still produces exactly one call: "at
     * least once per selected segment" is a rule a caller can count on,
     * and a zero-length APPn is legal. */
    do {
        size_t n = total - off;
        if (n > sizeof chunk) {
            n = sizeof chunk;
        }
        for (size_t i = 0; i < n; i++) {
            int c = rd_byte(&p->rd);
            if (c < 0) {
                return LOWZAG_ERR_TRUNCATED;
            }
            chunk[i] = (uint8_t)c;
        }
        if (!skipping) {
            int rc = p->cfg->marker(p->cfg->marker_ctx, m, off, total,
                                    chunk, n);
            if (rc < 0) {
                return LOWZAG_ERR_CANCELLED;
            }
            /* Positive is "not this segment": the rest is still read,
             * because the decoder has to get past it either way, but
             * nothing more of it is delivered. */
            skipping = (rc > 0);
        }
        off += n;
        /* Between chunks rather than once per segment: a colour profile
         * is a long time to leave a caller's watchdog unfed, and this is
         * the only place in the marker loop where the work is unbounded. */
        if (lz_cancelled(p)) {
            return LOWZAG_ERR_CANCELLED;
        }
    } while (off < total);

    return LOWZAG_OK;
}

/* Whether this marker's payload was asked for.  A NULL callback and a
 * zero mask both mean no, which is what a zeroed config gives and what
 * every version before 2.2.0 did. */
static int app_wanted(const lowzag_cfg_t *cfg, int m)
{
    return cfg->marker && m >= 0xE0 && m <= 0xEF &&
           (cfg->marker_mask & (1u << (m - 0xE0))) != 0;
}

static lowzag_err_t parse_dqt(lz_t *p)
{
    int len = read_u16(&p->rd);
    if (len < 2) {
        return LOWZAG_ERR_FORMAT;
    }
    len -= 2;
    while (len > 0) {
        int pq_tq = rd_byte(&p->rd);
        if (pq_tq < 0) {
            return LOWZAG_ERR_TRUNCATED;
        }
        int pq = pq_tq >> 4, tq = pq_tq & 15;
        if (tq > 3 || pq > 1) {
            return LOWZAG_ERR_FORMAT;
        }
        len -= 1 + 64 * (pq + 1);
        if (len < 0) {
            return LOWZAG_ERR_FORMAT;
        }
        for (int i = 0; i < 64; i++) {
            int v;
            if (pq) {
                v = read_u16(&p->rd);
            } else {
                v = rd_byte(&p->rd);
            }
            if (v < 0) {
                return LOWZAG_ERR_TRUNCATED;
            }
            p->qt[tq][ZZ[i]] = (uint16_t)v;
        }
        p->qt_def[tq] = 1;
    }
    return LOWZAG_OK;
}

static lowzag_err_t parse_dht(lz_t *p)
{
    int len = read_u16(&p->rd);
    if (len < 2) {
        return LOWZAG_ERR_FORMAT;
    }
    len -= 2;
    while (len > 0) {
        int tc_th = rd_byte(&p->rd);
        if (tc_th < 0) {
            return LOWZAG_ERR_TRUNCATED;
        }
        int tc = tc_th >> 4, th = tc_th & 15;
        if (tc > 1 || th > 3) {
            return LOWZAG_ERR_FORMAT;
        }
        uint8_t counts[16];
        int total = 0;
        for (int i = 0; i < 16; i++) {
            int c = rd_byte(&p->rd);
            if (c < 0) {
                return LOWZAG_ERR_TRUNCATED;
            }
            counts[i] = (uint8_t)c;
            total += c;
        }
        if (total > 256) {
            return LOWZAG_ERR_FORMAT;
        }
        uint8_t vals[256];
        for (int i = 0; i < total; i++) {
            int v = rd_byte(&p->rd);
            if (v < 0) {
                return LOWZAG_ERR_TRUNCATED;
            }
            vals[i] = (uint8_t)v;
        }
        len -= 1 + 16 + total;
        if (len < 0) {
            return LOWZAG_ERR_FORMAT;
        }
        huff_t *h = (tc == 0) ? &p->hdc[th] : &p->hac[th];
        if (huff_build(h, counts, vals, total) != 0) {
            return LOWZAG_ERR_FORMAT;
        }
    }
    return LOWZAG_OK;
}

static lowzag_err_t parse_sof2(lz_t *p)
{
    int len = read_u16(&p->rd);
    int prec = rd_byte(&p->rd);
    p->H = read_u16(&p->rd);
    p->W = read_u16(&p->rd);
    int ncomp = rd_byte(&p->rd);
    if (len < 0 || prec < 0 || p->H <= 0 || p->W <= 0 || ncomp < 0) {
        return LOWZAG_ERR_TRUNCATED;
    }
    if (prec != 8) {
        return LOWZAG_ERR_UNSUPPORTED;
    }
    /* Validate BEFORE storing: cleanup iterates p->ncomp, so a bogus
     * count here would walk past the component array and free garbage. */
    if (ncomp != 1 && ncomp != 3) {
        return LOWZAG_ERR_UNSUPPORTED;
    }
    p->ncomp = ncomp;
    /* Not a policy choice — a memory-safety boundary.  See LOWZAG_MAX_DIM. */
    if (p->W > LOWZAG_MAX_DIM || p->H > LOWZAG_MAX_DIM) {
        return LOWZAG_ERR_UNSUPPORTED;
    }
    if (len != 8 + 3 * ncomp) {
        return LOWZAG_ERR_FORMAT;
    }
    p->hmax = 1;
    p->vmax = 1;
    for (int i = 0; i < p->ncomp; i++) {
        comp_t *c = &p->comp[i];
        c->id = rd_byte(&p->rd);
        int hv = rd_byte(&p->rd);
        c->tq = rd_byte(&p->rd);
        if (c->tq < 0) {
            return LOWZAG_ERR_TRUNCATED;
        }
        c->h = hv >> 4;
        c->v = hv & 15;
        c->dc_cov = -1;              /* no first DC pass has reached it */
        if (c->h < 1 || c->h > 2 || c->v < 1 || c->v > 2 || c->tq > 3) {
            return LOWZAG_ERR_UNSUPPORTED;
        }
        if (c->h > p->hmax) {
            p->hmax = c->h;
        }
        if (c->v > p->vmax) {
            p->vmax = c->v;
        }
    }
    return LOWZAG_OK;
}

/* ── geometry + storage setup (after SOF) ───────────────────────────────────*/

static int ceil_div(int a, int b)
{
    return (a + b - 1) / b;
}

/* ── frame geometry, and the byte count that follows from it ────────────────
 *
 * lowzag_estimate() has to report, to the byte, what lowzag_decode()
 * will ask alloc() for — including how the mem_limit ladder degrades the
 * answer.  The only way to keep that true as the storage layout changes
 * is for the two to run the same code, so the arithmetic lives here,
 * once, and both go through plan_frame().
 *
 * In: the frame header.  Out: the geometry the decoder needs, plus
 * alloc_bytes, the sum of every lz_alloc() the decode will make.
 * Nothing is released before lowzag_decode() returns, so that sum is
 * also the high-water mark it reports as info.peak_alloc.
 */
typedef struct {
    int bw, bh;                      /* real block dims (non-interleaved) */
    int pad_bw, pad_bh;              /* MCU-padded dims (interleaved)     */
    int cbx, cby;                    /* first stored block                */
    int cbw, cbh;                    /* stored block rectangle            */
    int bs_h, bs_v;                  /* this component's IDCT size/axis   */
    int K;                           /* kept zigzag prefix covering them  */
    int out_w, out_h;                /* this component's decoded extent   */
} gcomp_t;

typedef struct {
    /* in: straight out of the SOF */
    int W, H, ncomp;
    int h[MAX_COMPS], v[MAX_COMPS];
    int hmax, vmax;
    /* in: the requested region, already clamped to the frame.  want_crop
     * is 0 for the whole frame, which is also what an all-zero cfg.crop_*
     * and a rectangle covering the frame both come to. */
    int want_crop;
    int rx, ry, rw, rh;
    /* out */
    int     scale, bs;
    int     full_out_w, full_out_h;  /* whole-frame output grid           */
    int     out_w, out_h;            /* delivered size                    */
    int     ox0, oy0;                /* delivered origin, output pixels   */
    int     crop_x, crop_y;          /* delivered rectangle, source px    */
    int     crop_w, crop_h;
    int     cropped;                 /* the crop actually restricts       */
    int     mcux, mcuy;
    int     mx0, my0, mx1, my1;      /* stored MCU rectangle              */
    int     chroma_steps;
    size_t  alloc_bytes;
    gcomp_t comp[MAX_COMPS];
} geom_t;

/* Per-axis IDCT size for component @i at whole-frame scale @s, and the
 * zigzag prefix that covers it.
 *
 * A subsampled component already carries fewer samples, so it needs a
 * LARGER block size than the luma to land on the same output grid: at
 * 1/2 overall, 2x2-subsampled chroma is decoded at full 8x8 and not
 * reduced at all.  Using the luma's size for every component (and
 * replicating afterwards) halves chroma resolution — visible as soft
 * color on downscaled photos. */
static void geom_block_size(geom_t *g, int i, int s)
{
    gcomp_t *c = &g->comp[i];
    int rh = s * g->h[i] / g->hmax;
    int rv = s * g->v[i] / g->vmax;
    c->bs_h = rh < 1 ? DCTSZ : DCTSZ / rh;
    c->bs_v = rv < 1 ? DCTSZ : DCTSZ / rv;
    if (c->bs_h > DCTSZ) { c->bs_h = DCTSZ; }
    if (c->bs_v > DCTSZ) { c->bs_v = DCTSZ; }
    if (c->bs_h < 1) { c->bs_h = 1; }
    if (c->bs_v < 1) { c->bs_v = 1; }
    c->K = keep_for_block(c->bs_h, c->bs_v);
}

/*
 * Snap the requested rectangle out to whole output pixels at scale @s,
 * and record what that delivers.  Outward, never inward, so the caller
 * never loses a pixel they asked for; the cost is that the delivered
 * size depends on where the rectangle starts, which is why it is
 * reported rather than left to be inferred.
 *
 * A rectangle that ends up covering the whole output grid is inert: it
 * is turned back into "no crop" here, so it allocates nothing extra.
 * That case is not cosmetic — at scale 1 the per-block map that unstored
 * blocks need is not otherwise allocated at all, so a full-frame crop
 * that stayed "active" would cost more than no crop.
 */
static void plan_crop(geom_t *g, int s)
{
    g->full_out_w = ceil_div(g->W, s);
    g->full_out_h = ceil_div(g->H, s);

    int x0 = 0, y0 = 0, x1 = g->full_out_w, y1 = g->full_out_h;
    if (g->want_crop) {
        x0 = g->rx / s;
        y0 = g->ry / s;
        x1 = ceil_div(g->rx + g->rw, s);
        y1 = ceil_div(g->ry + g->rh, s);
        if (x1 > g->full_out_w) { x1 = g->full_out_w; }
        if (y1 > g->full_out_h) { y1 = g->full_out_h; }
    }
    g->ox0     = x0;
    g->oy0     = y0;
    g->out_w   = x1 - x0;
    g->out_h   = y1 - y0;
    g->cropped = !(x0 == 0 && y0 == 0 &&
                   x1 == g->full_out_w && y1 == g->full_out_h);

    /* The source rectangle those output pixels cover, which is what
     * info.crop_* reports.  With no crop it comes out as the frame. */
    g->crop_x = x0 * s;
    g->crop_y = y0 * s;
    int ex = x1 * s, ey = y1 * s;
    if (ex > g->W) { ex = g->W; }
    if (ey > g->H) { ey = g->H; }
    g->crop_w = ex - g->crop_x;
    g->crop_h = ey - g->crop_y;

    /* Storage snaps out again, to whole MCUs.  That rectangle does not
     * move with the scale — an MCU is 8*hmax by 8*vmax SOURCE pixels
     * whatever the scale, and s divides 8*hmax, so the delivery snap
     * above cannot cross an MCU boundary the request did not.  Which is
     * what lets the mem_limit ladder change the scale under a crop
     * without changing which blocks are stored. */
    int mw = DCTSZ * g->hmax, mh = DCTSZ * g->vmax;
    g->mx0 = g->crop_x / mw;
    g->my0 = g->crop_y / mh;
    g->mx1 = ceil_div(g->crop_x + g->crop_w, mw);
    g->my1 = ceil_div(g->crop_y + g->crop_h, mh);
    if (g->mx1 > g->mcux) { g->mx1 = g->mcux; }
    if (g->my1 > g->mcuy) { g->my1 = g->mcuy; }

    for (int i = 0; i < g->ncomp; i++) {
        gcomp_t *c = &g->comp[i];
        c->cbx = g->mx0 * g->h[i];
        c->cby = g->my0 * g->v[i];
        c->cbw = (g->mx1 - g->mx0) * g->h[i];
        c->cbh = (g->my1 - g->my0) * g->v[i];
    }
}

/* Whether this component needs the per-block nonzero map: because the
 * kept prefix is short of 64, or because the crop leaves blocks whose
 * coefficients are not stored anywhere else.  geom_bytes() and
 * setup_frame() must agree on this to the byte. */
static int geom_needs_nz(const gcomp_t *c)
{
    return c->K < 64 || c->cbw < c->pad_bw || c->cbh < c->pad_bh;
}

/* Every byte lz_alloc() will be asked for: per component the coefficient
 * storage, its nonzero bitmap and its output strip, plus the shared RGB
 * row buffer and the per-component column maps.  The working state is
 * deliberately absent — it comes from cfg.state or straight from
 * cfg.alloc, and info.peak_alloc excludes it. */
static size_t geom_bytes(const geom_t *g)
{
    size_t need = (size_t)g->out_w * 3;                        /* rowbuf */
    need += (size_t)g->out_w * sizeof(uint16_t) * (size_t)g->ncomp;
    for (int i = 0; i < g->ncomp; i++) {
        const gcomp_t *c = &g->comp[i];
        need += (size_t)c->cbw * (size_t)c->cbh *
                (size_t)c->K * sizeof(int16_t);
        if (geom_needs_nz(c)) {
            need += (size_t)c->pad_bw * (size_t)c->pad_bh * 8;
        }
        need += (size_t)(c->cbw * c->bs_h) * (size_t)(g->v[i] * c->bs_v);
    }
    return need;
}

/* Choose the scale, lay out the frame, and fit it under cfg.mem_limit.
 * Reads only force_scale, target_w, target_h, crop_x/y/w/h and mem_limit
 * from @cfg; the callbacks are never touched, which is what lets
 * lowzag_estimate() answer for a config that has none. */
static lowzag_err_t plan_frame(geom_t *g, const lowzag_cfg_t *cfg)
{
    /* The region of interest, in source pixels, clamped to the frame.
     *
     * A nonzero origin with a zero extent is refused rather than read as
     * "no crop": the caller plainly meant a rectangle, and silently
     * decoding the whole frame into a buffer they sized for a region is
     * the failure this whole feature has to avoid. */
    g->want_crop = 0;
    if (cfg->crop_x != 0 || cfg->crop_y != 0 ||
        cfg->crop_w != 0 || cfg->crop_h != 0) {
        if (cfg->crop_x < 0 || cfg->crop_y < 0 ||
            cfg->crop_w <= 0 || cfg->crop_h <= 0) {
            return LOWZAG_ERR_INVALID_ARG;
        }
        if (cfg->crop_x >= g->W || cfg->crop_y >= g->H) {
            return LOWZAG_ERR_INVALID_ARG;   /* empty intersection */
        }
        g->rx = cfg->crop_x;
        g->ry = cfg->crop_y;
        g->rw = (cfg->crop_w > g->W - g->rx) ? g->W - g->rx : cfg->crop_w;
        g->rh = (cfg->crop_h > g->H - g->ry) ? g->H - g->ry : cfg->crop_h;
        g->want_crop = 1;
    }

    g->mcux = ceil_div(g->W, DCTSZ * g->hmax);
    g->mcuy = ceil_div(g->H, DCTSZ * g->vmax);

    /* Scale selection: the largest of {8,4,2,1} whose output still
     * covers the target, else 1.  With a crop the comparison is against
     * the CROP's output size, because that is the image the caller is
     * going to be handed — "show me this region, at least this big" is
     * one request.
     *
     * A caller who names no target has asked for no reduction, so the
     * answer is 1.  Without this test the loop below compares against a
     * target of 1x1, every candidate "covers" it, and 8 wins — which
     * meant a config carrying only the mandatory callbacks decoded a
     * 4000x3000 file to 500x375 and returned success.  Plausible-looking
     * wrong output with an OK return is the worst shape a bug can take,
     * and filling in only what the header marks as required is exactly
     * what a new caller does first. */
    int s = cfg->force_scale;
    if (s == 0) {
        int tw = cfg->target_w;
        int th = cfg->target_h;
        s = 1;
        if (tw > 0 || th > 0) {
            if (tw <= 0) { tw = 1; }
            if (th <= 0) { th = 1; }
            for (int cand = 8; cand > 1; cand >>= 1) {
                plan_crop(g, cand);
                if (g->out_w >= tw && g->out_h >= th) {
                    s = cand;
                    break;
                }
            }
        }
    }
    g->scale = s;
    g->bs    = DCTSZ / s;

    /* Storage grows with the finer chroma geom_block_size() gives a
     * subsampled component, so if the result would exceed the caller's
     * ceiling the chroma is stepped back down — quality degrades before
     * the decode fails outright. */
    for (int i = 0; i < g->ncomp; i++) {
        gcomp_t *c = &g->comp[i];
        int sw = ceil_div(g->W * g->h[i], g->hmax);   /* component samples */
        int sh = ceil_div(g->H * g->v[i], g->vmax);
        c->bw = ceil_div(sw, DCTSZ);
        c->bh = ceil_div(sh, DCTSZ);
        c->pad_bw = g->mcux * g->h[i];
        c->pad_bh = g->mcuy * g->v[i];
        geom_block_size(g, i, s);
    }
    plan_crop(g, s);

    for (;;) {
        size_t need = geom_bytes(g);
        if (!cfg->mem_limit || need <= cfg->mem_limit) {
            g->alloc_bytes = need;
            break;
        }

        /* Too big.  First give up the chroma resolution won above, then
         * — rather than refusing the image — decode the whole frame one
         * step smaller.  A softer, letterboxed slide beats an error
         * card. */
        int changed = 0;
        for (int i = 1; i < g->ncomp; i++) {
            gcomp_t *c = &g->comp[i];
            if (c->bs_h > g->bs || c->bs_v > g->bs) {
                if (c->bs_h > g->bs) { c->bs_h /= 2; }
                if (c->bs_v > g->bs) { c->bs_v /= 2; }
                c->K = keep_for_block(c->bs_h, c->bs_v);
                changed = 1;
                /* Report it.  This tier costs colour resolution while
                 * leaving scale and the output dimensions untouched, so
                 * a caller watching info.scale alone cannot see it —
                 * which is what the documentation used to tell them to
                 * do. */
                g->chroma_steps++;
            }
        }
        if (changed) {
            continue;
        }
        if (s >= 8 || cfg->force_scale) {
            return LOWZAG_ERR_NOMEM;      /* nothing left to give up */
        }
        s *= 2;
        g->scale = s;
        g->bs    = DCTSZ / s;
        for (int i = 0; i < g->ncomp; i++) {
            geom_block_size(g, i, s);
        }
        /* The crop is never given up to satisfy the ceiling — the caller
         * asked for that region — but the delivery snap does move with
         * the scale, so it is redone here.  The stored MCU rectangle
         * does not move; see plan_crop(). */
        plan_crop(g, s);
    }

    for (int i = 0; i < g->ncomp; i++) {
        gcomp_t *c = &g->comp[i];
        c->out_w = ceil_div(ceil_div(g->W * g->h[i], g->hmax) * c->bs_h, DCTSZ);
        c->out_h = ceil_div(ceil_div(g->H * g->v[i], g->vmax) * c->bs_v, DCTSZ);
    }
    return LOWZAG_OK;
}

static lowzag_err_t setup_frame(lz_t *p)
{
    geom_t g;
    memset(&g, 0, sizeof g);
    g.W     = p->W;
    g.H     = p->H;
    g.ncomp = p->ncomp;
    g.hmax  = p->hmax;
    g.vmax  = p->vmax;
    for (int i = 0; i < p->ncomp; i++) {
        g.h[i] = p->comp[i].h;
        g.v[i] = p->comp[i].v;
    }

    lowzag_err_t ge = plan_frame(&g, p->cfg);

    /* Copy the plan back whether it fitted or not.  lowzag_decode()
     * fills @info from these fields the moment the frame header is
     * parsed, and lowzag.h promises the dimensions are meaningful for
     * an error return that came after it.  On LOWZAG_ERR_NOMEM the last
     * thing plan_frame() tried is exactly what a caller wants to see:
     * the smallest frame lowzag was willing to attempt and the chroma
     * tiers it had already given up before deciding the ceiling was
     * unreachable.  (Through 1.0.2 the planner wrote into these fields
     * as it went, so this was free; keep it true now that it does
     * not.) */
    p->scale        = g.scale;
    p->bs           = g.bs;
    p->full_out_w   = g.full_out_w;
    p->full_out_h   = g.full_out_h;
    p->out_w        = g.out_w;
    p->out_h        = g.out_h;
    p->ox0          = g.ox0;
    p->oy0          = g.oy0;
    p->crop_x       = g.crop_x;
    p->crop_y       = g.crop_y;
    p->crop_w       = g.crop_w;
    p->crop_h       = g.crop_h;
    p->mcux         = g.mcux;
    p->mcuy         = g.mcuy;
    p->chroma_steps = g.chroma_steps;

    for (int i = 0; i < p->ncomp; i++) {
        comp_t *c = &p->comp[i];
        const gcomp_t *gc = &g.comp[i];
        c->bw     = gc->bw;
        c->bh     = gc->bh;
        c->pad_bw = gc->pad_bw;
        c->pad_bh = gc->pad_bh;
        c->cbx    = gc->cbx;
        c->cby    = gc->cby;
        c->cbw    = gc->cbw;
        c->cbh    = gc->cbh;
        c->bs_h   = gc->bs_h;
        c->bs_v   = gc->bs_v;
        c->K      = gc->K;
        c->out_w  = gc->out_w;
        c->out_h  = gc->out_h;
    }

    if (ge != LOWZAG_OK) {
        return ge;
    }

    for (int i = 0; i < p->ncomp; i++) {
        comp_t *c = &p->comp[i];
        const gcomp_t *gc = &g.comp[i];
        size_t nblocks, coef_bytes, padded, nz_bytes;
        if (!size_mul((size_t)c->cbw, (size_t)c->cbh, &nblocks) ||
            !size_mul(nblocks, (size_t)c->K * sizeof(int16_t), &coef_bytes) ||
            !size_mul((size_t)c->pad_bw, (size_t)c->pad_bh, &padded) ||
            !size_mul(padded, 8, &nz_bytes)) {
            return LOWZAG_ERR_NOMEM;
        }
        c->coef = lz_alloc(p, coef_bytes);
        if (!c->coef) {
            return LOWZAG_ERR_NOMEM;
        }
        lowzag_err_t ce = lz_clear(p, c->coef, coef_bytes);
        if (ce != LOWZAG_OK) {
            return ce;
        }
        if (geom_needs_nz(gc)) {
            c->nz = lz_alloc(p, nz_bytes);
            if (!c->nz) {
                return LOWZAG_ERR_NOMEM;
            }
            ce = lz_clear(p, c->nz, nz_bytes);
            if (ce != LOWZAG_OK) {
                return ce;
            }
        }
    }

    return LOWZAG_OK;
}

/* ── scan decoding ──────────────────────────────────────────────────────────*/

typedef struct {
    int ncomp;                       /* components in this scan */
    int ci[MAX_COMPS];               /* indices into p->comp    */
    int dc_tbl[MAX_COMPS], ac_tbl[MAX_COMPS];
    int ss, se, ah, al;
} scan_t;

/* Whether this block's coefficients are stored at all.  Without a crop
 * every block is, and this is constant-true. */
static inline int blk_in_crop(const comp_t *c, int bx, int by)
{
    return (unsigned)(bx - c->cbx) < (unsigned)c->cbw &&
           (unsigned)(by - c->cby) < (unsigned)c->cbh;
}

/* Locate a block's storage.  A block outside the crop has none, and gets
 * the discard block instead: the DC scans store into it unconditionally,
 * nothing reads it, and the alternative is a branch in the block loop. */
static inline int16_t *blk_coef(lz_t *p, comp_t *c, int bx, int by)
{
    if (blk_in_crop(c, bx, by)) {
        return c->coef + ((size_t)(by - c->cby) * (size_t)c->cbw +
                          (size_t)(bx - c->cbx)) * (size_t)c->K;
    }
    return p->discard;
}

/* The nonzero map is indexed on the full padded grid, because a
 * refinement scan's bit count depends on the history of blocks that are
 * never displayed as much as on the ones that are. */
static inline uint8_t *blk_nz(lz_t *p, comp_t *c, int bx, int by)
{
    (void)p;
    return c->nz + ((size_t)by * c->pad_bw + bx) * 8;
}

static lowzag_err_t restart_here(lz_t *p, const scan_t *sc)
{
    /* entropy segment must be at a marker (or already terminated) */
    while (!p->rd.marker) {
        p->rd.bitcnt = 0;
        p->rd.bitbuf = 0;
        bits_need_slow(&p->rd, 1);   /* consumes exactly one byte */
        if (p->rd.eof) {
            break;
        }
    }
    int m = p->rd.marker;
    if (m < 0xD0 || m > 0xD7) {
        return LOWZAG_ERR_FORMAT;
    }
    bits_reset(&p->rd);
    for (int i = 0; i < sc->ncomp; i++) {
        p->comp[sc->ci[i]].dc_pred = 0;
    }
    p->eobrun = 0;
    return LOWZAG_OK;
}

/* How much of the frame a first DC pass covered.  It is the one number
 * that distinguishes a partial image that is merely soft from one with
 * blocks that hold nothing at all and render as flat mid-grey, so it has
 * to be the worst case over the whole frame rather than over the scans
 * that happened to run.
 *
 * The distinction matters because a DC scan need not be interleaved.
 * The usual progressive script emits one DC scan covering every
 * component, and then this is simply that scan's progress; but a file
 * may carry one DC scan per component, and a stream cut between them
 * leaves the components that did not arrive with no coefficients
 * whatever.  Counting only the scans that ran reported 100% for exactly
 * that case — a 320x240 file cut after its luma DC scan came back
 * LOWZAG_PARTIAL, dc_coverage 100, and a monochrome picture 15 dB from
 * the truth.  So the coverage is recorded per component and the frame's
 * number is the worst of them, with a component no first DC pass reached
 * counting as the 0% it is. */
static void note_dc_cov(lz_t *p, const scan_t *sc, int done, int total)
{
    int pct = (total > 0)
              ? (int)(((int64_t)done * 100) / (int64_t)total)
              : 0;

    for (int s = 0; s < sc->ncomp; s++) {
        comp_t *c = &p->comp[sc->ci[s]];
        if (c->dc_cov < 0 || pct < c->dc_cov) {
            c->dc_cov = pct;
        }
    }
    int worst = 100;
    for (int i = 0; i < p->ncomp; i++) {
        int cov = (p->comp[i].dc_cov < 0) ? 0 : p->comp[i].dc_cov;
        if (cov < worst) {
            worst = cov;
        }
    }
    p->dc_cov = worst;
}

/* DC scans (interleaved or single-component). */
static LZ_NOINLINE lowzag_err_t decode_dc_scan(lz_t *p, const scan_t *sc)
{
    int first = (sc->ah == 0);
    int mcus, per_row;
    int interleaved = sc->ncomp > 1;

    if (interleaved) {
        per_row = p->mcux;
        mcus = p->mcux * p->mcuy;
    } else {
        comp_t *c = &p->comp[sc->ci[0]];
        per_row = c->bw;
        mcus = c->bw * c->bh;
    }

    int togo = p->restart ? p->restart : -1;

    /* The cancellation poll lives in this outer loop, once per block
     * row, so that the MCU loop below keeps the shape it had before a
     * poll existed.  A test inside it — even a decrementing counter —
     * costs 17-22% on a 12 MP file through register pressure; out here
     * it costs about 1%. */
    int m = 0;
    int done = 0;                    /* MCUs decoded from bytes that exist */
    while (m < mcus) {
        /* The input ran out mid-scan and the bit reader is fabricating
         * zeroes.  Stop here rather than decode the rest of the frame
         * out of nothing: the coefficients already stored are real, and
         * that is precisely what a partial decode is for.  Only for a
         * caller who asked — without allow_partial this loop is byte for
         * byte what it was, and the truncation is reported at the end of
         * the scan as before. */
        if (p->partial && p->rd.pad) {
            break;
        }
        int stop = m + per_row;
        if (stop > mcus) {
            stop = mcus;
        }
        if (lz_cancelled(p)) {
            return LOWZAG_ERR_CANCELLED;
        }
        for (; m < stop; m++) {
            if (togo == 0) {
                lowzag_err_t e = restart_here(p, sc);
                if (e != LOWZAG_OK) {
                    return e;
                }
                togo = p->restart;
            }
            for (int s = 0; s < sc->ncomp; s++) {
                comp_t *c = &p->comp[sc->ci[s]];
                int nb_h = interleaved ? c->h : 1;
                int nb_v = interleaved ? c->v : 1;
                for (int by = 0; by < nb_v; by++) {
                    for (int bx = 0; bx < nb_h; bx++) {
                        int gx, gy;
                        if (interleaved) {
                            gx = (m % p->mcux) * c->h + bx;
                            gy = (m / p->mcux) * c->v + by;
                        } else {
                            gx = m % c->bw;
                            gy = m / c->bw;
                        }
                        int16_t *co = blk_coef(p, c, gx, gy);
                        if (first) {
                            int t = huff_decode(&p->rd, &p->hdc[sc->dc_tbl[s]]);
                            if (t < 0 || t > 15) {
                                return LOWZAG_ERR_FORMAT;
                            }
                            int diff = t ? huff_extend(get_bits(&p->rd, t), t)
                                         : 0;
                            /* Clamp on every accumulate, not just before
                             * the store: this is what keeps |dc_pred|
                             * within DC_PRED_MAX on entry to the next
                             * `+=`, which is in turn what makes that
                             * addition unable to overflow.  Moving the
                             * clamp down to the store reintroduces the
                             * bug.  A malformed file can otherwise walk
                             * the predictor out of int range — undefined
                             * behaviour rather than a wrong pixel, and a
                             * -ftrapv build aborts on it. */
                            c->dc_pred += diff;
                            if (c->dc_pred > DC_PRED_MAX) {
                                c->dc_pred = DC_PRED_MAX;
                            }
                            if (c->dc_pred < -DC_PRED_MAX) {
                                c->dc_pred = -DC_PRED_MAX;
                            }
                            co[0] = (int16_t)((uint32_t)c->dc_pred << sc->al);
                        } else {
                            if (get_bit(&p->rd)) {
                                co[0] = (int16_t)(co[0] | (1 << sc->al));
                            }
                        }
                    }
                }
            }
            if (togo > 0) {
                togo--;
            }
        }
        /* Only if the bytes this row came out of were real ones: the pad
         * flag may have been raised part way through it. */
        if (!p->rd.pad) {
            done = m;
        }
    }
    if (first) {
        note_dc_cov(p, sc, done, mcus);
    }
    if (p->partial && p->rd.pad) {
        return LOWZAG_ERR_TRUNCATED;
    }
    return LOWZAG_OK;
}

/* AC scans: always single component, one block per MCU. */
static LZ_NOINLINE lowzag_err_t decode_ac_scan(lz_t *p, const scan_t *sc)
{
    comp_t *c = &p->comp[sc->ci[0]];
    const huff_t *ht = &p->hac[sc->ac_tbl[0]];
    if (!ht->defined) {
        return LOWZAG_ERR_FORMAT;
    }
    int mcus = c->bw * c->bh;
    int togo = p->restart ? p->restart : -1;
    int p1 = 1 << sc->al;
    int m1 = -(1 << sc->al);

    /* Poll once per block row, from an outer loop, so the block loop
     * below is byte for byte what it was.  See decode_dc_scan(). */
    int m = 0;
    while (m < mcus) {
        /* Input exhausted: stop, keeping what was really decoded.  See
         * decode_dc_scan(); the same gate, for the same reason. */
        if (p->partial && p->rd.pad) {
            return LOWZAG_ERR_TRUNCATED;
        }
        int stop = m + c->bw;
        if (stop > mcus) {
            stop = mcus;
        }
        if (lz_cancelled(p)) {
            return LOWZAG_ERR_CANCELLED;
        }
        for (; m < stop; m++) {
            if (togo == 0) {
                lowzag_err_t e = restart_here(p, sc);
                if (e != LOWZAG_OK) {
                    return e;
                }
                togo = p->restart;
            }
            int bx = m % c->bw, by = m / c->bw;
            int16_t *co = blk_coef(p, c, bx, by);
            uint8_t *nz = c->nz ? blk_nz(p, c, bx, by) : NULL;
            /* How much of this block is kept: the band-limited prefix
             * for a block inside the crop, nothing at all for one
             * outside it.  Folding the crop into the bound rather than
             * testing for it keeps both inner loops below the shape they
             * had when there was no such thing as a crop. */
            int kstore = blk_in_crop(c, bx, by) ? c->K : 0;

            if (sc->ah == 0) {
                /* ── AC first scan ── */
                if (p->eobrun > 0) {
                    p->eobrun--;
                } else {
                    for (int k = sc->ss; k <= sc->se; ) {
                        int rs = huff_decode(&p->rd, ht);
                        if (rs < 0) {
                            return LOWZAG_ERR_FORMAT;
                        }
                        int rr = rs >> 4, s = rs & 15;
                        if (s == 0) {
                            if (rr != 15) {
                                p->eobrun = (1u << rr) - 1;
                                if (rr) {
                                    p->eobrun += (uint32_t)get_bits(&p->rd, rr);
                                }
                                break;
                            }
                            k += 16;                         /* ZRL */
                        } else {
                            k += rr;
                            if (k > sc->se) {
                                return LOWZAG_ERR_FORMAT;
                            }
                            int v = huff_extend(get_bits(&p->rd, s), s) *
                                    (1 << sc->al);
                            if (k < kstore) {
                                co[k] = (int16_t)v;
                            }
                            if (nz) {
                                nz_set(nz, k);
                            }
                            k++;
                        }
                    }
                }
            } else {
                /* ── AC refinement scan ── */
                int k = sc->ss;
                if (p->eobrun == 0) {
                    while (k <= sc->se) {
                        int rs = huff_decode(&p->rd, ht);
                        if (rs < 0) {
                            return LOWZAG_ERR_FORMAT;
                        }
                        int rr = rs >> 4, s = rs & 15;
                        int newval = 0;
                        if (s == 0) {
                            if (rr != 15) {
                                p->eobrun = (1u << rr);
                                if (rr) {
                                    p->eobrun += (uint32_t)get_bits(&p->rd, rr);
                                }
                                break;       /* EOB run handled below */
                            }
                            /* rr == 15: skip 16 zero-history coeffs */
                        } else {
                            if (s != 1) {
                                return LOWZAG_ERR_FORMAT;
                            }
                            newval = get_bit(&p->rd) ? p1 : m1;
                        }
                        while (k <= sc->se) {
                            int stored = (k < kstore);
                            int nonzero = stored ? (co[k] != 0)
                                                 : (nz && nz_get(nz, k));
                            if (nonzero) {
                                if (get_bit(&p->rd) && stored) {
                                    if ((co[k] & p1) == 0) {
                                        co[k] = (int16_t)(co[k] +
                                                (co[k] >= 0 ? p1 : m1));
                                    }
                                }
                            } else {
                                if (rr == 0) {
                                    if (newval) {
                                        if (stored) {
                                            co[k] = (int16_t)newval;
                                        }
                                        if (nz) {
                                            nz_set(nz, k);
                                        }
                                    }
                                    k++;
                                    break;
                                }
                                rr--;
                            }
                            k++;
                        }
                    }
                }
                if (p->eobrun > 0) {
                    /* inside an EOB run: corrections run to band end */
                    for (; k <= sc->se; k++) {
                        int stored = (k < kstore);
                        int nonzero = stored ? (co[k] != 0)
                                             : (nz && nz_get(nz, k));
                        if (nonzero) {
                            if (get_bit(&p->rd) && stored) {
                                if ((co[k] & p1) == 0) {
                                    co[k] = (int16_t)(co[k] +
                                            (co[k] >= 0 ? p1 : m1));
                                }
                            }
                        }
                    }
                    p->eobrun--;
                }
            }
            if (togo > 0) {
                togo--;
            }
        }
    }
    if (p->partial && p->rd.pad) {
        return LOWZAG_ERR_TRUNCATED;
    }
    return LOWZAG_OK;
}

static lowzag_err_t parse_sos_and_decode(lz_t *p)
{
    int len = read_u16(&p->rd);
    int ns = rd_byte(&p->rd);
    if (len < 0 || ns < 1 || ns > MAX_COMPS) {
        return LOWZAG_ERR_FORMAT;
    }
    scan_t sc;
    memset(&sc, 0, sizeof sc);
    sc.ncomp = ns;
    for (int i = 0; i < ns; i++) {
        int cs = rd_byte(&p->rd);
        int tt = rd_byte(&p->rd);
        if (cs < 0 || tt < 0) {
            return LOWZAG_ERR_TRUNCATED;
        }
        int found = -1;
        for (int j = 0; j < p->ncomp; j++) {
            if (p->comp[j].id == cs) {
                found = j;
            }
        }
        if (found < 0) {
            return LOWZAG_ERR_FORMAT;
        }
        sc.ci[i] = found;
        sc.dc_tbl[i] = tt >> 4;
        sc.ac_tbl[i] = tt & 15;
        if (sc.dc_tbl[i] > 3 || sc.ac_tbl[i] > 3) {
            return LOWZAG_ERR_FORMAT;
        }
    }
    sc.ss = rd_byte(&p->rd);
    sc.se = rd_byte(&p->rd);
    int a = rd_byte(&p->rd);
    if (sc.ss < 0 || sc.se < 0 || a < 0) {
        return LOWZAG_ERR_TRUNCATED;
    }
    sc.ah = a >> 4;
    sc.al = a & 15;

    /* progressive scan legality (tolerant subset) */
    if (sc.ss == 0) {
        if (sc.se != 0) {
            return LOWZAG_ERR_FORMAT;    /* band may not span DC + AC */
        }
    } else {
        if (sc.se < sc.ss || sc.se > 63 || sc.ncomp != 1) {
            return LOWZAG_ERR_FORMAT;
        }
    }
    if (sc.al > 13 || sc.ah > 13) {
        return LOWZAG_ERR_FORMAT;
    }

    /* Every scan must be entropy-decoded, even one whose band lies
     * entirely above the coefficients we keep.  Skipping such a scan
     * looks tempting, but refinement scans routinely span a wider band
     * than the first scans that populated it (libjpeg's default script
     * emits first scans [1..5] and [6..63], then a single refinement
     * [1..63]).  A refinement's bit count depends on which coefficients
     * are already nonzero across its whole band, so dropping an earlier
     * scan desynchronizes every later one.  The nonzero bitmap tracks
     * all 64 positions for exactly this reason; only the VALUES outside
     * the kept band are discarded, which is where the memory saving
     * comes from. */

    if (sc.ss == 0) {
        const huff_t *dh = &p->hdc[sc.dc_tbl[0]];
        if (sc.ah == 0 && !dh->defined) {
            return LOWZAG_ERR_FORMAT;
        }
    }

    bits_reset(&p->rd);
    p->eobrun = 0;
    lowzag_err_t e = (sc.ss == 0) ? decode_dc_scan(p, &sc)
                                 : decode_ac_scan(p, &sc);
    if (e != LOWZAG_OK) {
        /* Past the end of the stream every bit is fabricated, so an
         * invalid Huffman code here is the file running out rather than
         * a malformed file, and a caller who asked for partial output
         * needs the difference.  A reader that reported FAILURE is
         * LOWZAG_ERR_IO even so — that is the caller's own I/O breaking,
         * not a short file, and it is checked first for that reason.
         *
         * The whole test is inside `p->partial`, and cancellation is
         * outside it, so a caller who did not opt in gets back exactly
         * the code 2.1.0 gave them for the same stream. */
        if (p->partial && e != LOWZAG_ERR_CANCELLED) {
            if (p->rd.io_err) {
                return LOWZAG_ERR_IO;
            }
            if (p->rd.pad) {
                return LOWZAG_ERR_TRUNCATED;
            }
        }
        return e;
    }
    if (p->rd.io_err) {
        return LOWZAG_ERR_IO;
    }
    /* If entropy ended exactly at a marker, hand it to the marker loop. */
    if (p->rd.marker && p->rd.marker != 0xD9) {
        rd_unget(&p->rd, (uint8_t)p->rd.marker);
        rd_unget(&p->rd, 0xFF);
    } else if (p->rd.marker == 0xD9 && p->rd.eof) {
        return LOWZAG_ERR_TRUNCATED;  /* EOF inside entropy data */
    } else if (p->rd.marker == 0xD9) {
        rd_unget(&p->rd, 0xD9);
        rd_unget(&p->rd, 0xFF);
    }
    bits_reset(&p->rd);
    p->scans_done++;                 /* this one ran start to finish */
    return LOWZAG_OK;
}

/* ── output stage ───────────────────────────────────────────────────────────*/

static inline uint8_t clamp8(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return (uint8_t)v;
}

/* ── inverse DCT ─────────────────────────────────────────────────────────────
 *
 * Separable, factored, integer.  The naive form — every output a sum
 * over every input — costs 64 multiply-accumulates per 8-point pass;
 * the even/odd decomposition below costs about a dozen, and it stays in
 * int32, which matters on a core with no 64-bit multiplier.
 *
 * Scaling follows libjpeg's islow: constants in Q13, the column pass
 * leaves two guard bits, and the row pass descales by CONST_BITS +
 * PASS1_BITS + 3 — the +3 being the 1/8 of the 2-D normalization.  A
 * reduced pass (4, 2 or 1 point over the low coefficients) uses the
 * same convention, so the two passes compose whatever sizes the
 * component asked for.
 */

#define CONST_BITS  13
#define PASS1_BITS  2
#define FIX(x)      ((int32_t)((x) * (1 << CONST_BITS) + 0.5))

/* 8-point odd part (Loeffler rotations, as in libjpeg islow) */
#define FIX_0_298631336  FIX(0.298631336)
#define FIX_0_390180644  FIX(0.390180644)
#define FIX_0_899976223  FIX(0.899976223)
#define FIX_1_175875602  FIX(1.175875602)
#define FIX_1_501321110  FIX(1.501321110)
#define FIX_1_961570560  FIX(1.961570560)
#define FIX_2_053119869  FIX(2.053119869)
#define FIX_3_072711026  FIX(3.072711026)
/* shared even part / 4-point odd part */
#define FIX_0_541196100  FIX(0.541196100)
#define FIX_0_765366865  FIX(0.765366865)
#define FIX_1_847759065  FIX(1.847759065)
#define FIX_1_306562965  FIX(1.306562965)
#define FIX_2_562915447  FIX(2.562915447)

#define DESCALE(x, n)   (((x) + (1 << ((n) - 1))) >> (n))

/* Coefficients are signed and shifting a negative value left is
 * undefined; go through unsigned, which is well defined and compiles
 * to the same instruction. */
#define LSHIFT(x, n)    ((int32_t)((uint32_t)(x) << (n)))

/*
 * One 1-D pass over @n low-frequency coefficients.  Reads in[k*istride]
 * for k < n, writes out[x*ostride] for x < n, descaled by @shift.
 * Inputs must already be bounded (see DEQUANT_CLAMP).
 */
static void idct_1d(const int32_t *in, int istride, int32_t *out, int ostride,
                    int n, int shift)
{
    int32_t t0, t1, t2, t3, t10, t11, t12, t13, z1, z2, z3, z4, z5;

    switch (n) {
    case 1:
        out[0] = DESCALE(LSHIFT(in[0], CONST_BITS), shift);
        return;

    case 2:
        /* cos(pi/4) folded away: the pass is a sum and a difference. */
        t0 = LSHIFT(in[0], CONST_BITS);
        t1 = LSHIFT(in[istride], CONST_BITS);
        out[0]        = DESCALE(t0 + t1, shift);
        out[ostride]  = DESCALE(t0 - t1, shift);
        return;

    case 4:
        /* even */
        t10 = LSHIFT(in[0] + in[2 * istride], CONST_BITS);
        t11 = LSHIFT(in[0] - in[2 * istride], CONST_BITS);
        /* odd: one rotation */
        z1  = in[istride];
        z2  = in[3 * istride];
        t0  = z1 * FIX_1_306562965 + z2 * FIX_0_541196100;
        t1  = z1 * FIX_0_541196100 - z2 * FIX_1_306562965;
        out[0]           = DESCALE(t10 + t0, shift);
        out[3 * ostride] = DESCALE(t10 - t0, shift);
        out[ostride]     = DESCALE(t11 + t1, shift);
        out[2 * ostride] = DESCALE(t11 - t1, shift);
        return;

    default: {
        /* even part: a 4-point transform of the even coefficients */
        z2 = in[2 * istride];
        z3 = in[6 * istride];
        z1 = (z2 + z3) * FIX_0_541196100;
        t2 = z1 + z3 * -FIX_1_847759065;
        t3 = z1 + z2 * FIX_0_765366865;
        z2 = in[0];
        z3 = in[4 * istride];
        t0 = LSHIFT(z2 + z3, CONST_BITS);
        t1 = LSHIFT(z2 - z3, CONST_BITS);
        t10 = t0 + t3;
        t13 = t0 - t3;
        t11 = t1 + t2;
        t12 = t1 - t2;

        /* odd part: three rotations over the odd coefficients */
        t0 = in[7 * istride];
        t1 = in[5 * istride];
        t2 = in[3 * istride];
        t3 = in[istride];
        z1 = t0 + t3;
        z2 = t1 + t2;
        z3 = t0 + t2;
        z4 = t1 + t3;
        z5 = (z3 + z4) * FIX_1_175875602;
        t0 = t0 * FIX_0_298631336;
        t1 = t1 * FIX_2_053119869;
        t2 = t2 * FIX_3_072711026;
        t3 = t3 * FIX_1_501321110;
        z1 = z1 * -FIX_0_899976223;
        z2 = z2 * -FIX_2_562915447;
        z3 = z3 * -FIX_1_961570560 + z5;
        z4 = z4 * -FIX_0_390180644 + z5;
        t0 += z1 + z3;
        t1 += z2 + z4;
        t2 += z2 + z3;
        t3 += z1 + z4;

        out[0]           = DESCALE(t10 + t3, shift);
        out[7 * ostride] = DESCALE(t10 - t3, shift);
        out[ostride]     = DESCALE(t11 + t2, shift);
        out[6 * ostride] = DESCALE(t11 - t2, shift);
        out[2 * ostride] = DESCALE(t12 + t1, shift);
        out[5 * ostride] = DESCALE(t12 - t1, shift);
        out[3 * ostride] = DESCALE(t13 + t0, shift);
        out[4 * ostride] = DESCALE(t13 - t0, shift);
        return;
    }
    }
}

/* Valid 8-bit JPEG coefficients stay well inside this; clamping keeps a
 * malformed file from overflowing the int32 accumulators (which would
 * be undefined behaviour, not merely wrong pixels). */
#define DEQUANT_CLAMP  4095

/* Ceiling on the pass-1 workspace; see the note before the clamp in
 * idct_block.  The bound comes from summing the absolute coefficients
 * of the linear forms in idct_1d: no term-by-term sum exceeds 169352x
 * its input, so 169352 * 12680 < INT32_MAX makes the second pass safe.
 * That is a loose bound — brute-forcing all 256 sign patterns through
 * the 8-point path gives a true worst case of 61214x, so the real
 * headroom is about 2.8x.  Loose is the right side to err on. */
#define WORK_MAX       12680

static inline int32_t dequant(int16_t coef, uint16_t q)
{
    int32_t v = (int32_t)coef * (int32_t)q;
    if (v >  DEQUANT_CLAMP) return  DEQUANT_CLAMP;
    if (v < -DEQUANT_CLAMP) return -DEQUANT_CLAMP;
    return v;
}

/* A block with no AC content is a flat patch of one value.  The runs
 * are 1..8 bytes, far too short for memset to be worth its call, so
 * they go out as whole words. */
static inline void fill_flat(int bs_h, int bs_v, int32_t dcval,
                             uint8_t *dst, int stride)
{
    uint8_t v = clamp8(DESCALE(LSHIFT(dcval, CONST_BITS),
                               CONST_BITS + 3) + 128);
    uint32_t w = 0x01010101u * v;
    for (int y = 0; y < bs_v; y++) {
        uint8_t *row = dst + y * stride;
        int x = 0;
        for (; x + 4 <= bs_h; x += 4) {
            memcpy(row + x, &w, 4);
        }
        for (; x < bs_h; x++) {
            row[x] = v;
        }
    }
}

/*
 * Inverse-transform one block's kept coefficients into @dst.  Only the
 * bs_h x bs_v low-frequency corner exists (see the band-limiting note
 * in lowzag.h), which is exactly what a reduced IDCT reads.
 */
static void idct_block(lz_t *p, int bs_h, int bs_v, int K, const int16_t *co,
                       const uint16_t *q, uint8_t *dst, int stride)
{
    (void)p;
    int32_t f[DCTSZ * DCTSZ];
    int32_t work[DCTSZ * DCTSZ];

    /* Dequantize into the corner, clearing only what is used.  Track
     * whether anything but the DC term survived: on real photographs
     * most blocks are DC-only or nearly so, and skipping the transform
     * for those is the single largest saving here. */
    /* Cheapest possible test first: if no AC coefficient was stored at
     * all, none of the dequantize work below can matter. */
    {
        int k = 1;
        while (k < K && !co[k]) {
            k++;
        }
        if (k == K) {
            fill_flat(bs_h, bs_v, dequant(co[0], q[0]), dst, stride);
            return;
        }
    }

    for (int i = 0; i < bs_v * bs_h; i++) {
        f[i] = 0;
    }
    bool ac = false;
    for (int k = 0; k < K; k++) {
        if (!co[k]) {
            continue;
        }
        int nat = ZZ[k];
        int r = nat >> 3, c = nat & 7;
        if (r < bs_v && c < bs_h) {
            f[r * bs_h + c] = dequant(co[k], q[nat]);
            if (k) {
                ac = true;
            }
        }
    }

    if (!ac) {                       /* flat block */
        fill_flat(bs_h, bs_v, dequant(co[0], q[0]), dst, stride);
        return;
    }

    /* Pass 1: columns.  A column with no AC term is constant, which is
     * common enough to be worth the test. */
    for (int c = 0; c < bs_h; c++) {
        bool col_ac = false;
        for (int v = 1; v < bs_v; v++) {
            if (f[v * bs_h + c]) {
                col_ac = true;
                break;
            }
        }
        if (!col_ac) {
            int32_t dcval = LSHIFT(f[c], PASS1_BITS);
            for (int y = 0; y < bs_v; y++) {
                work[y * bs_h + c] = dcval;
            }
            continue;
        }
        idct_1d(&f[c], bs_h, &work[c], bs_h, bs_v, CONST_BITS - PASS1_BITS);
    }

    /* The second pass shifts its inputs left by CONST_BITS and sums
     * several rotations, so its accumulators run to about 169352x the
     * largest value coming out of pass 1.  Beyond WORK_MAX that
     * overflows int32 — undefined behaviour, not just a wrong pixel,
     * and reachable from a malformed file (found by fuzzing).  Bounding
     * the workspace here is cheaper than widening every accumulator.
     *
     * The limit is far outside anything a real image produces: over the
     * whole 128-file test corpus at all four scales, plus camera
     * originals, the largest value seen here is 5048, a factor of 2.5
     * below the clamp.  A stream that reaches it is already relying on
     * coefficients no encoder of 8-bit samples can emit — DEQUANT_CLAMP
     * has bounded those too. */
    for (int i = 0; i < bs_v * bs_h; i++) {
        if (work[i] >  WORK_MAX) { work[i] =  WORK_MAX; }
        if (work[i] < -WORK_MAX) { work[i] = -WORK_MAX; }
    }

    /* Pass 2: rows, straight into the output block. */
    int32_t row[DCTSZ];
    for (int y = 0; y < bs_v; y++) {
        const int32_t *wr = &work[y * bs_h];
        bool row_ac = false;
        for (int x = 1; x < bs_h; x++) {
            if (wr[x]) {
                row_ac = true;
                break;
            }
        }
        if (!row_ac) {
            uint8_t v = clamp8(DESCALE(wr[0], PASS1_BITS + 3) + 128);
            memset(dst + y * stride, v, (size_t)bs_h);
            continue;
        }
        idct_1d(wr, 1, row, 1, bs_h, CONST_BITS + PASS1_BITS + 3);
        for (int x = 0; x < bs_h; x++) {
            dst[y * stride + x] = clamp8(row[x] + 128);
        }
    }
}

static lowzag_err_t emit_rows(lz_t *p)
{
    /* Walk MCU rows.  Each component is inverse-transformed at its own
     * block size into its own strip, then sampled into the output row:
     * a component coarser than the output replicates, a finer one
     * decimates, and a matching one copies.
     *
     * The row and column maps stay in whole-frame coordinates
     * throughout — @y is a row of the frame's output grid and colmap
     * holds the component column for a column of that grid — because
     * narrowing them would change which source sample each output pixel
     * comes from, and the crop is not allowed to change a pixel.  The
     * crop only decides which of them are computed at all; the 0-based
     * row index the callback sees is applied at the very end. */
    int oy1 = p->oy0 + p->out_h;

    for (int my = 0; my < p->mcuy; my++) {
        /* Output rows covered by this MCU row, intersected with the
         * crop.  An MCU row with nothing to deliver is skipped whole:
         * no IDCT, no upsampling. */
        int y0 = my * p->vmax * DCTSZ / p->scale;
        int y1 = (my + 1) * p->vmax * DCTSZ / p->scale;
        if (y1 > p->full_out_h) {
            y1 = p->full_out_h;
        }
        if (y0 < p->oy0) {
            y0 = p->oy0;
        }
        if (y1 > oy1) {
            y1 = oy1;
        }
        if (y0 >= y1) {
            continue;
        }
        if (lz_cancelled(p)) {
            return LOWZAG_ERR_CANCELLED;
        }

        for (int i = 0; i < p->ncomp; i++) {
            comp_t *c = &p->comp[i];
            int sw = c->cbw * c->bs_h;
            for (int r = 0; r < c->v; r++) {
                int by = my * c->v + r;
                for (int bx = 0; bx < c->cbw; bx++) {
                    idct_block(p, c->bs_h, c->bs_v, c->K,
                               blk_coef(p, c, c->cbx + bx, by), p->qt[c->tq],
                               p->strip[i] + (r * c->bs_v) * sw + bx * c->bs_h,
                               sw);
                }
            }
        }

        for (int y = y0; y < y1; y++) {
            uint8_t *out = p->rowbuf;
            if (p->ncomp == 1) {
                comp_t *c = &p->comp[0];
                int sw = c->cbw * c->bs_h;
                int sy = (int)((int64_t)y * c->out_h / p->full_out_h)
                         - my * c->v * c->bs_v;
                if (sy < 0) {
                    sy = 0;
                }
                const uint8_t *row = p->strip[0] + sy * sw;
                const uint16_t *cm = p->colmap[0];
                for (int x = 0; x < p->out_w; x++) {
                    uint8_t v = row[cm[x]];
                    out[3 * x] = out[3 * x + 1] = out[3 * x + 2] = v;
                }
            } else {
                const uint8_t *rp[MAX_COMPS];
                for (int i = 0; i < p->ncomp; i++) {
                    comp_t *c = &p->comp[i];
                    int sw = c->cbw * c->bs_h;
                    int sy = (int)((int64_t)y * c->out_h / p->full_out_h)
                             - my * c->v * c->bs_v;
                    if (sy < 0) {
                        sy = 0;
                    }
                    if (sy >= c->v * c->bs_v) {
                        sy = c->v * c->bs_v - 1;
                    }
                    rp[i] = p->strip[i] + sy * sw;
                }
                const uint16_t *cmy = p->colmap[0];
                const uint16_t *cmb = p->colmap[1];
                const uint16_t *cmr = p->colmap[2];
                for (int x = 0; x < p->out_w; x++) {
                    int Y  = rp[0][cmy[x]];
                    int cb = rp[1][cmb[x]];
                    int cr = rp[2][cmr[x]];
                    out[3 * x]     = clamp8(Y + p->cr_r[cr]);
                    out[3 * x + 1] = clamp8(Y + (int)((p->cb_g[cb]
                                                     + p->cr_g[cr]) >> 16));
                    out[3 * x + 2] = clamp8(Y + p->cb_b[cb]);
                }
            }
            p->cfg->row(p->cfg->row_ctx, y - p->oy0, p->out_w, out);
            p->rows_out++;
        }
    }
    return LOWZAG_OK;
}

/* ── struct versioning ──────────────────────────────────────────────────────
 *
 * Every public struct begins with a caller-set struct_size, and lowzag
 * never touches a byte beyond it.  Three cases, and the third is the one
 * that matters: a struct_size LARGER than this build's is a caller
 * compiled against a newer header, and the fields it is relying on do
 * not exist here.  Ignoring the excess would mean silently not applying
 * a crop and decoding the whole image into a buffer sized for a region,
 * so it is refused.  Zero is refused with it: a caller who zeroed the
 * struct and forgot the field fails on the first call, every time,
 * rather than the day a field is added.
 */
/*
 * A byte copy of a length known only at run time, and deliberately not
 * memcpy: with _FORTIFY_SOURCE a memcpy of unknown length into an object
 * of known size becomes __memcpy_chk, and the undefined-symbol set of
 * src/lowzag.c is a documented property of this file — memcpy and
 * memset, nothing else.  The lengths here are struct sizes, under a
 * hundred bytes, once per call.
 */
static void lz_copy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void lz_zero(void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;

    for (size_t i = 0; i < n; i++) {
        d[i] = 0;
    }
}

static int ss_bad(size_t given, size_t known)
{
    /* A struct whose first member is a size_t cannot be smaller than
     * one, so anything under that is not a lowzag struct of any version
     * — 0 included, which is what a caller who forgot the field has.
     * That floor is also what makes it safe to write the field back
     * inside a short struct_size.
     *
     * The multiple-of-size_t test is what keeps a WRONG value safe as
     * well as a missing one.  Every lowzag struct begins with a size_t
     * and holds pointers, so its alignment is that of a size_t and its
     * sizeof is a multiple of it — for this version and for every future
     * one.  No conforming caller can therefore produce a value this
     * rejects.  What it rules out is a size that falls in the middle of
     * a field: cfg_adopt() would copy half of a callback pointer and
     * zero the other half, which is not NULL, so the null-callback check
     * below passes and the decode calls into a spliced address.
     * Measured before this test existed, a struct_size of 65..68 — half
     * of cfg.free — segfaulted in lz_cleanup() instead of returning
     * LOWZAG_ERR_VERSION.  A field whose whole job is to make a mis-sized
     * struct diagnosable must not have a range that crashes. */
    return given < sizeof(size_t) || given > known ||
           given % sizeof(size_t) != 0;
}

/* Copy the caller's config into a full-size one, defaulting whatever
 * their (older) struct does not reach.  Everything downstream then reads
 * an object of known shape, and the "only fields inside struct_size are
 * read" rule is enforced in exactly one place. */
static lowzag_err_t cfg_adopt(lowzag_cfg_t *dst, const lowzag_cfg_t *src)
{
    if (ss_bad(src->struct_size, sizeof *dst)) {
        return LOWZAG_ERR_VERSION;
    }
    memset(dst, 0, sizeof *dst);
    lz_copy(dst, src, src->struct_size);
    dst->struct_size = sizeof *dst;
    return LOWZAG_OK;
}

/* Write @src back into the caller's @dst, within their struct_size and
 * leaving that field as they set it.  @dst->struct_size must already
 * have been checked with ss_bad(). */
static void info_store(lowzag_info_t *dst, const lowzag_info_t *src)
{
    lowzag_info_t tmp = *src;
    size_t n = dst->struct_size;

    if (n > sizeof tmp) {
        n = sizeof tmp;
    }
    tmp.struct_size = dst->struct_size;
    lz_copy(dst, &tmp, n);
}

/* ── top level ──────────────────────────────────────────────────────────────*/

static void lz_cleanup(lz_t *p)
{
    for (int i = 0; i < p->ncomp && i < MAX_COMPS; i++) {
        if (p->comp[i].coef) {
            p->cfg->free(p->cfg->alloc_ctx, p->comp[i].coef);
        }
        if (p->comp[i].nz) {
            p->cfg->free(p->cfg->alloc_ctx, p->comp[i].nz);
        }
        if (p->strip[i]) {
            p->cfg->free(p->cfg->alloc_ctx, p->strip[i]);
        }
        if (p->colmap[i]) {
            p->cfg->free(p->cfg->alloc_ctx, p->colmap[i]);
        }
    }
    if (p->rowbuf) {
        p->cfg->free(p->cfg->alloc_ctx, p->rowbuf);
    }
}

size_t lowzag_state_size(void)
{
    return sizeof(lz_t);
}

size_t lowzag_state_align(void)
{
    return LZ_ALIGNMENT;
}

lowzag_err_t lowzag_decode(const lowzag_cfg_t *user_cfg, lowzag_info_t *info)
{
    lowzag_cfg_t  cfgbuf;
    lowzag_info_t inf;

    /* The version fields come first, before anything is read, allocated
     * or emitted — including the zeroing below, which would otherwise
     * destroy the evidence it is about to be judged on. */
    if (info) {
        if (ss_bad(info->struct_size, sizeof *info)) {
            return LOWZAG_ERR_VERSION;
        }
    }
    /* The header promises @info is zeroed whenever the failure came
     * before the frame header.  Every rejection below qualifies, so
     * clear it first — hoisting this above the validation is the whole
     * point, and putting it after any early return silently leaves the
     * caller's struct uninitialised. */
    memset(&inf, 0, sizeof inf);
    if (info) {
        info_store(info, &inf);
    }

    if (!user_cfg) {
        return LOWZAG_ERR_INVALID_ARG;
    }
    lowzag_err_t ve = cfg_adopt(&cfgbuf, user_cfg);
    if (ve != LOWZAG_OK) {
        return ve;
    }
    const lowzag_cfg_t *cfg = &cfgbuf;

    if (!cfg->read || !cfg->row || !cfg->alloc || !cfg->free) {
        return LOWZAG_ERR_INVALID_ARG;
    }
    switch (cfg->force_scale) {          /* 0 = choose automatically */
    case 0: case 1: case 2: case 4: case 8:
        break;
    default:
        return LOWZAG_ERR_INVALID_ARG;
    }
    /* What can be judged without the frame header: the rest of the crop
     * validation needs the dimensions and happens in plan_frame(). */
    if (cfg->crop_x < 0 || cfg->crop_y < 0 ||
        cfg->crop_w < 0 || cfg->crop_h < 0) {
        return LOWZAG_ERR_INVALID_ARG;
    }

    /* The working set is one allocation of lowzag_state_size() bytes.  A
     * caller who cares where it lands — on a part with fast internal RAM
     * and slow external RAM, the distinction is worth several times the
     * decode time — supplies it through cfg->state; everyone else gets
     * it from cfg->alloc.  Either way nothing is static, so decodes on
     * separate threads do not touch each other. */
    lz_t *p = cfg->state;
    int state_is_ours = 0;
    if (p) {
        /* Any allocator returns memory aligned for every scalar type;
         * a hand-rolled buffer might not, and the misalignment would
         * surface as a fault deep in the decoder rather than here.
         * Testing against a narrower type than lz_t actually needs
         * waves through a buffer that is 4-mod-8 on LP64, which then
         * traps on the first member access — the check has to use the
         * real requirement. */
        if ((uintptr_t)p % LZ_ALIGNMENT != 0) {
            return LOWZAG_ERR_INVALID_ARG;
        }
        /* Must be tested before the memset below, which is what would
         * destroy the evidence along with the outer decode's arena. */
        if (p->in_use == LZ_IN_USE) {
            return LOWZAG_ERR_INVALID_ARG;
        }
    } else {
        p = cfg->alloc(cfg->alloc_ctx, sizeof *p);
        if (!p) {
            return LOWZAG_ERR_NOMEM;
        }
        state_is_ours = 1;
    }
    memset(p, 0, sizeof *p);
    p->in_use = LZ_IN_USE;
    p->cfg = cfg;
    p->rd.cfg = cfg;
    p->partial = (cfg->allow_partial != 0);
    p->dc_cov = -1;                  /* no DC scan has run yet */

    lowzag_err_t ret = LOWZAG_ERR_FORMAT;
    int got_sof = 0, got_scan = 0, got_eoi = 0, nscans = 0;
    /* The stream ended early and the caller asked for what had arrived.
     * Set only where a plain decode would have returned
     * LOWZAG_ERR_TRUNCATED. */
    int short_stream = 0;

    if (rd_byte(&p->rd) != 0xFF || rd_byte(&p->rd) != 0xD8) {
        p->in_use = 0;
        if (state_is_ours) {
            cfg->free(cfg->alloc_ctx, p);
        }
        return LOWZAG_ERR_NOT_JPEG;
    }

    for (;;) {
        /* One poll per marker segment.  Cheap next to reading the
         * segment, and it covers the long tail of an APPn a camera put
         * a colour profile in. */
        if (lz_cancelled(p)) {
            ret = LOWZAG_ERR_CANCELLED;
            goto out;
        }
        int m = next_marker(&p->rd);
        if (m < 0) {
            /* The reader reported FAILURE rather than end of input.  That
             * is the caller's own I/O breaking, not a short file, and
             * cfg.allow_partial says nothing about it — see the note in
             * parse_sos_and_decode(), which has always checked this and
             * is why the same test has to be here.  Inside p->partial, so
             * a caller who did not opt in gets 2.1.0's code unchanged. */
            if (p->partial && p->rd.io_err) {
                ret = LOWZAG_ERR_IO;
                goto out;
            }
            /* Out of file between segments.  With a DC pass behind us
             * there is a picture to show, if the caller wants one;
             * without, there is nothing and never was. */
            if (p->partial && got_sof && p->dc_cov >= 0) {
                short_stream = 1;
                break;
            }
            ret = got_scan ? LOWZAG_ERR_TRUNCATED : LOWZAG_ERR_FORMAT;
            goto out;
        }
        if (m == 0xD9) {             /* EOI */
            got_eoi = 1;
            break;
        }
        switch (m) {
        case 0xDB:                   /* DQT */
            ret = parse_dqt(p);
            break;
        case 0xC4:                   /* DHT */
            ret = parse_dht(p);
            break;
        case 0xC2:                   /* SOF2: progressive, Huffman */
            if (got_sof) {
                ret = LOWZAG_ERR_FORMAT;
                goto out;
            }
            ret = parse_sof2(p);
            if (ret == LOWZAG_OK) {
                ret = setup_frame(p);
                got_sof = 1;
                inf.src_w = p->W;
                inf.src_h = p->H;
                inf.out_w = p->out_w;
                inf.out_h = p->out_h;
                inf.crop_x = p->crop_x;
                inf.crop_y = p->crop_y;
                inf.crop_w = p->crop_w;
                inf.crop_h = p->crop_h;
                inf.scale = p->scale;
                inf.components = p->ncomp;
                inf.chroma_steps = p->chroma_steps;
                /* Published now rather than at the end: the row callback
                 * is entitled to read the geometry out of @info, and it
                 * runs long before this function returns. */
                if (info) {
                    info_store(info, &inf);
                }
            }
            break;
        case 0xC0: case 0xC1: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            /* other frame types: baseline/sequential/arithmetic/... */
            ret = (m == 0xC0 || m == 0xC1) ? LOWZAG_ERR_NOT_PROGRESSIVE
                                           : LOWZAG_ERR_UNSUPPORTED;
            goto out;
        case 0xDA:                   /* SOS */
            if (!got_sof) {
                ret = LOWZAG_ERR_FORMAT;
                goto out;
            }
            /* Real progressive scripts use ten scans or so; a crafted
             * file could otherwise replay scans forever, and each one
             * is a full pass over every block. */
            if (++nscans > LOWZAG_MAX_SCANS) {
                ret = LOWZAG_ERR_FORMAT;
                goto out;
            }
            ret = parse_sos_and_decode(p);
            got_scan = 1;
            break;
        case 0xDD: {                 /* DRI */
            int len = read_u16(&p->rd);
            int ri = read_u16(&p->rd);
            if (len != 4 || ri < 0) {
                ret = LOWZAG_ERR_FORMAT;
                goto out;
            }
            p->restart = ri;
            ret = LOWZAG_OK;
            break;
        }
        case 0x01:                   /* TEM: parameterless */
            ret = LOWZAG_OK;
            break;
        default:
            if (m >= 0xD0 && m <= 0xD7) {
                ret = LOWZAG_OK;      /* stray RST between segments */
            } else if (app_wanted(cfg, m)) {
                ret = deliver_app(p, m);
            } else {
                ret = skip_segment(p);
            }
            break;
        }
        if (ret != LOWZAG_OK) {
            /* The reader reported FAILURE.  Every segment parser turns a
             * failed rd_byte() into TRUNCATED or FORMAT without looking
             * at why, so this is the only place that can tell the two
             * apart, and the header promises the difference: a broken
             * read is LOWZAG_ERR_IO and never a partial image.  A reader
             * that would rather have the picture out of a dying socket
             * returns 0 — end of input — instead. */
            if (p->partial && p->rd.io_err &&
                (ret == LOWZAG_ERR_TRUNCATED || ret == LOWZAG_ERR_FORMAT)) {
                ret = LOWZAG_ERR_IO;
                goto out;
            }
            /* A short file, with a DC pass decoded, and a caller who
             * said they would take what arrived.  p->dc_cov is the test
             * because it is what says a scan reached the coefficients:
             * a file cut inside its SOS header has decoded nothing, and
             * an image of nothing is not worth a return code.
             *
             * FORMAT counts here as well as TRUNCATED, and only here.
             * Once the stream has ended, "malformed" is what a missing
             * length field and a Huffman code read out of padding both
             * look like — see lz_ran_out().  Everything else is still a
             * failure and still leaves by the same door.  Both arms test
             * lz_ran_out(): TRUNCATED out of skip_segment() or
             * deliver_app() means "rd_byte() failed", which is a broken
             * reader just as often as it is a short file. */
            if (p->partial && got_sof && p->dc_cov >= 0 && lz_ran_out(p) &&
                (ret == LOWZAG_ERR_TRUNCATED || ret == LOWZAG_ERR_FORMAT)) {
                short_stream = 1;
                break;
            }
            goto out;
        }
    }

    if (!got_sof || !got_scan || (!got_eoi && !short_stream)) {
        ret = LOWZAG_ERR_FORMAT;
        goto out;
    }
    for (int i = 0; i < p->ncomp; i++) {
        if (!p->qt_def[p->comp[i].tq]) {
            ret = LOWZAG_ERR_FORMAT;      /* would decode to flat grey */
            goto out;
        }
    }

    /* allocate output strips + row buffer, run the output pass */
    for (int i = 0; i < p->ncomp; i++) {
        comp_t *c = &p->comp[i];
        size_t sz = (size_t)(c->cbw * c->bs_h) * (size_t)(c->v * c->bs_v);
        p->strip[i] = lz_alloc(p, sz);
        if (!p->strip[i]) {
            ret = LOWZAG_ERR_NOMEM;
            goto out;
        }
    }
    p->rowbuf = lz_alloc(p, (size_t)p->out_w * 3);
    if (!p->rowbuf) {
        ret = LOWZAG_ERR_NOMEM;
        goto out;
    }
    for (int i = 0; i < p->ncomp; i++) {
        comp_t *c = &p->comp[i];
        p->colmap[i] = lz_alloc(p, (size_t)p->out_w * sizeof(uint16_t));
        if (!p->colmap[i]) {
            ret = LOWZAG_ERR_NOMEM;
            goto out;
        }
        /* Whole-frame sampling, then the strip's own origin subtracted:
         * output column x of the DELIVERED image is column ox0 + x of
         * the frame, and takes the same source sample it would in an
         * uncropped decode.  The subtraction is folded in here so the
         * pixel loop stays one table lookup.
         *
         * The clamp is a bound, not a policy: the crop's block rectangle
         * covers every column the delivered rectangle can name, so a
         * clamped entry would be a bug elsewhere — but an out-of-range
         * index here would be a read off the end of the strip. */
        int base = c->cbx * c->bs_h;
        int span = c->cbw * c->bs_h;
        for (int x = 0; x < p->out_w; x++) {
            int col = (int)((int64_t)(p->ox0 + x) * c->out_w / p->full_out_w)
                      - base;
            if (col < 0) {
                col = 0;
            }
            if (col >= span) {
                col = span - 1;
            }
            p->colmap[i][x] = (uint16_t)col;
        }
    }
    for (int i = 0; i < 256; i++) {
        int v = i - 128;
        p->cr_r[i] = (int32_t)((91881 * (int64_t)v + 32768) >> 16);
        p->cb_b[i] = (int32_t)((116130 * (int64_t)v + 32768) >> 16);
        p->cr_g[i] = (int32_t)(-46802 * (int64_t)v);
        p->cb_g[i] = (int32_t)(-22554 * (int64_t)v + 32768);
    }
    ret = emit_rows(p);
    /* Every row was delivered, out of fewer scans than the file meant to
     * carry.  A distinct code, because a caller who asked for an image
     * and got half a file has to be able to tell without reading @info —
     * LOWZAG_OK with a flag somewhere would be found by the diligent and
     * missed by everyone else. */
    if (ret == LOWZAG_OK && short_stream) {
        ret = LOWZAG_PARTIAL;
    }

out:
    if (info) {
        inf.peak_alloc = p->peak;
        inf.rows_delivered = p->rows_out;
        inf.scans_decoded = p->scans_done;
        inf.dc_coverage = (p->dc_cov < 0) ? 0 : p->dc_cov;
        /* Bytes the parser consumed, not bytes the reader handed over:
         * what is still sitting in the read buffer was never looked at,
         * and a caller asking how far the stream got means the former. */
        {
            size_t held = (size_t)(p->rd.fill - p->rd.pos) +
                          (size_t)p->rd.unget_n;
            inf.bytes_consumed = (p->rd.nread > held) ? p->rd.nread - held : 0;
        }
        if (!got_sof) {
            inf.src_w = inf.src_h = 0;
            inf.out_w = inf.out_h = 0;
            inf.crop_x = inf.crop_y = 0;
            inf.crop_w = inf.crop_h = 0;
            inf.scale = 0;
            inf.components = 0;
            inf.chroma_steps = 0;
        }
        info_store(info, &inf);
    }
    lz_cleanup(p);
    p->in_use = 0;
    if (state_is_ours) {
        cfg->free(cfg->alloc_ctx, p);
    }
    return ret;
}

const char *lowzag_strerror(lowzag_err_t err)
{
    switch (err) {
    case LOWZAG_OK:                   return "ok";
    case LOWZAG_ERR_NOT_JPEG:         return "not a JPEG stream";
    case LOWZAG_ERR_NOT_PROGRESSIVE:  return "JPEG is not progressive";
    case LOWZAG_ERR_UNSUPPORTED:      return "unsupported JPEG feature";
    case LOWZAG_ERR_FORMAT:           return "malformed JPEG stream";
    case LOWZAG_ERR_TRUNCATED:        return "truncated JPEG stream";
    case LOWZAG_ERR_NOMEM:            return "out of memory";
    case LOWZAG_ERR_IO:               return "read callback failed";
    case LOWZAG_ERR_INVALID_ARG:      return "invalid argument";
    case LOWZAG_ERR_CANCELLED:        return "cancelled by the caller";
    case LOWZAG_ERR_BUFFER_TOO_SMALL: return "output buffer too small";
    case LOWZAG_ERR_VERSION:          return "struct_size unset, or not a size "
                                             "this lowzag knows";
    case LOWZAG_PARTIAL:              return "truncated JPEG stream, partial "
                                             "image delivered";
    }
    return "unknown error";
}

/*
 * Walk the marker segments of a buffer prefix looking for the frame
 * header.  On LOWZAG_OK (SOF2) or LOWZAG_ERR_NOT_PROGRESSIVE (another
 * SOFn) *@off is the offset of the 0xFF that introduces it, and
 * data[*off + 9] — the component count, the last fixed field — is known
 * to be inside @len.
 *
 * lowzag_probe() and lowzag_estimate() share this so the two cannot
 * disagree about what counts as a frame header or about when a prefix
 * is merely too short.
 */
static lowzag_err_t scan_for_sof(const uint8_t *data, size_t len, size_t *off)
{
    if (!data) {
        return LOWZAG_ERR_INVALID_ARG;
    }
    /* NOT_JPEG only on the evidence of bytes that are actually there.  A
     * prefix of one 0xFF is consistent with a JPEG and the caller should
     * read more; a prefix of one 0x42 is not, and they should stop.
     * Lumping both into NOT_JPEG broke the growing-buffer loop the
     * header advertises, for the one caller who starts small. */
    if (len == 0) {
        return LOWZAG_ERR_TRUNCATED;
    }
    if (data[0] != 0xFF) {
        return LOWZAG_ERR_NOT_JPEG;
    }
    if (len == 1) {
        return LOWZAG_ERR_TRUNCATED;
    }
    if (data[1] != 0xD8) {
        return LOWZAG_ERR_NOT_JPEG;
    }
    if (len < 4) {
        return LOWZAG_ERR_TRUNCATED;
    }
    size_t i = 2;
    while (i + 4 <= len) {
        if (data[i] != 0xFF) {
            i++;
            continue;
        }
        uint8_t m = data[i + 1];
        if (m == 0xFF) {
            i++;
            continue;
        }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD9)) {
            i += 2;
            continue;
        }
        if (i + 4 > len) {
            break;
        }
        size_t seglen = ((size_t)data[i + 2] << 8) | data[i + 3];
        if ((m & 0xF0) == 0xC0 && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            /* SOFn — the frame header must be fully present, otherwise
             * the dimensions would come from bytes we never read. */
            if (i + 9 >= len) {
                return LOWZAG_ERR_TRUNCATED;
            }
            *off = i;
            return (m == 0xC2) ? LOWZAG_OK : LOWZAG_ERR_NOT_PROGRESSIVE;
        }
        if (seglen < 2) {
            return LOWZAG_ERR_NOT_JPEG;
        }
        i += 2 + seglen;
    }
    /* Ran out of buffer before any frame header: the caller needs to
     * show us more of the file, which is NOT the same as "not a JPEG".
     * Camera files routinely carry tens of kilobytes of EXIF/ICC ahead
     * of the frame header. */
    return LOWZAG_ERR_TRUNCATED;
}

lowzag_err_t lowzag_probe(const uint8_t *data, size_t len, int *w, int *h)
{
    size_t off = 0;
    lowzag_err_t e = scan_for_sof(data, len, &off);
    if ((e == LOWZAG_OK || e == LOWZAG_ERR_NOT_PROGRESSIVE) && w && h) {
        *h = ((int)data[off + 5] << 8) | data[off + 6];
        *w = ((int)data[off + 7] << 8) | data[off + 8];
    }
    return e;
}

/*
 * Fill @g from the SOF2 payload at @off, applying the same acceptance
 * rules as parse_sof2().  The one place the two differ is a header that
 * is present but self-inconsistent (a zero dimension, a segment length
 * that does not match the component count): from a stream parse_sof2()
 * cannot tell that from a short read and calls it truncated, whereas
 * here the bytes are demonstrably present, so it is LOWZAG_ERR_FORMAT.
 */
static lowzag_err_t parse_sof_buf(const uint8_t *data, size_t len, size_t off,
                                  geom_t *g)
{
    int seglen = ((int)data[off + 2] << 8) | data[off + 3];
    int prec   = data[off + 4];
    int H      = ((int)data[off + 5] << 8) | data[off + 6];
    int W      = ((int)data[off + 7] << 8) | data[off + 8];
    int ncomp  = data[off + 9];

    if (prec != 8) {
        return LOWZAG_ERR_UNSUPPORTED;
    }
    if (ncomp != 1 && ncomp != 3) {
        return LOWZAG_ERR_UNSUPPORTED;
    }
    /* Not a policy choice — a memory-safety boundary.  See LOWZAG_MAX_DIM. */
    if (W > LOWZAG_MAX_DIM || H > LOWZAG_MAX_DIM) {
        return LOWZAG_ERR_UNSUPPORTED;
    }
    if (W <= 0 || H <= 0) {
        return LOWZAG_ERR_FORMAT;
    }
    if (seglen != 8 + 3 * ncomp) {
        return LOWZAG_ERR_FORMAT;
    }
    /* The component descriptors are past what scan_for_sof() vouched
     * for, so this really is "show me more of the file". */
    if (off + 10 + 3 * (size_t)ncomp > len) {
        return LOWZAG_ERR_TRUNCATED;
    }

    g->W     = W;
    g->H     = H;
    g->ncomp = ncomp;
    g->hmax  = 1;
    g->vmax  = 1;
    for (int i = 0; i < ncomp; i++) {
        const uint8_t *d = data + off + 10 + 3 * (size_t)i;
        int hv = d[1];
        g->h[i] = hv >> 4;
        g->v[i] = hv & 15;
        if (g->h[i] < 1 || g->h[i] > 2 || g->v[i] < 1 || g->v[i] > 2 ||
            d[2] > 3) {
            return LOWZAG_ERR_UNSUPPORTED;
        }
        if (g->h[i] > g->hmax) { g->hmax = g->h[i]; }
        if (g->v[i] > g->vmax) { g->vmax = g->v[i]; }
    }
    return LOWZAG_OK;
}

lowzag_err_t lowzag_estimate(const lowzag_cfg_t *user_cfg,
                             const uint8_t *data, size_t len,
                             lowzag_estimate_t *out)
{
    lowzag_cfg_t use;
    geom_t g;
    size_t off = 0;

    if (!out) {
        return LOWZAG_ERR_INVALID_ARG;
    }
    if (ss_bad(out->struct_size, sizeof *out)) {
        return LOWZAG_ERR_VERSION;
    }
    {   /* zero within the caller's struct, keeping the field they set */
        size_t ss = out->struct_size;
        lz_zero(out, ss);
        out->struct_size = ss;
    }
    memset(&use, 0, sizeof use);
    if (user_cfg) {
        lowzag_err_t ve = cfg_adopt(&use, user_cfg);
        if (ve != LOWZAG_OK) {
            return ve;
        }
    }
    const lowzag_cfg_t *cfg = &use;      /* no cfg: default behaviour */
    if (!data) {
        return LOWZAG_ERR_INVALID_ARG;
    }
    switch (cfg->force_scale) {          /* 0 = choose automatically */
    case 0: case 1: case 2: case 4: case 8:
        break;
    default:
        return LOWZAG_ERR_INVALID_ARG;
    }
    if (cfg->crop_x < 0 || cfg->crop_y < 0 ||
        cfg->crop_w < 0 || cfg->crop_h < 0) {
        return LOWZAG_ERR_INVALID_ARG;
    }

    lowzag_err_t e = scan_for_sof(data, len, &off);
    if (e == LOWZAG_ERR_NOT_PROGRESSIVE) {
        /* lowzag_probe() lumps every non-SOF2 frame together, because
         * dispatch only cares that the file is not ours.  An estimate
         * is supposed to predict the decode, and lowzag_decode()
         * separates the two cases: SOF0 and SOF1 are what a baseline
         * decoder can take, and every other frame type (arithmetic,
         * lossless, hierarchical) is out of scope for one as well. */
        return (data[off + 1] == 0xC0 || data[off + 1] == 0xC1)
               ? LOWZAG_ERR_NOT_PROGRESSIVE : LOWZAG_ERR_UNSUPPORTED;
    }
    if (e != LOWZAG_OK) {
        return e;
    }
    memset(&g, 0, sizeof g);
    e = parse_sof_buf(data, len, off, &g);
    if (e != LOWZAG_OK) {
        return e;
    }
    e = plan_frame(&g, cfg);             /* the decode's own sizing code */
    if (e != LOWZAG_OK) {
        return e;                        /* ceiling too low for any scale */
    }

    lowzag_estimate_t est;
    memset(&est, 0, sizeof est);
    est.src_w         = g.W;
    est.src_h         = g.H;
    est.out_w         = g.out_w;
    est.out_h         = g.out_h;
    est.crop_x        = g.crop_x;
    est.crop_y        = g.crop_y;
    est.crop_w        = g.crop_w;
    est.crop_h        = g.crop_h;
    est.scale         = g.scale;
    est.components    = g.ncomp;
    est.chroma_steps  = g.chroma_steps;
    est.working_bytes = lowzag_state_size();
    est.alloc_bytes   = g.alloc_bytes;
    est.total_bytes   = est.working_bytes + est.alloc_bytes;

    est.struct_size = out->struct_size;
    lz_copy(out, &est, out->struct_size < sizeof est ? out->struct_size
                                                     : sizeof est);
    return LOWZAG_OK;
}
