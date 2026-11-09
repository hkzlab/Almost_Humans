/* statusbar.c - the in-level status bar
 *
 * The original's mechanism, translated to the flat 320x200x8 page:
 *
 * - The bar = the bottom 40 screen rows (y 152..191): _panel4.bit's top 40
 *   rows, one art byte per pixel, +0xC0-tagged.  The original draws it once
 *   (blit_area_if_dirty's first-frame pass, 1000:9A18, after shift_screen_up
 *   1000:99D8 ADDs the 0xC0C0 tag and copies the rows to +0x3200) into the
 *   dedicated low-VRAM band at A000:0x0000..0xDBF, shown at the screen
 *   bottom through the CRTC line compare ([0x5B0A] = 152).  The port's page
 *   is recomposed every tick, so the band is redrawn every tick instead.
 *
 * - The +0xC0 tag selects the DAC's 0xC0 bank: level_video_init
 *   (1000:32CD) copies _panel4's own palette's first 0xC0 bytes to 202F:0x1713
 *   = the live 256-entry palette's entries 0xC0..0xFF (0x14D3 + 0xC0*3).
 *   statusbar_palette() exposes that source; level.c installs it per tick.
 *
 * - Five digit marks (y 172..179), the bar's whole content: the countdown's
 *   minutes, seconds-tens, seconds-ones and the tribe counter's tens and
 *   ones.  A mark shows an 8x8 glyph cut from the PANELN1+PANELN2 _strip:
 *   the phase words are (digit+1)*16 glyph slots (build_statusbar_layout
 *   1000:961B) and mark row r is the _strip's 8 consecutive bytes at
 *   4*(phase + 2*r) — the seconds-tens mark reads at +0xC0 (PANELN2 = the
 *   _strip's second half).  The stamp (1000:9846) runs every frame.
 *
 * - statusbar_tick ports the INT 8 hook at 1000:96AD (armed by level_fade_in
 *   patching CS:[0x8B1C] := 0x339D): the tribe counter
 *   (tick_state_machine_5b22 1000:9767), the 70-frame second ([0x5B14]),
 *   the packed-decimal countdown with its slide animations
 *   ([0x5B10] = 2: 2 phase-bytes per 2 frames), and the time-up
 *   ([0x5B12] := 0, [0x5E44] := 0xFF).
 *
 * The _elapsed clock [0x5B0C..0E] is ported: one step per second in the same
 * gate (1000:96C6: ones 0..9, tens 0..5, minutes), reset by level_start
 * (1000:95B5), exposed as statusbar_elapsed_word() in the packing
 * check_highscore uses for the per-level best-time compare (1000:42CE).
 *
 * The line-compare reveal ([0x5B0A] 0xC9→0x98, one per tick) is ported:
 * `_bar_split` offsets the band, marks and action row; the time-up dialog
 * ([0x5E44] → the input_dispatch_loop pause branch) is menu_open_time_up /
 * menu_tick.  draw_level_text (0x342B) parks [0x5B0A] at 0xC9 for the level
 * end, which the port models as `statusbar_hide` (no composition at all,
 * since the bar is a display overlay there and the page shows the world).
 */
#include "statusbar.h"
#include "object.h"
#include "osystem.h"
#include "res.h"
#include "byteorder.h"

#include <stdlib.h>
#include <string.h>

#define STRIP_LEN     1280u /* PANELN1 (768) + PANELN2 (512), §12.7a    */
#define BAR_FIRST_ROW 152u /* the line-compare split, [0x5B0A] = 0x98  */
#define BAR_ROWS      40u /* _panel4's top 40 rows                      */
#define MARK_Y        172u /* bar row 20 (the destinations 0x6E2..)     */

/* [0x5B0A]: the bar's first screen row.  The original's line-compare reveal
 * starts at 201 (hidden) and the INT 8 hook 1000:331E decrements it to 0x98
 * (152) over 48 ticks; crtc_set_scroll sets the line compare to 2*row-1. */
static uint32_t _bar_split = 152u;

static const OSystem *_sb_osys;
static uint8_t *_strip; /* the four-plane panel _strip                */
static uint8_t *_panel4; /* LBM\PANEL4.BIT (the art + the palette)   */
static uint32_t _p4_size; /* its length: the charge bar reads 0x3EE0+  */
/* shift_screen_up's +0x3200 staging (1000:99D8): panel4's first 0xC800
 * bytes, +0xC0 tagged, with the charge trough drawn in by statusbar_charge_begin. */
static uint8_t _stage[0xC800];
static uint8_t *_icons3; /* APL\ICONS3.SPR: 12 x 16x24 (0x180 each)  */

static int32_t _active; /* [0x5B12]: the bar's ticking gate         */
/* The bar is a display overlay in the original (CRTC line compare); the
 * port composes it into the page, so [0x5B0A] beyond the screen has to mean
 * "do not compose".  draw_level_text (0x342B) parks it at 0xC9 for the level
 * end, and blit_bitmap_planes re-renders the world over those rows. */
static int32_t _bar_hidden;
static int32_t _time_up; /* [0x5E44]: the countdown reached zero     */

/* the countdown: values [0x5B19..1B], phases [0x5B1C/1E/20], slides
 * [0x5B16..18] — indices 0 = minutes, 1 = seconds-tens, 2 = seconds-ones */
static uint8_t _cd[3];
static uint16_t _cd_phase[3];
static uint8_t _cd_slide[3];
static uint8_t _sec_div; /* [0x5B14], seeded 0x46 = 70 frames       */
static uint8_t _slide_div; /* [0x5B10], seeded 2                      */

/* the _elapsed clock [0x5B0C..0E] = seconds ones (0..9), tens (0..5), minutes
 * (1000:96C6, one step per second; reset by level_start at 1000:95B5).  The
 * level-end score packs it as minutes<<8 | tens<<4 | ones for the per-level
 * best-time comparison (check_highscore 1000:42CE). */
static uint8_t _elapsed[3];

/* the tribe counter: values [0x5B25/26], phases [0x5B27/29], state
 * [0x5B22], slides [0x5B23/24] — indices 0 = tens, 1 = ones */
static uint8_t _td[2];
static uint16_t _td_phase[2];
static uint8_t _tc_state;
static uint8_t _tc_slide[2];

/* The mark x positions: the five destinations 0x6E2/0x6E6/0x6E8/0x728/0x72A
 * are bar row 20's bytes 2/6/8/72/74 at the 88-byte pitch; the flat page's
 * x = 4 * byte.  The seconds-tens mark (index 1) reads its glyphs at
 * +0xC0 — PANELN2, the _strip's cells 12..19. */
static const uint16_t _mark_x[5] = { 8, 24, 32, 288, 296 };
static const uint16_t _mark_bias[5] = { 0, 0xC0, 0, 0, 0 };

/* --- the action row (the bar's centre) ---------------------------------
 *
 * blit_sprite_loop (1000:990A) draws a *group* of icon slots selected by
 * [0x5B56] from the six word pointers at 202F:0x5B2B.  Each group is
 * `count, selected-cell, icon...` (the second byte is the 1-based selected
 * cell, stepped by SPACE 1000:3933) with 1-based ICONS3 cells; the shipped
 * groups are
 *
 *   1: 2 icons  02 01        2: 4 icons  03 04 05 06
 *   3: 4 icons  03 07 08 06  4: 4 icons  03 05 09 06
 *   5: 4 icons  09 0a 0b 0c  6: 1 icon   06
 *
 * so the level's start (group 1, [0x5B56] = 1) shows ICONS3 cells 1 and 0.
 * The row starts at x = (0x140 - (count*23 + (count-1)*12)) / 2, the slot
 * pitch is 0x1F and every slot sits at bar row 5 (1000:993B..9947).  Each
 * slot gets an embossed 23x31 bevel (1000:997D, its mask 0x48BC zeroed = a
 * pure shade: top/right -4, bottom/left +4, clamped to the bar bank's
 * 0xC0..0xCF greys) and the selected cell also the gold 21x29 inner frame at
 * +1,+1 (1000:996E from the 0x5B57 ramp d0..e3..d0, one ramp byte per pixel
 * or row; 1000:990A counts the header's second byte down to pick it).  The
 * icon (1000:9993) is the 16x24 cell at +4,+4: byte != 0 stored
 * +0xC0, 0 transparent.
 *
 * The group itself is the ACTION program at `0x5B37` (`[0x5B56]` selects it
 * through `202F:0x5B2B`): its first two bytes are `count, cursor`, and the
 * cursor is the byte SPACE steps and the script dispatcher reads.  The port
 * reads those bytes back through the same header for every draw, step and
 * dispatch (`object_action_byte`), so the highlighted cell and the executed op
 * cannot drift apart.  An id outside 1..6 has no header and draws no action
 * row (D-186). */
static const uint8_t _action_gold[40] = {
    /* 202F:0x5B57 */
    0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9,
    0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3,
    0xE3, 0xE2, 0xE1, 0xE0, 0xDF, 0xDE, 0xDD, 0xDC, 0xDB, 0xDA,
    0xD9, 0xD8, 0xD7, 0xD6, 0xD5, 0xD4, 0xD3, 0xD2, 0xD1, 0xD0,
};

int32_t statusbar_tribe_count_nonzero(void) { return (_td[0] | _td[1]) != 0; }
uint32_t statusbar_tribe_value(void) { return (uint32_t)_td[0] * 10u + _td[1]; }
uint32_t statusbar_tribe_digit(uint32_t i) { return _td[i & 1u]; }
void statusbar_set_tribe_digits(uint32_t tens, uint32_t ones)
{
    _td[0] = (uint8_t)(tens % 10u);
    _td[1] = (uint8_t)(ones % 10u);
}

void statusbar_save_params(uint8_t *out)
{
    if (!out) return;
    out[0] = _tc_state;
    /* [0x5B23] is the always-armed counter (it animates the ones digit,
     * [0x5B29]); [0x5B24] is the carry counter (the tens, [0x5B27]).  The
     * port's `_tc_slide[1]` is the always one, so the save slots swap -
     * D-108, closed 2026-11-05. */
    out[1] = _tc_slide[1];
    out[2] = _tc_slide[0];
    out[3] = _td[0];
    out[4] = _td[1];
    out[5] = (uint8_t)_td_phase[0];
    out[6] = 0u;
    out[7] = (uint8_t)_td_phase[1];
}

void statusbar_load_params(const uint8_t *in)
{
    if (!in) return;
    _tc_state = in[0];
    _tc_slide[1] = in[1]; /* [0x5B23] = the always-armed counter (D-108) */
    _tc_slide[0] = in[2]; /* [0x5B24] = the carry counter               */
    _td[0] = (uint8_t)(in[3] % 10u);
    _td[1] = (uint8_t)(in[4] % 10u);
    _td_phase[0] = in[5];
    _td_phase[1] = in[7];
}

int32_t statusbar_init(const OSystem *osys)
{
    _sb_osys = osys;
    return 0;
}

/**
 * @brief Write one 8-pixel mark row into the status band.
 *
 * @param page the page buffer
 * @param at the byte offset
 * @param x the x coordinate
 * @param src the source buffer
 */
static void _mark_row(uint8_t *page, uint32_t at, uint32_t x,
                      const uint8_t *src)
{
    for (uint32_t j = 0; j < 8u; j++)
        page[at * VIDEO_PAGE_W + x + j] = (uint8_t)(0xC0u | src[j]);
}

/** @brief stamp_statusbar_plane_bits (1000:9846) + copy_8rows_2bytes_to_vga
 * (1000:98D1): one mark = 16 consecutive _strip bytes laid out 8 px per
 * row, 8 rows.  The _strip values are the low nibbles {03,07,0E} = the
 * paper/antialias/ink greys; the +0xC0 tag carries them into the bank
 * level_video_init loaded.
 *
 * @param page the page buffer
 */
static void _stamp_marks(uint8_t *page)
{
    const uint16_t ph[5] = { _cd_phase[0], _cd_phase[1], _cd_phase[2],
                             _td_phase[0], _td_phase[1] };
    for (uint32_t m = 0; m < 5u; m++) {
        uint32_t base = 4u * (uint32_t)(ph[m] + _mark_bias[m]);
        for (uint32_t r = 0; r < 8u; r++) {
            uint32_t at = _bar_split + 20u + r;
            if (at >= VIDEO_PAGE_H_MODE13H) continue;
            _mark_row(page, at, _mark_x[m], _strip + base + 8u * r);
        }
    }
}

/** @brief The bar's background: _panel4's top 40 rows, +0xC0-tagged (shift_screen_up
 * 1000:99D8 + blit_area_if_dirty 1000:9A18).  The page's rows 192..199 stay
 * the world's — the original's visible mode is 192 rows and its page holds
 * unpainted memory below the band; the flat page has no such band, so the
 * present's crop decides (the 200-row page also serves the boot intro's
 * rows 182..197).
 *
 * @param page the page buffer
 */
static void _draw_band(uint8_t *page)
{
    for (uint32_t y = 0; y < BAR_ROWS; y++) {
        uint32_t at = _bar_split + y;
        uint8_t *dst;
        const uint8_t *src = _panel4 + y * 320u;

        if (at >= VIDEO_PAGE_H_MODE13H) continue;
        dst = page + at * VIDEO_PAGE_W;
        for (uint32_t x = 0; x < VIDEO_PAGE_W; x++)
            dst[x] = (uint8_t)(0xC0u | src[x]);
    }
}
void statusbar_charge_bar(uint32_t v)
{
    static const uint8_t shade[10] = { 0xDF, 0xE0, 0xE1, 0xE2, 0xE3,
                                       0xE3, 0xE2, 0xE1, 0xE0, 0xDF };
    uint8_t *page = _sb_osys ? _sb_osys->videoPage(0) : NULL;
    uint32_t i, r;

    if (!page || !_panel4) return; /* no bar art: nothing to draw */
    if (v < 0x100u) {
        uint32_t ax = 0x60u + (v >> 1); /* 0x372E's `SHR AX,1`        */
        uint32_t col = ax >> 2, p = ax & 3u;

        for (i = 0; i < 2u; i++) { /* 0x376D: CX = 2 passes      */
            uint32_t x = 4u * col + p;

            for (r = 0; r < 10u; r++) {
                uint32_t y = _bar_split + 15u + r;

                if (x < VIDEO_PAGE_W && y < VIDEO_PAGE_H_MODE13H)
                    page[y * VIDEO_PAGE_W + x] = shade[r];
            }
            if (++p == 4u) {
                p = 0;
                col++;
            }
        }
        return;
    }
    {
        uint32_t len = (0x1FFu - v) >> 1; /* 0x3726-0x372E              */
        uint32_t col = 24u + (len >> 2), p = len & 3u;
        /* 0x3EE0 in the +0x3200 staging = staged panel4 0xCE0 (the trough
         * is already in it, so the inset slices carry the trough edges). */
        uint32_t src = 0x0CE0u + len;

        if (src + 0x140u * 15u + 4u > sizeof _stage) return; /* past the art */
        for (i = 0; i < 2u; i++) {
            uint32_t x = 4u * col + p;

            for (r = 0; r < 15u; r++) {
                uint32_t y = _bar_split + 10u + r;
                uint8_t a = _stage[src + r * 0x140u + i];
                uint8_t b = _stage[src + r * 0x140u + i + 4u];

                if (x < VIDEO_PAGE_W && y < VIDEO_PAGE_H_MODE13H)
                    page[y * VIDEO_PAGE_W + x] = a;
                if (x + 4u < VIDEO_PAGE_W && y < VIDEO_PAGE_H_MODE13H)
                    page[y * VIDEO_PAGE_W + x + 4u] = b;
            }
            if (++p == 4u) {
                p = 0;
                col++;
            }
        }
    }
}

void statusbar_cycle_action(void) { object_step_action_cell(); /* 1000:3941 */ }

uint32_t statusbar_action_group(void) { return object_action_program(); }

uint32_t statusbar_action_count(uint32_t group)
{
    const uint8_t *h = object_program_header(group);

    return h ? (uint32_t)h[0] : 0u;
}

uint32_t statusbar_action_cell(uint32_t group)
{
    const uint8_t *h = object_program_header(group);

    return h ? (uint32_t)h[1] : 0u;
}

/** @brief 0xAFFE: one frame pixel — the mask byte when it is non-zero, else the
 * destination shaded by `d` and clamped to the bar bank.
 *
 * @param page the page buffer
 * @param x the x coordinate
 * @param y the y coordinate
 * @param d the offset
 * @param mask the mask
 */
static void _frame_px(uint8_t *page, uint32_t x, uint32_t y, int32_t d,
                      uint8_t mask)
{
    uint32_t at;
    int32_t v;
    if (x >= VIDEO_PAGE_W || y >= VIDEO_PAGE_H_MODE13H) return;
    at = y * VIDEO_PAGE_W + x;
    if (mask) {
        page[at] = mask;
        return;
    }
    v = (int32_t)page[at] + d;
    if (v >= 0xD0) v = 0xCF;
    if (v < 0xC0) v = 0xC0;
    page[at] = (uint8_t)v;
}

/** @brief 0xAF7D / 0xAF6E: the four edges in the original's order (top -4, left +4,
 * bottom +4, right -4).  Only the vertical runs advance the mask (0xAFF6's
 * INC SI per row); a horizontal run re-uses one byte for the whole edge
 * (0xAFF5 loops without touching SI), and the bottom edge reads the byte at
 * base+h because BX was stepped by the height first (0xAFAE).
 *
 * @param page the page buffer
 * @param x the x coordinate
 * @param y the y coordinate
 * @param w the width
 * @param h the height
 * @param mask the mask
 * @param base the base offset
 */
static void _frame_box(uint8_t *page, uint32_t x, uint32_t y, uint32_t w,
                       uint32_t h, const uint8_t *mask, uint32_t base)
{
    uint32_t i;
    uint8_t top = mask ? mask[base] : 0u;
    uint8_t bottom = mask ? mask[base + h] : 0u;

    for (i = 0; i < w; i++) _frame_px(page, x + i, y, -4, top);
    for (i = 0; i < h; i++)
        _frame_px(page, x, y + i, 4, mask ? mask[base + i] : 0u);
    for (i = 0; i < w; i++) _frame_px(page, x + i, y + h, 4, bottom);
    for (i = 0; i < h; i++)
        _frame_px(page, x + w, y + i, -4, mask ? mask[base + i] : 0u);
}

/**
 * @brief Draw one ACTION icon at (x, y).
 *
 * @param page the page buffer
 * @param x the x coordinate
 * @param y the y coordinate
 * @param cell the map cell
 */
static void _draw_action_icon(uint8_t *page, uint32_t x, uint32_t y,
                              uint32_t cell)
{
    const uint8_t *src;
    if (!_icons3 || cell >= 12u) return;
    src = _icons3 + cell * 0x180u;
    for (uint32_t r = 0; r < 24u; r++)
        for (uint32_t c = 0; c < 16u; c++) {
            uint8_t v = src[r * 16u + c];
            if (!v) continue;
            if (x + c < VIDEO_PAGE_W && y + r < VIDEO_PAGE_H_MODE13H)
                page[(y + r) * VIDEO_PAGE_W + x + c] = (uint8_t)(v + 0xC0u);
        }
}

/**
 * @brief Draw the action row's icons.
 *
 * @param page the page buffer
 */
static void _draw_actions(uint8_t *page)
{
    /* An id outside 1..6 has no header (D-186): the original resolves a
     * garbage pointer and draws from it, the port leaves the row blank. */
    if (!object_action_header()) return;
    /* Every step is a 16-bit word operation: the span/x below reproduce
     * `MUL BH` x2, `ADD`, `SUB 0x140`, `NEG`, `SHR 1`, and BP is the
     * original's do-while counter (count 0 draws 0xFFFF times). */
    uint32_t count = object_action_byte(0u);
    uint32_t cursor = object_action_byte(1u);
    uint16_t span = (uint16_t)((uint16_t)((uint8_t)count * 0x17u) +
                               (uint16_t)((uint8_t)(count - 1u) * 0x0Cu));
    uint16_t ax = (uint16_t)(VIDEO_PAGE_W - span); /* SUB 0x140 / NEG */
    uint16_t x = (uint16_t)(ax >> 1); /* AX is 16-bit and wraps below */
    uint32_t y = _bar_split + 5u;
    uint32_t i = 0;
    uint16_t bp = (uint16_t)count;

    do {
        _frame_box(page, x, y, 23u, 31u, NULL, 0u);
        /* 1000:990A: DI starts at the cursor and is decremented each slot,
         * so the gold 21x29 inner frame lands on cell `cursor - 1`. */
        if (cursor != 0u && i == cursor - 1u)
            _frame_box(page, x + 1u, y + 1u, 21u, 29u, _action_gold, 6u);
        _draw_action_icon(page, x + 4u, y + 4u,
                          (uint32_t)(object_action_byte(2u + i) - 1u));
        x = (uint16_t)(x + 4u + 0x1Fu); /* +4 then +0x1F, 16-bit */
        i++;
        bp--;
    } while (bp != 0u);
}

/** @brief 0x9A18's incremental path: the band's x 48..271 slice from the clean
 * panel art (the original copies 56 bytes at the +3 source stride per plane,
 * which covers x 48..271; the digit marks and the outer columns are left
 * alone).  Used at the charge entry and on the loop's exit.
 *
 * @param page the page buffer
 */
static void _draw_band_slice(uint8_t *page)
{
    for (uint32_t y = 0; y < BAR_ROWS; y++) {
        uint32_t at = _bar_split + y;

        if (at >= VIDEO_PAGE_H_MODE13H) continue;
        for (uint32_t x = 48u; x <= 271u; x++)
            page[at * VIDEO_PAGE_W + x] =
                (uint8_t)(0xC0u | _panel4[y * 320u + x]);
    }
}

void statusbar_charge_begin(void)
{
    uint8_t *page = _sb_osys ? _sb_osys->videoPage(0) : NULL;

    if (!page || !_panel4) return;
    _draw_band_slice(page);
    _frame_box(page, 0x5Fu, _bar_split + 0x0Cu, 0x82u, 0x0Fu, NULL, 0u);
    /* 1000:99D8's staging + the 0x9B7D trough: the second-half art at
     * 0x3EE0+len reads the staged trough, not clean panel4. */
    for (uint32_t i = 0; i < sizeof _stage; i++)
        _stage[i] = (uint8_t)(0xC0u | _panel4[i]);
    _frame_box(_stage, 0x5Fu, 0x0Cu, 0x82u, 0x0Fu, NULL, 0u);
}

void statusbar_charge_end(void)
{
    uint8_t *page = _sb_osys ? _sb_osys->videoPage(0) : NULL;

    if (!page || !_panel4) return;
    _draw_band_slice(page);
    _draw_actions(page);
}

/** @brief tick_state_machine_5b22 (1000:9767): the tribe counter.  The value
 * changes are instant; the phases then slide one 16-byte cell over 8
 * frames so the new digit rolls in.  [0x5B22]: 0 idle, 0xFF roll down
 * (armed by object_record_init 1000:F682 when a human is lost), 1 roll
 * up (no reachable arm), 2/3 slide in progress.
 */
static void _tribe_count_tick(void)
{
    /* the guard tests the word at [0x5B25]: both digits zero -> stop */
    uint16_t both = rd_le_u16(_td);

    switch (_tc_state) {
    case 0: return;
    case 2:
        if (_tc_slide[1]) {
            _td_phase[1] = (uint16_t)(_td_phase[1] - 2u);
            _tc_slide[1]--;
        }
        if (_tc_slide[0]) {
            _td_phase[0] = (uint16_t)(_td_phase[0] - 2u);
            _tc_slide[0]--;
        }
        if (!(_tc_slide[0] | _tc_slide[1])) _tc_state = 0;
        return;
    case 3:
        if (_tc_slide[1]) {
            _td_phase[1] = (uint16_t)(_td_phase[1] + 2u);
            _tc_slide[1]--;
        }
        if (_tc_slide[0]) {
            _td_phase[0] = (uint16_t)(_td_phase[0] + 2u);
            _tc_slide[0]--;
        }
        if (!(_tc_slide[0] | _tc_slide[1])) _tc_state = 0;
        return;
    case 1:
        if (both == 0x0909u) {
            _tc_state = 0;
            return;
        } /* 99: capped    */
        _tc_state = 3;
        _tc_slide[1] = 8;
        if (_td[1]++ == 9) {
            _td[1] = 0;
            _td_phase[1] = 0; /* roll up from the blank    */
            _tc_slide[0] = 8;
            if (_td[0]++ == 9) {
                _tc_state = 0;
                _tc_slide[0] = 0;
            }
        }
        return;
    default: /* 0xFF: the roll down      */
        if (both == 0) {
            _tc_state = 0;
            return;
        }
        _tc_state = 2;
        _tc_slide[1] = 8;
        if (_td[1]-- == 0) {
            _td[1] = 9;
            _td_phase[1] = 0xB0; /* slide down from cell 11  */
            _tc_slide[0] = 8;
            if (_td[0]-- == 0) {
                _tc_state = 0; /* 00: stop                 */
                _tc_slide[0] = 0;
            }
        }
        return;
    }
}

/* The per-second block of the 1000:96AD ticker: the packed-decimal
 * countdown.  Every second the ones digit decrements and its glyph slides
 * down one cell ([0x5B16] := 8 unconditionally); the carry rolls 0->9
 * (phase 0xB0, one cell above the target) and 0->5 (0x70, in PANELN2);
 * a minutes underflow is the time-up. */
/**
 * @brief 1000:96C6: the _elapsed clock's per-second step (ones 0..9, tens 0..5).
 */
static void _elapsed_tick(void)
{
    if (++_elapsed[0] == 10u) {
        _elapsed[0] = 0u;
        if (++_elapsed[1] == 6u) {
            _elapsed[1] = 0u;
            _elapsed[2]++;
        }
    }
}

/**
 * @brief Tick the status bar's clock and its time-up flag.
 */
static void _clock_tick(void)
{
    _cd_slide[2] = 8;
    if (_cd[2]-- == 0) {
        _cd[2] = 9;
        _cd_phase[2] = 0xB0;
        _cd_slide[1] = 8;
        if (_cd[1]-- == 0) {
            _cd[1] = 5;
            _cd_phase[1] = 0x70;
            _cd_slide[0] = 8;
            if (_cd[0]-- == 0) {
                /* TIME UP: [0x5B16..18] := 0, [0x5B12] := 0, [0x5E44] := 0xFF */
                _cd_slide[0] = _cd_slide[1] = _cd_slide[2] = 0;
                _active = 0;
                _time_up = 1;
            }
        }
    }
}

void statusbar_tick(void)
{
    uint8_t *page;

    if (!_sb_osys) return;
    if (_bar_hidden) return; /* 1000:342B: [0x5B0A] = 0xC9, no split */
    if (_bar_split > 152u) _bar_split--; /* 1000:331E, one per display period */
    page = _sb_osys->videoPage(0);
    if (!_strip || !_panel4 || !page) return;

    _draw_band(page);
    /* 1000:990A is not called inside the charge loop: the entry's 0x9A18
     * repaint (statusbar_charge_begin) has just erased the action row, and
     * the exit's 0x990A (statusbar_charge_end) brings it back. */
    if (!object_charge_active()) _draw_actions(page);

    if (_active) {
        int32_t died = 0;

        _tribe_count_tick();

        if (--_sec_div == 0) {
            _sec_div = 70; /* one second at 70 Hz      */
            _elapsed_tick(); /* 1000:96C6                */
            _clock_tick();
            died = _time_up; /* the original skips the
                                                  slide block on time-up  */
        }
        if (!died && --_slide_div == 0) {
            _slide_div = 2;
            if (_cd_slide[2]) {
                _cd_phase[2] = (uint16_t)(_cd_phase[2] - 2u);
                _cd_slide[2]--;
            }
            if (_cd_slide[1]) {
                _cd_phase[1] = (uint16_t)(_cd_phase[1] - 2u);
                _cd_slide[1]--;
            }
            if (_cd_slide[0]) {
                _cd_phase[0] = (uint16_t)(_cd_phase[0] - 2u);
                _cd_slide[0]--;
            }
        }
    }

    _stamp_marks(page);
}

void statusbar_start(uint16_t packedTimeBcd, uint32_t tribeMembers,
                     uint32_t tribeRequired)
{
    uint32_t n1Size = 0, n2Size = 0, i3Size = 0;
    uint16_t chk = 0;
    uint8_t *n1, *n2;
    uint8_t m = (uint8_t)(packedTimeBcd >> 8);
    uint8_t s = (uint8_t)(packedTimeBcd & 0xFFu);
    uint16_t diff;

    n1 = res_load_chunk("APL\\PANELN1.SPR", &n1Size, &chk);
    if (!n1 || n1Size != 768u) {
        free(n1);
        return;
    }
    n2 = res_load_chunk("APL\\PANELN2.SPR", &n2Size, &chk);
    if (!n2 || n2Size != 512u) {
        free(n1);
        free(n2);
        return;
    }
    free(_strip);
    _strip = malloc(STRIP_LEN);
    if (!_strip) {
        free(n1);
        free(n2);
        return;
    }
    memcpy(_strip, n1, 768u);
    memcpy(_strip + 768u, n2, 512u);
    free(n1);
    free(n2);

    /* level_video_init's load (1000:32AD): the +0xC0 paragraph stride puts
     * PANELN2 right after PANELN1 — one 1280-byte _strip (§12.7a)         */
    free(_panel4);
    _p4_size = 0;
    _panel4 = res_load_chunk("LBM\\PANEL4.BIT", &_p4_size, &chk);
    if (!_panel4 || _p4_size < 0xFA00u + 768u) {
        free(_panel4);
        _panel4 = NULL;
        return;
    }

    free(_icons3);
    _icons3 = res_load_chunk("APL\\ICONS3.SPR", &i3Size, &chk);
    if (!_icons3 || i3Size != 4608u) {
        free(_icons3);
        _icons3 = NULL;
    }

    _bar_split = 0xC9u; /* 1000:3040: hidden at level start */
    _bar_hidden = 0;
    _cd[0] = m;
    _cd[1] = (uint8_t)(s >> 4);
    _cd[2] = (uint8_t)(s & 0xFu);
    _cd_phase[0] = (uint16_t)((m + 1u) * 16u);
    _cd_phase[1] = (uint16_t)(((s >> 4) + 1u) * 16u);
    _cd_phase[2] = (uint16_t)(((s & 0xFu) + 1u) * 16u);
    _cd_slide[0] = _cd_slide[1] = _cd_slide[2] = 0;

    diff = (uint16_t)(tribeMembers - tribeRequired);
    _td[0] = (uint8_t)(diff / 10u);
    _td[1] = (uint8_t)(diff % 10u);
    _td_phase[0] = (uint16_t)((_td[0] + 1u) * 16u);
    _td_phase[1] = (uint16_t)((_td[1] + 1u) * 16u);
    _tc_state = 0;
    _tc_slide[0] = _tc_slide[1] = 0;

    _elapsed[0] = _elapsed[1] = _elapsed[2] = 0u; /* level_start 1000:95B5  */
    _sec_div = 70; /* [0x5B14] = 0x46          */
    _slide_div = 2; /* [0x5B10] = 2             */
    /* _active stays 0: [0x5B12] is level_fade_in's, set when the start chain
     * completes — the countdown must not run during the fade (see level.c) */
    _time_up = 0;
}

const uint8_t *statusbar_palette(void)
{ return _panel4 ? _panel4 + 0xFA00u : NULL; }

void statusbar_arm_tribe_count(void) { _tc_state = 0xFF; }

int32_t statusbar_time_up(void) { return _time_up; }

void statusbar_set_active(int32_t on) { _active = on ? 1 : 0; }
void statusbar_set_split(uint32_t row)
{
    _bar_split = row > VIDEO_PAGE_H_MODE13H ? VIDEO_PAGE_H_MODE13H : row;
    _bar_hidden = 0; /* an explicit split re-composes it */
}
void statusbar_hide(void)
{
    _bar_hidden = 1;
    _bar_split = 0xC9u;
}
uint32_t statusbar_split(void) { return _bar_split; }

uint16_t statusbar_time_word(void)
{
    return (uint16_t)(((uint32_t)_cd[0] << 8) |
                      (((uint32_t)_cd[1] & 0x0Fu) << 4) |
                      ((uint32_t)_cd[2] & 0x0Fu));
}

uint16_t statusbar_elapsed_word(void)
{
    return (uint16_t)(((uint32_t)_elapsed[2] << 8) |
                      (((uint32_t)_elapsed[1] & 0x0Fu) << 4) |
                      ((uint32_t)_elapsed[0] & 0x0Fu));
}
