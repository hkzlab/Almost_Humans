#include "audio.h"
#include "map.h"
#include "font.h"
#include "game_strs.h"
#include "hiscore.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "score.h"
#include "textpage.h"
#include "video.h"
#include "byteorder.h"
#include "width.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAP_PAGE     VIDEO_PAGE_BYTES
#define ALPHA_WINDOW 0x2000u // 1000:0705's fixed 4096-word source window
#define NAME_MAX     13u
#define GRID_CELLS   30u

enum {
    MAP_CONGRATS,
    MAP_NAME_FADE,
    MAP_NAME,
    MAP_ENTER, // 1000:049A..0x0516: the spin + white fade after Enter
    MAP_LIST_HEADER,
    MAP_LIST,
    MAP_EXIT, // 1000:062B: fade all 256 entries to black, hold, save
    MAP_EXIT_FADE
};
struct MapFade {
    uint16_t first, entries, div, target;
};

/* 1000:024B..1000:0266: the qualifying page's seven lines at DS:0x7BB..0x85C
 * (verbatim from the image), drawn at x=0xA0, y=0x19 + 0x10*line with
 * blit_text_tile's 0x20 mask.  The lines are game_strs' MAP_CONGRATS block. */

static uint8_t *_map_high;
static uint32_t _map_high_size;
static uint8_t *_map_font;
static uint8_t *_alpha_packed;     // apl\alpha.spr, the 4bpp sheet
static uint8_t *_alpha_sheet;      // its 4-plane expansion (64 KB)
static uint8_t _map_up;
static uint8_t _map_armed;  /* the first tick ignores the key that skipped the
                           * ending cutscene */
static uint8_t _map_phase;
static char _map_name_store[NAME_MAX + 2u];
static char *_map_name = _map_name_store + 1u; // one byte for 0x756's delete
static uint8_t _map_name_len;
static uint8_t _map_col;
/* 1000:03FB/0x0405: the fire append waits for the ISR tick counter [0x5AC8]
 * to reach 0x23 and resets it; the name screen arms it at entry (0x030A). */
static uint16_t _map_fire_wait;
/* 1000:05F4/0613: a panel step is 0x14 frames, one [0x8AE] unit per frame. */
static uint8_t _map_step; // frames left in the current step
static int8_t _map_step_dir; // +1 = up (0x060C), -1 = down (0x05ED)
/* 1000:0251's MOV CX,2 loop: the congratulation page is drawn twice, with
 * flip_page_swap (1000:87C7) between.  The port presents once per tick, so
 * the main loop's present after each map_tick is the flip equivalent. */
static uint8_t _map_congrats_draws;
/* 1000:028C..030A: the name-entry transition's fade/rectangle steps. */
static uint8_t _map_trans; // 0..6; 6 = the ring is up
#define MAP_FADE_LOG 16u
static uint8_t _map_fade_starts; // the palette_build_fade calls made
static struct MapFade _map_fade_log[MAP_FADE_LOG];
/* the 0x48BC ramp: zeroed = black, the opening's 0x0239 target. */
static const uint8_t _map_black[768];

/* 1000:021E..0x0285: the opening's fade/animate steps. */
#define MAP_INTRO_WAIT 4u
static uint8_t _map_intro;
/* 1000:0233's 0x8C palette_animate frames, one per tick (each presents). */
static uint8_t _map_intro_frames;
/* the top-ten list's 0x0518 header frames (the 15 outlines) before 0x0577. */
static uint8_t _map_list_step;
/* 1000:049A..0x0516: the Enter spin, the white ramp and the two fades. */
static uint8_t _map_enter_phase; // 0 = to white, 1 = back to working
static uint16_t _map_enter_bx; // the 0x04B9/0x0515 counter
static uint8_t _map_white[0xC0]; // the 0x48BC ramp during the spin
/* 1000:0577/0x0595: the offscreen list page (cleared, then the rows).  It is
 * the pool's head, [0x8A4] (menu_loop 1000:013B), a heap segment - not a VGA
 * page - so it is allocated with the screen and released by _map_close. */
static uint8_t *_map_off;
/* 1000:063A/0x063D: the 0x4B-frame hold after the exit fade (present_frames_75). */
static uint8_t _map_exit_hold;
static uint8_t _map_music_fade;

/* 1000:048/0x04A: the ring angle and its per-draw step.  `_map_col` is the
 * cell index derived from the angle (`(0x10E - angle) / 0x0C`). */
static uint16_t _map_angle;
static uint8_t _map_angle_step;
static uint8_t _map_spin; // frames left in the entry spin (20 + 3)
/* 1000:03B5..0x03F8: a left/right press is four 3-degree draws plus the three
 * 0x0330 holds, not one 12-degree jump. */
static uint8_t _map_move_steps;
static int8_t _map_move_dir; // -1 = left (0x03DA), +1 = right (0x03B5)
static uint8_t _map_move_hold;
/* 1000:0368 clears [0x5ACE]: after a typed key the action bit is swallowed
 * until the key is released (Z is the shipped action key). */
static uint8_t _map_fire_lock;
/* [0x48A3]: the ISR's last-key latch; the menu takes it once, so a key that
 * lands during the spin/holds is not lost and a held key does not repeat. */
static uint8_t _map_pending_key;

/* 1000:0CBE's display list at [0x596]: {target x,y, current x,y}. */
struct MapGlyph {
    int16_t tx, ty;
    int16_t cx, cy;
};
static struct MapGlyph _map_glyphs[NAME_MAX];

/* 1000:0A1A/0A1C/0A1E: the double-buffered cell list.  Each entry is the
 * mode-X byte address of one 11-row x 4-byte cell; 0x0BB0 clears the previous
 * frame's list (bit 5 of each pixel), then 0x0CBE swaps the two. */
#define MAP_DIRTY_MAX 64u
static uint16_t _map_dirty[2][MAP_DIRTY_MAX];
static uint8_t _map_dirty_n[2];
static uint8_t _map_dirty_cur;
static uint8_t _map_dirty_prev;

/** @brief 1000:0B85: the page is reset to HIGH.BIT.
 *
 * @param page the page buffer
 */
static void _map_page_copy(uint8_t *page);

/** @brief 1000:040B..0x0D76: the ring cell at an angle, `(0x10E-angle)/0x0C`.
 *
 * @return the cell index (0..29)
 */
static uint8_t _map_col_from_angle(void);

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
 *   BP = 157 + (sin[a+90] * 66  >> 14) - 8      ; the x, in pixels
 *   C2 =  79 + (sin[a]    * 62  >> 14) - 8      ; the y, in quarter-rows
 *   dest = (C2 * 80) + 240 + (BP >> 2) + [0x5AC4]
 *   src  = cell*0x40 + 0x0C + (BP & 3)*0x4000
 *
 * `sin` is the 360-entry Q14 table at DS:0xB8 shifted right 14, so the radii
 * are 66 px and 62 rows: the ring is centred at px 156 / row 77, and the
 * `+0x0C` skips the first 3 rows of each 0x40-byte cell (1000:0F8F draws 11
 * rows = 44 MOVSBs).  The band table at DS:0xA8 is all zeros in the image and
 * nothing writes it, so `byte[0xA8 + (a+0x16)/45]` (the `SHL AX,3` term) is
 * zero.  Each cell is 11 rows x 4 bytes at a page stride of 0x50 (80) - the
 * map page has no overscan, unlike the level's 88.
 *
 * The source byte's four bits are the four pixels of its 4-px group (the plane
 * passes write the same byte to all four planes, so the bits agree).
 * `1000:0F8F` sets write mode 2 + Data Rotate OR + Bit Mask 0x20, so a set bit
 * ORs 0x20 into its pixel (the 0x20 colour bank) and a clear one leaves the
 * page untouched; `1000:0BB0` clears bit 5 of the previous frame's cells.  The
 * `BP & 3` plane picks which of the four pre-shifted copies of the letter is
 * used - that is what gives the ring its sub-pixel horizontal placement.
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
 * is the offscreen page at `[0x8A4]` (`1000:0874` sets `DS = [0x8A4]`), read
 * at `[0x8AE]*80 + 6`; the 304-row range is why the entry animation runs
 * `[0x8AE]` from `0x130` (304) down to 2.  Each frame makes five
 * `vga_blit_row` (`1000:0874`) calls:
 *
 *   dest   = { 0xE66, 0x14A6, 0x1AE6, 0x2126, 0x2766 }   (rows 46/66/86/106/126, col 6)
 *   mask   = { 0xA0,  0xB0,   0xE0,   0xF0,   0x30 }     (the GC Bit Mask)
 *   source = page + [0x8AE]*80 + 6, plus 0x640 per call
 *
 * and `vga_blit_row` copies 14 iterations of `4 / +7 / 31 / +9 / 15 / +0xE`
 * MOVSB runs (80 byte addresses per iteration) with write mode 2 and
 * `Bit Mask = CH`: source bits 0..3 drive the four pixels, and the mask
 * selects the colour bits written (the rest come from the latch), so the
 * frame tints the page's own text/art instead of overwriting it.
 *
 * The five frame blocks and `[0x8A2]` (1..6): each step is 0x14 frames of
 * `[0x8AE]` moving one unit per frame (`1000:05F4`/`0613`), so the band
 * scrolls through the fixed frame windows rather than changing contents.
 */
static uint8_t *_band_sheet; // apl\band.spr: 304 rows x 80 bytes

uint16_t g_map_box_bottom; // [0x8AE]
uint16_t g_map_panel; // [0x8A2]: 1..6

/**
 * @brief Accessor: the running score as one 32-bit value.
 *
 * @return the score
 */
static uint32_t _map_score(void)
{ return ((uint32_t)score_total_hi() << 16) | score_total_lo(); }

/**
 * @brief Rotate a word right by p positions (XCHG/SHR/XCHG).
 *
 * @param bx the word to rotate
 * @param p the rotation count
 * @return the rotated word
 */
static uint16_t _alpha_ror16(uint16_t bx, uint32_t p)
{
    for (uint16_t r = 0; r < p; r++) bx = (uint16_t)((bx >> 1) | (bx << 15));
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

    for (uint16_t k = 0; k < 4u; k++) {
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

    for (uint16_t k = 0; k < 4u; k++) {
        al = (uint8_t)((al << 1) | (ah & 1u));
        ah = (uint8_t)(ah >> 1);
    }
    return (uint8_t)(al & 0x0Fu);
}

void alpha_expand(const uint8_t *src, uint8_t *dst)
{
    for (uint16_t p = 0; p < 4u; p++) {
        uint16_t si = 0;
        uint32_t di = p * 0x4000u; // 4 writes/row reach 0x10000

        for (uint16_t i = 0; i < 0x1000u; i++) {
            uint16_t bx = _alpha_ror16(rd_le_u16(src + si), p);

            si += 2u;
            dst[di++] = _alpha_hi_right((uint8_t)(bx & 0xFFu)); // BL
            dst[di++] = _alpha_lo_left((uint8_t)(bx & 0xFFu));
            /* MOV BL,BH */
            dst[di++] = _alpha_hi_right((uint8_t)((bx >> 8) & 0xFFu));
            dst[di++] = _alpha_lo_left((uint8_t)((bx >> 8) & 0xFFu));
        }
    }
}

/**
 * @brief OR the colour-5 overlay of a cell byte into four page pixels.
 *
 * `queue_dirty_job` (1000:0F8F) sets GC write mode 2, Bit Mask 0x20 and Data
 * Rotate = OR (0x10), so source bit p writes plane p's bit 5 and leaves every
 * other bit from the latch: a set bit gives `pixel | 0x20`, a clear one leaves
 * the pixel untouched.  This is the 0x20 colour bank the ring/name use.
 *
 * @param page the page buffer
 * @param px the x position
 * @param py the y position
 * @param v the nibble (one bit per pixel)
 */
static void _splat_four(uint8_t *page, uint32_t px, uint32_t py, uint32_t v)
{
    for (uint16_t bit = 0; bit < 4u; bit++)
        if ((v >> bit) & 1u) page[py * VIDEO_PAGE_W + px + bit] |= 0x20u;
}

/** @brief Record one cell destination in the frame's list ([0x1A1A] cursor).
 *
 * @param dst the mode-X byte address
 */
static void _map_dirty_add(uint16_t dst)
{
    uint8_t n = _map_dirty_n[_map_dirty_cur];

    if (n < MAP_DIRTY_MAX) {
        _map_dirty[_map_dirty_cur][n] = dst;
        _map_dirty_n[_map_dirty_cur] = n + 1u;
    }
}

/** @brief 1000:0BB0 clear_dirty_rects: clear bit 5 of the previous frame's
 * cells (11 rows x 4 byte addresses each, the same footprint as the draw).
 *
 * @param page the page buffer
 */
static void _map_dirty_clear(uint8_t *page)
{
    const uint16_t *list = _map_dirty[_map_dirty_prev];
    uint8_t n = _map_dirty_n[_map_dirty_prev];

    if (!page) return;
    for (uint16_t i = 0; i < n; i++) {
        uint16_t dst = list[i];

        for (uint16_t r = 0; r < 11u; r++)
            for (uint16_t c = 0; c < 4u; c++) {
                uint16_t a = (dst + r * 80u + c) & 0xFFFFu;
                uint16_t row = a / 80u, col = a % 80u;

                if (row >= VIDEO_PAGE_H_MODE13H) continue;
                for (uint16_t p = 0; p < 4u; p++)
                    page[row * VIDEO_PAGE_W + col * 4u + p] &= (uint8_t)~0x20u;
            }
    }
}

/** @brief 1000:0CBE's opening: clear the previous list and swap the buffers.
 *
 * @param page the page buffer
 */
static void _map_dirty_begin(uint8_t *page)
{
    uint8_t fill;

    _map_dirty_clear(page);
    fill = _map_dirty_prev;
    _map_dirty_prev = _map_dirty_cur;
    _map_dirty_cur = fill;
    _map_dirty_n[fill] = 0u;
}

/** @brief Reset both cell lists (screen entry; nothing to erase yet).
 */
static void _map_dirty_reset(void)
{
    _map_dirty_n[0] = _map_dirty_n[1] = 0u;
    _map_dirty_cur = 0u;
    _map_dirty_prev = 1u;
}

/** @brief 1000:119F cos_scaled (angle + 0x5A, wrapped) / 1000:11AA sin_scaled:
 * CX + (sin[a] * DX) >> 14, with the negative half's extra `- DX*4` (the
 * listing's asymmetry, kept).  The 14 is the 14 `SHR DX,1`/`RCR AX,1` pairs
 * at 0x11B7..0x11ED and 0x11FC..0x1232.
 *
 * @param angle the rotation angle
 * @param cx the centre coordinate
 * @param dx the radius
 * @param plus90 non-zero to add 90 degrees to the angle
 * @return the scaled coordinate
 */
static uint32_t _alpha_rot(uint32_t angle, uint32_t cx, uint32_t dx,
                           int32_t plus90)
{
    uint32_t a = (((int32_t)angle + (plus90 ? 0x5Au : 0)) % 0x168u);
    int16_t v = _alpha_sin[a];
    uint32_t p = (uint32_t)(uint16_t)v * (uint16_t)dx;
    uint32_t r = ((cx + (uint16_t)(p >> 14)) & 0xFFFFu);

    if (v < 0) r = ((r - dx * 4u) & 0xFFFFu);
    return r;
}

void alpha_cell(uint32_t cell, uint32_t angle, uint16_t *src, uint16_t *dst)
{
    uint16_t bp = (_alpha_rot(angle, 0x9Du, 0x42u, 1) - 8u) & 0xFFFFu;
    uint16_t c2 = (_alpha_rot(angle, 0x4Fu, 0x3Eu, 0) - 8u) & 0xFFFFu;

    *dst = (uint16_t)((c2 * 80u + 240u + (bp >> 2)) & 0xFFFFu);
    *src = (uint16_t)((cell * 0x40u + 0x0Cu + (bp & 3u) * 0x4000u) & 0xFFFFu);
}

/** @brief one 16x4-byte cell at page byte `dst`: 11 rows, the listing's 44
 * MOVSBs (1000:0F8F).  The source's four bits are the four pixels of its
 * 4-px group; a set bit ORs 0x20 and a clear one leaves the pixel.
 *
 * @param page the page buffer
 * @param src the source buffer
 * @param dst the destination buffer
 */
static void _alpha_blit_cell(uint8_t *page, uint16_t src, uint16_t dst)
{
    for (uint16_t r = 0; r < 11u; r++) {
        uint16_t at = (dst + r * 80u) & 0xFFFFu;

        for (uint16_t c = 0; c < 4u; c++) {
            uint8_t v = _alpha_sheet[(src + r * 4u + c) & 0xFFFFu];
            uint16_t px = (at % 80u) * 4u + c * 4u;
            uint16_t py = at / 80u;

            if (!v || py >= VIDEO_PAGE_H_MODE13H || px + 3u >= VIDEO_PAGE_W)
                continue;
            _splat_four(page, px, py, v);
        }
    }
}

/**
 * @brief Draw the 30-cell letter wheel and optionally record its cells.
 *
 * `rotate_spinner_cell` (`1000:0F93`) selects a sheet block through
 * `byte[0xA8 + (angle+0x16)/0x2D]`; this image's table is **all zero**
 * (confirmed live at the name screen), so every cell uses block 0 and the
 * glyphs stay upright.  The block's rows 3..13 are drawn at `dst`.
 *
 * @param page the page buffer
 * @param base the ring angle ([0x48])
 * @param record non-zero to add each cell address to the dirty list
 */
static void _map_wheel_draw(uint8_t *page, uint32_t base, int32_t record)
{
    if (!page || !_alpha_sheet) return;
    for (uint16_t i = 0; i < 30u; i++) {
        uint16_t src, dst;

        alpha_cell(i, (base + i * 0x0Cu) % 0x168u, &src, &dst);
        _alpha_blit_cell(page, src, dst);
        if (record) _map_dirty_add(dst);
    }
}

void alpha_draw_wheel(uint8_t *page, uint32_t base)
{ _map_wheel_draw(page, base, 0); }

/** @brief 1000:0CBE's name target x for `len` characters: 9 px apart, the run
 * centred on 0xA0, `-8 - ((len*9 - 1)/2 - 0xA0)`.
 *
 * @param len the name length
 * @return the first target x
 */
static int32_t _map_name_target_x(uint32_t len)
{ return (-8 - (((int32_t)(len * 9u) - 1) / 2 - 0xA0)); }

/**
 * @brief One name glyph through queue_dirty_job: dest = y*80 + 0xF0 + x/4,
 * source = column*0x40 + 0xC + (x&3)*0x4000.
 *
 * @param page the page buffer
 * @param c the character
 * @param x the current x
 * @param y the current y
 * @param record non-zero to add the address to the dirty list
 * @return 1 when the character has a cell
 */
static int32_t _map_glyph_draw(uint8_t *page, char c, int32_t x, int32_t y,
                               int32_t record)
{
    uint8_t col;
    uint16_t src, dst;

    if (c == ' ') col = 0x1Au;
    else if (c == '.') col = 0x1Bu;
    else if (c >= 'A' && c <= 'Z') col = (uint8_t)((uint32_t)(c - 'A'));
    else return 0;
    src = u16(col * 0x40u + 0x0Cu + ((uint32_t)x & 3u) * 0x4000u);
    dst = u16((uint32_t)(y + 3) * 80u + ((uint32_t)x >> 2));
    _alpha_blit_cell(page, src, dst);
    if (record) _map_dirty_add(dst);
    return 1;
}

void alpha_draw_name(uint8_t *page, const char *name)
{
    uint8_t len = 0;
    int32_t x;

    while (name && name[len]) len++;
    if (!page || !_alpha_sheet || !len) return;
    x = _map_name_target_x(len);
    for (uint16_t i = 0; i < len; i++) {
        _map_glyph_draw(page, name[i], x, 0x45, 0);
        x += 9;
    }
}

/** @brief One <=4-px step of `current` toward `target` (1000:0D32..0x0D76).
 *
 * @param cur the current coordinate
 * @param target the target coordinate
 * @return the advanced coordinate
 */
static int32_t _map_step_toward(int32_t cur, int32_t target)
{
    int32_t d = target - cur;

    if (d > 4) d = 4;
    else if (d < -4) d = -4;
    return cur + d;
}

/**
 * @brief 1000:0CBE's name pass: refresh each target, advance the display
 * list's current positions by <=4 px and draw the glyphs there.
 *
 * @param page the page buffer
 * @param record non-zero to add each glyph address to the dirty list
 */
static void _map_name_draw(uint8_t *page, int32_t record)
{
    uint8_t len = 0;
    int32_t x;

    while (_map_name[len]) len++;
    if (len > NAME_MAX) len = NAME_MAX;
    if (!page || !_alpha_sheet || !len) return;
    x = _map_name_target_x(len);
    for (uint16_t i = 0; i < len; i++) {
        _map_glyphs[i].tx = (int16_t)(x);
        _map_glyphs[i].ty = 0x45;
        _map_glyphs[i].cx =
            (int16_t)_map_step_toward(_map_glyphs[i].cx, _map_glyphs[i].tx);
        _map_glyphs[i].cy =
            (int16_t)_map_step_toward(_map_glyphs[i].cy, _map_glyphs[i].ty);
        _map_glyph_draw(page, _map_name[i], _map_glyphs[i].cx,
                        _map_glyphs[i].cy, record);
        x += 9;
    }
}

/**
 * @brief Write the masked four pixels of one page byte.
 *
 * 1000:0874 blits with GC write mode 2, Map Mask 0x0F and Bit Mask = the
 * box's mask, Data Rotate = replace.  Write mode 2 sends source bits 0..3 to
 * planes 0..3 (the four pixels of a page byte), and the Bit Mask picks which
 * colour bits are written; the rest come from the latch.  So pixel p gets its
 * mask bits set when source bit p is 1 and cleared when it is 0.
 *
 * @param page the page buffer
 * @param row the row
 * @param col the column
 * @param v the source byte
 * @param mask the Bit Mask
 */
static void _blit_four(uint8_t *page, uint32_t row, uint32_t col, uint32_t v,
                       uint32_t mask)
{
    for (uint16_t p = 0; p < 4u; p++) {
        uint8_t *px = &page[row * VIDEO_PAGE_W + col * 4u + p];

        *px = (uint8_t)((*px & (uint8_t)~mask) | (((v >> p) & 1u) ? mask : 0u));
    }
}

/** @brief The offscreen page's 4-pixel group at an 80-byte address.
 *
 * `1000:0874` reads the page at `[0x8AE]*80 + 6` one byte (four pixels) at a
 * time; the offscreen page is stored flat here, so pack the four pixels'
 * low bits back into the source byte the way `_blit_four` consumes it.
 *
 * @param a the 80-byte page address
 * @return the four pixels as bits 0..3
 */
static uint32_t _map_off_byte(uint32_t a)
{
    uint16_t row = (uint16_t)(a / 80u), col = (a % 80u) * 4u;
    uint8_t v = 0;

    if (!_map_off || row >= VIDEO_PAGE_H_MODE13H) return 0u;
    for (uint16_t p = 0; p < 4u; p++)
        v |= (_map_off[row * VIDEO_PAGE_W + col + p] & 1u) << p;
    return v;
}

/**
 * @brief One blit-row pass: the three runs and their gaps.
 *
 * @param page the page buffer
 * @param d the destination cursor (in/out)
 * @param s the source cursor (in/out)
 * @param mask the mask
 */
static void _blit_runs(uint8_t *page, uint16_t *d, uint16_t *s, uint32_t mask)
{
    static const uint32_t runs[3] = { 4u, 31u, 15u };
    static const uint32_t gaps[3] = { 7u, 9u, 0x0Eu };

    for (uint16_t k = 0; k < 3u; k++) {
        for (uint16_t n = 0; n < runs[k]; n++) {
            uint8_t v = (uint8_t)(_map_off_byte(*s));
            uint16_t row = (*d / 80u), col = (*d % 80u);

            if (row < VIDEO_PAGE_H_MODE13H) _blit_four(page, row, col, v, mask);
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
    uint16_t d = (uint16_t)dstByte, s = (uint16_t)srcByte;

    for (uint16_t iter = 0; iter < 14u; iter++) _blit_runs(page, &d, &s, mask);
}

void map_draw_box_edges(uint8_t *page, uint32_t row)
{
    static const uint32_t dstByte[5] = { 0xE66u, 0x14A6u, 0x1AE6u, 0x2126u,
                                         0x2766u };
    static const uint32_t mask[5] = { 0xA0u, 0xB0u, 0xE0u, 0xF0u, 0x30u };
    uint16_t src = u16(row * 80u + 6u);

    if (!page || !_band_sheet) return;
    for (uint16_t i = 0; i < 5u; i++) {
        _map_blit_row(page, dstByte[i], src, mask[i]);
        src += 0x640u; // ADD SI,0x640
    }
}

/* --- the band's staging page and its four-strip animation (1000:09DA/0A72/
 * 07B2/0E77/0EBE/0EEC/0F3E) ----------------------------------------------
 *
 * `1000:12D3` parks BAND.SPR's four planes at A000:0xC000 (76 rows of 80 byte
 * addresses) - video.c's shared A000 model in the port, at its absolute
 * address.  The page is only read by `blit_palette_fade_row` (`1000:0A72`):
 * for a channel's 8-bit phase it copies 320 contiguous byte addresses from
 * `0xC000 + phase*0x140` into the page at `0x2DB8/ch*8 + [0x5AC4]`, laid out
 * as 40 rows of 8 byte addresses at the page's 80-byte row stride (the 0x48
 * gap plus the 8 copied bytes).  A byte address is a 4-pixel group, so the
 * strip is 32 px wide and 40 rows tall at x = 96+32*ch, y = 146.
 *
 * `palette_animate` ticks the four scripts first, then compares each channel
 * word with its `[0xA0..0xA6]` shadow and blits on a change; `palette_reset`
 * seeds the shadows and animates twice.  The scripts' state bytes are the
 * image's `[0x75]..[0x8B]` (the tables at 0x80/0x8A are `{1,0,1,0,0xFF}`), and
 * the driver events (not ported) set the 0x75/0x79/0x7E/0x88 triggers.
 */
#define MAP_STAGE_BASE  0xC000u
#define MAP_STAGE_ADDRS 6080u
static uint8_t _map_stage_ready;

/* the channel words [0x90]/[0x92]/[0x94]/[0x96], their previous-value shadows
 * [0x98..0x9E] and the compared shadows [0xA0..0xA6]. */
static uint16_t _map_ch[4], _map_prev_ch[4], _map_shadow[4];

/* the four scripts' state bytes, from the image's [0x75]..[0x8B]. */
static uint8_t _map_b75, _map_b76, _map_b77, _map_b78, _map_b79, _map_b7b;
static uint8_t _map_b7c, _map_b7d, _map_b7e, _map_b7f, _map_b81;
static uint8_t _map_b86, _map_b87, _map_b88, _map_b89, _map_b8b;

/** @brief 1000:12D3: de-interleave BAND.SPR's four plane bytes into the
 * staging page (plane p of byte address i is source[i*4+p]).
 */
static void _map_stage_fill(void)
{
    if (!_band_sheet) return;
    for (uint16_t i = 0; i < MAP_STAGE_ADDRS; i++)
        for (uint16_t p = 0; p < 4u; p++)
            video_vram_group((uint16_t)(MAP_STAGE_BASE + i))[p] =
                _band_sheet[(i * 4u + p) & 0xFFFFu];
    _map_stage_ready = 1;
}

/** @brief the colour of pixel j of the staging page's byte address a.
 *
 * Measured on the running game: the four plane bytes at one address are the
 * four pixels' full 8-bit colours, pixel j = plane j; the page is a plain
 * 320-px row-major image whose 4-pixel groups are split across the planes.
 * The band staging page decodes to BAND.SPR row-major byte for byte.
 *
 * @param a the staging byte address
 * @param j the pixel within the 4-pixel group
 * @return the colour
 */
static uint8_t _map_stage_pixel(uint32_t a, uint32_t j)
{ return video_vram_group((uint16_t)(MAP_STAGE_BASE + a))[j & 3u]; }

/** @brief 1000:0A72: one channel's 8-byte x 40-row strip from the staging page
 * at `phase*0x140` byte addresses into the page at `0x2DB8 + ch*8`.
 *
 * @param phase the channel's 8-bit phase
 * @param ch the channel (0..3)
 * @param page the page buffer
 */
static void _map_blit_strip(uint16_t phase, uint32_t ch, uint8_t *page)
{
    uint32_t src = (phase & 0xFFu) * 0x140u;
    uint16_t dst = (uint16_t)(0x2DB8u + ch * 8u);

    if (!page || !_map_stage_ready) return;
    for (uint16_t r = 0; r < 40u; r++)
        for (uint16_t c = 0; c < 8u; c++) {
            uint32_t a = src + r * 8u + c;
            uint16_t da = dst + r * 80u + c; // the 0x48 gap + 8 bytes
            uint32_t px = (da / 80u) * VIDEO_PAGE_W + (da % 80u) * 4u;

            if (a >= MAP_STAGE_ADDRS) continue;
            if (da / 80u >= VIDEO_PAGE_H_MODE13H) continue;
            for (uint16_t j = 0; j < 4u; j++)
                page[px + j] = _map_stage_pixel(a, j);
        }
}

/** @brief 1000:0A72 for all four channels: force the strip layer onto the page.
 *
 * `1000:0221`/`0x09DA` only blit a channel whose word changed, so a page
 * re-copy (1000:024B) would leave the display without its strips.  The
 * original's strips live on the display page and survive that offscreen copy;
 * the port's one-page model needs this full redraw.
 *
 * @param page the page buffer
 */
static void _map_draw_strips(uint8_t *page)
{
    static const uint16_t off[4] = { 0u, 3u, 8u, 0x0Eu };

    if (!page || !_map_stage_ready) return;
    for (uint16_t ch = 0; ch < 4u; ch++)
        _map_blit_strip((uint16_t)(_map_ch[ch] + off[ch]), ch, page);
}

/** @brief 1000:0E77 anim_tick_1: the [0x94] channel's 10-frame cycler (the
 * event byte [0x75] takes precedence, writing `[0x76]&3 + 2`).
 */
static void _map_anim_tick_1(void)
{
    if (_map_b75) {
        _map_b75 = 0u;
        _map_b76++;
        _map_ch[2] = (uint16_t)((_map_b76 & 3u) + 2u);
        _map_b77 = 0x0Au;
        return;
    }
    _map_b77 = (uint8_t)(_map_b77 - 1u);
    if ((int8_t)_map_b77 >= 0) return;
    if (_map_ch[2] < 2u) {
        _map_ch[2] ^= 0x0100u; // XOR word [0x95],1
        _map_b77 = 0x0Au;
    } else {
        _map_ch[2] = 1u;
        _map_b77 = 0x0Au;
    }
}

/** @brief 1000:0EBE anim_tick_2: the [0x90] channel's 5-frame cycler.
 */
static void _map_anim_tick_2(void)
{
    uint16_t ax = 1u;

    _map_b78 = (uint8_t)(_map_b78 - 1u);
    if ((int8_t)_map_b78 >= 0) return;
    _map_b78 = 5u;
    if (_map_b79) {
        ax = 2u;
        _map_b7b++;
        if (!(_map_b7b & 1u)) ax = 0u;
    }
    _map_b79 = 0u;
    _map_ch[0] = ax;
}

/** @brief the two table-walking ticks (1000:0EEC `fall_entry_b` for [0x92],
 * 1000:0F3E `anim_tick_4` for [0x96]).  0x80's and 0x8A's tables are both
 * `{1, 0, 1, 0, 0xFF}`; the 0xFF entry wraps the walk and decrements the
 * repeat counter.
 *
 * @param ch the channel index (1 or 3)
 * @param timer the script's 5-frame counter
 * @param repeat the repeat counter byte
 * @param idx the table cursor byte
 * @param step the phase counter byte
 * @param next the phase byte written on each wrap
 */
static void _map_anim_table_tick(uint32_t ch, uint8_t *timer, uint8_t *repeat,
                                 uint8_t *idx, uint8_t *step, uint8_t *next)
{
    static const uint8_t table[8] = { 1u, 0u, 1u, 0u, 0xFFu, 0u, 0u, 0u };
    uint8_t bl;

    *timer = (uint8_t)(*timer - 1u);
    if ((int8_t)*timer >= 0) return;
    *timer = 5u;
    if (*repeat == 0u) {
        _map_ch[ch] = 0u;
        return;
    }
    bl = 0u;
    if (*idx == 0u) {
        (*step)++;
        bl = (uint8_t)((*step & 3u) + 1u);
        *next = bl;
    }
    bl = (uint8_t)(*idx & 7u); // the image's 0x80..0x87 window
    _map_ch[ch] = table[bl];
    if (table[(uint32_t)bl + 1u] & 0x80u) {
        bl = 0u;
        (*repeat)--;
    } else {
        bl++;
    }
    *idx = bl;
}

/** @brief 1000:0E6B tick_effects: the four scripts' per-frame tail.
 */
static void _map_anim_ticks(void)
{
    _map_anim_tick_1();
    _map_anim_tick_2();
    _map_anim_table_tick(1u, &_map_b7c, &_map_b7e, &_map_b7f, &_map_b7d,
                         &_map_b81);
    _map_anim_table_tick(3u, &_map_b86, &_map_b88, &_map_b89, &_map_b87,
                         &_map_b8b);
}

/** @brief 1000:09DA palette_animate: tick the scripts, then blit a channel
 * whose word changed from its shadow.
 */
static void _map_palette_animate(void)
{
    static const uint16_t off[4] = { 0u, 3u, 8u, 0x0Eu };
    uint8_t *page = video_page(0);

    _map_anim_ticks();
    for (uint16_t ch = 0; ch < 4u; ch++) {
        if (_map_ch[ch] == _map_shadow[ch]) continue;
        _map_prev_ch[ch] = _map_shadow[ch]; // 0xA0 = the old 0x98
        _map_shadow[ch] = _map_ch[ch];
        _map_blit_strip((uint16_t)(_map_ch[ch] + off[ch]), ch, page);
    }
}

void map_palette_reset(void)
{
    for (uint16_t i = 0; i < 4u; i++) {
        _map_shadow[i] = 0xFFFFu;
        _map_prev_ch[i] = 0xFFFEu;
    }
    _map_palette_animate();
    _map_palette_animate();
}

uint16_t map_anim_channel_for_probe(uint32_t ch)
{ return ch < 4u ? _map_ch[ch] : 0u; }

void map_anim_tick_for_probe(void) { _map_palette_animate(); }

uint8_t map_name_col_for_probe(void) { return _map_col; }

uint16_t map_name_fire_wait_for_probe(void) { return _map_fire_wait; }

const char *map_name_for_probe(void) { return _map_name; }

uint8_t map_intro_step_for_probe(void) { return _map_intro; }

uint8_t map_enter_step_for_probe(void) { return _map_enter_phase; }

uint16_t map_enter_bx_for_probe(void) { return _map_enter_bx; }

uint8_t *map_off_page_for_probe(void) { return _map_off; }

uint16_t map_name_angle_for_probe(void) { return _map_angle; }

void map_name_set_angle_for_probe(uint32_t angle)
{ _map_angle = angle % 0x168u; }

int32_t map_glyph_for_probe(uint32_t i, int32_t *tx, int32_t *ty, int32_t *cx,
                            int32_t *cy)
{
    if (i >= NAME_MAX) return 0;
    if (tx) *tx = _map_glyphs[i].tx;
    if (ty) *ty = _map_glyphs[i].ty;
    if (cx) *cx = _map_glyphs[i].cx;
    if (cy) *cy = _map_glyphs[i].cy;
    return 1;
}

uint8_t map_stage_ready_for_probe(void) { return _map_stage_ready; }

uint8_t map_stage_at(uint32_t addr, uint32_t plane)
{
    if (addr >= MAP_STAGE_ADDRS || plane >= 4u) return 0u;
    return video_vram_group((uint16_t)(MAP_STAGE_BASE + addr))[plane];
}

void map_draw_list(uint8_t *page)
{
    uint16_t row = 0u;

    if (!page) return;
    memset(page, 0, MAP_PAGE); // 1000:0577
    for (uint16_t i = 0; i < hiscore_count(); i++) {
        char buf[24];

        snprintf(buf, sizeof buf, "%u", i + 1u);
        textpage_draw_line_mask_at(page, buf, 0x20u, row, 0x0Fu);
        textpage_draw_line_mask_at(page, hiscore_name(i), 0x82u, row, 0x0Fu);
        snprintf(buf, sizeof buf, "%" PRIu32, hiscore_score(i));
        textpage_draw_line_mask_at(page, buf, 0x102u, row, 0x0Fu);
        row += 0x14u;
    }
}

void map_draw_list_header(uint8_t *page)
{
    if (!page) return;
    for (uint16_t i = 0; i < 5u; i++) {
        uint8_t y = (uint8_t)(0x2Du + 0x14u * i);

        map_draw_rect(page, 0x12u, y, 0x1Cu, 0x0Fu); // 1000:0B5F
        map_draw_rect(page, 0x32u, y, 0xA0u, 0x0Fu);
        map_draw_rect(page, 0xD6u, y, 0x58u, 0x0Fu);
    }
    textpage_draw_line_mask_at(page,
                               game_strs[g_game_lang][GAME_STR_MAP_TOP_TEN],
                               0xA0u, 0x14u, 0x20u);
}

void map_draw_congrats(uint8_t *page)
{
    for (uint16_t i = 0; i < 7u; i++)
        textpage_draw_line_mask_at(
            page, game_strs[g_game_lang][GAME_STR_MAP_CONGRATS_0 + i], 0xA0u,
            0x19u + 0x10u * i, 0x20u);
}

uint8_t map_congrats_draws(void) { return _map_congrats_draws; }

void map_draw_rect(uint8_t *page, uint32_t x, uint32_t y, uint32_t w,
                   uint32_t h)
{
    if (!page) return;
    for (uint16_t i = 0; i < w; i++) {
        if (y < VIDEO_PAGE_H_MODE13H && x + i < VIDEO_PAGE_W)
            page[y * VIDEO_PAGE_W + x + i] |= 0x40u;
        if (y + h < VIDEO_PAGE_H_MODE13H && x + i < VIDEO_PAGE_W)
            page[(y + h) * VIDEO_PAGE_W + x + i] |= 0x50u;
    }
    for (uint16_t i = 0; i < h; i++) {
        if (y + i < VIDEO_PAGE_H_MODE13H && x < VIDEO_PAGE_W)
            page[(y + i) * VIDEO_PAGE_W + x] |= 0x50u;
        if (y + i < VIDEO_PAGE_H_MODE13H && x + w < VIDEO_PAGE_W)
            page[(y + i) * VIDEO_PAGE_W + x + w] |= 0x40u;
    }
}

uint8_t map_transition_step(void) { return _map_trans; }

uint8_t map_fade_starts(void) { return _map_fade_starts; }

int32_t map_fade_at(uint32_t i, uint32_t *first, uint32_t *entries,
                    uint32_t *div, uint32_t *target)
{
    if (i >= _map_fade_starts || i >= MAP_FADE_LOG) return 0;
    if (first) *first = _map_fade_log[i].first;
    if (entries) *entries = _map_fade_log[i].entries;
    if (div) *div = _map_fade_log[i].div;
    if (target) *target = _map_fade_log[i].target;
    return 1;
}

/** @brief Build a fade to an arbitrary target and log it.
 *
 * @param first the first entry
 * @param entries the entry count
 * @param div the divisor
 * @param target the target palette
 * @param targetId the logged target id
 */
static void _map_build_fade_to(uint16_t first, uint16_t entries, uint16_t div,
                               const uint8_t *target, uint16_t targetId)
{
    palette_build_fade(first, entries, div, target);
    if (_map_fade_starts < MAP_FADE_LOG) {
        _map_fade_log[_map_fade_starts].first = first;
        _map_fade_log[_map_fade_starts].entries = entries;
        _map_fade_log[_map_fade_starts].div = div;
        _map_fade_log[_map_fade_starts].target = targetId;
    }
    _map_fade_starts++;
}

/** @brief 1000:0273/028C/029B/02AD/02BF/02F5/0307: palette_build_fade with the
 * working palette's offset as the target (0x4BBC + N).
 *
 * @param first the first entry
 * @param entries the entry count
 * @param div the divisor
 * @param targetOff the target offset
 */
static void _map_build_fade(uint16_t first, uint16_t entries, uint16_t div,
                            uint16_t targetOff)
{
    _map_build_fade_to(first, entries, div, palette_working() + targetOff,
                       targetOff);
}

void map_draw_name(uint8_t *page)
{
    if (!page) return;
    _map_dirty_begin(page);
    _map_wheel_draw(page, _map_angle, 1);
    _map_name_draw(page, 1);
}

/* Start a map page: clear it, then lay HIGH.BIT under it. */
static void _map_page_copy(uint8_t *page)
{
    if (!page) return;
    memset(page, 0, MAP_PAGE);
    if (_map_high)
        memcpy(page, _map_high,
               _map_high_size < MAP_PAGE ? _map_high_size : MAP_PAGE);
}

/** @brief 1000:02C5: HIGH.BIT plus the band strips' palette reset.
 *
 * @param page the page buffer
 */
static void _map_page_reset(uint8_t *page)
{
    _map_page_copy(page);
    /* 1000:0221/0x02E6: the band strips sit under the art that follows. */
    if (_map_stage_ready) map_palette_reset();
}

/** @brief the whole screen: high.bit's page, then the state's own art and the
 * box frame (1000:07E6).
 */
static void _map_redraw(void)
{
    uint8_t *page = video_page(0);

    if (!page) return;
    if (_map_phase == MAP_NAME || _map_phase == MAP_ENTER) map_draw_name(page);
    else if (_map_phase == MAP_LIST) {
        /* 1000:05A5/05F8/0617: only the frame bands; the rows stay in the
         * offscreen page and the display keeps high.bit + the header. */
        map_draw_box_edges(page, g_map_box_bottom);
    }
}

/**
 * @brief One left/right animation tick: four 3-degree draws, then the three
 * 0x0330 holds (1000:03B5..0x03F8).
 *
 * @return 1 when a tick was consumed, 0 when the animation is idle
 */
static int32_t _map_move_tick(void)
{
    if (_map_move_steps > 0) {
        int32_t a = (int32_t)_map_angle + _map_move_dir * 3 + 0x168;

        _map_angle = (uint16_t)(a % 0x168u);
        _map_col = _map_col_from_angle();
        _map_redraw();
        _map_move_steps = (uint8_t)(_map_move_steps - 1u);
        if (_map_move_steps == 0u) _map_move_hold = 3;
        return 1;
    }
    if (_map_move_hold > 0) {
        _map_redraw(); // 1000:0330
        _map_move_hold--;
        return 1;
    }
    return 0;
}

uint8_t map_begin(void)
{
    if (_map_up) return 1;

    res_set_disc(0u); // entry 0x151A [0xF7E]=0
    _map_high = res_load_chunk("LBM\\HIGH.BIT", &_map_high_size, NULL);
    uint8_t *page = video_page(0);
    if (_map_high && _map_high_size >= MAP_PAGE + 768u)
        palette_load_working(_map_high + MAP_PAGE); // 1000:01C3/8D24

    _map_font = res_load_chunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(_map_font);

    /* 1000:01A2..0x01AB: apl\alpha.spr expanded 4bpp -> four planes */
    g_osys->memFree(_alpha_packed);
    _alpha_packed = NULL;
    g_osys->memFree(_alpha_sheet);
    _alpha_sheet = NULL;
    {
        uint32_t asz = 0;

        _alpha_packed = res_load_chunk("APL\\ALPHA.SPR", &asz, NULL);
        /* 1000:0705 transposes a fixed 4096-word (8192-B) window.  The
         * shipped file is 8128 B, so the window's 64-byte tail is zeroed
         * rather than read past the chunk (the original's tail is whatever
         * follows ALPHA in its pool; the cells that would use it are never
         * drawn). */
        if (_alpha_packed && asz < ALPHA_WINDOW) {
            uint8_t *win = g_osys->memAlloc(ALPHA_WINDOW);

            if (!win) {
                g_osys->memFree(_alpha_packed);
                _alpha_packed = NULL;
            } else {
                memset(win, 0, ALPHA_WINDOW);
                memcpy(win, _alpha_packed, asz);
                g_osys->memFree(_alpha_packed);
                _alpha_packed = win;
            }
        }
    }
    if (_alpha_packed) {
        _alpha_sheet = g_osys->memAlloc(65536u);
        if (_alpha_sheet) alpha_expand(_alpha_packed, _alpha_sheet);
    }
    /* 1000:01B7: apl\band.spr, the 304 x 80 sheet */
    g_osys->memFree(_band_sheet);
    _band_sheet = NULL;
    _band_sheet = res_load_chunk("APL\\BAND.SPR", NULL, NULL);
    _map_stage_ready = 0;
    _map_stage_fill(); // 1000:12D3

    /* 1000:01CD: the running total is offered to the table.  A qualifying one
     * opens the name entry first, then the list. */
    _map_name[0] = 0;
    _map_name_len = 0u;
    _map_col = 0u;
    _map_angle = 0x10Eu;
    _map_angle_step = 0u;
    _map_spin = 0;
    _map_move_steps = 0;
    _map_move_dir = 0;
    _map_move_hold = 0;
    _map_fire_lock = 0;
    _map_pending_key = 0;
    memset(_map_glyphs, 0, sizeof _map_glyphs);
    _map_dirty_reset();
    _map_fade_starts = 0u;
    _map_intro = 0u;
    _map_intro_frames = 0u;
    _map_list_step = 0u;
    _map_enter_phase = 0u;
    _map_enter_bx = 0u;
    _map_exit_hold = 0u;
    g_osys->memFree(_map_off);
    _map_off = g_osys->memAlloc(MAP_PAGE);
    if (!_map_off) return 0;
    memset(_map_off, 0, MAP_PAGE);
    g_map_box_bottom = 0x130u; // 1000:0518's [0x8AE]
    g_map_panel = 0u; // 0 = the entry animation
    _map_step = 0;
    _map_step_dir = 0;
    _map_phase = hiscore_qualifies(_map_score()) ? MAP_CONGRATS : MAP_EXIT;
    _map_congrats_draws = 0u;
    if (_map_phase == MAP_CONGRATS) {
        /* 1000:018D-0196: play_tune("mus\hiscore") / music_resume /
         * timing_loop for the congrats + name entry. */
        audio_play_tune(audio_tune_hiscore);
        audio_music_resume();
        audio_fade_in();
        _map_music_fade = 1; // 1000:018D-0196's timing_loop
        /* 1000:021E..0x0233: high.bit, the strips, then the band's 0x10..0x1F
         * colours fade in (the rest of the DAC is still the previous page's). */
        _map_page_copy(page);
        map_palette_reset(); // 0x0221
        _map_build_fade(0x10u, 0x10u, 0x1Eu, 0x30u); // 0x0224
    }

    _map_up = 1;
    _map_armed = 0;
    return 1;
}

static uint8_t _map_col_from_angle(void)
{ return (((0x10Eu + 0x168u - _map_angle) % 0x168u) / 0x0Cu); }

/** @brief 1000:0518's header: high.bit, the 15 outlines and the title.  The
 * page is cleared at 0x0577 on the next tick before the rows are drawn.
 */
static void _map_list_begin(void)
{
    uint8_t *page = video_page(0);

    _map_phase = MAP_LIST_HEADER;
    _map_list_step = 0u;
    _map_enter_phase = 2u; // the test seam: the Enter spin is over
    if (!page) return;
    _map_page_copy(page); // 0x0526
    map_draw_list_header(page); // 0x052F..0x054E
    if (_map_stage_ready) map_palette_reset(); // 0x053B
    /* 0x055E: the working palette is reloaded from high.bit, then 0x0568
     * fades 0x20..0x5F to it before the rows' box unroll starts. */
    if (_map_high && _map_high_size >= MAP_PAGE + 768u)
        palette_load_working(_map_high + MAP_PAGE);
    _map_build_fade(0x20u, 0x40u, 0x1Eu, 0x60u); // 0x0568
}

/** @brief 1000:049A..0x0516: the Enter spin, white ramp and two fades.
 *
 * `0x049A` fills the 0x48BC ramp with 0x3F (white) and `0x04B4` fades palette
 * 0x20..0x5F to it; `0x04BC` spins the wheel until that fade ends.  `0x04EA`
 * then copies working[0x60..0x7F] into the ramp twice and `0x050E` fades
 * 0x20..0x5F back to the working palette; the spin continues until it ends,
 * then `0x0518` draws the header.
 */
static void _map_enter_begin(void)
{
    memset(_map_white, 0x3Fu, sizeof _map_white); // 0x049A
    _map_build_fade_to(0x20u, 0x40u, 0x64u, _map_white, 0xFFFFu); // 0x04B4
    _map_enter_phase = 0u;
    _map_enter_bx = 1u;
    _map_phase = MAP_ENTER;
}

/** @brief 1000:062B: fade all 256 DAC entries to the zeroed 0x48BC ramp.
 *
 * `0x062B` targets `0x48BC` with `BP=0x23` and `CX=0x100`; `0x063A` waits for it, `0x063D`
 * presents 0x4B frames (`present_frames_75`), then 0x8389 stops the music and `0x06CC` saves.
 */
static void _map_exit_begin(void)
{
    _map_build_fade_to(0u, 0x100u, 0x23u, _map_black, 0xFFFFu); // 0x062B
    _map_exit_hold = 0x4Bu; // 0x063D
    audio_fade_out(); // 0x063D: present_frames_75
    _map_phase = MAP_EXIT_FADE;
}

/** @brief 1000:063D..0x0650: save the table and release the map assets. */
static void _map_close(void)
{
    _map_up = 0;
    audio_music_stop(); // 0x0640
    hiscore_write(); // 0x06CC
    g_osys->memFree(_map_high);
    _map_high = NULL;
    g_osys->memFree(_map_font);
    _map_font = NULL;
    g_osys->memFree(_alpha_packed);
    _alpha_packed = NULL;
    g_osys->memFree(_alpha_sheet);
    _alpha_sheet = NULL;
    g_osys->memFree(_band_sheet);
    _band_sheet = NULL;
    g_osys->memFree(_map_off); // the [0x8A4] pool head from map_begin
    _map_off = NULL;
    _map_stage_ready = 0;
    video_arena_release(); // the list/header text arena
    font_publish(NULL);
}

/**
 * @brief 1000:040B: one grid column applied to the 13-character buffer.
 *
 * @param column the column
 * @return 1 when Enter committed and the list header is up
 */
static int32_t _map_apply(uint8_t column)
{
    uint8_t n0 = _map_name_len;

    if (hiscore_name_apply(_map_name, column, &_map_name_len)) {
        /* 0x1D: commit the 14-byte name and insert, then 0x049A's spin. */
        hiscore_insert(_map_score(), _map_name);
        _map_enter_begin();
        return 1;
    }
    if (column != 0x1Cu) {
        /* 1000:0452..0x0459: the new glyph starts at (0x95,0x16). */
        uint8_t i = n0 < NAME_MAX ? n0 : NAME_MAX - 1u;

        _map_glyphs[i].cx = 0x95;
        _map_glyphs[i].cy = 0x16;
    }
    return 0;
}

uint8_t map_tick(const InputState *in)
{
    if (!_map_up) return 0;
    if (!_map_armed) {
        _map_armed = 1;
        return 1;
    }
    if (_map_music_fade) { // the congrats trio's fade-in blocks
        if (audio_fade_busy()) return 1;
        _map_music_fade = 0;
    }
    _map_palette_animate(); // 1000:09DA, every menu-loop iteration

    if (_map_phase == MAP_EXIT) {
        _map_exit_begin(); // 1000:062B: fade/save, no list
        return 1;
    }
    if (_map_phase == MAP_EXIT_FADE) {
        if (palette_fade_active()) return 1; // 0x062B..0x063A
        if (_map_exit_hold > 0u) { // 0x063D present_frames_75
            _map_exit_hold--;
            return 1;
        }
        if (audio_fade_busy()) return 1; // 0x063D's blocking fade
        _map_close(); // 0x06CC
        return 0;
    }
    if (_map_phase == MAP_CONGRATS) {
        /* 1000:021E..0x0285: high.bit + palette_reset + the band's 0x10..0x1F
         * fade (built in map_begin), 0x8C animates, the 0x20..0x3F fade to
         * black, high.bit again, the text twice, then the 0..0x3F fade in. */
        switch (_map_intro) {
        case 0:
            /* 1000:0233: 0x8C palette_animate frames, one present per tick.
             * The 0x0224 fade (0x10..0x1F) steps inside them, so the band
             * strips fade in while the stone table is still black. */
            _map_intro_frames++;
            if (_map_intro_frames < 0x8Cu) return 1;
            _map_build_fade_to(0x20u, 0x20u, 2u, _map_black,
                               0xFFFFu); // 0x0239
            _map_intro = 1u;
            return 1;
        case 1: {
            uint8_t *page = video_page(0);

            if (palette_fade_active()) return 1;
            _map_page_copy(page); // 0x024B
            _map_draw_strips(page); // the display strips survive the copy
            map_draw_congrats(page); // 0x024E, first pass
            _map_congrats_draws = 1u;
            _map_intro = 2u;
            return 1;
        }
        case 2: {
            uint8_t *page = video_page(0);

            map_draw_congrats(page); // 0x0251's second pass after the flip
            _map_congrats_draws = 2u;
            _map_build_fade(0u, 0x40u, 0x28u, 0u); // 0x0273
            _map_intro = 3u;
            return 1;
        }
        case 3:
            if (palette_fade_active()) return 1;
            _map_intro = MAP_INTRO_WAIT;
            return 1;
        default: break;
        }
        if (in->lastKey || (in->joyBits & 0x80u)) {
            _map_phase = MAP_NAME_FADE; // 1000:0285: the key wait
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
            _map_build_fade(0x80u, 0x80u, 2u, 0x180u); // 1000:028F
            _map_trans = 1;
            return 1;
        case 1:
            if (palette_fade_active()) return 1;
            _map_build_fade(0x20u, 0x20u, 0x1Eu, 0x120u); // 1000:02A1
            _map_trans = 2;
            return 1;
        case 2:
            if (palette_fade_active()) return 1;
            _map_build_fade(0x40u, 0x20u, 2u, 0x120u); // 1000:02B3
            _map_trans = 3;
            return 1;
        case 3: {
            uint8_t *page = video_page(0);

            if (palette_fade_active()) return 1;
            if (page) {
                _map_page_reset(page); // 1000:02C5
                map_draw_rect(page, 0x95u, 9u, 0xFu, 0xFu); // 1000:02C8
                map_draw_rect(page, 0x60u, 0x45u, 0x77u, 0xFu); // 1000:02D7
            }
            _map_build_fade(0x40u, 0x20u, 0x1Eu, 0xC0u); // 1000:02E9
            _map_trans = 4;
            return 1;
        }
        case 4:
            if (palette_fade_active()) return 1;
            /* 1000:02FB/030A: the last fade starts and the ring goes up
             * without waiting for it ([0x5AC8] = 0x23 and the entry spin). */
            _map_build_fade(0x20u, 0x20u, 0x46u, 0x60u);
            _map_trans = 6;
            _map_phase = MAP_NAME;
            _map_fire_wait = 0x23u; // 1000:030A: [0x5AC8] := 0x23
            _map_angle = 0x10Eu; // 1000:0310
            _map_angle_step = 0x12u; // 1000:0316
            _map_spin = 23; // 20 stepped draws + the three 0x0330 draws
            _map_col = 0u;
            _map_move_steps = 0;
            _map_move_hold = 0;
            _map_fire_lock = 0;
            _map_pending_key = 0;
            _map_dirty_reset();
            return 1;
        default:
            _map_phase = MAP_NAME;
            _map_fire_wait = 0x23u;
            _map_angle = 0x10Eu;
            _map_angle_step = 0x12u;
            _map_spin = 23;
            _map_move_steps = 0;
            _map_move_hold = 0;
            _map_fire_lock = 0;
            _map_pending_key = 0;
            return 1;
        }
    }

    if (_map_phase == MAP_NAME) {
        if (_map_fire_wait < 0xFFFFu) _map_fire_wait++; // the [0x5AC8] tick
        if (in->lastKey) _map_pending_key = in->lastKey; // [0x48A3] latch

        /* 1000:031C..0x0342: the ring spins one full revolution (20 x 0x12)
         * and then holds for the three 0x0330 draws. */
        if (_map_spin > 0) {
            _map_redraw();
            if (_map_spin > 3) {
                _map_angle = (_map_angle + _map_angle_step) % 0x168u; // 0x0DF3
            } else {
                _map_angle_step = 0u; // 1000:032A
            }
            _map_spin--;
            _map_col = _map_col_from_angle();
            return 1;
        }
        if (_map_move_tick()) return 1; // four 3-degree draws + three holds

        if (!(in->joyBits & 0x80u)) _map_fire_lock = 0;

        uint8_t committed = 0;

        if (in->joyBits & 0x04u) { // left: 1000:03DA, [0x48] -= 3 x4
            _map_move_dir = -1;
            _map_move_steps = 4;
            return (uint8_t)(_map_move_tick());
        }
        if (in->joyBits & 0x08u) { // right: 1000:03B5, [0x48] += 3 x4
            _map_move_dir = 1;
            _map_move_steps = 4;
            return (uint8_t)(_map_move_tick());
        }
        if (_map_pending_key) {
            uint8_t c = _map_pending_key;
            uint8_t cell = 0xFFu;

            _map_pending_key = 0u; // 1000:0363 [0x48A3] = 0
            if (c >= 'a' && c <= 'z') c -= 32u;
            if (c >= 'A' && c <= 'Z') cell = c - 'A';
            else if (c == ' ') cell = 0x1Au;
            else if (c == '.') cell = 0x1Bu;
            else if (c == 8u || c == 0x7Fu) cell = 0x1Cu; // delete
            else if (c == 13u || c == 10u) cell = 0x1Du; // enter
            _map_fire_lock = 1; // 1000:0368 [0x5ACE] = 0
            if (cell != 0xFFu) {
                /* 1000:039C..0x03B3: the key turns the ring to the cell and
                 * then falls into the shared append at 0x040B (the typed
                 * path bypasses the fire debounce). */
                _map_col = cell;
                _map_angle = (0x10Eu + 0x168u - cell * 0x0Cu) % 0x168u;
                committed = (uint8_t)(_map_apply(cell));
            }
        } else if ((in->joyBits & 0x80u) && !_map_fire_lock &&
                   _map_fire_wait >= 0x23u) {
            /* 1000:03FB CMP [0x5AC8],0x23 / 0x0405 reset */
            _map_fire_wait = 0u;
            committed = (uint8_t)(_map_apply(_map_col));
        }
        if (committed) return 1;
        _map_redraw(); // 1000:033C
        return 1;
    }

    if (_map_phase == MAP_ENTER) {
        /* 1000:04BC: step = min(BX/8, 0x10), the ring turns left and the
         * 0x0224-style present happens every frame. */
        uint8_t step = (uint8_t)(_map_enter_bx / 8u);

        if (step > 0x10u) step = 0x10u;
        _map_angle = (_map_angle + 0x168u - step) % 0x168u;
        _map_col = _map_col_from_angle();
        _map_redraw(); // 0x0CBE
        if (palette_fade_active()) {
            _map_enter_bx++;
            return 1;
        }
        if (_map_enter_phase == 0u) {
            /* 0x04EA: the 0x60-byte working[0x60..0x7F] block, twice */
            memcpy(_map_white, palette_working() + 0x120u, 0x60u);
            memcpy(_map_white + 0x60u, palette_working() + 0x120u, 0x60u);
            _map_build_fade_to(0x20u, 0x40u, 0x3Cu, _map_white,
                               0xFFFFu); // 0x050E
            _map_enter_phase = 1u;
            _map_enter_bx++;
            return 1;
        }
        _map_list_begin(); // 0x0518
        return 1;
    }

    if (_map_phase == MAP_LIST_HEADER) {
        /* 0x053E/0x0558: the title's two draws; 0x0577/0x0595: the rows into
         * the offscreen page; 0x0598: wait for the 0x0568 fade; 0x059B: the
         * 0x12E-frame box unroll. */
        _map_list_step++;
        if (_map_list_step <= 2u) return 1;
        if (_map_list_step == 3u) {
            map_draw_list(_map_off); // 0x0577 + 0x0595
            return 1;
        }
        if (palette_fade_active()) return 1; // 0x0598
        g_map_box_bottom = 0x130u; // 0x059B
        g_map_panel = 0u;
        _map_step = 0;
        _map_step_dir = 0;
        _map_phase = MAP_LIST;
        return 1;
    }

    /* 1000:0518's entry animation: [0x8AE] runs from 0x130 down to 2, one step
     * per frame, so the five frame rows unroll; [0x8A2] becomes 1 when it ends
     * and the panel loop (0x05B9) takes over. */
    if (g_map_panel == 0u) {
        _map_redraw(); // 1000:05A5: draw, wait, then SUB
        if (g_map_box_bottom > 2u) g_map_box_bottom--;
        else g_map_panel = 1u;
        return 1;
    }
    /* 1000:05F4/0613: a panel step is 0x14 frames long, not instant.  Each
     * frame draws the frame and moves [0x8AE] by one; [0x8A2] changes only
     * after the 0x14th.  Up (bit 0x01) adds 1, down (bit 0x02) subtracts 1. */
    if (_map_step > 0) {
        _map_redraw(); // 1000:05F8: draw, wait, then ADD/SUB
        g_map_box_bottom =
            (uint16_t)((int32_t)g_map_box_bottom + _map_step_dir);
        _map_step = (uint8_t)(_map_step - 1u);
        if (_map_step == 0u)
            g_map_panel = (uint16_t)((int32_t)g_map_panel + _map_step_dir);
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
        _map_exit_begin(); // 1000:062B: fade all 256 entries, then save
        return 1;
    }
    return 1;
}
