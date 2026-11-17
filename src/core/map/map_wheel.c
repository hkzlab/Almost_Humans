#include "map/map.h"
#include "map/map_internal.h"

#include "byteorder.h"
#include "width.h"

#include <stdint.h>

uint8_t *_alpha_packed; // apl\alpha.spr, the 4bpp sheet
uint8_t *_alpha_sheet;  // its 4-plane expansion (64 KB)

/* 1000:048/0x04A: the ring angle; the flow turns it. */
uint16_t _map_angle;

static char _map_name_store[NAME_MAX + 2u];
char *_map_name = _map_name_store + 1u; // one byte for 0x756's delete

/* 1000:0CBE's display list at [0x596]: {target x,y, current x,y}. */
struct MapGlyph _map_glyphs[NAME_MAX];

/* 1000:0A1A/0A1C/0A1E: the double-buffered cell list.  Each entry is the
 * mode-X byte address of one 11-row x 4-byte cell; 0x0BB0 clears the previous
 * frame's list (bit 5 of each pixel), then 0x0CBE swaps the two. */
#define MAP_DIRTY_MAX 64u
static uint16_t _map_dirty[2][MAP_DIRTY_MAX];
static uint8_t _map_dirty_n[2];
static uint8_t _map_dirty_cur;
static uint8_t _map_dirty_prev;

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
        if ((v >> bit) & 1u) page[py * VIDEO_SCREEN_W + px + bit] |= 0x20u;
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

                if (row >= VIDEO_SCREEN_H) continue;
                for (uint16_t p = 0; p < 4u; p++)
                    page[row * VIDEO_SCREEN_W + col * 4u + p] &=
                        (uint8_t)~0x20u;
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
void _map_dirty_reset(void)
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

            if (!v || py >= VIDEO_SCREEN_H || px + 3u >= VIDEO_SCREEN_W)
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

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
void alpha_draw_wheel(uint8_t *page, uint32_t base)
{ _map_wheel_draw(page, base, 0); }
#endif /* HUMANSRE_TEST_PROBES */

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

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
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
#endif /* HUMANSRE_TEST_PROBES */

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

void map_draw_name(uint8_t *page)
{
    if (!page) return;
    _map_dirty_begin(page);
    _map_wheel_draw(page, _map_angle, 1);
    _map_name_draw(page, 1);
}

/* Start a map page: clear it, then lay HIGH.BIT under it. */

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
const char *map_name_for_probe(void) { return _map_name; }

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
#endif /* HUMANSRE_TEST_PROBES */
