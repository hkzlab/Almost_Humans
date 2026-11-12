#include "font.h"
#include "osystem.h"
#include "byteorder.h"
#include "width.h"

#include <stdint.h>
#include <stdio.h>

/* 202F:0x1E28 - the charset (44 glyphs + the terminator). */
static const char _charset[45] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!., '?ae";

/* 202F:0x1E55 - the advances, then the terminator slot's 0x50. */
static const uint8_t _advance[45] = { 7, 8, 8, 8,  7, 7, 8, 8, 3,   7, 8,  7,
                                      9, 8, 8, 7,  8, 7, 8, 9, 7,   8, 11, 8,
                                      7, 9, 7, 7,  8, 8, 7, 8, 7,   7, 8,  7,
                                      3, 3, 3, 10, 3, 7, 7, 7, 0x50 };

/* [0x1E1C] - the published apl\text.spr (loaded by the attract's
 * game_loop prologue and the front-end later; _blit_glyph's only source). */
static const uint8_t *_glyphs;

static int _arena_warned;     // the overflow warning fires once

int32_t font_init(void)
{
    _arena_warned = 0;
    return 0;
}

void font_publish(const uint8_t *textSpr) { _glyphs = textSpr; }

/** @brief The _charset lookup: the match index, or 44 (the terminator slot) when
 * the character is not in the table (draw_text's scan stops there).
 *
 * @param c the character
 * @return the glyph index, or 44 when not found
 */
static uint32_t _charset_index(uint8_t c)
{
    for (uint16_t i = 0; i < 44u; i++)
        if ((uint8_t)_charset[i] == c) return i;
    return 44u;
}

/** @brief The nibble scatter's net effect (the 4x SHL/RCR + 4x SHR of 1000:4a3c,
 * and the 4x SHR/RCL of 1000:4a5b): the nibble, 4-bit-reversed.
 *
 * @param v the four-pixel byte
 * @return the scattered nibble
 */
static uint8_t _rev4(uint32_t v)
{
    v &= 0xFu;
    return (uint8_t)(((v & 1u) << 3) | ((v >> 1) & 1u) << 2 |
                     ((v >> 2) & 1u) << 1 | ((v >> 3) & 1u));
}

/** @brief The port's arena bound: the glyph is dropped and the caller warned
 * instead of writing past the buffer.  The original writes through ES with no
 * bound; see deviation D-258.
 *
 * @param capacity the arena size in bytes
 * @param row the arena row
 * @param col the arena byte column
 */
static void _arena_overflow(uint32_t capacity, uint32_t row, uint32_t col)
{
    char msg[160];

    if (_arena_warned) return;
    _arena_warned = 1;
    snprintf(msg, sizeof msg,
             "font: glyph at arena row %u col %u does not fit the %u-byte "
             "arena; dropped (reported once)",
             row, col, capacity);
    if (g_osys && g_osys->warn) g_osys->warn(msg);
}

/** @brief _blit_glyph (1000:49FC) - one 16-row glyph into the arena.  `x` is the
 * screen-pixel column (the doubled scale), `row` the arena row (0..199).
 *
 * @param arena the text arena
 * @param capacity the arena size in bytes (the tile must fit whole)
 * @param x the x coordinate
 * @param row the row
 * @param gi the glyph index
 * @return 1 when the tile was written, 0 when it was dropped
 */
static int _blit_glyph(uint8_t *arena, uint32_t capacity, int32_t x,
                       uint32_t row, uint32_t gi)
{
    if (!_glyphs) return 0;

    uint8_t col = (uint8_t)((uint32_t)((x >= 0 ? x : 0) >> 2));
    uint32_t di = row * 80u + col;

    /* the tile's last write is row 15, byte di + 15*80 + 5 */
    if (di + 15u * 80u + 6u > capacity) {
        _arena_overflow(capacity, row, col);
        return 0;
    }
    uint16_t si = u16(gi * 32u); // the glyph stride (SHL CX,1 x5)
    uint8_t k = (uint8_t)((x >= 0 ? x : 0) & 3); // the x%4 pre-shift

    for (uint16_t r = 0; r < 16u; r++) {
        uint16_t ax = rd_le_u16(_glyphs + si);
        si += 2;
        uint8_t dh = 0;
        if (k) { // 4a2e..4a38
            ax = (uint16_t)((ax >> 8) | (ax << 8)); // XCHG AL,AH
            for (uint16_t i = 0; i < k; i++) {
                uint8_t cf = (uint8_t)(ax & 1u);
                ax = (uint16_t)(ax >> 1);
                dh = (uint8_t)((dh >> 1) | (cf << 7)); // RCR DH,1
            }
            ax = (uint16_t)((ax >> 8) | (ax << 8)); // XCHG AL,AH
        }
        uint8_t al = (uint8_t)(ax & 0xFFu);
        uint8_t ah = (uint8_t)(ax >> 8);

        /* the six arena writes, bits 3..0 of six consecutive bytes.  The
         * original writes through ES unbounded; the whole tile was checked
         * against `capacity` above (D-258). */
        arena[di] |= _rev4(al >> 4); // 4a3c..4a54
        arena[di + 1] |= _rev4(al); // 4a57..4a6b
        arena[di + 2] |= _rev4(ah >> 4); // 4a72..4a8e
        arena[di + 3] |= _rev4(ah); // 4a91..4aa5
        arena[di + 4] |= _rev4(dh >> 4); // 4aac..4ac8
        arena[di + 5] |= _rev4(dh); // 4acb..4adf
        di += 0x4Au + 6u; // the 80-byte row stride
    }
    return 1;
}

uint32_t font_text_width(const char *s)
{
    uint16_t w = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && *p != 0x0Du; p++)
        w += _advance[_charset_index(*p)];
    return w;
}

void font_draw_text(uint8_t *arena, const char *s, uint32_t centreX,
                    uint32_t row)
{ font_draw_text_bounded(arena, UINT32_MAX, s, centreX, row); }

void font_draw_text_bounded(uint8_t *arena, uint32_t capacity, const char *s,
                            uint32_t centreX, uint32_t row)
{
    if (!_glyphs || !arena) return;

    uint16_t width = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && *p != 0x0Du; p++)
        width += _advance[_charset_index(*p)];

    int32_t x = (int32_t)centreX - (width >> 1);

    for (const uint8_t *p = (const uint8_t *)s; *p && *p != 0x0Du; p++) {
        uint8_t gi = (uint8_t)_charset_index(*p);
        if (gi == 44u) continue; // no glyph: skip, no _advance
        _blit_glyph(arena, capacity, x, row, gi);
        x += _advance[gi];
    }
}
