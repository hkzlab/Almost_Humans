#include "lzw.h"
#include "byteorder.h"

#include <stdint.h>

/* --- the LZ machine (decode_bitstream, per compressed block) ---------- */
typedef struct LzState {
    LZWWordReader rd; // reads the next u16 from the stream
    void *rd_ctx; // reader context (volume or memory)
    uint8_t *out; // the whole decoded resource
    uint32_t base; // this block's first output byte
    uint32_t p; // the backward cursor (fills down)
    uint32_t buf; // the bit buffer
    uint16_t chk; // the running BP (checksum) accumulator
    uint32_t under; // cursor wrapped past 0 (scene-form CMP
    /*  DI,-1 -> END: the game discards the   */
    /*  overshoot bytes and stops)            */
} LzState;

/* Memory reader adapter for decoding LZW objects straight out of a
 * loaded image (e.g. an .ANI's chunk data). */
typedef struct {
    const uint8_t *d;
    uint32_t pos;
    uint32_t len;
    uint32_t over; // a read ran past the loaded image
    uint32_t ate_tail; // the stream was consumed to the image's end
} MemRd;

/**
 * @brief Read one bit from the LZ stream.
 *
 * @param s the LZ decoder state
 * @return the bit
 */
static uint32_t _lz_bit(LzState *s)
{
    uint8_t bit = s->buf & 1u;
    s->buf >>= 1;
    if (s->buf == 0) {
        uint16_t w = s->rd(s->rd_ctx); // the refill delivers its bit 0
        s->chk ^= w; // every word read XORs into BP
        bit = w & 1u;
        s->buf = (w | 0x10000u) >> 1;
    }
    return bit;
}

/**
 * @brief Read n bits from the LZ stream.
 *
 * @param s the LZ decoder state
 * @param n the count
 * @return the n-bit value
 */
static uint32_t _lz_bits(LzState *s, uint32_t n)
{
    uint16_t v = 0;
    while (n--) v = (uint16_t)((v << 1) | _lz_bit(s));
    return v;
}

/**
 * @brief Copy n literal bytes from the LZ stream.
 *
 * @param s the LZ decoder state
 * @param n the count
 */
static void _lz_literals(LzState *s, uint32_t n)
{
    while (n--) {
        uint8_t byte =
            (uint8_t)(_lz_bits(s, 8)); // b4e8: the read happens FIRST
        if (s->p == 0) {
            s->under = 1;
            return;
        } /* b4fd: DI == -1 -> END:
           * the store is skipped but the read's XOR stays in
           * BP - the aborted decode fails the checksum (the
           * game's b446 unwind) */
        s->out[--s->p] = byte;
    }
}

/**
 * @brief Copy one LZ back-reference of n bytes.
 *
 * @param s the LZ decoder state
 * @param n the count
 * @param distbits the distance bit count
 */
static void _lz_match(LzState *s, uint32_t n, uint32_t distbits)
{
    uint16_t d = (uint16_t)(_lz_bits(s, distbits));
    while (n--) {
        uint8_t byte;
        if (s->p == 0) {
            s->under = 1;
            return;
        } // CMP DI,-1 -> END
        /* The source is read after the cursor step (`p -= 1;
         * out[p] = out[p + d]`).  The original re-bases ES every 16K chunk
         * (1000:9156), so the source never wraps at 64K of the flat buffer. */
        s->p -= 1;
        byte = s->out[s->p + d];
        s->out[s->p] = byte;
    }
}

/* shortcut: a corrupt stream can read up to 4095 bytes past the written
 * region of `out` here (the asm wraps inside its always-mapped 64K scratch
 * segment; the port has no such floor).  Deviation D-23.  Every
 * shipped asset passes the
 * checksum gate, so only corrupt data reaches this - bound the read when
 * handling untrusted RSCs. */

uint16_t lzw_block(LZWWordReader rd, void *ctx, uint8_t *out, uint32_t base,
                   uint32_t clen)
{
    /* p starts at the block's last byte: the fill runs downwards from there
     * and the block loop ends at `base`. */
    LzState s = { rd, ctx, out, base, base + clen, 0, 0, 0 };
    /* The checker is fresh per block exactly as decode_bitstream resets BP to
     * the stream's checksum field (the 91c7 XOR is dead - 91c9 MOV BP,AX
     * overwrites it), then XORs the bit-buffer word and every refill word. */
    s.chk = s.rd(s.rd_ctx); // the checksum field
    s.buf = s.rd(s.rd_ctx); // the initial bit buffer
    s.chk ^= (uint16_t)s.buf;
    while (!s.under && s.p > s.base) {
        if (_lz_bit(&s) == 0) {
            if (_lz_bit(&s) == 1) _lz_match(&s, 2, 8);
            else _lz_literals(&s, _lz_bits(&s, 3) + 1);
        } else {
            uint8_t b = (uint8_t)(_lz_bits(&s, 2));
            if (b == 0) _lz_match(&s, 3, 9);
            else if (b == 1) _lz_match(&s, 4, 10);
            else if (b == 2) _lz_match(&s, _lz_bits(&s, 8) + 1, 12);
            else _lz_literals(&s, _lz_bits(&s, 8) + 9);
        }
    }
    return s.chk;
}

/**
 * @brief The in-memory word reader for the LZ decoder.
 *
 * @param ctx the reader context
 * @return the next word
 */
static uint16_t _mem_word_reader(void *ctx)
{
    MemRd *mr = (MemRd *)ctx;
    if (mr->pos + 2 > mr->len) {
        mr->over = 1;
        return 0;
    }
    uint16_t w = rd_le_u16(mr->d + mr->pos);
    mr->pos += 2;
    if (mr->pos == mr->len)
        mr->ate_tail = 1; // the stream ran to the image's exact end
    return w;
}

int32_t lzw_decode_mem(const uint8_t *data, uint32_t data_size, uint32_t off,
                       uint8_t *out, uint32_t out_size, uint16_t *chkOut)
{
    if (off + 6 > data_size) return -1;
    uint16_t size = rd_le_u16(data + off);
    if (size > out_size) return -1;

    MemRd mr = { data, off + 2, data_size, 0, 0 };
    uint16_t chk = lzw_block(_mem_word_reader, &mr, out, 0, size);
    if (mr.over || mr.ate_tail) {
        /* A stream consumed to the image's exact end fails the gate: the
         * object is never blitted and the previous frame holds.  Every
         * scene file's last object is such a tail-eater. */
        chk = 0xFFFFu;
    }
    if (chkOut) *chkOut = chk;
    return (int32_t)size;
}
