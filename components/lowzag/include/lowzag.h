/*
 * lowzag — a progressive JPEG decoder for memory-constrained systems.
 *
 * Copyright (c) 2026 Tom (defensiblelogic.com)
 * SPDX-License-Identifier: MIT
 *
 * Decodes progressive (SOF2) JPEG streams at a power-of-two reduced
 * scale (1/1, 1/2, 1/4, 1/8), optionally restricted to a rectangle of
 * the source image, in a single forward pass over the file, never
 * seeking, never buffering the compressed file, and streaming output
 * rows out as they are produced.  It does hold coefficient storage for
 * the whole frame; that is unavoidable.
 *
 * The point of it is memory.  A progressive JPEG stores each block's
 * coefficients a few bits at a time, scattered across a dozen or more
 * scans, so a decoder cannot finish any block until the last scan has
 * been read: it has to hold coefficient storage for the entire frame.
 * Every general-purpose decoder surveyed for this project sizes that
 * storage from the SOURCE dimensions at the full 64 coefficients per
 * block — libjpeg-turbo's JBLOCK is 128 bytes and it allocates one per
 * block regardless of the scale you ask for.  On a 12-megapixel 4:2:0
 * photograph that is about 36 MB whether you want the full image or a
 * thumbnail.
 *
 * A reduced-scale IDCT only ever reads the low-frequency corner of each
 * block, so this decoder only stores that corner, plus a 64-bit
 * per-block map of which coefficients were nonzero (needed because
 * successive-approximation refinement bit counts depend on the whole
 * band's nonzero history, not just the part that is kept).  A crop goes
 * further: a block outside the rectangle needs no coefficients at all,
 * only its 8-byte map.
 *
 * STORAGE STILL SCALES WITH THE SOURCE IMAGE, NOT THE OUTPUT — only
 * the constant shrinks.  Budget from the largest file you intend to
 * accept, read out of the frame header, not from the size you want on
 * screen.  Measured on a 12 MP 4:2:0 progressive file, whole frame and
 * with an 800x480 source window:
 *
 *      scale           1/1      1/2      1/4      1/8
 *   lowzag           36.2 MB  23.0 MB   8.9 MB   3.6 MB
 *   lowzag + crop     3.4 MB   3.0 MB   2.5 MB   2.3 MB
 *   libjpeg          36.1 MB  36.1 MB  36.1 MB  36.1 MB
 *
 * (lowzag figures are its own peak_alloc; libjpeg's is the size of the
 * coefficient virtual array it requests. libjpeg-turbo 3.x ships no
 * backing-store backend, so that figure is not a tunable default.)
 *
 * The two savings are complementary rather than multiplicative, and the
 * table above is the honest way to read them.  Reducing the scale saves
 * most at 1/8 and nothing at 1/1; cropping saves most at 1/1, where a
 * block costs 128 bytes of coefficients against 8 of map, and little at
 * 1/8, where it costs 2 against 8.  Together they reach 15.8x on that
 * file, not the 107x that multiplying the two headline figures would
 * suggest.  The floor both of them run into is the map: 8 bytes for
 * every block in the source frame, 2.26 MB at 12 MP 4:2:0, which no
 * setting removes.
 *
 * Every scan is still entropy-decoded — skipping one desynchronizes the
 * refinement scans that follow — but only the surviving corner is ever
 * dequantized, inverse-transformed and upsampled.  Cropping therefore
 * costs no parse time and saves none: it saves memory, and the back-end
 * work for the blocks it drops.
 *
 * Portable C99 with no platform dependencies: I/O, row output and
 * allocation all go through caller-supplied callbacks, so the same
 * source builds on a host (for differential testing against libjpeg)
 * and on a microcontroller.  The only library functions src/lowzag.c
 * calls are memcpy and memset; compiled -O2 -fno-stack-protector, those
 * are the object file's only undefined symbols.  (Hardening flags add
 * their own runtime references — -fstack-protector pulls in
 * __stack_chk_fail — which matters only for a freestanding port.)
 *
 * lowzag_decode_mem() below needs malloc and free, so it lives in a
 * second translation unit, src/lowzag_mem.c, and the property above is
 * the reason.  Configure a STATIC build with -DLOWZAG_BUILD_MEM_API=OFF
 * to leave that file out entirely; the rest of the API does not
 * reference it.  A shared build always contains it, so that every
 * liblowzag.so.2 exports the same symbols.
 *
 * SCOPE.  Progressive (SOF2, Huffman) only; baseline files are the
 * caller's problem and lowzag_probe() will tell you which you have.
 * 8-bit precision, 1 component (grayscale) or 3 (YCbCr), sampling
 * factors 1 and 2.  Restart markers are supported.  Arithmetic coding,
 * 12-bit, CMYK/Adobe transforms and lossless JPEG are not.
 *
 * Metadata is not parsed either — EXIF is a TIFF parser and an ICC
 * profile is a colour-management problem, and neither belongs in a JPEG
 * decoder.  What lowzag does is hand over the bytes: cfg.marker below
 * receives the payload of whichever APPn segments cfg.marker_mask
 * selects, in chunks, without buffering them, because it is walking
 * those segments anyway and the caller may be streaming a file they do
 * not have in memory.  lowzag_exif_orientation() turns a captured APP1
 * into the one number most callers wanted.
 *
 * THREADING.  There is no global or static state.  Concurrent decodes
 * are fine as long as each has its own lowzag_cfg_t and its own working
 * state (see cfg.state below).  A single decode is not reentrant: do
 * not call lowzag_decode() from inside one of its own callbacks — row,
 * read, alloc, free, cancel or marker.  Reentering with the same
 * cfg.state is refused with LOWZAG_ERR_INVALID_ARG rather than
 * corrupting the outer decode, which is what used to happen — the inner
 * call cleared the outer one's bookkeeping, leaked its buffers, and let
 * it return LOWZAG_OK having emitted a fraction of the image.  That
 * refusal is a sentinel word inside cfg.state, which is why a
 * caller-supplied state buffer has to be zeroed before its first use;
 * see cfg.state below.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOWZAG_VERSION_MAJOR 2
#define LOWZAG_VERSION_MINOR 2
#define LOWZAG_VERSION_PATCH 1
#define LOWZAG_VERSION_STRING "2.2.1"
/* major*10000 + minor*100 + patch, for #if tests.  2.1.0 shipped this as
 * 20000, which is 2.0.0's value: the macro was not updated with the rest
 * of the version and nothing checked it.  test/check_docs.py now derives
 * it from the three numbers above, so it cannot drift again.  A caller
 * testing >= 20000 is unaffected; one testing >= 20100 to detect 2.1.0
 * was told 2.1.0 was 2.0.0, and there is no way to fix that in
 * retrospect. */
#define LOWZAG_VERSION_NUMBER 20201

/*
 * restrict, spelled portably.  Used where two pointer arguments must
 * not overlap; the qualifier states the contract in the signature,
 * where it cannot be missed, and the prose below states the
 * consequence.
 */
#if defined(__cplusplus)
#  if defined(__GNUC__) || defined(_MSC_VER)
#    define LOWZAG_RESTRICT __restrict
#  else
#    define LOWZAG_RESTRICT
#  endif
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#  define LOWZAG_RESTRICT restrict
#else
#  define LOWZAG_RESTRICT
#endif

typedef enum {
    LOWZAG_OK = 0,
    LOWZAG_ERR_NOT_JPEG,        /* no SOI / not a JPEG stream            */
    LOWZAG_ERR_NOT_PROGRESSIVE, /* baseline or other frame type — caller
                                  should route to a baseline decoder     */
    LOWZAG_ERR_UNSUPPORTED,     /* legal JPEG, feature out of scope
                                  (CMYK, arithmetic coding, 12-bit,
                                  sampling factors > 2, ...)             */
    LOWZAG_ERR_FORMAT,          /* malformed stream                      */
    LOWZAG_ERR_TRUNCATED,       /* EOF before EOI                        */
    LOWZAG_ERR_NOMEM,           /* allocation failed, or mem_limit too
                                  low to decode the frame at any scale   */
    LOWZAG_ERR_IO,              /* read callback reported failure        */
    LOWZAG_ERR_INVALID_ARG,     /* null/invalid configuration            */
    LOWZAG_ERR_CANCELLED,       /* cancel callback asked to stop         */
    LOWZAG_ERR_BUFFER_TOO_SMALL,/* lowzag_decode_mem: @out cannot hold
                                  the image.  Distinct from NOMEM, which
                                  is about lowzag's own working storage  */
    LOWZAG_ERR_VERSION,         /* a struct_size field is unset, is from
                                  a newer lowzag than this build, or is
                                  not a size any version could have      */
    LOWZAG_PARTIAL,             /* cfg.allow_partial only: the stream
                                  ended before EOI, and what had arrived
                                  was rendered.  Every row was delivered;
                                  the picture is coarser than the file
                                  intended and may hold flat mid-grey
                                  blocks.  See cfg.allow_partial and
                                  info.dc_coverage.  Deliberately not
                                  spelled LOWZAG_ERR_: it is not a
                                  failure, and it is not LOWZAG_OK       */
} lowzag_err_t;

/* A short human-readable description of @err.  Never NULL, never
 * allocates, safe to call from any thread. */
const char *lowzag_strerror(lowzag_err_t err);

/*
 * ── VERSIONING ─────────────────────────────────────────────────────
 *
 * Every public struct begins with a size_t struct_size that the CALLER
 * sets to sizeof of that struct.  It is how lowzag knows which fields
 * exist in the object it has been handed, and it is what makes adding
 * a field a non-event from 2.0.0 onwards.
 *
 *     lowzag_cfg_t cfg = { .struct_size = sizeof cfg, ... };
 *
 * or, for a struct that is memset rather than initialised:
 *
 *     memset(&cfg, 0, sizeof cfg);
 *     cfg.struct_size = sizeof cfg;
 *
 * The LOWZAG_*_INIT macros below do the same thing and are evaluated in
 * the caller's translation unit against the caller's header, which is
 * the only place the right answer is known.  There is deliberately no
 * lowzag_cfg_init() function: a function inside an older library would
 * write an older size into a newer caller's struct, which is precisely
 * the mistake the field exists to catch.
 *
 * What lowzag does with it:
 *
 *   struct_size == sizeof(the library's struct)
 *       Caller and library agree.  Everything is used.
 *
 *   struct_size < sizeof(the library's struct)
 *       An older caller.  Only fields that fit inside struct_size are
 *       read or written; the rest take their documented defaults.  A
 *       field added after 2.0.0 will always have "zero means what 2.0.0
 *       did", so this is a real promise and not a hope.
 *
 *   struct_size > sizeof(the library's struct), or == 0, or not a
 *   multiple of sizeof(size_t)
 *       LOWZAG_ERR_VERSION, before anything is read, allocated or
 *       emitted.
 *
 * The last case covers a caller built against a newer header than the
 * library it linked, a caller who zeroed the struct and forgot the
 * field, and a caller who put something in it that is not a sizeof at
 * all.  All are refused rather than guessed at.  Forgetting is the
 * common mistake, so it fails on the first call, every time, with an
 * error whose text names the field — the alternative, treating 0 as
 * "the 2.0.0 size", would work today and start silently misreading
 * structs the day a field is added, which is the exact failure the
 * field exists to prevent.
 *
 * The multiple-of-size_t requirement costs a conforming caller nothing:
 * every struct here begins with a size_t and holds pointers, so its
 * alignment is a size_t's and its sizeof is a multiple of one, in this
 * version and in every future one.  It is there because a size that
 * lands in the MIDDLE of a field is worse than one that is too large —
 * half a callback pointer is not NULL, so it survives the null check
 * and is then called.  Rejecting it keeps "the field makes a mis-sized
 * struct diagnosable" true for every value, not just the plausible
 * ones.
 *
 * NOTE that this is a check on the value, not on the object: a caller
 * who writes a correct struct_size into a struct that is not really
 * that big is telling lowzag something untrue and gets what anyone gets
 * for that.  Set it with sizeof of the object you are passing.
 *
 * For the same reason, DO NOT PASS AN UNINITIALISED STRUCT.  Through
 * 1.1.1 lowzag_decode() opened by memsetting the whole of *info, so a
 * bare `lowzag_info_t info;` was correct and is what callers wrote.  It
 * cannot do that now — it has to read struct_size to learn how much of
 * the object exists before it may write any of it — so an uninitialised
 * one hands over whatever the stack held.  Nearly always that is
 * refused; a leftover word that happens to be a multiple of
 * sizeof(size_t) inside the struct's size is indistinguishable from an
 * older caller's, and lowzag will fill that much and leave the rest as
 * it found it.  LOWZAG_INFO_INIT costs a word and removes the question.
 * The same goes for lowzag_estimate_t.
 *
 * The rules that keep this true, which bind the library and not the
 * caller: fields are only ever appended; a new field's zero value means
 * the behaviour of the version before it; existing fields never change
 * name, type, order or meaning; lowzag never touches a byte beyond the
 * caller's struct_size.  The shared library's SOVERSION moves only if
 * one of those is broken.
 */

/*
 * Fill @buf with up to @len bytes; return the number read, 0 at end of
 * input, or a negative value on I/O error (which surfaces as
 * LOWZAG_ERR_IO).  Called strictly sequentially — lowzag never seeks, so
 * a pipe or socket is a valid source.  A short read is not an error and
 * not treated as EOF: return 0 for that.
 */
typedef int (*lowzag_read_cb_t)(void *ctx, uint8_t *buf, size_t len);

/*
 * One finished output row, @w pixels wide, at row @y of the delivered
 * image.  Rows are delivered exactly once each, in increasing @y, with
 * @y running 0..out_h-1 — row 0 is the top of the CROP when there is
 * one, not the top of the frame.
 *
 * The format is always 3 bytes per pixel, R,G,B, regardless of how many
 * components the file has.  A grayscale JPEG arrives with its luma
 * replicated across all three channels, so callers need no special
 * case.  @rgb points into lowzag's own scratch buffer and is only valid
 * until the callback returns; copy what you need.
 *
 * There is no return value.  To stop a decode, use cfg.cancel, which
 * also reaches the entropy-decoding pass that runs before the first row
 * appears — on a 12 MP file that is most of the decode, and it is the
 * half a row-callback return value could never touch.
 */
typedef void (*lowzag_row_cb_t)(void *ctx, int y, int w, const uint8_t *rgb);

/*
 * Return nonzero to abandon the decode; lowzag_decode() then returns
 * LOWZAG_ERR_CANCELLED.  Optional: a NULL cancel means a decode always
 * runs to completion.
 *
 * Called from both passes — while the scans are being entropy-decoded
 * and while rows are being emitted — at a bounded amount of work per
 * call, never inside the per-block or per-pixel loops.  Concretely, it
 * is called once per marker segment, once per 64 KB while the
 * coefficient arrays are being cleared, once per block row of every
 * scan, and once per MCU row of the output pass: about 3,000 times for
 * a 12 MP file, which costs under 1% of a decode.
 *
 * Worst case measured between consecutive calls, 4000x3000 4:2:0.  Two
 * files, because entropy density is what drives this and one number
 * would hide it — the same frame size costs five times the gap when the
 * scans are dense:
 *
 *   4000x3000 4:2:0        2 GHz aarch64      ESP32-S3, 240 MHz
 *                          1/4       1/1      1/4       1/1
 *   photo, 247 KB         0.07 ms   0.64 ms    6 ms     57 ms
 *   pure noise, 9.8 MB    0.33 ms   1.41 ms   29 ms    125 ms
 *
 * The photograph is test/corpus/photo-4000x3000-default.jpg.  The
 * ESP32-S3 column is the host column scaled by the README's measurement
 * of the same part (5.7 s where that host takes 64 ms, a factor of 89)
 * rather than a reading taken there.
 *
 * One reading HAS been taken there, on a CrowPanel ESP32-S3 at 240 MHz
 * running examples/esp32 against its 640x480 sample at 1/4: 483 polls
 * over a 212 ms decode, worst gap 2.4 ms.  (2.1.0 reported 481 for the
 * same decode.  One of the two is the APP1 segment that sample now
 * carries, which the marker loop polls before reading whatever it is;
 * the other is the single chunk that 32-byte payload is delivered in,
 * and only a caller who attaches cfg.marker pays for it.)  That
 * is a much smaller frame than the table's, so it does not confirm the
 * 12-megapixel projections; it does show the mechanism behaving on
 * target, and that the scaled figures are not optimistic at the small
 * end.  The example prints this
 * for whatever part is in front of you.
 *
 * SIZE A TIMEOUT FROM THE NOISE ROW.  It is the worst case, and dense
 * entropy data is what an adversary sends; the photograph's 6 ms is the
 * pleasant end of the range and not a bound.
 *
 * test/poll_latency.c is what measures this, over whatever files it is
 * given, at all four scales; `make -C test check-polls` runs it on the
 * corpus, whose largest file is the photograph.  The noise file is 12 MP
 * of random pixels and is not committed — regenerate it with the
 * img_noise() helper in test/gen_corpus.py at 4000x3000 and
 * `cjpeg -quality 90 -progressive`.  The ESP32 example prints the real
 * figure for the part in front of you rather than any projection.
 *
 * On cancellation every allocation lowzag made is released, no further
 * row is delivered, the read callback is not called again, and @info —
 * if one was given — holds the frame geometry if the header had been
 * parsed, along with rows_delivered and the peak_alloc actually
 * reached.  Cancelling is not a failure of the file: a caller that
 * blacklists images on error should not blacklist on this one.
 *
 * A synchronous decode has no other point of contact with the outside
 * world, so this is also where a caller feeds a watchdog, yields, or
 * checks a deadline.  It must not call back into lowzag (see THREADING
 * above).  A flag set by another task is the simplest useful form:
 *
 *     static int stop_cb(void *ctx) { return *(volatile int *)ctx; }
 *     cfg.cancel = stop_cb;  cfg.cancel_ctx = &stop_flag;
 */
typedef int (*lowzag_cancel_cb_t)(void *ctx);

/*
 * ── APPn PASSTHROUGH ───────────────────────────────────────────────
 *
 * The payload of an APPn segment, delivered in chunks as it is read.
 * lowzag does not interpret a byte of it: EXIF is a TIFF parser and an
 * ICC profile is a colour-management problem, and a JPEG decoder that
 * grew either would be a worse JPEG decoder.  What it can do, being the
 * thing that is already walking the marker segments, is hand the bytes
 * to whoever asked for them.
 *
 * @marker    the marker byte, 0xE0..0xEF for APP0..APP15.
 * @offset    where this chunk starts in the payload.  The first chunk of
 *            a segment has offset 0; chunks arrive in order and together
 *            cover the payload exactly.
 * @total     this segment's whole payload length, known from the segment
 *            header before any of it is read.  Decide from it whether to
 *            keep the segment at all — that is how a caller refuses a
 *            60 KB thumbnail without having copied it anywhere.
 * @data/@len this chunk.  @len is at most a few hundred bytes and has no
 *            other guarantee — do not assume a chunk size, and do not
 *            assume one call per segment.  @data points into lowzag's
 *            own buffer and is dead when the callback returns: copy what
 *            you want to keep.
 *
 * The payload is everything after the two-byte segment length, so an
 * EXIF APP1 arrives starting with "Exif\0\0" and a JFIF APP0 with
 * "JFIF\0".  A zero-length segment produces exactly one call with
 * @len 0 and @total 0, so "at least one call per selected segment" holds
 * without a special case.
 *
 * WHY CHUNKS.  lowzag does not buffer the compressed file and will not
 * start: an EXIF APP1 with a thumbnail in it runs to tens of kilobytes,
 * and "lowzag allocates whatever the file asks for" would hand an
 * attacker an allocation whose size is a field in the file.  Chunks cost
 * the caller a little bookkeeping and cost lowzag nothing.  A caller who
 * wants the segment whole allocates @total bytes on the first chunk —
 * after checking it against a ceiling of their own — and memcpy's each
 * chunk to @offset.
 *
 * A segment length is sixteen bits, so no single payload exceeds 65533
 * bytes.  Anything larger is split across segments by the format itself:
 * an ICC profile of a few megabytes arrives as dozens of APP2 segments,
 * each carrying "ICC_PROFILE\0" and its sequence number, and it is the
 * caller who reassembles them if they want the profile.  lowzag delivers
 * each segment on its own and knows nothing about what is in them.
 *
 * RETURN VALUE.  Sign, not value:
 *
 *     0  (LOWZAG_MARKER_CONTINUE)  keep the chunks coming
 *   > 0  (LOWZAG_MARKER_SKIP)      not this segment.  The rest of it is
 *                                  read and dropped, no further chunk of
 *                                  it is delivered, later segments still
 *                                  are, and the decode is unaffected.
 *                                  This is the answer to a segment that
 *                                  is over your ceiling, and to the
 *                                  fortieth APP2 of a colour profile you
 *                                  decided not to keep.
 *   < 0  (LOWZAG_MARKER_STOP)      abandon the decode.  lowzag_decode()
 *                                  returns LOWZAG_ERR_CANCELLED, having
 *                                  freed everything and emitted no row —
 *                                  the same contract cfg.cancel has, and
 *                                  the same code, because from the
 *                                  outside it is the same event: the
 *                                  caller asked to stop.
 *
 * ORDER.  Every segment lowzag reads is delivered before any row,
 * whatever its position in the file: the entropy pass finishes before
 * the output pass begins, so even an APPn sitting between the eighth and
 * ninth scans arrives ahead of row 0.  Segments arrive in file order.  A
 * caller can therefore have its EXIF orientation in hand by the time the
 * first row shows up.
 *
 * What is NOT delivered: anything after the last byte lowzag reads.  A
 * segment past EOI is never seen, and neither is one in the tail of a
 * truncated file — with cfg.allow_partial a short stream still returns
 * an image, and the metadata that did not arrive simply did not arrive.
 *
 * The callback must not call back into lowzag (see THREADING above).
 * cfg.cancel is polled between chunks, so a caller who attaches one is
 * not left waiting out a megabyte of colour profile.
 */
typedef int (*lowzag_marker_cb_t)(void *ctx, int marker,
                                  size_t offset, size_t total,
                                  const uint8_t *data, size_t len);

#define LOWZAG_MARKER_CONTINUE  0
#define LOWZAG_MARKER_SKIP      1
#define LOWZAG_MARKER_STOP    (-1)

/* Bit n of cfg.marker_mask selects APPn, n = 0..15.  There is
 * deliberately no LOWZAG_APP_EXIF: APP1 carries EXIF and it carries XMP,
 * and a name that claims otherwise would be wrong half the time. */
#define LOWZAG_APP(n)     (1u << (n))
#define LOWZAG_APP_ALL    0xFFFFu

typedef struct {
    /* Set to sizeof(lowzag_cfg_t).  See VERSIONING above. */
    size_t struct_size;

    /* input */
    lowzag_read_cb_t read;
    void            *read_ctx;
    /* output */
    lowzag_row_cb_t  row;
    void            *row_ctx;
    /* optional; NULL means the decode cannot be stopped */
    lowzag_cancel_cb_t cancel;
    void              *cancel_ctx;

    /*
     * Allocation for coefficient storage and the row/strip buffers.
     * These are the large, cold allocations — on a part with external
     * RAM this is where it should come from.  Every block lowzag
     * allocates is released before lowzag_decode() returns, on the
     * success, failure and cancellation paths alike.  All four of
     * read/row/alloc/free are required; a NULL one yields
     * LOWZAG_ERR_INVALID_ARG.
     */
    void *(*alloc)(void *ctx, size_t size);
    void  (*free)(void *ctx, void *ptr);
    void   *alloc_ctx;

    /*
     * Optional working state, lowzag_state_size() bytes, aligned as any
     * allocator would return.  This is the hot inner-loop set (Huffman
     * lookup tables, colour tables, the read buffer) and wants fast
     * memory; supplying it here keeps it out of whatever alloc()
     * returns.  Leave it NULL and lowzag takes it from alloc() instead.
     *
     * INITIALISE THE BUFFER BEFORE ITS FIRST USE — calloc() it, or
     * memset() it once at startup.  lowzag reads one word of it on entry
     * (the reentrancy sentinel described under THREADING) and has to do
     * that before writing anything, because writing is precisely what
     * would destroy an outer decode's bookkeeping.  On a buffer that has
     * never been written, that is a read of an indeterminate value:
     * Valgrind and MemorySanitizer both report it, and one bit pattern
     * in 2^64 would be refused with LOWZAG_ERR_INVALID_ARG for no
     * reason.  Any fill will do — the decoder does not read the contents
     * for anything else — but zero is the one to use.
     *
     * Reuse needs nothing further: lowzag clears the sentinel before it
     * returns, on every path, so one buffer feeds decode after decode.
     */
    void   *state;

    /*
     * Scale selection.  0 chooses the largest of 1, 2, 4, 8 whose
     * output still covers target_w x target_h; or force 1, 2, 4 or 8.
     * Anything else is LOWZAG_ERR_INVALID_ARG.
     *
     * target_w/target_h are the size of the DELIVERED image, so with a
     * crop they are compared against the crop's output size, not the
     * whole frame's.  "Show me this region, at least this big" is one
     * request and reads as one.
     *
     * Leaving all three at 0 means full resolution: no target is no
     * constraint, so nothing is given up.
     */
    int     force_scale;
    int     target_w, target_h;

    /*
     * Region of interest, in SOURCE pixels — the coordinate system the
     * frame header uses and lowzag_probe() reports, not the reduced
     * output grid.  All four zero means the whole frame.
     *
     * Source coordinates because the rectangle has to survive the scale
     * changing underneath it: automatic selection and the mem_limit
     * ladder below can both land on a different scale than the caller
     * expected, and an output-space rectangle would then name a
     * different part of the picture.  A source rectangle names the same
     * pixels whatever the scale turns out to be.
     *
     * What it saves is memory: a block outside the rectangle keeps only
     * its 8-byte nonzero map instead of the 2*K bytes of coefficients,
     * and the strip and row buffers shrink to the crop's width.  It
     * also skips the inverse transform and upsampling for those blocks,
     * which is the back half of the decode.  It cannot save parse time
     * — every scan is still entropy-decoded, because a later refinement
     * scan's bit count depends on the nonzero history of blocks it will
     * never display.
     *
     * The rectangle is clamped to the frame; if the intersection is
     * empty the result is LOWZAG_ERR_INVALID_ARG.  So is a negative
     * coordinate or extent, and so is a nonzero crop_x or crop_y with
     * crop_w or crop_h left at zero — that combination is a caller
     * error and is refused rather than read as "no crop".
     *
     * An arbitrary rectangle is accepted.  It is snapped OUTWARD to
     * whole output pixels at the chosen scale, never inward, so the
     * caller never loses a pixel they asked for; internally it is
     * snapped outward again to whole MCUs, which affects only
     * alloc_bytes.  info.crop_* and lowzag_estimate_t.crop_* report the
     * rectangle actually delivered, and out_w/out_h its size.  The snap
     * is worth reading back: an 800x480 window at an odd offset comes
     * out 100x61 at 1/8, not 100x60.  Align crop_x, crop_y, crop_w and
     * crop_h to 8 and it is exact at every scale.
     *
     * Within the delivered rectangle the pixels are bit-identical to
     * the same sub-rectangle of an uncropped decode at the same scale.
     * Cropping changes what is stored and what is emitted, never how a
     * pixel is computed.
     *
     * A rectangle that covers the whole frame is inert: it costs
     * nothing and allocates nothing extra.  Note that a crop is a
     * saving only up to a point — at 1/1 the per-block map that
     * out-of-crop blocks need is not otherwise allocated at all, so a
     * rectangle covering more than about 93% of a 12 MP frame costs
     * more than no crop.  lowzag_estimate() will say so before you
     * commit to it.
     */
    int     crop_x, crop_y, crop_w, crop_h;

    /*
     * Ceiling in bytes on what lowzag will request from alloc(), 0 for
     * no limit.  It covers the coefficient and map arrays and the
     * row/strip buffers — that is, everything except the working state
     * above.
     *
     * SET THIS IF THE INPUT IS UNTRUSTED.  Memory follows the source
     * dimensions in the frame header, and a few hundred bytes of
     * attacker-controlled header can legitimately ask for gigabytes: a
     * 214-byte file declaring 24000x24000 in 4:4:4 really does need
     * ~3.4 GB at scale 1.  Nothing is overflowing when that happens —
     * the arithmetic is right and the memory is genuinely required —
     * but 0 means the only backstop is whatever alloc() does when it
     * runs out.  0 is also what a zeroed config gives you.
     *
     * It is not only a failure threshold.  Over the limit, lowzag gives
     * something up rather than refusing the image, in two tiers:
     *
     *   1. chroma resolution, reported in info.chroma_steps.  Scale and
     *      the output dimensions do not change, so info.scale cannot
     *      see this one.
     *   2. the whole frame drops a scale, reported in info.scale.
     *
     * A crop is applied before either tier, so it makes the ceiling
     * bite later or not at all.  It is never given up to satisfy the
     * ceiling: the caller asked for that region and a decoder that
     * quietly widened it would be worse than one that failed.  At scale
     * 8 there is nothing left to give up and the result is
     * LOWZAG_ERR_NOMEM.
     */
    size_t  mem_limit;

    /*
     * APPn passthrough.  Optional; NULL delivers nothing, which is what
     * every version before 2.2.0 did.  See lowzag_marker_cb_t above for
     * the delivery contract.
     *
     * marker_mask selects which segments are delivered: bit n is APPn,
     * so LOWZAG_APP(1) is EXIF, LOWZAG_APP(2) is an ICC profile, and
     * LOWZAG_APP_ALL is all sixteen.  A mask is not a nicety — it is
     * what stops a caller who wants a 40-byte orientation tag from being
     * handed every byte of an ICC profile spread over dozens of APP2
     * segments, only to drop it on the floor a chunk at a time.
     *
     * A non-NULL callback with a zero mask delivers nothing.  That
     * combination is a caller error the compiler cannot see, and it is
     * called out here because zero is what a memset leaves behind: set
     * the mask when you set the callback.
     *
     * Only APPn is selectable.  COM and the rest are skipped as before —
     * they are not what anyone asked for, and a bit for them can be
     * appended the day someone does.
     */
    lowzag_marker_cb_t marker;
    void              *marker_ctx;
    uint32_t           marker_mask;

    /*
     * Return an image from a stream that ended early.  0 is what every
     * version before 2.2.0 did: LOWZAG_ERR_TRUNCATED, and no rows.
     *
     * Partial decoding is the format's own feature.  A progressive file
     * is a sequence of scans, each refining the last, so a stream that
     * stops after the DC scan and two AC scans still describes a
     * complete, coarser picture — which is exactly what a browser shows
     * while a photograph loads.  Nonzero says: if the file runs out
     * after at least one scan has been decoded, render what arrived
     * rather than throwing it away.
     *
     * The result is LOWZAG_PARTIAL, never LOWZAG_OK.  A caller who asked
     * for an image and got half a file must be able to tell, and a
     * distinct return code is the only way to tell them that they cannot
     * miss.  LOWZAG_OK continues to mean, exactly as before, that the
     * whole file was decoded.
     *
     * What a partial decode delivers is a FULL FRAME: every row 0..out_h-1
     * arrives once, out_w pixels wide, rows_delivered == out_h.  Only the
     * content is coarser.  Blocks that no scan reached hold no
     * coefficients at all and render as flat mid-grey (128,128,128) — if
     * the DC scan itself was cut short, that is a band of grey across the
     * bottom of the picture, and it is not a decoder bug.
     * info.dc_coverage is how you find out before showing it to anyone:
     * it is the percentage of the frame that the DC pass reached, 100 for
     * every complete file.  A caller who will not show a half-grey image
     * checks that number and treats a low one as a failure of its own.
     *
     * Nothing else changes.  A complete file decodes to the same pixels
     * and returns LOWZAG_OK whether or not this is set; a truncated file
     * still returns LOWZAG_ERR_TRUNCATED when it is not.  A read callback
     * that reports FAILURE (a negative return) is still LOWZAG_ERR_IO
     * rather than a partial image: that is the caller's own I/O breaking,
     * not a short file, and lowzag is in no position to decide the
     * difference is unimportant.  A reader that wants the partial image
     * out of a broken socket returns 0 — end of input — instead.
     *
     * Sizing is unaffected: lowzag_estimate() reports the same
     * alloc_bytes either way, and a partial decode makes exactly the
     * allocations a complete one would.
     */
    int     allow_partial;
} lowzag_cfg_t;

typedef struct {
    /* Set to sizeof(lowzag_info_t).  See VERSIONING above. */
    size_t struct_size;

    int src_w, src_h;          /* frame dimensions                      */
    int out_w, out_h;          /* delivered dimensions                  */
    int crop_x, crop_y;        /* source rectangle actually delivered,  */
    int crop_w, crop_h;        /*   = 0,0,src_w,src_h with no crop      */
    int scale;                 /* denominator actually used             */
    int components;            /* 1 or 3                                */
    int chroma_steps;          /* chroma tiers given up to mem_limit,
                                  0 when nothing was sacrificed         */
    int rows_delivered;        /* rows handed to cfg.row before the
                                  return.  == out_h on LOWZAG_OK; less
                                  after a cancellation or a truncated
                                  file, and then it is how much of a
                                  lowzag_decode_mem() buffer is valid   */
    size_t peak_alloc;         /* high-water mark of alloc'd bytes.
                                  Excludes the working state, which is
                                  lowzag_state_size() on top            */

    /*
     * How much of the file arrived.  Filled on every return path,
     * whether or not cfg.allow_partial is set — they describe the
     * stream, not the feature.
     *
     * scans_decoded  scans decoded from start to finish.  A scan the
     *                stream cut in half is not counted, though the
     *                coefficients it did deliver are kept and used.
     *                A complete progressive file is usually 6 to 12.
     * dc_coverage    percentage of the frame that the first DC pass
     *                reached, 0..100.  100 for every complete file, and
     *                for a truncated one whose DC scan finished — then
     *                no block is empty and the picture is merely soft.
     *                Below 100, that fraction of the blocks have no
     *                coefficients whatever and come out flat mid-grey;
     *                in the usual case of one interleaved DC scan they
     *                are the last (100 - dc_coverage)% of the frame.
     *                0 means no block anywhere got a DC term, so the
     *                whole image is grey.
     *
     *                It is the WORST component, not an average, and a
     *                component that no first DC pass reached counts as
     *                0.  That matters because DC scans need not be
     *                interleaved: a file with one DC scan per component,
     *                cut between them, has whole components with nothing
     *                in them, and reporting the scans that did run would
     *                say 100 for a monochrome picture.  Reading this as
     *                "100 means every block of every component has a DC
     *                term" is correct and is the only reading that is.
     * bytes_consumed bytes of the stream the parser actually consumed
     *                before it stopped.  Bytes cfg.read handed over and
     *                lowzag had not reached are not counted.
     */
    int    scans_decoded;
    int    dc_coverage;
    size_t bytes_consumed;
} lowzag_info_t;

typedef struct {
    /* Set to sizeof(lowzag_estimate_t).  See VERSIONING above. */
    size_t struct_size;

    int src_w, src_h;          /* frame dimensions from the header      */
    int out_w, out_h;          /* what a decode with this cfg emits     */
    int crop_x, crop_y;        /* source rectangle it would deliver     */
    int crop_w, crop_h;
    int scale;                 /* denominator that would be chosen      */
    int components;            /* 1 or 3                                */
    int chroma_steps;          /* chroma tiers that would be given up   */
    size_t working_bytes;      /* == lowzag_state_size()                */
    size_t alloc_bytes;        /* bytes decode would ask alloc() for    */
    size_t total_bytes;        /* working_bytes + alloc_bytes           */
} lowzag_estimate_t;

/*
 * Initialisers that set struct_size and zero everything else.  Use them
 * or set the field by hand; they are ordinary aggregate initialisers
 * and carry no ABI of their own.
 *
 * C++ callers building with -Wextra will see -Wmissing-field-initializers
 * from these — g++ suppresses that warning for designated initialisers
 * in C but not in C++ — and should write the two-line form instead:
 * lowzag_cfg_t cfg = {}; cfg.struct_size = sizeof cfg;
 */
#if defined(__cplusplus) && __cplusplus < 202002L
#  define LOWZAG_CFG_INIT      { sizeof(lowzag_cfg_t) }
#  define LOWZAG_INFO_INIT     { sizeof(lowzag_info_t) }
#  define LOWZAG_ESTIMATE_INIT { sizeof(lowzag_estimate_t) }
#else
#  define LOWZAG_CFG_INIT      { .struct_size = sizeof(lowzag_cfg_t) }
#  define LOWZAG_INFO_INIT     { .struct_size = sizeof(lowzag_info_t) }
#  define LOWZAG_ESTIMATE_INIT { .struct_size = sizeof(lowzag_estimate_t) }
#endif

/* Bytes required for cfg.state.  Constant for a given build. */
size_t lowzag_state_size(void);

/*
 * Alignment required of cfg.state, in bytes.  Constant for a given
 * build, and a power of two.
 *
 * Anything an allocator hands back is aligned for every scalar type and
 * so already satisfies this; the function exists for callers who place
 * the state deliberately, which on a part with fast internal RAM and
 * slow external RAM is the reason cfg.state exists at all.  Those
 * callers cannot otherwise know the number: the working state contains
 * a 64-bit field, so it wants 8-byte alignment on targets where a
 * pointer is 4, and ESP-IDF's heap_caps_calloc(..., MALLOC_CAP_INTERNAL
 * | MALLOC_CAP_8BIT) returns 4-byte-aligned memory that lowzag then
 * refuses with LOWZAG_ERR_INVALID_ARG.  Ask, and allocate accordingly:
 *
 *     void *st = heap_caps_aligned_calloc(lowzag_state_align(), 1,
 *                                         lowzag_state_size(),
 *                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
 */
size_t lowzag_state_align(void);

/*
 * Decode one progressive JPEG.  On LOWZAG_OK every output row was
 * delivered exactly once.  @info may be NULL; when it is not, its
 * struct_size must be set — LOWZAG_INFO_INIT, not a bare declaration,
 * which 1.1.1 tolerated because it zeroed the struct for you and this
 * version cannot (see VERSIONING) — and it is filled as soon as the
 * frame header is parsed, so its dimension fields are meaningful even
 * for most error returns (they are zeroed if the failure came before the
 * header).  rows_delivered and peak_alloc are filled on every return
 * path.
 *
 * LOWZAG_ERR_CANCELLED means cfg.cancel asked to stop, or a marker
 * callback returned LOWZAG_MARKER_STOP, and is not a failure of the
 * file.  LOWZAG_PARTIAL means cfg.allow_partial was set and the stream
 * ended early: every row was delivered, from fewer scans than the file
 * intended, and info.dc_coverage says how much of the frame any scan
 * reached.  Every other nonzero return is a failure.
 */
lowzag_err_t lowzag_decode(const lowzag_cfg_t *cfg, lowzag_info_t *info);

/*
 * Cheap classification for dispatch, reading only the header bytes:
 *
 *   LOWZAG_OK                  progressive JPEG, @w/@h filled in
 *   LOWZAG_ERR_NOT_PROGRESSIVE a JPEG, but baseline or another SOF type
 *   LOWZAG_ERR_NOT_JPEG        not a JPEG at all
 *   LOWZAG_ERR_TRUNCATED       what is present is consistent with a
 *                             JPEG but no frame header appeared within
 *                             @len — read more and call again with a
 *                             larger prefix
 *   LOWZAG_ERR_INVALID_ARG     @data is NULL
 *
 * @w and @h may be NULL.  A camera JPEG can carry many kilobytes of
 * EXIF and colour-profile data before its frame header, so treat
 * LOWZAG_ERR_TRUNCATED as "grow the buffer", not as a failure — 64 KB is
 * enough for anything sane.  NOT_JPEG is returned only on the evidence
 * of bytes actually present: a one-byte 0xFF prefix is TRUNCATED, a
 * one-byte 0x42 prefix is NOT_JPEG.
 */
lowzag_err_t lowzag_probe(const uint8_t *data, size_t len, int *w, int *h);

/*
 * What a decode of this file would cost, from the frame header alone.
 * Nothing is decoded and nothing is allocated.
 *
 * @cfg is the config the decode would run with, because force_scale,
 * target_w, target_h, crop_x/y/w/h and mem_limit all change the answer.
 * Only those and struct_size are read: read, row, cancel, alloc, free
 * and state are never touched and may be NULL, so a config built for
 * this call alone is fine.  A NULL @cfg means default behaviour —
 * automatic scale, no target, no crop, no ceiling — which is full
 * resolution.
 *
 * cfg.marker and cfg.allow_partial are not read either, and neither
 * changes the answer.  This function is handed a buffer rather than a
 * stream, so the caller already has whatever APPn bytes are in it — no
 * segment is delivered from here, and none is from lowzag_probe()
 * either.  A partial decode allocates exactly what a complete one does,
 * so alloc_bytes covers it unchanged; the difference is only that a
 * truncated file now reaches the output pass, and therefore reports the
 * peak_alloc this call predicted instead of stopping short of it.
 *
 * alloc_bytes is exact, not an approximation: it is the same
 * computation the decoder itself performs, so it equals the peak_alloc
 * that a completed lowzag_decode() reports for the same file and
 * config, down to the byte, including any degradation forced by
 * cfg.mem_limit and any saving from cfg.crop_*.  When the ceiling
 * bites, scale and chroma_steps say how — they are what the decode
 * would report, not what was asked for.  working_bytes is
 * lowzag_state_size(), which the caller owes on top whether it supplies
 * cfg.state or lets alloc() provide it; total_bytes is the sum.
 *
 * With a crop this is the whole point of the call: it reports the
 * snapped rectangle, the output size and the byte count before a single
 * block is decoded, so a caller can decide whether the region fits and
 * adjust it if it does not.
 *
 * Returns:
 *   LOWZAG_OK                  @out filled in
 *   LOWZAG_ERR_TRUNCATED       no frame header within @len — read more
 *                             of the file and call again
 *   LOWZAG_ERR_NOT_JPEG        not a JPEG at all
 *   LOWZAG_ERR_NOT_PROGRESSIVE baseline (SOF0/SOF1) — route it to a
 *                             baseline decoder
 *   LOWZAG_ERR_UNSUPPORTED     out of scope (12-bit, CMYK, sampling
 *                             factors > 2, over the dimension cap, and
 *                             frame types no baseline decoder takes
 *                             either — arithmetic, lossless,
 *                             hierarchical, which lowzag_probe() does
 *                             not separate out but lowzag_decode does)
 *   LOWZAG_ERR_FORMAT          frame header present but malformed
 *   LOWZAG_ERR_NOMEM           cfg.mem_limit too low even at 1/8, which
 *                             is what the decode would return too
 *   LOWZAG_ERR_INVALID_ARG     @data or @out NULL, a bad force_scale,
 *                             or a crop that does not meet the frame
 *   LOWZAG_ERR_VERSION         a struct_size is unset, too new, or not a
 *                             size any version of the struct could have
 *
 * Only the header is read, so this cannot say whether the entropy data
 * that follows is well formed; a file that estimates cleanly can still
 * fail to decode.
 */
lowzag_err_t lowzag_estimate(const lowzag_cfg_t *cfg,
                             const uint8_t *data, size_t len,
                             lowzag_estimate_t *out);

/*
 * Decode a whole JPEG held in memory straight into a caller-supplied
 * pixel buffer, with no callbacks.  Lives in src/lowzag_mem.c because
 * it is the only part of lowzag that uses malloc and free; leave that
 * file out of a static build (-DLOWZAG_BUILD_MEM_API=OFF) and the rest
 * is unaffected.
 *
 * @cfg carries what the decode should do — force_scale, target_w,
 * target_h, crop_x/y/w/h, cancel, mem_limit, state — and this function
 * supplies how it talks to the world, so read, row, alloc, free and
 * their contexts are ignored and should be left zero.  It is the same
 * @cfg lowzag_estimate() takes, and in the normal flow it is the same
 * object: size the buffer with one call, decode with the next.  @cfg
 * may not be NULL here; a decode with no ceiling and no target is a
 * choice worth writing down.
 *
 * @jpeg/@len is the complete file.  @out receives tightly packed
 * RGB888, out_w * out_h * 3 bytes, top row first, no padding between
 * rows — the same 3-bytes-per-pixel format the row callback delivers,
 * so a grayscale JPEG arrives with its luma replicated.  @jpeg and @out
 * must not overlap; they are restrict-qualified and an overlapping call
 * is undefined, not merely wrong.
 *
 * Passing @out = NULL with @out_size = 0 is a size query: @info is
 * filled with the dimensions the decode would produce and the result is
 * LOWZAG_ERR_BUFFER_TOO_SMALL, without decoding or allocating
 * anything.  That is the same result and the same code path as a real
 * buffer that turns out to be too small, so
 *
 *     e = lowzag_decode_mem(&cfg, jpeg, len, NULL, 0, &info);
 *     buf = malloc((size_t)info.out_w * info.out_h * 3);
 *     e = lowzag_decode_mem(&cfg, jpeg, len, buf, size, &info);
 *
 * works without a separate lowzag_estimate() call.  @out = NULL with a
 * nonzero @out_size is a caller bug and is LOWZAG_ERR_INVALID_ARG.
 * @info may be NULL, but then there is nothing to read the size out of.
 *
 * LOWZAG_ERR_BUFFER_TOO_SMALL means the pixel buffer does not fit the
 * image; LOWZAG_ERR_NOMEM means cfg.mem_limit is too low to decode the
 * frame at all, or malloc failed.  They used to be the same code and a
 * caller could not tell which had happened.
 *
 * On any error after the buffer has been accepted — a truncated file, a
 * malformed one, a cancellation — @out holds info.rows_delivered
 * finished rows and the rest is untouched.  Only LOWZAG_OK means a
 * complete image.  LOWZAG_PARTIAL, which needs cfg.allow_partial, means
 * a complete buffer — every row is written — from a stream that ended
 * early; see cfg.allow_partial and info.dc_coverage.
 *
 * cfg.marker and cfg.marker_mask are carried through and work here as
 * they do in lowzag_decode(), for callers who want the APPn payloads
 * without walking the file themselves.  The whole file is already in
 * memory on this path, so the chunking buys nothing; it is the same
 * callback because it is the same decoder.
 */
lowzag_err_t lowzag_decode_mem(const lowzag_cfg_t *cfg,
                               const uint8_t *LOWZAG_RESTRICT jpeg, size_t len,
                               uint8_t *LOWZAG_RESTRICT out, size_t out_size,
                               lowzag_info_t *info);

/*
 * The EXIF orientation in a captured APP1 payload: 1..8, or 0 if there
 * is not one.
 *
 * @app1/@len is the payload exactly as cfg.marker delivered it —
 * starting at "Exif\0\0", not at the 0xFF 0xE1 or the length.  A segment
 * captured in chunks must be reassembled first.  Orientation lives in
 * IFD0, within the first few hundred bytes of every camera file seen, so
 * a caller who caps its capture at a few kilobytes still finds it; a
 * caller who caps it lower may not, and gets 0.
 *
 * Fifty lines of TIFF header walking, and it is what almost every caller
 * of the marker callback actually wants — the alternative is that every
 * one of them writes these fifty lines, and the ones who get it wrong
 * get it wrong against attacker-controlled bytes.  So: no allocation, no
 * recursion, no sub-IFDs, every offset checked against @len, a bound on
 * the entry count, and any disagreement at all answered with 0 rather
 * than a guess.  0 means "no usable orientation here" and covers a
 * truncated payload, a foreign APP1 (XMP also lives in APP1), a
 * malformed TIFF header, a tag that is absent, and a value outside 1..8.
 *
 * The value is what the file claims and nothing more; lowzag does not
 * rotate anything.  1 is upright and the other seven are the usual
 * EXIF transposes.
 *
 * This lives in src/lowzag_mem.c, with lowzag_decode_mem(), so that
 * src/lowzag.c keeps its property of referencing nothing but memcpy and
 * memset.  A static build configured with -DLOWZAG_BUILD_MEM_API=OFF
 * therefore does not have it, and a freestanding build that does call it
 * links the translation unit that also contains malloc and free — one
 * object file's worth, which -ffunction-sections -Wl,--gc-sections
 * removes again.
 */
int lowzag_exif_orientation(const uint8_t *app1, size_t len);

#ifdef __cplusplus
}
#endif
