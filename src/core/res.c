#include "res.h"
#include "lzw.h"
#include "osystem.h"
#include "byteorder.h"
#include "fs/fs.h"

#include <stdint.h>

typedef struct VolReader {
    FSVolume *v;
    uint32_t pos;
} VolReader;

/* The intro-movie disc table, ported from `202F:0x6287` (1000:AFF8's
 * lookup).  Corpus-verified 10/10: ANI<idx>.ANI ships in exactly the
 * HUMANS.00N the table names (extraction index.txt, all languages):
 * ANI00,04,07,08,09 -> disc 2; ANI01,05 -> disc 0; ANI02,03,06 -> disc 1.
 * The two zeros after index 9 (the listing's bytes 10-11) have no consumer
 * evidence, so anything >= 10 returns 0xFF like the original's 0xFF arm. */
static const uint8_t _res_movie_disc_table[10] = {
    2, 0, 1, 1, 2, 0, 1, 2, 2, 2
};

/* The [0xF7E]: the disc number name-based loads come from.  The original
 * never scans the discs - the disk-screen sequence and the level headers
 * ([+0x01] through the table above) pick the number, the loaders build the
 * ONE volume-file name from it. */
static uint16_t _res_current_disc = 0;

/* The backend (and through it the filesystem module); res_init binds it. */

void res_set_disc(uint16_t disc) { _res_current_disc = disc; }

uint8_t res_movie_disc(uint32_t idx)
{
    if (idx >= sizeof _res_movie_disc_table) return 0xFF;
    return _res_movie_disc_table[idx];
}

/**
 * @brief Read n bytes from the volume at an absolute offset.
 *
 * @param r the volume reader
 * @param at the byte offset
 * @param dst the destination buffer
 * @param n the count
 * @return 0 on success, -1 on a short read
 */
static int32_t _vol_read_at(VolReader *r, uint32_t at, uint8_t *dst, uint32_t n)
{
    uint32_t got = g_osys->fs->read(r->v, at, dst, n);
    if (got != n) return -1;
    return 0;
}

/**
 * @brief Read one little-endian word from the volume cursor.
 *
 * A short read is swallowed as 0 and the cursor stays put: the LZWWordReader
 * ABI has no failure channel.  The original is no better off - its
 * resource_getword (1000:92aa) ignores the INT 21h result at 1000:92f1 and
 * decodes from its stale 8 KB buffer - so only the corrupt-stream output
 * differs (deviation D-263).
 *
 * @param r the volume reader
 * @return the word
 */
static uint16_t _vol_word(VolReader *r)
{
    /* The following seems quite ridiculous just to return
     * two bytes, but the need a way to remain endianness
     * agnostic. The compiler will take care of simplify
     * things where this absurdity is not needed. Hopefully.
     */
    uint8_t tmp[2];
    if (_vol_read_at(r, r->pos, tmp, 2)) return 0;
    r->pos += 2;
    return rd_le_u16(tmp);
}

/**
 * @brief Volume reader adapter: reads u16 words at VolReader::pos.
 *
 * @param ctx the reader context
 * @return the next word
 */
static uint16_t _vol_word_reader(void *ctx)
{ return _vol_word((VolReader *)ctx); }

/* --- the chunk directory and the block loop -------------------------- */

/** @brief The chunk name is uppercased and SPACE-padded to its 18 bytes in the
 * record (the live DS:0x4799 header: `MUS\LEVJ.ALB      `), so the compare
 * folds case and accepts 0 or ' ' as the pad.
 *
 * @param req the required name
 * @param rec the record's 18 name bytes
 * @return 1 on a match, 0 otherwise
 */
static int32_t _name_equal(const char *req, const uint8_t *rec)
{
    const char *a = req;
    for (uint16_t i = 0; i < 18; i++) {
        char cb = (char)rec[i];
        if (*a == 0) return cb == 0 || cb == ' ';
        char ca = *a++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return *a == 0;
}

/** @brief Decode the chunk whose record lives at `off` into a heap buffer
 * of `length` bytes (caller frees).
 *
 * `chkOut` receives the resource-level BP as resource_decode_chunk drives it:
 * the flag word XORs in for both paths (1000:9193), stored blocks XOR their
 * payload words (1000:919F..91A7) and each LZ block RESETS it (1000:91C9).
 *
 * @param r the volume reader
 * @param off the offset
 * @param rec the record's 0x40 bytes
 * @param length the length
 * @param chkOut the output checksum
 * @return the decoded buffer, or NULL on failure
 */
static uint8_t *_res_decode_chunk(VolReader *r, uint32_t off,
                                  const uint8_t *rec, uint32_t length,
                                  uint16_t *chkOut)
{
    uint16_t block = rd_le_u16(rec + 0x36);
    uint8_t *out = g_osys->memAlloc(length ? length : 1);
    uint16_t acc = 0;
    if (!out) return NULL;
    if (block == 0u) { /* the original's stored loop copies 0x10000 words
                        * (LOOP with CX = 0) and never advances the chunk;
                        * the port refuses the malformed record instead -
                        * deviation D-198. */
        g_osys->memFree(out);
        return NULL;
    }

    r->pos = off;
    uint32_t done = 0;
    uint8_t failed = 0;
    while (done < length) {
        uint16_t flag = _vol_word(r);
        acc ^= flag; // 1000:9193, both paths
        if (flag == 0) { // stored raw  919f..91a7
            uint32_t take =
                (uint32_t)block < length - done ? block : length - done;
            /* 1000:9199 MOV CX,[0x4781] / SHR CX,1 / STOSW / LOOP: the arm
             * writes WORDS, so an odd tail byte is never written.  The
             * stream still advances by the full take (1000:9135/0x915F),
             * which is where the next flag word is read. */
            uint32_t copy = take & ~1u;
            if (_vol_read_at(r, r->pos, out + done, copy)) {
                failed = 1;
                break;
            }
            for (uint16_t i = 0; i < copy; i += 2)
                acc ^= rd_le_u16(out + done + i);
            r->pos += take;
            done += take;
        } else { // LZ block
            uint32_t clen = _vol_word(r);
            if (clen > length - done) {
                failed = 1;
                break;
            }
            /* lzw_block owns the cursor: the block fills backwards from
             * out[done + clen) down to out[done]. */
            acc = lzw_block(_vol_word_reader, r, out, done,
                            clen); // MOV BP,AX reset (91c9)
            done += clen;
        }
    }
    if (failed) {
        g_osys->memFree(out);
        return NULL;
    }
    if (chkOut) *chkOut = acc;
    return out;
}

uint8_t *res_load_chunk(const char *name, uint32_t *sizeOut, uint16_t *chkOut)
{
    uint8_t *result = NULL;
    /* One volume, the disc the caller installed (the original's [0xF7E]-
     * built "HUMANS  .00N" open; a miss is the insert-disc-prompt case). */
    FSVolume *v = g_osys->fs->openVolume(_res_current_disc);
    if (!v) return NULL;
    VolReader r = { v, 0 };

    uint8_t head[2];
    if (_vol_read_at(&r, 0, head, 2)) {
        g_osys->fs->closeVolume(v);
        return NULL;
    }
    uint16_t count = rd_le_u16(head);

    for (uint16_t i = 0; i < count && !result; i++) {
        uint8_t rec[0x40];
        if (_vol_read_at(&r, 2u + i * 0x40u, rec, sizeof rec)) break;
        if (!_name_equal(name, rec)) continue;

        uint32_t length = rd_le_u32(rec + 0x38);
        uint32_t off = rd_le_u32(rec + 0x3c);
        result = _res_decode_chunk(&r, off, rec, length, chkOut);
        if (result && sizeOut) *sizeOut = length;
    }
    g_osys->fs->closeVolume(v);
    return result;
}

int32_t res_init(void) { return 0; }

uint8_t res_volume_present(uint16_t disc)
{
    FSVolume *v = g_osys->fs->openVolume(disc);
    if (!v) return 0;
    g_osys->fs->closeVolume(v);
    return 1;
}
