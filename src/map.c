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
 * and the name screen has no `NAME:` line.  The band's destination is still
 * the port's page origin (port_deviations.md D-12).  The name-entry
 * transition follows 1000:028C..030A: five palette fades, the HIGH.BIT
 * re-copy and the two 1000:1251 outlines.  The six `[0x8A2]` positions are
 * not content panels: they are 0x14-frame scroll steps of the same band.
 *
 * Entry state: entry 0x151A sets [0xF7E]=0 and calls draw_status_indicator
 * (the disc-0 probe) before menu_loop, so the asset loads from disc 0.
 */
#include "game.h"
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
static const char *const mapCongrats[7] = {
    "WELL DONE!",
    "YOUR TRIBE HAS EVOLVED TO BECOME",
    "ONE OF THE TEN MOST ADVANCED IN",
    "THE ENTIRE WORLD!",
    "THE NAME OF YOUR TRIBE WILL NOW",
    "BE CARVED INTO THE ROCK THAT MARKS",
    "THE TEN MOST CIVILISED TRIBES!",
};

static uint8_t *mapHigh;
static uint32_t mapHighSize;
static uint8_t *mapFont;
static uint8_t *alphaPacked;     /* apl\alpha.spr, the 4bpp sheet        */
static uint8_t *alphaSheet;      /* its 4-plane expansion (64 KB)         */
static int32_t mapUp;
static int32_t mapArmed;  /* the first tick ignores the key that skipped the
                           * ending cutscene */
static int32_t mapPhase;
static char mapName[NAME_MAX + 1u];
static uint32_t mapNameLen;
static uint32_t mapCol;
/* 1000:05F4/0613: a panel step is 0x14 frames, one [0x8AE] unit per frame. */
static int32_t mapStep;      /* frames left in the current step */
static int32_t mapStepDir;   /* +1 = up (0x060C), -1 = down (0x05ED) */
/* 1000:0251's MOV CX,2 loop: the congratulation page is drawn twice, with
 * flip_page_swap (1000:87C7) between.  The port presents once per tick, so
 * the main loop's present after each map_tick is the flip equivalent. */
static uint32_t mapCongratsDraws;
/* 1000:028C..030A: the name-entry transition's fade/rectangle steps. */
static uint32_t mapTrans;        /* 0..6; 6 = the ring is up */
static uint32_t mapFadeStarts;   /* the palette_build_fade calls made */
static struct MapFade mapFadeLog[8];

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
static const int16_t alphaSin[360] = {
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
static uint8_t *bandSheet; /* apl\band.spr: 304 rows x 80 bytes      */

uint32_t map_boxBottom; /* [0x8AE]                                */
uint32_t map_panel; /* [0x8A2]: 1..6                          */

static uint32_t mapScore(void)
{ return ((uint32_t)score_totalHi() << 16) | score_totalLo(); }

/* 1000:0705 expand_nibbles_4plane: the packed 4bpp sheet into four planes of
 * 0x4000 bytes (a literal port of the bit twiddling). */
void alpha_expand(const uint8_t *src, uint8_t *dst)
{
    for (uint32_t p = 0; p < 4u; p++) {
        uint32_t si = 0, di = p * 0x4000u;

        for (uint32_t i = 0; i < 0x1000u; i++) {
            uint16_t bx = rd_le_u16(src + si);
            uint8_t al = 0, ah;

            si += 2u;
            for (uint32_t r = 0; r < p; r++)
                bx = (uint16_t)((bx >> 1) | (bx << 15)); /* XCHG/SHR/XCHG */
            ah = (uint8_t)(bx & 0xFFu); /* BL */
            for (uint32_t k = 0; k < 4u; k++) { /* SHL AH / RCR AL */
                al = (uint8_t)((al >> 1) | ((ah & 0x80u) ? 0x80u : 0u));
                ah = (uint8_t)(ah << 1);
            }
            dst[di++] = (uint8_t)(al >> 4);
            al = 0;
            ah = (uint8_t)(bx & 0xFFu);
            for (uint32_t k = 0; k < 4u; k++) { /* SHR AH / RCL AL */
                al = (uint8_t)((al << 1) | (ah & 1u));
                ah = (uint8_t)(ah >> 1);
            }
            dst[di++] = (uint8_t)(al & 0x0Fu);
            ah = (uint8_t)((bx >> 8) & 0xFFu); /* MOV BL,BH */
            al = 0;
            for (uint32_t k = 0; k < 4u; k++) {
                al = (uint8_t)((al >> 1) | ((ah & 0x80u) ? 0x80u : 0u));
                ah = (uint8_t)(ah << 1);
            }
            dst[di++] = (uint8_t)(al >> 4);
            al = 0;
            ah = (uint8_t)((bx >> 8) & 0xFFu);
            for (uint32_t k = 0; k < 4u; k++) {
                al = (uint8_t)((al << 1) | (ah & 1u));
                ah = (uint8_t)(ah >> 1);
            }
            dst[di++] = (uint8_t)(al & 0x0Fu);
        }
    }
}

/* 1000:119F sin_deg / 1000:11AA: CX + (sin[a] * DX) >> 13, with the negative
 * half's extra `- DX*4` (the listing's asymmetry, kept). */
static uint32_t alpha_rot(uint32_t angle, uint32_t cx, uint32_t dx,
                          int32_t plus90)
{
    uint32_t a = ((uint32_t)((int32_t)angle + (plus90 ? 0x5Au : 0)) % 0x168u);
    int16_t v = alphaSin[a];
    uint32_t p = (uint32_t)(uint16_t)v * (uint16_t)dx;
    uint32_t r = (uint32_t)((cx + (uint16_t)(p >> 14)) & 0xFFFFu);

    if (v < 0) r = (uint32_t)((r - dx * 4u) & 0xFFFFu);
    return r;
}

/* 1000:0F93: the ring angle -> (destination byte, source offset).  The x is
 * `157 + 132*cos` px, the y `79 + 124*sin` quarter-rows; the page stride is 80
 * and the `+0xF0` adds three rows. */
void alpha_cell(uint32_t cell, uint32_t angle, uint32_t *src, uint32_t *dst)
{
    uint32_t bp = (alpha_rot(angle, 0x9Du, 0x42u, 1) - 8u) & 0xFFFFu;
    uint32_t c2 = (alpha_rot(angle, 0x4Fu, 0x3Eu, 0) - 8u) & 0xFFFFu;

    *dst = (c2 * 80u + 240u + (bp >> 2)) & 0xFFFFu;
    *src = (cell * 0x40u + 0x0Cu + (bp & 3u) * 0x4000u) & 0xFFFFu;
}

/* one 16x4-byte cell at page byte `dst`.  The four plane passes write the same
 * bytes to all four planes, so a byte's four bits are the four pixels of its
 * group: bit k set -> the pixel is 0x0F, clear -> untouched (transparent). */
static void alpha_blitCell(uint8_t *page, uint32_t src, uint32_t dst)
{
    for (uint32_t r = 0; r < 16u; r++) {
        uint32_t at = (dst + r * 80u) & 0xFFFFu;

        for (uint32_t c = 0; c < 4u; c++) {
            uint32_t v = alphaSheet[(src + r * 4u + c) & 0xFFFFu];
            uint32_t px = (at % 80u) * 4u + c * 4u;
            uint32_t py = at / 80u;

            if (!v || py >= 200u || px + 3u >= 320u) continue;
            for (uint32_t bit = 0; bit < 4u; bit++)
                if ((v >> bit) & 1u) page[py * 320u + px + bit] = 0x0Fu;
        }
    }
}

void alpha_drawWheel(uint8_t *page, uint32_t base)
{
    if (!page || !alphaSheet) return;
    for (uint32_t i = 0; i < 30u; i++) {
        uint32_t src, dst;

        alpha_cell(i, (base + i * 0x0Cu) % 0x168u, &src, &dst);
        alpha_blitCell(page, src, dst);
    }
}

/* 1000:0CBE init_display_lists' name pass: the typed characters at row 0x45,
 * 9 px apart, x centred as `-8 - ((len*9 - 1)/2 - 0xA0)`, each glyph drawn
 * through queue_dirty_job (0x0F8F): dest = y*80 + 0xF0 + x/4, source =
 * column*0x40 + 0xC + (x&3)*0x4000. */
void alpha_drawName(uint8_t *page, const char *name)
{
    uint32_t len = 0, x;

    while (name && name[len]) len++;
    if (!page || !alphaSheet || !len) return;
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
        alpha_blitCell(page, col * 0x40u + 0x0Cu + (x & 3u) * 0x4000u,
                       0x45u * 80u + 0xF0u + (x >> 2));
        x += 9u;
    }
}

/* 1000:12D3: the 1:1 band copy (6080 bytes, four plane passes, source stride
 * 4).  The output is the 76-row band at the page origin. */
void map_bandFill(uint8_t *page)
{
    if (!page || !bandSheet) return;
    for (uint32_t i = 0; i < 6080u; i++) {
        uint32_t row = i / 80u, col = i % 80u;

        if (row >= 76u) break;
        for (uint32_t p = 0; p < 4u; p++) {
            uint32_t v = bandSheet[(i * 4u + p) & 0xFFFFu];

            for (uint32_t j = 0; j < 4u; j++)
                if ((v >> j) & 1u) page[row * 320u + col * 4u + j] = 0x0Fu;
        }
    }
}

/* 1000:0874 vga_blit_row: 14 x (4 / +7 / 24 / +9 / 16 / +0xE) MOVSB runs. */
static void map_blitRow(uint8_t *page, uint32_t dstByte, uint32_t srcByte,
                        uint32_t mask)
{
    static const uint32_t runs[3] = { 4u, 24u, 16u };
    static const uint32_t gaps[3] = { 7u, 9u, 0x0Eu };
    uint32_t d = dstByte, s = srcByte;

    for (uint32_t iter = 0; iter < 14u; iter++) {
        for (uint32_t k = 0; k < 3u; k++) {
            for (uint32_t n = 0; n < runs[k]; n++) {
                uint32_t v = bandSheet[(s % 24320u)];
                uint32_t row = (d / 80u), col = (d % 80u);

                if (row < 200u) {
                    for (uint32_t j = 0; j < 4u; j++)
                        if (((mask >> (4u + j)) & 1u) && ((v >> j) & 1u))
                            page[row * 320u + col * 4u + j] = 0x0Fu;
                }
                d++;
                s++;
            }
            d += gaps[k];
            s += gaps[k];
        }
    }
}

/* 1000:07E6: the five frame rows from sheet row `[0x8AE]`. */
void map_drawBoxEdges(uint8_t *page, uint32_t row)
{
    static const uint32_t dstByte[5] = { 0xE66u, 0x14A6u, 0x1AE6u, 0x2126u,
                                         0x2766u };
    static const uint32_t mask[5] = { 0xA0u, 0xB0u, 0xE0u, 0xF0u, 0x30u };
    uint32_t src = row * 80u + 6u;

    if (!page || !bandSheet) return;
    for (uint32_t i = 0; i < 5u; i++) {
        map_blitRow(page, dstByte[i], src, mask[i]);
        src += 0x640u; /* ADD SI,0x640 */
    }
}

/* draw_score_block (1000:0651): the rank is centred at x=0x20, the name at
 * x=0x82, the 32-bit score at x=0x102, row pitch [0x36]=0x14 from base
 * [0x38]=0; the header is blit_text_tile at x=0xA0, y=0x14. */
void map_drawList(uint8_t *page)
{
    uint32_t row = 0u;

    textpage_drawLineMaskAt(page, "THE TOP TEN TRIBES", 0xA0u, 0x14u, 0x20u);
    for (uint32_t i = 0; i < hiscore_count(); i++) {
        char buf[24];

        snprintf(buf, sizeof buf, "%u", i + 1u);
        textpage_drawLineMaskAt(page, buf, 0x20u, row, 0x0Fu);
        textpage_drawLineMaskAt(page, hiscore_name(i), 0x82u, row, 0x0Fu);
        snprintf(buf, sizeof buf, "%" PRIu32, hiscore_score(i));
        textpage_drawLineMaskAt(page, buf, 0x102u, row, 0x0Fu);
        row += 0x14u;
    }
}

/* 1000:021E..1000:0266: the seven-line congratulation page. */
void map_drawCongrats(uint8_t *page)
{
    for (uint32_t i = 0; i < 7u; i++)
        textpage_drawLineMaskAt(page, mapCongrats[i], 0xA0u, 0x19u + 0x10u * i,
                                0x20u);
}

/* Test/probe seam: 1 after the first draw, 2 after the flip's redraw. */
uint32_t map_congratsDraws(void) { return mapCongratsDraws; }

/* 1000:1251: a rectangle outline in the flat page.  The top edge ORs 0x40,
 * the left and bottom 0x50, the right 0x40 (1000:1263..1298). */
void map_drawRect(uint8_t *page, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
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

uint32_t map_transitionStep(void) { return mapTrans; }
uint32_t map_fadeStarts(void) { return mapFadeStarts; }
int32_t map_fadeAt(uint32_t i, uint32_t *first, uint32_t *entries,
                   uint32_t *div, uint32_t *target)
{
    if (i >= mapFadeStarts || i >= 8u) return 0;
    if (first) *first = mapFadeLog[i].first;
    if (entries) *entries = mapFadeLog[i].entries;
    if (div) *div = mapFadeLog[i].div;
    if (target) *target = mapFadeLog[i].target;
    return 1;
}

/* 1000:0273/028C/029B/02AD/02BF/02F5/0307: palette_build_fade with the
 * working palette's offset as the target (0x4BBC + N). */
static void map_buildFade(uint32_t first, uint32_t entries, uint32_t div,
                          uint32_t targetOff)
{
    palette_buildFade(first, entries, div, palette_working() + targetOff);
    if (mapFadeStarts < 8u) {
        mapFadeLog[mapFadeStarts].first = first;
        mapFadeLog[mapFadeStarts].entries = entries;
        mapFadeLog[mapFadeStarts].div = div;
        mapFadeLog[mapFadeStarts].target = targetOff;
    }
    mapFadeStarts++;
}

void map_drawName(uint8_t *page)
{
    /* 1000:0CBE: the 30-cell ring from apl\alpha.spr, then the typed name in
     * the same glyph cells (row 0x45, 9 px apart).  The original draws no
     * `NAME:` label and no top-ten list on this screen. */
    alpha_drawWheel(page, (uint32_t)(0x10Eu - mapCol * 0x0Cu) % 0x168u);
    alpha_drawName(page, mapName);
}

/* 1000:02C5 / 1000:0B85: the page is reset to HIGH.BIT whenever the screen
 * changes (the congratulations page is not left under the name ring). */
static void map_pageReset(uint8_t *page)
{
    memset(page, 0, MAP_PAGE);
    if (mapHigh)
        memcpy(page, mapHigh, mapHighSize < MAP_PAGE ? mapHighSize : MAP_PAGE);
}

/* the whole screen: the band layer (1000:12D3), the card, then the state's
 * own art and the box frame (1000:07E6). */
static void map_redraw(void)
{
    uint8_t *page = video_page(0);

    if (!page) return;
    if (mapPhase == MAP_CONGRATS) map_drawCongrats(page);
    else if (mapPhase == MAP_NAME) map_drawName(page);
    else if (mapPhase == MAP_LIST) map_drawList(page);
    if (mapPhase == MAP_LIST) map_drawBoxEdges(page, map_boxBottom);
}

int32_t map_begin(void)
{
    if (mapUp) return 1;

    res_setDisc(0u); /* entry 0x151A [0xF7E]=0 */
    mapHigh = res_loadChunk("LBM\\HIGH.BIT", &mapHighSize, NULL);
    uint8_t *page = video_page(0);
    map_pageReset(page);
    if (mapHigh && mapHighSize >= MAP_PAGE + 768u) {
        palette_loadWorking(mapHigh + MAP_PAGE); /* 1000:01C3/8D24 */
        palette_setDac(mapHigh + MAP_PAGE, 0, 256);
    }

    mapFont = res_loadChunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(mapFont);

    /* 1000:01A2..0x01AB: apl\alpha.spr expanded 4bpp -> four planes */
    free(alphaPacked);
    alphaPacked = NULL;
    free(alphaSheet);
    alphaSheet = NULL;
    alphaPacked = res_loadChunk("APL\\ALPHA.SPR", NULL, NULL);
    if (alphaPacked) {
        alphaSheet = calloc(1, 65536u);
        if (alphaSheet) alpha_expand(alphaPacked, alphaSheet);
    }
    /* 1000:01B7: apl\band.spr, the frame's 304 x 80 sheet */
    free(bandSheet);
    bandSheet = NULL;
    bandSheet = res_loadChunk("APL\\BAND.SPR", NULL, NULL);

    /* 1000:01CD: the running total is offered to the table.  A qualifying one
     * opens the name entry first, then the list. */
    mapName[0] = 0;
    mapNameLen = 0u;
    mapCol = 0u;
    map_boxBottom = 0x130u; /* 1000:0518's [0x8AE] */
    map_panel = 0u; /* 0 = the entry animation */
    mapStep = 0;
    mapStepDir = 0;
    /* 1000:01CD: a total that beats none leaves via 0x062B without the list;
     * a qualifying one gets the congratulation page, then the name entry. */
    mapPhase = hiscore_qualifies(mapScore()) ? MAP_CONGRATS : MAP_EXIT;
    map_bandFill(page); /* 1000:12D3 */
    if (mapPhase == MAP_CONGRATS) mapCongratsDraws = 1;
    map_redraw(); /* 1000:024B: first pass */

    mapUp = 1;
    mapArmed = 0;
    return 1;
}

/* 1000:040B: one grid column applied to the 13-character buffer. */
static void map_apply(uint32_t column)
{
    uint8_t *page;

    if (hiscore_nameApply(mapName, column, &mapNameLen)) {
        /* 0x1D: commit the 14-byte name and insert */
        hiscore_insert(mapScore(), mapName);
        mapPhase = MAP_LIST;
    }
    page = video_page(0);
    if (!page) return;
    map_pageReset(page);
    if (mapPhase == MAP_NAME) map_drawName(page);
    else if (mapPhase == MAP_LIST) map_drawList(page);
}

int32_t map_tick(const InputState *in)
{
    if (!mapUp) return 0;
    if (!mapArmed) {
        mapArmed = 1;
        return 1;
    }

    if (mapPhase == MAP_EXIT) {
        mapUp = 0; /* 1000:062B: fade/save, no list */
        hiscore_write();
        free(mapHigh);
        mapHigh = NULL;
        free(mapFont);
        mapFont = NULL;
        free(alphaPacked);
        alphaPacked = NULL;
        free(alphaSheet);
        alphaSheet = NULL;
        free(bandSheet);
        bandSheet = NULL;
        font_publish(NULL);
        return 0;
    }
    if (mapPhase == MAP_CONGRATS) {
        if (mapCongratsDraws < 2u) {
            /* 1000:0251's second iteration; the present at the end of this
             * tick is flip_page_swap's equivalent in the one-page tick model. */
            mapCongratsDraws++;
            map_redraw();
            if (mapCongratsDraws == 2u)
                map_buildFade(0u, 0x40u, 0x28u, 0u); /* 1000:0273 */
            return 1;
        }
        if (in->lastKey || (in->joyBits & 0x80u)) {
            mapPhase = MAP_NAME_FADE; /* 1000:0285: the key wait */
            mapTrans = 0;
        }
        return 1;
    }
    if (mapPhase == MAP_NAME_FADE) {
        /* The order is 1000:028C..030A: wait the congrats fade, then three
         * fades, the HIGH.BIT re-copy + two rectangles, then two more fades. */
        switch (mapTrans) {
        case 0:
            if (palette_fadeActive()) return 1;
            map_buildFade(0x80u, 0x80u, 2u, 0x180u); /* 1000:028F */
            mapTrans = 1;
            return 1;
        case 1:
            if (palette_fadeActive()) return 1;
            map_buildFade(0x20u, 0x20u, 0x1Eu, 0x120u); /* 1000:02A1 */
            mapTrans = 2;
            return 1;
        case 2:
            if (palette_fadeActive()) return 1;
            map_buildFade(0x40u, 0x20u, 2u, 0x120u); /* 1000:02B3 */
            mapTrans = 3;
            return 1;
        case 3: {
            uint8_t *page = video_page(0);

            if (palette_fadeActive()) return 1;
            if (page) {
                map_pageReset(page); /* 1000:02C5 */
                map_drawRect(page, 0x95u, 9u, 0xFu, 0xFu); /* 1000:02C8 */
                map_drawRect(page, 0x60u, 0x45u, 0x77u, 0xFu); /* 1000:02D7 */
            }
            map_buildFade(0x40u, 0x20u, 0x1Eu, 0xC0u); /* 1000:02E9 */
            mapTrans = 4;
            return 1;
        }
        case 4:
            if (palette_fadeActive()) return 1;
            /* 1000:02FB/0307: the last fade starts and the ring goes up
             * without waiting for it (1000:030A clears [0x5AC8] and falls in). */
            map_buildFade(0x20u, 0x20u, 0x46u, 0x60u);
            mapTrans = 6;
            mapPhase = MAP_NAME;
            map_redraw();
            return 1;
        default: mapPhase = MAP_NAME; return 1;
        }
    }

    if (mapPhase == MAP_NAME) {
        uint32_t column = 0xFFu;

        if (in->joyBits & 0x04u) { /* left  */
            mapCol = (mapCol + GRID_CELLS - 1u) % GRID_CELLS;
            return 1;
        }
        if (in->joyBits & 0x08u) { /* right */
            mapCol = (mapCol + 1u) % GRID_CELLS;
            return 1;
        }
        if (in->joyBits & 0x80u) column = mapCol; /* fire selects the cell */
        if (in->lastKey) {
            uint32_t c = in->lastKey;

            if (c >= 'a' && c <= 'z') c -= 32u;
            if (c >= 'A' && c <= 'Z') column = c - 'A';
            else if (c == ' ') column = 0x1Au;
            else if (c == '.') column = 0x1Bu;
            else if (c == 8u || c == 0x7Fu) column = 0x1Cu; /* delete */
            else if (c == 13u || c == 10u) column = 0x1Du; /* enter  */
        }
        if (column != 0xFFu) map_apply(column);
        return 1;
    }

    /* 1000:0518's entry animation: [0x8AE] runs from 0x130 down to 2, one step
     * per frame, so the five frame rows unroll; [0x8A2] becomes 1 when it ends
     * and the panel loop (0x05B9) takes over. */
    if (map_panel == 0u) {
        map_redraw(); /* 1000:05A5: draw, wait, then SUB */
        if (map_boxBottom > 2u) map_boxBottom--;
        else map_panel = 1u;
        return 1;
    }
    /* 1000:05F4/0613: a panel step is 0x14 frames long, not instant.  Each
     * frame draws the frame and moves [0x8AE] by one; [0x8A2] changes only
     * after the 0x14th.  Up (bit 0x01) adds 1, down (bit 0x02) subtracts 1. */
    if (mapStep > 0) {
        map_redraw(); /* 1000:05F8: draw, wait, then ADD/SUB */
        map_boxBottom = (uint32_t)((int32_t)map_boxBottom + mapStepDir);
        if (--mapStep == 0)
            map_panel = (uint32_t)((int32_t)map_panel + mapStepDir);
        return 1;
    }
    if (in->joyBits & 0x01u) {
        if (map_panel < 6u) {
            mapStep = 0x14;
            mapStepDir = 1;
        }
        map_redraw();
        return 1;
    }
    if (in->joyBits & 0x02u) {
        if (map_panel > 1u) {
            mapStep = 0x14;
            mapStepDir = -1;
        }
        map_redraw();
        return 1;
    }

    if (in->lastKey || (in->joyBits & 0x80u)) {
        mapUp = 0;
        hiscore_write(); /* 1000:062B decrypt_region_38a: the table is saved
                              * when menu_loop is left */
        free(mapHigh);
        mapHigh = NULL;
        free(mapFont);
        mapFont = NULL;
        free(alphaPacked);
        alphaPacked = NULL;
        free(alphaSheet);
        alphaSheet = NULL;
        free(bandSheet);
        bandSheet = NULL;
        font_publish(NULL);
        return 0;
    }
    return 1;
}
