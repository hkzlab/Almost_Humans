/* font.c — TEXT.SPR _glyphs + draw_text (game_structure.md §1 subsystem 12,
 * re_formats.md §`TEXT.SPR` + §9.4 "The attract text arena").
 *
 * The shipped tables, extracted from the EXE image (file 0x106F0 + off):
 *   the _charset at 202F:0x1E28 — 44 _glyphs, then the NUL terminator;
 *   the per-glyph advances at 202F:0x1E55 — 44 entries, and the byte at
 *   0x1E81 (= 0x50) is the terminator's "_advance" (draw_text's no-match
 *   path stops at the terminator and reads its slot — 1000:49ad).
 *
 * draw_text (1000:4989) measures the string through the _advance table,
 * centres it (AX -= width/2) and blits each glyph through _blit_glyph
 * (1000:49FC) into the caller's arena at row*80 + x/4.  _blit_glyph's
 * nibble scatter, verified bit-exact against the shipped font
 * (tools/attract_text_sim.py; render analysis/spr_renders/attract_sim_arena.png):
 * each source byte's high nibble, 4-bit-reversed, ORs into arena byte
 * [DI], its low nibble into [DI+1], the second row byte's nibbles into
 * [DI+2]/[DI+3], and the two nibbles of DH — the bits the x%4 pre-shift
 * (XCHG / SHR AX,1 / RCR DH,1) rotated out of the row word — into
 * [DI+4]/[DI+5].  With the arena's 1-bpp packing (4 px per byte, bit 0 =
 * the leftmost), the net effect is that every 2-bpp glyph pixel becomes
 * two consecutive 1-bpp pixels: the glyph doubles horizontally, which is
 * what the _advance table's "_advance ~= 2*ink + 1" encodes.
 */
#include "font.h"
#include "osystem.h"
#include "byteorder.h"

/* 202F:0x1E28 — the _charset (44 _glyphs + the terminator). */
static const char _charset[45] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!., '?ae";

/* 202F:0x1E55 — the advances, then the terminator slot's 0x50. */
static const uint8_t _advance[45] = { 7, 8, 8, 8,  7, 7, 8, 8, 3,   7, 8,  7,
                                      9, 8, 8, 7,  8, 7, 8, 9, 7,   8, 11, 8,
                                      7, 9, 7, 7,  8, 8, 7, 8, 7,   7, 8,  7,
                                      3, 3, 3, 10, 3, 7, 7, 7, 0x50 };

/* [0x1E1C] — the published apl\text.spr (loaded by the attract's
 * game_loop prologue and the front-end later; _blit_glyph's only source). */
static const uint8_t *_glyphs;

/** @brief Bind the font module to the backend. */
int32_t font_init(const OSystem *osys)
{
    (void)osys;
    return 0;
}

/** @brief Publish the TEXT.SPR glyph bank. */
void font_publish(const uint8_t *textSpr) { _glyphs = textSpr; }

/** The _charset lookup: the match index, or 44 (the terminator slot) when
 * the character is not in the table (draw_text's scan stops there). */
static uint32_t _charset_index(uint8_t c)
{
    for (uint32_t i = 0; i < 44u; i++)
        if ((uint8_t)_charset[i] == c) return i;
    return 44u;
}

/** The nibble scatter's net effect (the 4x SHL/RCR + 4x SHR of 1000:4a3c,
 * and the 4x SHR/RCL of 1000:4a5b): the nibble, 4-bit-reversed. */
static uint8_t _rev4(uint32_t v)
{
    v &= 0xFu;
    return (uint8_t)(((v & 1u) << 3) | ((v >> 1) & 1u) << 2 |
                     ((v >> 2) & 1u) << 1 | ((v >> 3) & 1u));
}

/** _blit_glyph (1000:49FC) — one 16-row glyph into the arena.  `x` is the
 * screen-pixel column (the doubled scale), `row` the arena row (0..199). */
static void _blit_glyph(uint8_t *arena, int32_t x, uint32_t row, uint32_t gi)
{
    if (!_glyphs) return;

    uint32_t di = row * 80u + (uint32_t)((x >= 0 ? x : 0) >> 2);
    uint32_t si = gi * 32u; /* the glyph stride (SHL CX,1 x5) */
    uint32_t k = (uint32_t)(x >= 0 ? x : 0) & 3u; /* the x%4 pre-shift */

    for (uint32_t r = 0; r < 16u; r++) {
        uint16_t ax = rd_le_u16(_glyphs + si);
        si += 2;
        uint8_t dh = 0;
        if (k) { /* 4a2e..4a38 */
            ax = (uint16_t)((ax >> 8) | (ax << 8)); /* XCHG AL,AH */
            for (uint32_t i = 0; i < k; i++) {
                uint8_t cf = (uint8_t)(ax & 1u);
                ax = (uint16_t)(ax >> 1);
                dh = (uint8_t)((dh >> 1) | (cf << 7)); /* RCR DH,1 */
            }
            ax = (uint16_t)((ax >> 8) | (ax << 8)); /* XCHG AL,AH */
        }
        uint8_t al = (uint8_t)(ax & 0xFFu);
        uint8_t ah = (uint8_t)(ax >> 8);

        /* the six arena writes, bits 3..0 of six consecutive bytes.  No
         * arena bound: the original writes through ES unbounded, and the
         * credits strip's tiles live far past effect 2's 200-row arena
         * (the callers keep the writes in their buffer's range). */
        arena[di] |= _rev4(al >> 4); /* 4a3c..4a54  */
        arena[di + 1] |= _rev4(al); /* 4a57..4a6b  */
        arena[di + 2] |= _rev4(ah >> 4); /* 4a72..4a8e  */
        arena[di + 3] |= _rev4(ah); /* 4a91..4aa5  */
        arena[di + 4] |= _rev4(dh >> 4); /* 4aac..4ac8  */
        arena[di + 5] |= _rev4(dh); /* 4acb..4adf  */
        di += 0x4Au + 6u; /* the 80-byte row stride */
    }
}

/** draw_text's own measure: the sum of the _advance table (the same walk
 * draw_text does before centring).  STOPs at CR/NUL, like draw_text. */
uint32_t font_text_width(const char *s)
{
    uint32_t w = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && *p != 0x0Du; p++)
        w += _advance[_charset_index(*p)];
    return w;
}

/** draw_text (1000:4989): centre `s` on centreX and blit it at arena row
 * `row`.  Unknown characters _advance by the terminator slot but draw
 * nothing (the two search loops' no-match paths). */
void font_draw_text(uint8_t *arena, const char *s, uint32_t centreX,
                    uint32_t row)
{
    if (!_glyphs || !arena) return;

    uint32_t width = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && *p != 0x0Du; p++)
        width += _advance[_charset_index(*p)];

    int32_t x = (int32_t)centreX - (int32_t)(width >> 1);

    for (const uint8_t *p = (const uint8_t *)s; *p && *p != 0x0Du; p++) {
        uint32_t gi = _charset_index(*p);
        if (gi == 44u) continue; /* no glyph: skip, no _advance */
        _blit_glyph(arena, x, row, gi);
        x += _advance[gi];
    }
}
