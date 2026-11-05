/* map.c — menu_loop (1000:012A), the MAP screen after the ending, and the
 * high-score insert that runs at its top.
 *
 * BEHAVIOUR (re_formats.md §11, from the listing):
 *  - the screen opens on the running total `[0x592]/[0x594]`; `1000:01CD`
 *    walks the ten 18-byte entries at DS:0x4DE from the bottom up and drops the
 *    candidate in below the first it beats, shifting the rest down one slot.
 *    A total that beats none is discarded;
 *  - when it qualifies, the insert screen runs: a 30-cell grid (0x00..0x19 =
 *    'A'..'Z', 0x1A space, 0x1B '.', 0x1C delete, 0x1D enter) over a
 *    13-character buffer at DS:0x757 (the cursor is a 1-D strip: one press is
 *    0x0C units, 30 cells, `1000:040B`).  Letters also type directly - the
 *    `0x19 < (byte)(key + 0xBF)` branch maps 'A'..'Z' onto the same columns;
 *  - Enter commits 14 bytes into the inserted entry with 0x1A -> space and
 *    0x1B -> '.', then the screen shows "THE TOP TEN TRIBES" (DS:0x87B,
 *    `1000:0518`) and `draw_score_block` (`1000:0651`) lists the ten;
 *  - the table is saved on the way out (`1000:062B` `decrypt_region_38a`).
 *
 * RENDERING: the frame/band and the name wheel are the listing's art.  The
 * list is `draw_score_block`'s layout (rank/name/score centred at
 * 0x20/0x82/0x102, pitch 0x14 from base 0; header at 0xA0/0x14), the
 * qualifying total sees the seven-line congratulation page before the ring,
 * and the name screen has no `NAME:` line.  The page behind the text is
 * `lbm\high.bit` only: the band sheet is NOT a page-origin layer.  The live
 * capture shows the origin rows as high.bit's stone slab and the band
 * texture along the bottom via the animated strips, which are not ported
 * (port_deviations.md D-34, REQUIRES INVESTIGATION); the earlier origin fill
 * painted the slab with 0x0F noise.  The name-entry
 * transition follows 1000:028C..030A: five palette fades, the HIGH.BIT
 * re-copy and the two 1000:1251 outlines.  The six `[0x8A2]` positions are
 * not content panels: they are 0x14-frame scroll steps of the same band.
 *
 * Entry state: entry 0x151A sets [0xF7E]=0 and calls draw_status_indicator
 * (the disc-0 probe) before menu_loop, so the asset loads from disc 0.
 */
#include "map.h"
#include "font.h"
#include "hiscore.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "score.h"
#include "textpage.h"
#include "video.h"
#include "byteorder.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_PAGE   (320u * 200u)
#define NAME_MAX   13u
#define GRID_CELLS 30u

enum {
    MAP_CONGRATS,
    MAP_NAME_FADE,
    MAP_NAME,
    MAP_LIST,
    MAP_EXIT
};
struct MapFade {
    uint32_t first, entries, div, target;
};

/* 1000:024B..1000:0266: the qualifying page`s seven lines at DS:0x7BB..0x85C
 * (verbatim, from the image), drawn at x=0xA0, y=0x19 + 0x10*line with
 * blit_text_tile`s 0x20 mask. */
static const char *const _map_congrats[7] = {
    "WELL DONE!",
    "YOUR TRIBE HAS EVOLVED TO BECOME",
    "ONE OF THE TEN MOST ADVANCED IN",
    "THE ENTIRE WORLD!",
    "THE NAME OF YOUR TRIBE WILL NOW",
    "BE CARVED INTO THE ROCK THAT MARKS",
    "THE TEN MOST CIVILISED TRIBES!",
};

static uint8_t *_map_high;
static uint32_t _map_high_size;
static uint8_t *_map_font;
static uint8_t *_alpha_packed;     /* apl\alpha.spr, the 4bpp sheet        */
static uint8_t *_alpha_sheet;      /* its 4-plane expansion (64 KB)         */
static int32_t _map_up;
static int32_t _map_armed;  /* the first tick ignores the key that skipped the
                           * ending cutscene */
static int32_t _map_phase;
static char _map_name_store[NAME_MAX + 2u];
static char *_map_name = _map_name_store + 1u; /* one byte for 0x756's delete */
static uint32_t _map_name_len;
static uint32_t _map_col;
/* 1000:05F4/0613: a panel step is 0x14 frames, one [0x8AE] unit per frame. */
static int32_t _map_step; /* frames left in the current step */
static int32_t _map_step_dir; /* +1 = up (0x060C), -1 = down (0x05ED) */
/* 1000:0251's MOV CX,2 loop: the congratulation page is drawn twice, with
 * flip_page_swap (1000:87C7) between.  The port presents once per tick, so
 * the main loop's present after each map_tick is the flip equivalent. */
static uint32_t _map_congrats_draws;
/* 1000:028C..030A: the name-entry transition's fade/rectangle steps. */
static uint32_t _map_trans; /* 0..6; 6 = the ring is up */
static uint32_t _map_fade_starts; /* the palette_build_fade calls made */
static struct MapFade _map_fade_log[8];

/* --- the letter wheel: ALPHA.SPR + rotate_spinner_cell -------------------
 *
 * `menu_loop` loads `apl\alpha.spr` (`1000:01A2`, DS:0x793) into the scratch
 * and expands it with `expand_nibbles_4plane` (`1000:0705`) into a 64 KB,
 * four-plane buffer ([0x778]): 4096 source words per plane, each expanded to
 * four bytes, so a plane is 0x4000 bytes.  The expansion is a 4bpp bit
 * transpose: the word is rotated right by the plane number and its four
 * nibbles are written as four bytes (two from BL, two from BH).
 *
 * `rotate_spinner_cell` (`1000:0F93`) then places one of **30 letter cells**
 * on a ring.  With the angle `a` (0..359):
 *
 *   BP = 157 + (sin[a+90] * 66  >> 13) - 8      ; the x, in pixels
 *   C2 =  79 + (sin[a]    * 62  >> 13) - 8      ; the y, in quarter-rows
 *   dest = (C2 * 80) + 240 + (BP >> 2) + [0x5AC4]
 *   src  = cell*0x40 + 0x0C + (BP & 3)*0x4000
 *
 * `sin` is the 360-entry Q14 table at DS:0xB8 shifted right 14, so the radii
 * are 66 px and 62 rows: the ring is centred at px 156 / row 77, and the
 * `+0x0C` skips the file's 12-byte prologue.  The band table at DS:0xA8 is all
 * zeros in the image and nothing writes it, so `byte[0xA8 + (a+0x16)/45]` (the
 * `SHL AX,3` term) is zero.  Each cell is 16 rows x 4 bytes at a page stride of
 * 0x50 (80) - the map page has no overscan, unlike the level's 88.
 *
 * The source byte's four bits are the four pixels of its 4-px group (the plane
 * passes write the same byte to all four planes, so the bits agree); a set bit
 * is 0x0F and a clear one leaves the page untouched.  The `BP & 3` plane picks
 * which of the four pre-shifted copies of the letter is used - that is what
 * gives the ring its sub-pixel horizontal placement.
 */

/* the 360-entry Q14 sine table at DS:0xB8 (the image's exact values) */
static const int16_t _alpha_sin[360] = {
    0,      286,    572,    857,    1143,   1428,   1713,   1997,   2280,
    2563,   2845,   3126,   3406,   3686,   3964,   4240,   4516,   4790,
    5063,   5334,   5604,   5872,   6138,   6402,   6664,   6924,   7182,
    7438,   7692,   7943,   8192,   8438,   8682,   8923,   9162,   9397,
    9630,   9680,   10087,  10311,  10531,  10749,  10963,  11174,  11381,
    11585,  11786,  11982,  12176,  12365,  12551,  12733,  12911,  13085,
    13255,  13421,  13583,  13741,  13894,  14044,  14189,  14330,  14466,
    14598,  14726,  14849,  14962,  15082,  15191,  15296,  15396,  15491,
    15582,  15668,  15749,  15826,  15897,  15964,  16026,  16083,  16135,
    16182,  16225,  16262,  16294,  16322,  16344,  16362,  16374,  16382,
    16384,  16382,  16374,  16362,  16344,  16322,  16294,  16262,  16225,
    16182,  16135,  16083,  16026,  15964,  15897,  15826,  15749,  15668,
    15582,  15491,  15396,  15296,  15191,  15082,  14962,  14849,  14726,
    14598,  14466,  14330,  14189,  14044,  13894,  13741,  13583,  13421,
    13255,  13085,  12911,  12733,  12551,  12365,  12176,  11982,  11786,
    11585,  11381,  11174,  10963,  10749,  10531,  10311,  10087,  9860,
    9630,   9397,   9162,   8923,   8682,   8438,   8192,   7943,   7692,
    7438,   7182,   6924,   6664,   6402,   6138,   5872,   5604,   5334,
    5063,   4790,   4516,   4240,   3964,   3686,   3406,   3126,   2845,
    2563,   2280,   1997,   1713,   1428,   1143,   857,    572,    286,
    0,      -286,   -572,   -857,   -1143,  -1428,  -1713,  -1997,  -2280,
    -2563,  -2845,  -3126,  -3406,  -3686,  -3964,  -4240,  -4516,  -4790,
    -5063,  -5334,  -5604,  -5872,  -6138,  -6402,  -6664,  -6924,  -7182,
    -7438,  -7692,  -7943,  -8192,  -8438,  -8682,  -8923,  -9162,  -9397,
    -9630,  -9860,  -10087, -10311, -10531, -10749, -10963, -11174, -11381,
    -11585, -11786, -11982, -12176, -12365, -12551, -12733, -12911, -13085,
    -13255, -13421, -13583, -13741, -13894, -14044, -14189, -14330, -14466,
    -14598, -14726, -14849, -14962, -15082, -15191, -15296, -15396, -15491,
    -15582, -15668, -15749, -15826, -15897, -15964, -16026, -16083, -16135,
    -16182, -16225, -16262, -16294, -16322, -16344, -16362, -16374, -16382,
    -16384, -16382, -16374, -16362, -16344, -16322, -16294, -16262, -16225,
    -16182, -16135, -16083, -16026, -15964, -15897, -15826, -15746, -15668,
    -15582, -15491, -15396, -15296, -15191, -15082, -14962, -14849, -14726,
    -14598, -14466, -14330, -14189, -14044, -13894, -13741, -13583, -13421,
    -13255, -13085, -12911, -12733, -12551, -12365, -12176, -11982, -11786,
    -11585, -11381, -11174, -10963, -10749, -10531, -10311, -10087, -9860,
    -9630,  -9397,  -9162,  -8923,  -8682,  -8438,  -8192,  -7943,  -7692,
    -7438,  -7182,  -6924,  -6664,  -6402,  -6138,  -5872,  -5604,  -5334,
    -5063,  -4790,  -4516,  -4240,  -3964,  -3686,  -3406,  -3126,  -2845,
    -2563,  -2280,  -1997,  -1713,  -1428,  -1143,  -857,   -572,   -286,
};

/* --- the box frame: BAND.SPR, vga_write_masked_plane_copy, draw_box_edges --
 *
 * `menu_loop` loads `apl\band.spr` (`1000:01B7`, DS:0x7AE) into [0x776] and
 * immediately copies it with `vga_write_masked_plane_copy` (`1000:12D3`):
 * four plane passes, each copying **0x17C0 = 6080** bytes with the source
 * advancing **4** bytes per output byte, so the file's 24320 bytes fill a
 * 6080-byte band (76 rows of 320 px) four planes deep.
 *
 * `draw_box_edges` (`1000:07E6`) then draws the five box frames.  Its source
 * row is `[0x8AE]` and the sheet is exactly **304 rows of 80 bytes** = BAND's
 * size, which is why the entry animation can run `[0x8AE]` from `0x130` (304)
 * down to 2.  Each frame makes five `vga_blit_row` (`1000:0874`) calls:
 *
 *   dest   = { 0xE66, 0x14A6, 0x1AE6, 0x2126, 0x2766 }   (rows 46/66/86/106/126, col 6)
 *   mask   = { 0xA0,  0xB0,   0xE0,   0xF0,   0x30 }     (the GDC Bit Mask)
 *   source = [0x8AE]*80 + 6, plus 0x640 (20 rows) per call
 *
 * and `vga_blit_row` copies 14 iterations of `4 / +7 / 24 / +9 / 16 / +0xE`
 * MOVSB runs (74 destination bytes per iteration) with `Bit Mask = CH`, i.e.
 * each of the five rows writes only part of each byte's pixels and the five
 * together build the frame.
 *
 * The five frame blocks and `[0x8A2]` (1..6): each step is 0x14 frames of
 * `[0x8AE]` moving one unit per frame (`1000:05F4`/`0613`), so the band
 * scrolls through the fixed frame windows rather than changing contents.
 *
 * deviation: the original's destination is `A000:0xC000` in a four-plane page
 * the port does not model; the band is drawn as the same 76-row, 320-px layer
 * at the page origin, and the Bit Mask's bits 4..7 - which are pixels 0..3 of
 * a byte in this page's 4-px-per-byte layout - select which pixels of the
 * group a row writes.  The row positions, the run structure, the masks, the
 * source offsets and the animation are the listing's.
 */
static uint8_t *_band_sheet; /* apl\band.spr: 304 rows x 80 bytes      */

uint32_t g_map_box_bottom; /* [0x8AE]                                */
uint32_t g_map_panel; /** [0x8A2]: 1..6                          */

/**
 * @brief Accessor: the running score as one 32-bit value.
 *
 * @return The running score as one 32-bit value.
 */
static uint32_t _map_score(void)
{ return ((uint32_t)score_total_hi() << 16) | score_total_lo(); }

/**
 * @brief Rotate a word right by p positions (XCHG/SHR/XCHG).
 *
 * @param bx the x value
 * @param p the rotation count
 * @return the rotated word
 */
static uint16_t _alpha_ror16(uint16_t bx, uint32_t p)
{
    for (uint32_t r = 0; r < p; r++) bx = (uint16_t)((bx >> 1) | (bx << 15));
    return bx;
}

/**
 * @brief SHL AH / RCR AL over four steps; the high nibble of the result.
 *
 * @param ah the byte to rotate
 * @return the high nibble after the four rotates
 */
static uint8_t _alpha_hi_right(uint8_t ah)
{
    uint8_t al = 0;

    for (uint32_t k = 0; k < 4u; k++) {
        al = (uint8_t)((al >> 1) | ((ah & 0x80u) ? 0x80u : 0u));
        ah = (uint8_t)(ah << 1);
    }
    return (uint8_t)(al >> 4);
}

/**
 * @brief SHR AH / RCL AL over four steps; the low nibble of the result.
 *
 * @param ah the byte to rotate
 * @return the low nibble after the four rotates
 */
static uint8_t _alpha_lo_left(uint8_t ah)
{
    uint8_t al = 0;

    for (uint32_t k = 0; k < 4u; k++) {
        al = (uint8_t)((al << 1) | (ah & 1u));
        ah = (uint8_t)(ah >> 1);
    }
    return (uint8_t)(al & 0x0Fu);
}

/** @brief 1000:0705 expand_nibbles_4plane: the packed 4bpp sheet into four planes of
 * 0x4000 bytes (a literal port of the bit twiddling).
 *
 * @param src the source buffer
 * @param dst the destination buffer
 */
void alpha_expand(const uint8_t *src, uint8_t *dst)
{
    for (uint32_t p = 0; p < 4u; p++) {
        uint32_t si = 0, di = p * 0x4000u;

        for (uint32_t i = 0; i < 0x1000u; i++) {
            uint16_t bx = _alpha_ror16(rd_le_u16(src + si), p);

            si += 2u;
            dst[di++] = _alpha_hi_right((uint8_t)(bx & 0xFFu)); /* BL */
            dst[di++] = _alpha_lo_left((uint8_t)(bx & 0xFFu));
            /* MOV BL,BH */
            dst[di++] = _alpha_hi_right((uint8_t)((bx >> 8) & 0xFFu));
            dst[di++] = _alpha_lo_left((uint8_t)((bx >> 8) & 0xFFu));
        }
    }
}

/**
 * @brief Splat one nibble as four page pixels.
 *
 * @param page the page buffer
 * @param px the x position
 * @param py the y position
 * @param v the nibble
 */
static void _splat_four(uint8_t *page, uint32_t px, uint32_t py, uint32_t v)
{
    for (uint32_t bit = 0; bit < 4u; bit++)
        if ((v >> bit) & 1u) page[py * 320u + px + bit] = 0x0Fu;
}

/** @brief 1000:119F cos_scaled (angle + 0x5A, wrapped) / 1000:11AA sin_scaled:
 * CX + (sin[a] * DX) >> 14, with the negative half's extra `- DX*4` (the
 * listing's asymmetry, kept).  The 14 is the 14 `SHR DX,1`/`RCR AX,1` pairs
 * at 0x11B7..0x11ED and 0x11FC..0x1232.
 *
 * @param angle the rotation angle
 * @param cx the x value
 * @param dx the x delta
 * @param plus90 the angle plus 90 degrees
 * @return the scaled coordinate
 */
static uint32_t _alpha_rot(uint32_t angle, uint32_t cx, uint32_t dx,
                           int32_t plus90)
{
    uint32_t a = ((uint32_t)((int32_t)angle + (plus90 ? 0x5Au : 0)) % 0x168u);
    int16_t v = _alpha_sin[a];
    uint32_t p = (uint32_t)(uint16_t)v * (uint16_t)dx;
    uint32_t r = (uint32_t)((cx + (uint16_t)(p >> 14)) & 0xFFFFu);

    if (v < 0) r = (uint32_t)((r - dx * 4u) & 0xFFFFu);
    return r;
}

/** @brief 1000:0F93: the ring angle -> (destination byte, source offset).  The x is
 * `157 + 132*cos` px, the y `79 + 124*sin` quarter-rows; the page stride is 80
 * and the `+0xF0` adds three rows.
 *
 * @param cell the map cell
 * @param angle the rotation angle
 * @param src the source buffer
 * @param dst the destination buffer
 */
void alpha_cell(uint32_t cell, uint32_t angle, uint32_t *src, uint32_t *dst)
{
    uint32_t bp = (_alpha_rot(angle, 0x9Du, 0x42u, 1) - 8u) & 0xFFFFu;
    uint32_t c2 = (_alpha_rot(angle, 0x4Fu, 0x3Eu, 0) - 8u) & 0xFFFFu;

    *dst = (c2 * 80u + 240u + (bp >> 2)) & 0xFFFFu;
    *src = (cell * 0x40u + 0x0Cu + (bp & 3u) * 0x4000u) & 0xFFFFu;
}

/** @brief one 16x4-byte cell at page byte `dst`.  The four plane passes write the same
 * bytes to all four planes, so a byte's four bits are the four pixels of its
 * group: bit k set -> the pixel is 0x0F, clear -> untouched (transparent).
 *
 * @param page the page buffer
 * @param src the source buffer
 * @param dst the destination buffer
 */
static void _alpha_blit_cell(uint8_t *page, uint32_t src, uint32_t dst)
{
    for (uint32_t r = 0; r < 16u; r++) {
        uint32_t at = (dst + r * 80u) & 0xFFFFu;

        for (uint32_t c = 0; c < 4u; c++) {
            uint32_t v = _alpha_sheet[(src + r * 4u + c) & 0xFFFFu];
            uint32_t px = (at % 80u) * 4u + c * 4u;
            uint32_t py = at / 80u;

            if (!v || py >= 200u || px + 3u >= 320u) continue;
            _splat_four(page, px, py, v);
        }
    }
}

/**
 * @brief Draw the letter wheel.
 *
 * @param page the page buffer
 * @param base the base offset
 */
void alpha_draw_wheel(uint8_t *page, uint32_t base)
{
    if (!page || !_alpha_sheet) return;
    for (uint32_t i = 0; i < 30u; i++) {
        uint32_t src, dst;

        alpha_cell(i, (base + i * 0x0Cu) % 0x168u, &src, &dst);
        _alpha_blit_cell(page, src, dst);
    }
}

/** @brief 1000:0CBE init_display_lists' name pass: the typed characters at row 0x45,
 * 9 px apart, x centred as `-8 - ((len*9 - 1)/2 - 0xA0)`, each glyph drawn
 * through queue_dirty_job (0x0F8F): dest = y*80 + 0xF0 + x/4, source =
 * column*0x40 + 0xC + (x&3)*0x4000.
 *
 * @param page the page buffer
 * @param name the name
 */
void alpha_draw_name(uint8_t *page, const char *name)
{
    uint32_t len = 0, x;

    while (name && name[len]) len++;
    if (!page || !_alpha_sheet || !len) return;
    x = (uint32_t)(-8 - (int32_t)(((int32_t)(len * 9u) - 1) / 2 - 0xA0));
    for (uint32_t i = 0; i < len; i++) {
        uint32_t c = (uint8_t)name[i];
        uint32_t col;

        if (c == ' ') col = 0x1Au;
        else if (c == '.') col = 0x1Bu;
        else if (c >= 'A' && c <= 'Z') col = c - 'A';
        else {
            x += 9u;
            continue;
        }
        _alpha_blit_cell(page, col * 0x40u + 0x0Cu + (x & 3u) * 0x4000u,
                         0x45u * 80u + 0xF0u + (x >> 2));
        x += 9u;
    }
}

/**
 * @brief Write the masked four pixels of one sheet byte.
 *
 * @param page the page buffer
 * @param row the row
 * @param col the column
 * @param v the sheet byte
 * @param mask the mask
 */
static void _blit_four(uint8_t *page, uint32_t row, uint32_t col, uint32_t v,
                       uint32_t mask)
{
    for (uint32_t j = 0; j < 4u; j++)
        if (((mask >> (4u + j)) & 1u) && ((v >> j) & 1u))
            page[row * 320u + col * 4u + j] = 0x0Fu;
}

/**
 * @brief One blit-row pass: the three runs and their gaps.
 *
 * @param page the page buffer
 * @param d the destination cursor (in/out)
 * @param s the source cursor (in/out)
 * @param mask the mask
 */
static void _blit_runs(uint8_t *page, uint32_t *d, uint32_t *s, uint32_t mask)
{
    static const uint32_t runs[3] = { 4u, 31u, 15u };
    static const uint32_t gaps[3] = { 7u, 9u, 0x0Eu };

    for (uint32_t k = 0; k < 3u; k++) {
        for (uint32_t n = 0; n < runs[k]; n++) {
            uint32_t v = _band_sheet[(*s % 24320u)];
            uint32_t row = (*d / 80u), col = (*d % 80u);

            if (row < 200u) _blit_four(page, row, col, v, mask);
            (*d)++;
            (*s)++;
        }
        *d += gaps[k];
        *s += gaps[k];
    }
}

/**
 * @brief 1000:0874 vga_blit_row: 14 x (4 / +7 / 31 / +9 / 15 / +0xE) MOVSB runs.
 *
 * @param page the page buffer
 * @param dstByte the destination byte
 * @param srcByte the source byte
 * @param mask the mask
 */
static void _map_blit_row(uint8_t *page, uint32_t dstByte, uint32_t srcByte,
                          uint32_t mask)
{
    uint32_t d = dstByte, s = srcByte;

    for (uint32_t iter = 0; iter < 14u; iter++) _blit_runs(page, &d, &s, mask);
}

/**
 * @brief 1000:07E6: the five frame rows from sheet row `[0x8AE]`.
 *
 * @param page the page buffer
 * @param row the row
 */
void map_draw_box_edges(uint8_t *page, uint32_t row)
{
    static const uint32_t dstByte[5] = { 0xE66u, 0x14A6u, 0x1AE6u, 0x2126u,
                                         0x2766u };
    static const uint32_t mask[5] = { 0xA0u, 0xB0u, 0xE0u, 0xF0u, 0x30u };
    uint32_t src = row * 80u + 6u;

    if (!page || !_band_sheet) return;
    for (uint32_t i = 0; i < 5u; i++) {
        _map_blit_row(page, dstByte[i], src, mask[i]);
        src += 0x640u; /* ADD SI,0x640 */
    }
}

/** @brief draw_score_block (1000:0651): the rank is centred at x=0x20, the name at
 * x=0x82, the 32-bit score at x=0x102, row pitch [0x36]=0x14 from base
 * [0x38]=0; the header is blit_text_tile at x=0xA0, y=0x14.
 *
 * @param page the page buffer
 */
void map_draw_list(uint8_t *page)
{
    uint32_t row = 0u;

    textpage_draw_line_mask_at(page, "THE TOP TEN TRIBES", 0xA0u, 0x14u, 0x20u);
    for (uint32_t i = 0; i < hiscore_count(); i++) {
        char buf[24];

        snprintf(buf, sizeof buf, "%u", i + 1u);
        textpage_draw_line_mask_at(page, buf, 0x20u, row, 0x0Fu);
        textpage_draw_line_mask_at(page, hiscore_name(i), 0x82u, row, 0x0Fu);
        snprintf(buf, sizeof buf, "%" PRIu32, hiscore_score(i));
        textpage_draw_line_mask_at(page, buf, 0x102u, row, 0x0Fu);
        row += 0x14u;
    }
}

/**
 * @brief 1000:021E..1000:0266: the seven-line congratulation page.
 *
 * @param page the page buffer
 */
void map_draw_congrats(uint8_t *page)
{
    for (uint32_t i = 0; i < 7u; i++)
        textpage_draw_line_mask_at(page, _map_congrats[i], 0xA0u,
                                   0x19u + 0x10u * i, 0x20u);
}

/**
 * @brief Test/probe seam: 1 after the first draw, 2 after the flip's redraw.
 *
 * @return the draw pass (1 or 2)
 */
uint32_t map_congrats_draws(void) { return _map_congrats_draws; }

/** @brief 1000:1251: a rectangle outline in the flat page.  The top edge ORs 0x40,
 * the left and bottom 0x50, the right 0x40 (1000:1263..1298).
 *
 * @param page the page buffer
 * @param x the x coordinate
 * @param y the y coordinate
 * @param w the width
 * @param h the height
 */
void map_draw_rect(uint8_t *page, uint32_t x, uint32_t y, uint32_t w,
                   uint32_t h)
{
    if (!page) return;
    for (uint32_t i = 0; i < w; i++) {
        if (y < 200u && x + i < 320u) page[y * 320u + x + i] |= 0x40u;
        if (y + h < 200u && x + i < 320u) page[(y + h) * 320u + x + i] |= 0x50u;
    }
    for (uint32_t i = 0; i < h; i++) {
        if (y + i < 200u && x < 320u) page[(y + i) * 320u + x] |= 0x50u;
        if (y + i < 200u && x + w < 320u) page[(y + i) * 320u + x + w] |= 0x40u;
    }
}

/**
 * @brief Accessor: the MAP transition step.
 *
 * @return The MAP transition step.
 */
uint32_t map_transition_step(void) { return _map_trans; }
/**
 * @brief Accessor: the MAP fade's start count.
 *
 * @return The MAP fade's start count.
 */
uint32_t map_fade_starts(void) { return _map_fade_starts; }
/**
 * @brief Accessor: one MAP fade entry.
 *
 * @param i the index
 * @param first the first entry
 * @param entries the entry count
 * @param div the divisor
 * @param target the target value
 * @return One MAP fade entry.
 */
int32_t map_fade_at(uint32_t i, uint32_t *first, uint32_t *entries,
                    uint32_t *div, uint32_t *target)
{
    if (i >= _map_fade_starts || i >= 8u) return 0;
    if (first) *first = _map_fade_log[i].first;
    if (entries) *entries = _map_fade_log[i].entries;
    if (div) *div = _map_fade_log[i].div;
    if (target) *target = _map_fade_log[i].target;
    return 1;
}

/** @brief 1000:0273/028C/029B/02AD/02BF/02F5/0307: palette_build_fade with the
 * working palette's offset as the target (0x4BBC + N).
 *
 * @param first the first entry
 * @param entries the entry count
 * @param div the divisor
 * @param targetOff the target offset
 */
static void _map_build_fade(uint32_t first, uint32_t entries, uint32_t div,
                            uint32_t targetOff)
{
    palette_build_fade(first, entries, div, palette_working() + targetOff);
    if (_map_fade_starts < 8u) {
        _map_fade_log[_map_fade_starts].first = first;
        _map_fade_log[_map_fade_starts].entries = entries;
        _map_fade_log[_map_fade_starts].div = div;
        _map_fade_log[_map_fade_starts].target = targetOff;
    }
    _map_fade_starts++;
}

/**
 * @brief Draw the typed name ring.
 *
 * @param page the page buffer
 */
void map_draw_name(uint8_t *page)
{
    /* 1000:0CBE: the 30-cell ring from apl\alpha.spr, then the typed name in
     * the same glyph cells (row 0x45, 9 px apart).  The original draws no
     * `NAME:` label and no top-ten list on this screen. */
    alpha_draw_wheel(page, (uint32_t)(0x10Eu - _map_col * 0x0Cu) % 0x168u);
    alpha_draw_name(page, _map_name);
}

/** @brief 1000:02C5 / 1000:0B85: the page is reset to HIGH.BIT whenever the screen
 * changes (the congratulations page is not left under the name ring).
 *
 * @param page the page buffer
 */
static void _map_page_reset(uint8_t *page)
{
    memset(page, 0, MAP_PAGE);
    if (_map_high)
        memcpy(page, _map_high,
               _map_high_size < MAP_PAGE ? _map_high_size : MAP_PAGE);
}

/** @brief the whole screen: high.bit's page, then the state's own art and the
 * box frame (1000:07E6).
 */
static void _map_redraw(void)
{
    uint8_t *page = video_page(0);

    if (!page) return;
    if (_map_phase == MAP_CONGRATS) map_draw_congrats(page);
    else if (_map_phase == MAP_NAME) map_draw_name(page);
    else if (_map_phase == MAP_LIST) map_draw_list(page);
    if (_map_phase == MAP_LIST) map_draw_box_edges(page, g_map_box_bottom);
}

/**
 * @brief Open the MAP screen.
 *
 * @return 1 when the MAP screen opened, 0 otherwise
 */
int32_t map_begin(void)
{
    if (_map_up) return 1;

    res_set_disc(0u); /* entry 0x151A [0xF7E]=0 */
    _map_high = res_load_chunk("LBM\\HIGH.BIT", &_map_high_size, NULL);
    uint8_t *page = video_page(0);
    _map_page_reset(page);
    if (_map_high && _map_high_size >= MAP_PAGE + 768u) {
        palette_load_working(_map_high + MAP_PAGE); /* 1000:01C3/8D24 */
        palette_set_dac(_map_high + MAP_PAGE, 0, 256);
    }

    _map_font = res_load_chunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(_map_font);

    /* 1000:01A2..0x01AB: apl\alpha.spr expanded 4bpp -> four planes */
    free(_alpha_packed);
    _alpha_packed = NULL;
    free(_alpha_sheet);
    _alpha_sheet = NULL;
    _alpha_packed = res_load_chunk("APL\\ALPHA.SPR", NULL, NULL);
    if (_alpha_packed) {
        _alpha_sheet = calloc(1, 65536u);
        if (_alpha_sheet) alpha_expand(_alpha_packed, _alpha_sheet);
    }
    /* 1000:01B7: apl\band.spr, the frame's 304 x 80 sheet */
    free(_band_sheet);
    _band_sheet = NULL;
    _band_sheet = res_load_chunk("APL\\BAND.SPR", NULL, NULL);

    /* 1000:01CD: the running total is offered to the table.  A qualifying one
     * opens the name entry first, then the list. */
    _map_name[0] = 0;
    _map_name_len = 0u;
    _map_col = 0u;
    g_map_box_bottom = 0x130u; /* 1000:0518's [0x8AE] */
    g_map_panel = 0u; /* 0 = the entry animation */
    _map_step = 0;
    _map_step_dir = 0;
    /* 1000:01CD: a total that beats none leaves via 0x062B without the list;
     * a qualifying one gets the congratulation page, then the name entry. */
    _map_phase = hiscore_qualifies(_map_score()) ? MAP_CONGRATS : MAP_EXIT;
    if (_map_phase == MAP_CONGRATS) _map_congrats_draws = 1;
    _map_redraw(); /* 1000:024B: first pass */

    _map_up = 1;
    _map_armed = 0;
    return 1;
}

/**
 * @brief 1000:040B: one grid column applied to the 13-character buffer.
 *
 * @param column the column
 */
static void _map_apply(uint32_t column)
{
    uint8_t *page;

    if (hiscore_name_apply(_map_name, column, &_map_name_len)) {
        /* 0x1D: commit the 14-byte name and insert */
        hiscore_insert(_map_score(), _map_name);
        _map_phase = MAP_LIST;
    }
    page = video_page(0);
    if (!page) return;
    _map_page_reset(page);
    if (_map_phase == MAP_NAME) map_draw_name(page);
    else if (_map_phase == MAP_LIST) map_draw_list(page);
}

/**
 * @brief One MAP tick; 1 = still up.
 *
 * @param in the input state
 * @return 1 while the MAP screen is up
 */
int32_t map_tick(const InputState *in)
{
    if (!_map_up) return 0;
    if (!_map_armed) {
        _map_armed = 1;
        return 1;
    }

    if (_map_phase == MAP_EXIT) {
        _map_up = 0; /* 1000:062B: fade/save, no list */
        hiscore_write();
        free(_map_high);
        _map_high = NULL;
        free(_map_font);
        _map_font = NULL;
        free(_alpha_packed);
        _alpha_packed = NULL;
        free(_alpha_sheet);
        _alpha_sheet = NULL;
        free(_band_sheet);
        _band_sheet = NULL;
        font_publish(NULL);
        return 0;
    }
    if (_map_phase == MAP_CONGRATS) {
        if (_map_congrats_draws < 2u) {
            /* 1000:0251's second iteration; the present at the end of this
             * tick is flip_page_swap's equivalent in the one-page tick model. */
            _map_congrats_draws++;
            _map_redraw();
            if (_map_congrats_draws == 2u)
                _map_build_fade(0u, 0x40u, 0x28u, 0u); /* 1000:0273 */
            return 1;
        }
        if (in->lastKey || (in->joyBits & 0x80u)) {
            _map_phase = MAP_NAME_FADE; /* 1000:0285: the key wait */
            _map_trans = 0;
        }
        return 1;
    }
    if (_map_phase == MAP_NAME_FADE) {
        /* The order is 1000:028C..030A: wait the congrats fade, then three
         * fades, the HIGH.BIT re-copy + two rectangles, then two more fades. */
        switch (_map_trans) {
        case 0:
            if (palette_fade_active()) return 1;
            _map_build_fade(0x80u, 0x80u, 2u, 0x180u); /* 1000:028F */
            _map_trans = 1;
            return 1;
        case 1:
            if (palette_fade_active()) return 1;
            _map_build_fade(0x20u, 0x20u, 0x1Eu, 0x120u); /* 1000:02A1 */
            _map_trans = 2;
            return 1;
        case 2:
            if (palette_fade_active()) return 1;
            _map_build_fade(0x40u, 0x20u, 2u, 0x120u); /* 1000:02B3 */
            _map_trans = 3;
            return 1;
        case 3: {
            uint8_t *page = video_page(0);

            if (palette_fade_active()) return 1;
            if (page) {
                _map_page_reset(page); /* 1000:02C5 */
                map_draw_rect(page, 0x95u, 9u, 0xFu, 0xFu); /* 1000:02C8 */
                map_draw_rect(page, 0x60u, 0x45u, 0x77u, 0xFu); /* 1000:02D7 */
            }
            _map_build_fade(0x40u, 0x20u, 0x1Eu, 0xC0u); /* 1000:02E9 */
            _map_trans = 4;
            return 1;
        }
        case 4:
            if (palette_fade_active()) return 1;
            /* 1000:02FB/0307: the last fade starts and the ring goes up
             * without waiting for it (1000:030A clears [0x5AC8] and falls in). */
            _map_build_fade(0x20u, 0x20u, 0x46u, 0x60u);
            _map_trans = 6;
            _map_phase = MAP_NAME;
            _map_redraw();
            return 1;
        default: _map_phase = MAP_NAME; return 1;
        }
    }

    if (_map_phase == MAP_NAME) {
        uint32_t column = 0xFFu;

        if (in->joyBits & 0x04u) { /* left  */
            _map_col = (_map_col + GRID_CELLS - 1u) % GRID_CELLS;
            return 1;
        }
        if (in->joyBits & 0x08u) { /* right */
            _map_col = (_map_col + 1u) % GRID_CELLS;
            return 1;
        }
        if (in->joyBits & 0x80u) column = _map_col; /* fire selects the cell */
        if (in->lastKey) {
            uint32_t c = in->lastKey;

            if (c >= 'a' && c <= 'z') c -= 32u;
            if (c >= 'A' && c <= 'Z') column = c - 'A';
            else if (c == ' ') column = 0x1Au;
            else if (c == '.') column = 0x1Bu;
            else if (c == 8u || c == 0x7Fu) column = 0x1Cu; /* delete */
            else if (c == 13u || c == 10u) column = 0x1Du; /* enter  */
        }
        if (column != 0xFFu) _map_apply(column);
        return 1;
    }

    /* 1000:0518's entry animation: [0x8AE] runs from 0x130 down to 2, one step
     * per frame, so the five frame rows unroll; [0x8A2] becomes 1 when it ends
     * and the panel loop (0x05B9) takes over. */
    if (g_map_panel == 0u) {
        _map_redraw(); /* 1000:05A5: draw, wait, then SUB */
        if (g_map_box_bottom > 2u) g_map_box_bottom--;
        else g_map_panel = 1u;
        return 1;
    }
    /* 1000:05F4/0613: a panel step is 0x14 frames long, not instant.  Each
     * frame draws the frame and moves [0x8AE] by one; [0x8A2] changes only
     * after the 0x14th.  Up (bit 0x01) adds 1, down (bit 0x02) subtracts 1. */
    if (_map_step > 0) {
        _map_redraw(); /* 1000:05F8: draw, wait, then ADD/SUB */
        g_map_box_bottom =
            (uint32_t)((int32_t)g_map_box_bottom + _map_step_dir);
        if (--_map_step == 0)
            g_map_panel = (uint32_t)((int32_t)g_map_panel + _map_step_dir);
        return 1;
    }
    if (in->joyBits & 0x01u) {
        if (g_map_panel < 6u) {
            _map_step = 0x14;
            _map_step_dir = 1;
        }
        _map_redraw();
        return 1;
    }
    if (in->joyBits & 0x02u) {
        if (g_map_panel > 1u) {
            _map_step = 0x14;
            _map_step_dir = -1;
        }
        _map_redraw();
        return 1;
    }

    if (in->lastKey || (in->joyBits & 0x80u)) {
        _map_up = 0;
        hiscore_write(); /* 1000:062B decrypt_region_38a: the table is saved
                              * when menu_loop is left */
        free(_map_high);
        _map_high = NULL;
        free(_map_font);
        _map_font = NULL;
        free(_alpha_packed);
        _alpha_packed = NULL;
        free(_alpha_sheet);
        _alpha_sheet = NULL;
        free(_band_sheet);
        _band_sheet = NULL;
        font_publish(NULL);
        return 0;
    }
    return 1;
}
