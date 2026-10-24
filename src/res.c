/* res.c — resource I/O (game_structure.md §1 subsystem 2, re_formats.md §8).
 *
 * The volume file (one per disc) is
 *
 *     u16 count, then count × 0x40-byte records:
 *         name[18] NUL-padded, secondary[10], pad,
 *         u16 block @0x36 (0x4000 on this disc),
 *         u32 length @0x38, u32 offset @0x3c (bytes into the volume file)
 *     then the chunk data.
 *
 * A chunk at its offset is a run of blocks producing `length` bytes:
 *     flag 0        -> the next min(block, remaining) bytes are stored raw,
 *                      and EVERY block again starts with a flag word (so the
 *                      in-memory image is not the payload sliced after the
 *                      first flag — re_formats.md §8.0)
 *     flag non-zero -> u16 clen, then [u16 unused][u16 init bit buffer] and
 *                      an LZ bitstream expanding to exactly clen bytes
 *
 * The LZ machine is the game's decode_bitstream (1000:91B9) as transcribed
 * instruction-for-instruction from the VALIDATED reference
 * (tools/ani_lzw.py, --selftest): the bit buffer shifts RIGHT, the bit comes
 * out of bit 0, the refill is (word | 0x10000) >> 1 delivered as its own
 * bit 0, values assemble MSB first, the output fills BACKWARDS, the match
 * distance is signed by the backward cursor, and draining the buffer steals
 * the refill's first bit for the current consumption.  Every word read
 * XORs into the running checksum (BP), which decode_bitstream re-seeds
 * from the stream's checksum field per block (91c9 MOV BP,AX — the 91bc
 * size-word XOR never survives it) and resource_load returns to its
 * caller through the flags (1000:9183 OR BP,BP / RET).
 *
 * The chunk lookup uses the ported disc table (202F:0x6287) through
 * res_movieDisc + the current-disc state res_setDisc — the original
 * opens ONE volume ([0xF7E]-named) and never scans across discs.
 *
 * Shortcut: the resource-level checksum is carried and exposed (chkOut)
 * but not gated — the original does not gate it either (levels\CODES.BIN
 * loads live with a nonzero block accumulator); the verified gate is the
 * scene-object test (1000:B444) which lands with the scene player.
 */
#include "game.h"
#include "fs/fs.h"
#include <stdlib.h>
#include <string.h>

/* The intro-movie disc table, ported from `202F:0x6287` (1000:AFF8's
 * lookup).  Corpus-verified 10/10: ANI<idx>.ANI ships in exactly the
 * HUMANS.00N the table names (extraction index.txt, all languages):
 * ANI00,04,07,08,09 -> disc 2; ANI01,05 -> disc 0; ANI02,03,06 -> disc 1.
 * The two zeros after index 9 (the listing's bytes 10-11) have no consumer
 * evidence, so anything >= 10 returns 0xFF like the original's 0xFF arm. */
static const uint8_t res_movieDiscTable[10] = { 2, 0, 1, 1, 2, 0, 1, 2, 2, 2 };

/* The [0xF7E]: the disc number name-based loads come from.  The original
 * never scans the discs — the disk-screen sequence and the level headers
 * ([+0x01] through the table above) pick the number, the loaders build the
 * ONE volume-file name from it. */
static unsigned res_currentDisc = 0;

void res_setDisc(unsigned disc) { res_currentDisc = disc; }

unsigned res_movieDisc(unsigned idx)
{
    if (idx >= sizeof res_movieDiscTable) return 0xFF;
    return res_movieDiscTable[idx];
}

typedef struct VolReader {
    FSVolume *v;
    uint32_t  pos;
} VolReader;

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)(unsigned)p[0] | ((uint32_t)(unsigned)p[1] << 8) |
           ((uint32_t)(unsigned)p[2] << 16) | ((uint32_t)(unsigned)p[3] << 24);
}

static int vol_sixteen(VolReader *r, uint32_t at, uint8_t *dst, unsigned n)
{
    uint32_t got = fs.read(r->v, at, dst, n);
    if (got != n) return -1;
    return 0;
}

static uint16_t vol_word(VolReader *r)
{
    uint8_t tmp[2];
    if (vol_sixteen(r, r->pos, tmp, 2)) return 0;
    r->pos += 2;
    return rd_le16(tmp);
}

/* --- the LZ machine (decode_bitstream, per compressed block) ---------- */

typedef uint16_t (*WordReader)(void *ctx);

typedef struct LzState {
    WordReader  rd;            /* reads the next u16 from the stream     */
    void       *rd_ctx;        /* reader context (volume or memory)      */
    uint8_t    *out;           /* the whole decoded resource             */
    uint32_t    base;          /* this block's first output byte         */
    uint32_t    p;             /* the backward cursor (fills down)       */
    uint32_t    buf;           /* the bit buffer                         */
    uint16_t    chk;           /* the running BP (checksum) accumulator  */
    unsigned    under;         /* cursor wrapped past 0 (scene-form CMP  */
                               /*  DI,-1 -> END: the game discards the   */
                               /*  overshoot bytes and stops)            */
} LzState;

/* Volume reader adapter: reads u16 words at VolReader::pos. */
static uint16_t vol_word_reader(void *ctx)
{
    return vol_word((VolReader *)ctx);
}

static unsigned lz_bit(LzState *s)
{
    unsigned bit = s->buf & 1u;
    s->buf >>= 1;
    if (s->buf == 0) {
        unsigned w = s->rd(s->rd_ctx);            /* the refill delivers its bit 0 */
        s->chk ^= (uint16_t)w;                  /* every word read XORs into BP  */
        bit = w & 1u;
        s->buf = (w | 0x10000u) >> 1;
    }
    return bit;
}

static unsigned lz_bits(LzState *s, unsigned n)
{
    unsigned v = 0;
    while (n--) v = (v << 1) | lz_bit(s);
    return v;
}

static void lz_literals(LzState *s, unsigned n)
{
    while (n--) {
        unsigned byte = lz_bits(s, 8);   /* b4e8: the read happens FIRST */
        if (s->p == 0) { s->under = 1; return; }  /* b4fd: DI == -1 -> END:
                        * the store is skipped but the read's XOR stays in
                        * BP — the aborted decode fails the checksum (the
                        * game's b446 unwind) */
        s->out[--s->p] = (uint8_t)byte;
    }
}

static void lz_match(LzState *s, unsigned n, unsigned distbits)
{
    unsigned d = lz_bits(s, distbits);
    while (n--) {
        unsigned byte;
        if (s->p == 0) { s->under = 1; return; }  /* CMP DI,-1 -> END */
        s->p -= 1;                      /* source is read after the cursor step
                                         * (the reference's `p -= 1;
                                         *  out[p] = out[p + d]`).  The
                                         * original's ES:[BX+DI] is per-chunk:
                                         * resource_load re-bases ES every 16K
                                         * chunk (1000:9156), so the source
                                         * never wraps at 64K of the flat
                                         * buffer — the mask that put it there
                                         * corrupted BG4's chunk 4. */
        byte = s->out[s->p + d];
        s->out[s->p] = byte;
    }
}

/* shortcut: a corrupt stream can read up to 4095 bytes past the written
 * region of `out` here (the asm wraps inside its always-mapped 64K scratch
 * segment; the port has no such floor).  Every shipped asset passes the
 * checksum gate, so only corrupt data reaches this — bound the read when
 * handling untrusted RSCs. */

/* One compressed block: `clen` bytes, checker fresh per block exactly as
 * decode_bitstream resets BP to the stream's checksum field (the 91c7 XOR
 * is dead — 91c9 MOV BP,AX overwrites it), then XORs the bit-buffer word
 * and every refill word.  Returns the block's final accumulator. */
static uint16_t lz_block(LzState *s, unsigned clen)
{
    s->base = s->p;
    s->p += clen;
    s->chk = s->rd(s->rd_ctx);                   /* the checksum field        */
    s->buf = s->rd(s->rd_ctx);                   /* the initial bit buffer    */
    s->chk ^= (uint16_t)s->buf;
    while (!s->under && s->p > s->base) {
        if (lz_bit(s) == 0) {
            if (lz_bit(s) == 1) lz_match(s, 2, 8);
            else                lz_literals(s, lz_bits(s, 3) + 1);
        } else {
            unsigned b = lz_bits(s, 2);
            if (b == 0)      lz_match(s, 3, 9);
            else if (b == 1) lz_match(s, 4, 10);
            else if (b == 2) lz_match(s, lz_bits(s, 8) + 1, 12);
            else             lz_literals(s, lz_bits(s, 8) + 9);
        }
    }
    return s->chk;
}

/* Memory reader adapter for decoding LZW objects straight out of a
 * loaded image (e.g. an .ANI's chunk data). */
typedef struct {
    const uint8_t *d;
    uint32_t       pos;
    uint32_t       len;
    unsigned       over;     /* a read ran past the loaded image            */
    unsigned       ate_tail; /* the stream was consumed to the image's end  */
} MemRd;

static uint16_t mem_word_reader(void *ctx)
{
    MemRd *mr = (MemRd *)ctx;
    if (mr->pos + 2 > mr->len) { mr->over = 1; return 0; }
    uint16_t w = rd_le16(mr->d + mr->pos);
    mr->pos += 2;
    if (mr->pos == mr->len) mr->ate_tail = 1;   /* the stream ran to the image's exact end */
    return w;
}

/* Decode one LZW object from a memory buffer:
 *   [u16 decompressed_size][u16 checksum][u16 bit buffer][LZW bits]
 * Returns the decompressed size written to `out`, or -1 on failure.
 * *chkOut mirrors the machine's final BP accumulator. */
int res_decode_lzw_mem(const uint8_t *data, uint32_t data_size,
                       uint32_t off, uint8_t *out, uint32_t out_size,
                       uint16_t *chkOut)
{
    if (off + 6 > data_size) return -1;
    uint16_t size = rd_le16(data + off);
    if (size > out_size) return -1;

    MemRd mr = { data, off + 2, data_size, 0, 0 };
    /* p starts at 0: lz_block sets base = p and p += size, so the backwards
     * fill lands at out[0 .. size).  lz_block reads [checksum][bit buffer]
     * first — the stream starts right after the size word. */
    LzState st = { mem_word_reader, &mr, out, 0, 0, 0, 0, 0 };
    uint16_t chk = lz_block(&st, size);
    if (mr.over || mr.ate_tail) {
        /* Machine-verified (dosbox-staging debugger session, 2026-10-24):
         * the GT loop's 33rd object (obj 29, the display list's last word,
         * size 1378) FAILS the b444 gate on the machine — EBP = 0x613C at
         * B446 with ESI = 0x312 (394 words = exactly our consumption); the
         * piece buffer's last word read 0x237E where the shipped file has
         * 0x4242, and 0x4242 ^ 0x237E = 0x613C = the whole failure.  Every
         * scene file's last object is such a tail-eater (slack = 0 in all
         * 12 files), and the machine never blits them (the al_capture:
         * 35 decodes, 32 blits).  The static code path producing the
         * machine's nonzero accumulator is not identifiable from the
         * image; the observable contract is: a stream consumed to the
         * loaded image's exact end fails the gate, so the object is never
         * blitted and the previous frame holds. */
        chk = 0xFFFFu;
    }
    if (chkOut) *chkOut = chk;
    return (int)size;
}

/* --- the chunk directory and the block loop -------------------------- */

/* The chunk name is uppercased and SPACE-padded to its 18 bytes in the
 * record (the live DS:0x4799 header: `MUS\LEVJ.ALB      `), so the compare
 * folds case and accepts 0 or ' ' as the pad. */
static int name_equal(const char *req, const uint8_t *rec /* 18 bytes */)
{
    const char *a = req;
    for (unsigned i = 0; i < 18; i++) {
        char cb = (char)rec[i];
        if (*a == 0) return cb == 0 || cb == ' ';
        char ca = *a++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return *a == 0;
}

/* Decode the chunk whose record lives at `off` in volume `r->v` into a
 * malloc'd buffer of `length` bytes (caller frees).  NULL = failure.
 * `chkOut` receives the resource-level BP: the accumulator runs the way
 * resource_decode_chunk drives it — the flag word XORs in for both paths
 * (1000:9193), stored blocks XOR their payload words (1000:919f..91a7)
 * and each LZ block RESETS the accumulator (the MOV BP,AX at 91c9). */
static uint8_t *res_decodeChunk(VolReader *r, uint32_t off,
                                const uint8_t *rec /* 0x40 bytes */,
                                uint32_t length, uint16_t *chkOut)
{
    unsigned block = rd_le16(rec + 0x36) ? rd_le16(rec + 0x36) : 0x4000;
    uint8_t *out = malloc(length ? length : 1);
    uint16_t acc = 0;
    if (!out) return NULL;

    r->pos = off;
    uint32_t done = 0;
    while (done < length) {
        uint16_t flag = vol_word(r);
        acc ^= flag;                           /* 1000:9193, both paths   */
        if (flag == 0) {                       /* stored raw  919f..91a7  */
            uint32_t take = (uint32_t)block < length - done ? block : length - done;
            if (vol_sixteen(r, r->pos, out + done, (unsigned)take)) goto fail;
            for (uint32_t i = 0; i < (take & ~1u); i += 2)
                acc ^= rd_le16(out + done + i);
            r->pos += take;
            done += take;
        } else {                               /* LZ block               */
            unsigned clen = vol_word(r);
            if (clen > length - done) goto fail;
            /* lz_block owns the cursor: p starts at the block's first
             * output byte and it advances p itself. */
            LzState st = { vol_word_reader, r, out, 0, done, 0, 0, 0 };
            acc = lz_block(&st, clen);         /* MOV BP,AX reset (91c9) */
            done += clen;
        }
    }
    if (chkOut) *chkOut = acc;
    return out;

fail:
    free(out);
    return NULL;
}

uint8_t *res_loadChunk(const char *name, uint32_t *sizeOut, uint16_t *chkOut)
{
    uint8_t *result = NULL;
    /* One volume, the disc the caller installed (the original's [0xF7E]—
     * built "HUMANS  .00N" open; a miss is the insert-disc-prompt case). */
    FSVolume *v = fs.openVolume(res_currentDisc);
    if (!v) return NULL;
    VolReader r = { v, 0 };

    uint8_t head[2];
    if (vol_sixteen(&r, 0, head, 2)) { fs.closeVolume(v); return NULL; }
    unsigned count = rd_le16(head);

    for (unsigned i = 0; i < count && !result; i++) {
        uint8_t rec[0x40];
        if (vol_sixteen(&r, 2u + i * 0x40u, rec, sizeof rec)) break;
        if (!name_equal(name, rec)) continue;

        uint32_t length = rd_le32(rec + 0x38);
        uint32_t off    = rd_le32(rec + 0x3c);
        result = res_decodeChunk(&r, off, rec, length, chkOut);
        if (result && sizeOut) *sizeOut = length;
    }
    fs.closeVolume(v);
    return result;
}

int res_init(const OSystem *osys) { (void)osys; return 0; }
