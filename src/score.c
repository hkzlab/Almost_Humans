#include "score.h"
#include "font.h"
#include "gameflow.h"
#include "hiscore.h"
#include "level.h"
#include "object.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "statusbar.h"
#include "video.h"
#include "byteorder.h"
#include "width.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SCORE_PAGE VIDEO_PAGE_BYTES

/* --- 1000:46FC / 0x4697 / 0x46A3: the NEW RECORD stamp -------------------
 *
 * blink_tick toggles [0x1DC4]: 0 draws the off phase (0x46A3), 1 the on phase
 * (0x46FC).  0x46FC draws apl\record.spr ([0x1DDE]) at page byte 0x3CA0 +
 * [0x4106]: 0x18 rows x 64 bytes, a non-zero byte as 0xC0 + v, four Map-Mask
 * passes reading src[4j+p] into pixel 4j+p.  0x3CA0 = 176*88 + 32, so the
 * 64 x 24 stamp sits at row 176, px 128..191.
 *
 * 0x4697/0x46A3 are one body at 0x46AF with source and destination swapped:
 * ES and DS are both A000 (B8 00 A0 / 8E C0 / 8E D8), so the scratch is
 * A000:0000.  With GC Bit Mask 0x00 the zero mask bits select the latch, which
 * holds all four planes of the address just read: the copy carries the whole
 * 4-plane cell.  That is why the port saves and restores every pixel.
 */
#define STAMP_ROW 176u
#define STAMP_COL 32u

enum {
    SCORE_OFF,
    SCORE_WAIT,
    SCORE_FADE,
    SCORE_HOLD
};

/* --- check_highscore's data (1000:41B8..4352) ---------------------------
 *
 * `1000:41B8` recomputes the member count `[0x11FC]` from the bar's tribe counter
 * digits plus the live placed records:
 *
 *   [0x11FC] = [0x5B25]*10 + [0x5B26]
 *            + count of the eight 0x3E75 records whose +6 bit 7 is clear and
 *              whose +8 is neither 0x1D nor 0x1E
 *
 * then `[0x1DCA] += [0x1F41]*[0x11FC]*10` and the placed-object bonus (`0x41FA`),
 * `[0x1DCE] = remaining_seconds*10`, and the best-time table decides NEW RECORD:
 * the table is `DS:0x38A + level*2`, a 160-entry u16 array pre-filled with the
 * `0x0999` (9:99) sentinel - all 160 entries are that sentinel in the shipped
 * image, so the first win on any level is always a record.  The elapsed word is
 * `[0x1E84]` (minutes<<8 | BCD seconds, `statusbar_elapsed_word()`), compared with
 * `<`; a better time is stored back and `[0x1DD6] = 0xFF`.  The level's two terms
 * are then added into the running total `[0x592]/[0x594]`, with the displayed
 * copy `[0x1DD2]/[0x1DD4]` holding the *old* total - `1000:475D` then counts the
 * display up 10 at a time.
 */
static uint16_t _score_level_term;    // [0x1DCA]
static uint16_t _score_time_term;     // [0x1DCE]
static uint16_t _score_total_lo, _score_total_hi;       // [0x592]/[0x594]
static uint16_t _score_shown_lo, _score_shown_hi; // [0x1DD2]/[0x1DD4]
static uint16_t _score_delay; // [0x1DC6]
static uint16_t _score_screen_timer; /* [0x1DC8]: 0x15E, the screen's
                                                   * own countdown - exits when the
                                                   * sign bit sets */
static uint16_t _score_best_time; // [0x1E88]
static uint8_t _score_is_record; // [0x1DD6]
static uint8_t _score_blink = 0x00; /* [0x1DD7]: the image's 0, toggles on the
                                       * first decrement's borrow */
static uint8_t _score_blink_phase; // [0x1DC4]: the RECORD's on/off

static uint8_t *_score_bit;
static uint32_t _score_size;
static uint8_t *_score_font;
static uint8_t *_score_digits; // apl\SCORE.SPR (the 4x16 digit blocks)
static uint8_t *_score_record; // apl\RECORD.SPR (the NEW RECORD stamp)
static uint8_t *_score_num; // apl\SCORENUM.SPR (20 slots of 0x40)
static uint8_t _score_phase;
static uint8_t _score_armed;
static uint8_t _score_arm_fade; // the entry fade is still running
static uint16_t _score_hold;

/* 1000:46AF: 24 rows x 16 page bytes at the 88-byte stride, Map Mask 0x0F
 * and read map 0.  A page address is a 4-pixel group (plane p = pixel 4j+p),
 * so the copy carries the group's plane-0 pixel (the first of the four) and
 * writes it back to all four.
 *
 * The scratch itself is A000:0000, while the screen's page base ([0x4106])
 * is elsewhere (the live screen had 0x1230), so the scratch is NOT part of
 * the visible page.  The port keeps it in a private buffer of the same shape
 * instead of the flat page's top-left.
 *
 * The cell is saved and restored PER PIXEL: a live capture (level 1 with a
 * NEW RECORD) shows the off phase restoring the card byte-identically. */
static uint8_t _stamp_under[24u * 64u];

uint16_t score_members(void)
{
    uint8_t n = statusbar_tribe_value();
    uint16_t live = object_live_count();

    (void)live;
    for (uint16_t i = 0; i < object_static_slots(); i++) {
        const uint8_t *r = object_slot(i);

        if (!r || (r[6] & 0x80u)) continue;
        {
            uint16_t ty = rd_le_u16(r + 8);

            if (ty == 0x1Du || ty == 0x1Eu) continue;
        }
        n++;
    }
    return n;
}

void score_add_level_term(uint16_t v)
{ _score_level_term = (uint16_t)(_score_level_term + v); }

uint16_t score_level_term_for_probe(void) { return _score_level_term; }

/**
 * @brief 1000:41EA..4235: the level's two terms.
 */
static void _score_terms(void)
{
    uint16_t levelNo = gameflow_level_current() + 1u;
    uint16_t members = score_members();

    /* 1000:41E6: the recount is STORED into [0x11FC] before the terms use it
     * (1000:41EA `MUL word [0x11FC]`).  Nothing else in the image decrements
     * the tribe per death, so this store is what tightens the next level's
     * "need N humans" gate and the bar's spare count (D-106). */
    level_set_members(members);

    /* 1000:41F6 is an ADD: [0x1DCA] also carries whatever the handler-4
     * steps (1000:C9B9) added during play, so the term accumulates. */
    _score_level_term = (uint16_t)(_score_level_term + levelNo * members * 10u);
    for (uint16_t i = 0; i < object_static_slots(); i++) {
        uint8_t cell = (uint8_t)(object_record_map_cell(i));

        if (cell == 0xA8u)
            _score_level_term = (uint16_t)(_score_level_term + 1000u);
        else if (cell == 0xA7u)
            _score_level_term = (uint16_t)(_score_level_term + 2000u);
    }
    {
        uint16_t t = statusbar_time_word();
        uint8_t mins = (uint8_t)(t >> 8);
        uint8_t secs = (uint8_t)(((t >> 4) & 0xFu) * 10u + (t & 0xFu));

        _score_time_term = (uint16_t)((mins * 60u + secs) * 10u); // 1000:429B..
    }
}

void score_set_total(uint32_t lo, uint32_t hi)
{
    _score_total_lo = (uint16_t)lo;
    _score_total_hi = (uint16_t)hi;
}

void score_prepare(void)
{
    uint16_t levelNo = gameflow_level_current() + 1u;
    uint16_t elapsed = statusbar_elapsed_word(); // [0x1E84]
    uint16_t idx = levelNo;

    /* [0x1DCA] is NOT cleared here: the original only drains it through the
     * score screen (1000:4786..0x478D), so a lost level's term carries over. */
    _score_terms();
    _score_is_record = 0; // 1000:42E4
    if (elapsed < hiscore_best(idx)) { // 1000:42F9
        hiscore_set_best(idx, elapsed); // 1000:42FD
        _score_best_time = elapsed; // 1000:4301
        _score_is_record = 1; // 1000:4305
    }
    _score_shown_lo = _score_total_lo; // 1000:433A
    _score_shown_hi = _score_total_hi;
    {
        uint32_t total = ((uint32_t)_score_total_hi << 16) | _score_total_lo;

        total += _score_level_term; // 1000:4341
        total += _score_time_term; // 1000:4348
        _score_total_lo = (uint16_t)(total & 0xFFFFu);
        _score_total_hi = (uint16_t)(total >> 16);
    }
    _score_delay = 0x50u; // 1000:4327
    _score_screen_timer = 0x15Eu; // 1000:432D [0x1DC8]
}

/* 1000:467A blink_tick: while [0x1DD6] (NEW RECORD) is set, a 0x19-frame
 * counter toggles [0x1DC4], the RECORD sprite's on/off phase.  The two drawing
 * halves at 0x4697 / 0x46FC are ported by `score_stamp_draw`/`stampCopy`
 * (below), which keep the same 0x3CA0 address and the four plane passes. */
uint8_t score_blink_on(void) { return _score_blink_phase; }

uint8_t score_blink_tick(void)
{
    if (!_score_is_record) return 0;
    /* 1000:4685/0x468A: SUB 1 / JNS -> the borrow (bit 7) resets to 0x19 and
     * toggles; the image's counter starts at 0, so the first call toggles. */
    _score_blink = (uint8_t)(_score_blink - 1u);
    if (_score_blink < 0x80u) return 0;
    _score_blink = 0x19u;
    _score_blink_phase ^= 1;
    return _score_blink_phase;
}

uint8_t score_counters_run(void)
{
    if (_score_delay) {
        _score_delay--;
        return 1;
    }
    if (_score_level_term) {
        uint32_t shown = ((uint32_t)_score_shown_hi << 16) | _score_shown_lo;

        if (_score_level_term < 10u) {
            shown += _score_level_term; // 1000:478B..4798
            _score_level_term = 0;
        } else {
            _score_level_term = (uint16_t)(_score_level_term - 10u);
            shown += 10u; // 1000:479E
        }
        _score_shown_lo = (uint16_t)(shown & 0xFFFFu);
        _score_shown_hi = (uint16_t)(shown >> 16);
        return 1;
    }
    if (_score_time_term) {
        uint32_t shown = ((uint32_t)_score_shown_hi << 16) | _score_shown_lo;

        if (_score_time_term < 10u) {
            shown += _score_time_term;
            _score_time_term = 0;
        } else {
            _score_time_term = (uint16_t)(_score_time_term - 10u);
            shown += 10u;
        }
        _score_shown_lo = (uint16_t)(shown & 0xFFFFu);
        _score_shown_hi = (uint16_t)(shown >> 16);
        return 1;
    }
    /* both terms spent: the screen's own countdown runs.  It exits when the
     * sign bit sets, i.e. after 0x15E + 1 ticks (1000:47D2's [0x1DC8]--). */
    _score_screen_timer = (uint16_t)(_score_screen_timer - 1u);
    return (_score_screen_timer & 0x8000u) ? 0 : 1;
}

uint8_t score_is_record(void) { return _score_is_record; }
uint16_t score_best_for(uint32_t level) { return hiscore_best(level); }
uint16_t score_best_time(void) { return _score_best_time; }
uint16_t score_total_lo(void) { return _score_total_lo; }
uint16_t score_total_hi(void) { return _score_total_hi; }
uint32_t score_shown_lo(void) { return _score_shown_lo; }
uint32_t score_shown_hi(void) { return _score_shown_hi; }

uint32_t score_level_score(void)
{
    uint16_t t;
    uint32_t mins, secs, remaining, bonus;
    uint16_t levelNo;

    if (!level_outcome()) return 0u; // 55B3: a lost level adds none

    t = statusbar_time_word(); // [0x5B19/1A/1B]
    mins = (uint32_t)(t >> 8);
    secs = (((t >> 4) & 0xFu) * 10u + (t & 0xFu));
    remaining = mins * 60u + secs;

    levelNo = gameflow_level_current() + 1u; // [0x1F41]
    /* the original multiplies by the *recounted* [0x11FC] (1000:41B8: the
     * tribe counter digits plus the live records), not by the level-start tribe */
    bonus = levelNo * score_members() * 10u + remaining * 10u;

    /* 55FA..5635: the eight static records' cells, 0xA8 (+1000) / 0xA7 (+2000) */
    for (uint16_t i = 0; i < object_static_slots(); i++) {
        uint8_t cell = (uint8_t)(object_record_map_cell(i));
        if (cell == 0xA8u) bonus += 1000u;
        else if (cell == 0xA7u) bonus += 2000u;
    }
    (void)remaining;
    return bonus;
}

void score_stamp_draw(uint8_t *page, uint8_t on)
{
    uint32_t r, j;

    if (!page) return;
    if (!on) {
        /* 1000:46A3: A000:0000 -> the cell. */
        for (r = 0; r < 24u; r++)
            for (j = 0; j < 64u; j++) {
                uint8_t py = (uint8_t)(STAMP_ROW + r);
                uint16_t px = u16(STAMP_COL * 4u + j);

                if (py >= VIDEO_PAGE_H_MODE13H || px >= VIDEO_PAGE_W) continue;
                page[py * VIDEO_PAGE_W + px] = _stamp_under[r * 64u + j];
            }
        return;
    }
    /* 1000:4697: the cell -> A000:0000, then draw the sprite. */
    for (r = 0; r < 24u; r++)
        for (j = 0; j < 64u; j++) {
            uint8_t py = (uint8_t)(STAMP_ROW + r);
            uint16_t px = u16(STAMP_COL * 4u + j);

            _stamp_under[r * 64u + j] =
                (py < VIDEO_PAGE_H_MODE13H && px < VIDEO_PAGE_W)
                    ? page[py * VIDEO_PAGE_W + px]
                    : 0u;
        }
    /* 1000:46FC: apl\RECORD.SPR is chunky 64x24; the four passes read
     * src[4j+p] into plane p at address j, i.e. pixel 4j+p = src[4j+p].
     * A zero byte leaves the saved underlay alone. */
    for (r = 0; r < 24u; r++)
        for (j = 0; j < 64u; j++) {
            uint8_t v = _score_record ? _score_record[r * 64u + j] : 0u;
            uint8_t py = (uint8_t)(STAMP_ROW + r);
            uint16_t px = u16(STAMP_COL * 4u + j);

            if (!v || py >= VIDEO_PAGE_H_MODE13H || px >= VIDEO_PAGE_W)
                continue;
            page[py * VIDEO_PAGE_W + px] = (uint8_t)(v + 0xC0u);
        }
}

/** @brief 1000:462D / 1000:4578: the score screen's digit columns.
 *
 * `blit_score_block` (0x462D): `SI = digit*256` into SCORE.SPR (ten 0x100-byte
 * slots).  The page address is a 4-pixel group (plane p = pixel 4j+p).  Each
 * plane pass reads 16 rows x 4 bytes and pushes/pops only DI (and CX), NOT SI,
 * so pass p starts 64 bytes on and reads the slot's quarter p: the glyph is a
 * 16 x 16 chunky image stored plane-major, pixel 4k+p = src[p*64 + r*4 + k],
 * drawn as 0xC0 | v (`or_region_c0` 0x4278 biases the sheet).
 *
 * @param page the page buffer
 * @param digit the digit
 * @param x the x coordinate
 * @param y the y coordinate
 */
static void _score_digit_block(uint8_t *page, uint32_t digit, int32_t x,
                               int32_t y)
{
    const uint8_t *src;

    if (!_score_digits || digit > 9u) return; // ten 0x100-byte slots
    src = _score_digits + digit * 256u; // SI = AX = digit*0x100
    /* 1000:462D: 16 rows x 4 bytes per plane pass.  The pass pushes/restores
     * only DI, NOT SI, so pass p starts 64 bytes on and reads the slot's
     * quarter p: the glyph is a 16x16 chunky image stored plane-major,
     * pixel 4k+p = src[p*64 + r*4 + k].  The runtime OR of 0xC0 into the
     * sheet (0x4278) biases the colour, and every byte is stored (opaque). */
    for (uint16_t r = 0; r < 16u; r++)
        for (uint16_t px = 0; px < 16u; px++) {
            uint8_t v =
                (uint8_t)(src[(px & 3u) * 64u + r * 4u + (px >> 2)] | 0xC0u);
            int32_t dx = x + (int32_t)px;
            int32_t dy = y + (int32_t)r;

            if (dx >= 0 && dx < (int32_t)VIDEO_PAGE_W && dy >= 0 &&
                dy < (int32_t)VIDEO_PAGE_H_MODE13H)
                page[(uint32_t)dy * VIDEO_PAGE_W + (uint32_t)dx] = v;
        }
}

void score_column(uint8_t *page, uint32_t value, int32_t x, int32_t y,
                  uint32_t digits)
{
    uint32_t d = digits;

    if (!d) return;
    for (uint16_t i = 0; i < d; i++) {
        _score_digit_block(page, (value % 10u),
                           x + (int32_t)(4u * (d - 1u - i)) * 4, y);
        value /= 10u;
    }
}

void score_draw_columns(uint8_t *page)
{
    score_column(page, _score_level_term, 0xE0, 0x10, 5u); // (0xE0, 0x10)
    score_column(page, _score_time_term, 0xE0, 0x42, 5u); // (0xE0, 0x42)
    score_column(page, ((uint32_t)_score_shown_hi << 16) | _score_shown_lo,
                 0xA0, 0x74, 9u); // (0xA0, 0x74)
}

/** @brief 1000:44BB / 1000:450F: the elapsed and best times.
 *
 * `draw_scorenum_three_passes` (`0x44BB`) draws one word as **three digits** at
 * `(AX, BX)` on the 88-address page, `DI` addresses into the SCORENUM slot:
 *
 *   iVar2 = BX*88 + (AX>>2) + [0x4106]
 *   digit 1 = (DX>>8) & 0x0F        at iVar2 + 6
 *   digit 2 = (DX>>4) & 0x0F        at iVar2 + 8
 *   digit 3 =  DX      & 0x0F       at iVar2 + 10
 *
 * and `blit_scorenum_slot_plane` (`0x450F`) draws one slot: `DS:[0x1DDC]` +
 * `digit*0x40` + `BP`, **8 rows x 2 page addresses** (`src[0]` and `src[4]`,
 * `src += 8` per row) at `dst += 0x56` after the two stores (88 per row), each
 * byte stored as `v | 0xC0`.  The four plane passes take source byte +p, so
 * pixel x of a row is `src[r*8 + x]`: the slot is a chunky 8x8 image.
 *
 * So the two times are drawn with SCORENUM's 8x8 digits: `(0x50, 0xB5)` the
 * elapsed word `[0x1E84]` with `BP = 0` (slots 0..9) and `(0xF0, 0xB5)` the best
 * time `[0x1E88]` with `BP = 0x280` (slots 10..19) - one page address = 4 px,
 * so the digits sit at row 181, px 104..127 and px 264..287.
 *
 * @param page the page buffer
 * @param digit the digit
 * @param bp the SCORENUM slot base
 * @param off the page byte offset
 */
static void _score_num_block(uint8_t *page, uint32_t digit, uint32_t bp,
                             uint32_t off)
{
    uint16_t row = (uint16_t)(off / 88u), col = off % 88u;
    uint16_t px = col * 4u; // one address = 4 px

    if (!page || !_score_num || digit > 19u || row >= VIDEO_PAGE_H_MODE13H)
        return;
    /* 1000:450F: 8 rows x 2 addresses.  Pass p takes src[slot + r*8 + p] and
     * src[slot + r*8 + 4 + p], so pixel x of the row is src[r*8 + x]: the
     * slot is a chunky 8x8 image, stored as v | 0xC0. */
    for (uint16_t r = 0; r < 8u; r++) {
        uint16_t y = row + r;

        if (y >= VIDEO_PAGE_H_MODE13H) break;
        for (uint16_t x = 0; x < 8u; x++) {
            if (px + x >= VIDEO_PAGE_W) continue;
            page[y * VIDEO_PAGE_W + px + x] =
                (uint8_t)(_score_num[digit * 0x40u + bp + r * 8u + x] | 0xC0u);
        }
    }
}

void score_draw_num3(uint8_t *page, uint32_t ax, uint16_t value, uint32_t bx,
                     uint32_t bp)
{
    uint16_t off = (uint16_t)(bx * 88u + (ax >> 2));

    _score_num_block(page, ((value >> 8) & 0x0Fu), bp, off + 6u);
    _score_num_block(page, ((value >> 4) & 0x0Fu), bp, off + 8u);
    _score_num_block(page, (value & 0x0Fu), bp, off + 10u);
}

void score_draw_card(uint8_t *page)
{
    if (!page || !_score_bit || _score_size < VIDEO_PAGE_BYTES) return;
    /* 1000:4403: SI walks the chunky 320-byte row at stride 4 and DI is the
     * 4-px page group, so pixel 4j+p = src[4j+p] | 0xC0.  A zero byte is LEFT
     * ALONE (the level caption). */
    for (uint16_t i = 0; i < VIDEO_PAGE_BYTES; i++) {
        uint8_t v = _score_bit[i];

        if (v) page[i] = (uint8_t)(v + 0xC0u);
    }
}

uint8_t score_begin(uint32_t value)
{
    if (_score_phase != SCORE_OFF) return 1;

    score_prepare(); // 1000:41B8..4352: the terms, best, totals

    /* 1000:428D: `MOV BX,0x128F; CALL 0x88E9` (crtc_write) sets CRTC 0x12 =
     * 0x8F, the 400-scanline vertical display, i.e. the 320x200 field - the
     * score card's.  (The full mode set at 0x8300 is called at the *end*,
     * 0x439F, for the newspaper.) */
    video_set_mode(VIDEO_MODE_13H);
    (void)value;
    res_set_disc(1u); // the score assets' disc
    free(_score_bit);
    _score_bit = NULL;
    _score_size = 0;
    _score_bit = res_load_chunk("LBM\\SCORE.BIT", &_score_size, NULL);
    uint8_t *page = video_page(0);
    /* The page is NOT cleared: check_highscore draws the card straight over
     * whatever the level's caption left on it (draw_level_text 0x33A3 ends
     * with the page drawn and the DAC faded to black), and
     * reblit_framebuffer_88 skips a zero source byte. */
    /* 1000:4278..4284 or_region_c0 runs with ES = [0x1DDA] = APL\SCORE.SPR,
     * BP = 0, BX = 0x100 and CX = 0xA, i.e. it ORs 0xC0 over BX*CX = 0xA00
     * bytes = the sheet's ten 0x100-byte digit slots, not into the page.
     * _score_digit_block applies the same bias itself, so the result matches. */
    if (_score_bit) {
        score_draw_card(page);
        if (_score_size >= SCORE_PAGE + 768u) {
            /* 1000:4333 palette_build_fade(0x100, 0, 0x23, 0x14D3): the screen
             * FADES IN from the level's (black) palette to 0x14D3, whose
             * 0xC0..0xFF bank is the card's own 64 colours - the file's first
             * 64 entries.  The fade is stepped by the tick and waited out
             * before the input loop. */
            static uint8_t fadeTarget[768];

            /* 0x4333 targets 0x14D3, the LEVEL palette (sealed to grey), not
             * the image working palette 0x4BBC.  The original's two prep
             * steps (1000:4287/0x428A) run on 0x14D3 first: entries
             * 0x80..0xDF become grey and the card's 64 entries take
             * 0xC0..0xFF. */
            level_palette_score_prepare(_score_bit + SCORE_PAGE);
            memcpy(fadeTarget, level_palette(), sizeof fadeTarget);
            palette_build_fade(0u, 0x100u, 0x23u, fadeTarget);
        }
        _score_arm_fade = 1;
    }
    free(_score_font);
    _score_font = res_load_chunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(_score_font);
    free(_score_digits);
    _score_digits = res_load_chunk("APL\\SCORE.SPR", NULL, NULL); // 1000:4264
    free(_score_record);
    _score_record = res_load_chunk("APL\\RECORD.SPR", NULL, NULL); // 1000:4276
    free(_score_num);
    _score_num = res_load_chunk("APL\\SCORENUM.SPR", NULL, NULL); // 1000:4269

    /* 1000:430E/431A draw_scorenum_three_passes: the elapsed time at
     * (0x50, 0xB5) and the best time at (0xF0, 0xB5), both row 181. */
    score_draw_num3(page, 0x50u, statusbar_elapsed_word(), 0xB5u, 0u);
    score_draw_num3(page, 0xF0u, _score_best_time, 0xB5u, 0x280u);

    /* 1000:4471: the three counter columns (the level term, the time term and
     * the running total) in SCORE.SPR's digits. */
    score_draw_columns(page);
    /* The NEW RECORD stamp is apl\record.spr at row 176, px 128..191
     * (0x3CA0 = 176*88 + 32); blink_tick redraws it. */

    _score_phase = SCORE_WAIT;
    _score_armed = 0;
    return 1;
}

uint8_t score_tick(const InputState *in)
{
    static const uint8_t black[768];
    switch (_score_phase) {
    case SCORE_WAIT:
        /* The port's caller hands in the same InputState that dismissed the end
         * page, so the first tick skips the key test; the original calls
         * poll_input inside its loop and so never sees that key. */
        if (_score_arm_fade) { // 1000:4333's wait_palette_fade_done
            if (palette_fade_active()) return 1;
            _score_arm_fade = 0;
        }
        if (!_score_armed) {
            _score_armed = 1;
            return 1;
        }
        {
            uint8_t *page = video_page(0);
            uint8_t alive;

            /* tick_score_counters (1000:475D): draw_score_columns, blink_tick,
             * then one 10-point step.  The drawing happens every frame. */
            if (page) score_draw_columns(page);
            /* 1000:467A: the expiry toggles [0x1DC4] and runs the *new*
             * phase's handler - 1000:46A3 restores the cell (off), 1000:46FC
             * saves it and draws the sprite (on).  Both halves run. */
            {
                uint8_t was = score_blink_on();

                (void)score_blink_tick();
                if (page && score_blink_on() != was)
                    score_stamp_draw(page, score_blink_on());
            }
            alive = score_counters_run();

            /* check_highscore's loop: while ([0x1DC8] & 0x8000) == 0, and it
             * also breaks on the fire bit ([0x5ACE] & 0x80) or [0x48A3].  The
             * screen ends by itself after 0x15E ticks. */
            if (!alive || in->lastKey || (in->joyBits & 0x80u)) {
                palette_build_fade(0u, 0x100u, 0x23u, black); // 1000:438A
                _score_phase = SCORE_FADE;
            }
        }
        return 1;
    case SCORE_FADE:
        if (palette_fade_active()) return 1;
        _score_phase = SCORE_HOLD;
        _score_hold = 0x4Bu; // present_frames_75
        return 1;
    case SCORE_HOLD:
        if (_score_hold > 0u) {
            _score_hold--;
            return 1;
        }
        _score_phase = SCORE_OFF;
        /* 1000:439F's tail: [0x4105] = 0 and [0x1DC4] = 0 - the stamp's phase
         * is cleared as the mode goes back to 13h. */
        _score_blink_phase = 0;
        free(_score_bit);
        _score_bit = NULL;
        _score_size = 0;
        free(_score_font);
        _score_font = NULL;
        free(_score_record);
        _score_record = NULL;
        free(_score_num);
        _score_num = NULL;
        font_publish(NULL);
        return 0;
    default: return 0;
    }
}
