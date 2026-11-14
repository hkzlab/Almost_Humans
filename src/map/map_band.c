/* The MAP screen's band: BAND.SPR's box frame and its four-strip animation
 * (1000:12D3/07E6/0A72/09DA).
 */

#include "map/map.h"
#include "map/map_internal.h"

#include "video.h"
#include "width.h"

#include <stdint.h>

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
uint8_t *_band_sheet; // apl\band.spr: 304 rows x 80 bytes

uint16_t g_map_box_bottom; // [0x8AE]
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
uint8_t _map_stage_ready;

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
void _map_stage_fill(void)
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
void _map_draw_strips(uint8_t *page)
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
void _map_palette_animate(void)
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


#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
uint16_t map_anim_channel_for_probe(uint32_t ch)
{ return ch < 4u ? _map_ch[ch] : 0u; }

void map_anim_tick_for_probe(void) { _map_palette_animate(); }

uint8_t map_stage_ready_for_probe(void) { return _map_stage_ready; }
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
uint8_t map_stage_at(uint32_t addr, uint32_t plane)
{
    if (addr >= MAP_STAGE_ADDRS || plane >= 4u) return 0u;
    return video_vram_group((uint16_t)(MAP_STAGE_BASE + addr))[plane];
}
#endif /* HUMANSRE_TEST_PROBES */
