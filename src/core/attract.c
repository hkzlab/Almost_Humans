#include "attract.h"
#include "font.h"
#include "game_strings.h"
#include "hiscore.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "game_resources.h"
#include "video.h"
#include "byteorder.h"
#include "width.h"

#include <stdint.h>
#include <string.h>

/* --- the machine --------------------------------------------------------- */

/* --- the page pair and the CRTC start (the planar space is video.c's) ---- */
static uint16_t _page_draw, _page_disp; // [0x5AC4]/[0x5AC6]
static uint16_t _display_start; // [0x4106]: the CRTC start (bytes)
static uint16_t _saved_draw, _saved_disp; // game_loop's PUSH pair (1387)

/* the strings at 202F:0xC38/0xC4D/0xC73/0xC79 are
 * game_strings' ATTRACT entries */

/* the 0x48BC ramp analog: it enters the attract zeroed (the boot's
 * clear_fade_ramp) and nothing in the attract ever writes it, so every
 * "fade to 0x48BC" is a fade to black. */
static const uint8_t _ramp0[768];

/* --- the module state --------------------------------------------------- */
static AttractPhase _phase = ATTRACT_IDLE;
static uint16_t _counter; // the multi-tick _phase countdown
static uint8_t _frame; // effect 2's _frame: 0 = scores, 1..16
static uint8_t _wipe_iter; // effect 2's 79-step wipe iteration
static uint16_t _d3b; // [0xD3B]: the slide's display row
static uint8_t *_img_buf; // [0xC32]: the loaded image
static uint8_t *_text_spr; // [0x1E1C] analog: the loaded font
static uint8_t _colour1881 = 0x43; // CS:0x1881: _blit_80x200's colour base

/* --- effect 4's credits strip ---------------------------------------------
 * The 192-KB work area [0xD76] (clear_bitmaps_6 1000:1B3B, re-zeroed per
 * screenful): a vertical bitmap of 80-byte rows - [0] the row signature,
 * [1] the write-mode-2 bit mask, [2..19] pad, [20..79] the presented 60
 * bytes = the text arena's pixels 80..319. */
#define E4_STRIP_BYTES 196608u
static uint8_t *_e4strip; // allocated when effect 4 loads, freed on exit
static uint8_t
    _row_sig[VIDEO_PAGE_H_MODE13H]; // CS:0x1CF7: the row-signature cache
static uint8_t _row_sig_init; // the shipped 0x80 fill runs once
static uint8_t *_credtext; // [0xC9F]: the loaded _credtext.bin
static uint32_t _credtext_size;
static uint32_t _cred_cursor; // [0xD6C]: the _credtext cursor
static uint16_t _rows_built; // [0xD39]: the strip rows built
static uint16_t _scroll_row; // [0xD78]: the scroll window's start row

/* --- effect 1's wipe: intro_wipe_step (1000:14C3) -----------------------
 * The damped oscillator on [0xC35]=pos, [0xC36]=dir (0=shrink), [0xC37]=speed:
 * trace 199->0 (speed 32), grow 16 -> peak 136, shrink -> 0, grow 8 -> 36,
 * -> 0, 4 -> 10, 2 -> 3, 1 -> 1, 0 -> exit (pos==0 && dir==0). */
static uint8_t _wipe_pos, _wipe_speed;
static uint8_t _wipe_dir;

/** @brief 202F:0x38C: the per-level best times, as the attract shows them.
 *
 * 160 records of [seconds][minutes] (two pseudo-decimal nibbles / one digit),
 * shipped as 99 09 ("9:99") for every level; hiscore_best(level) is the same
 * 160 u16 the score screen compares against.
 *
 * @param levelMinus1 the zero-based level
 * @param rec the record
 */
static void _fastest_time(uint32_t levelMinus1, uint8_t rec[2])
{
    uint16_t v = hiscore_best(levelMinus1 + 1u);

    wr_le_u16(rec, v); // [seconds][minutes]
}

int32_t attract_init(void) { return 0; }

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
uint32_t attract_phase_for_probe(void) { return (uint32_t)_phase; }

const uint8_t *attract_strip_for_probe(uint32_t *rowsOut)
{
    if (rowsOut) *rowsOut = _rows_built;
    return _e4strip;
}
#endif /* HUMANSRE_TEST_PROBES */

/* --- the VRAM primitives are video.c's (shared A000 model) -------------- */

/** @brief begin_page_flip (1000:8821) + the swap (1000:87C7): the display target is
 * the drawing page, then the pair swaps.  The original's spin until the ISR
 * applies [0x4106] is implicit: the port composes from _display_start at the
 * tick's present.
 */
static void _flip_page(void)
{
    uint16_t t = _page_draw;
    _display_start = _page_draw;
    _page_draw = _page_disp;
    _page_disp = t;
}

/**
 * @brief _wait_page (1000:195B): _display_start := [0xD3B]*80, no page-pair change.
 *
 * @param row the row
 */
static void _wait_page(uint16_t row) { _display_start = u16(row * 80u); }

/**
 * @brief The present: the 200-row window at _display_start, flat.
 */
static void _compose_window(void)
{
    video_vram_present(video_page(0), _display_start, 80u, 0u,
                       VIDEO_PAGE_H_MODE13H);
}

/**
 * @brief One step of the attract wipe effect.
 */
static void _wipe_step(void)
{
    uint8_t pos = _wipe_pos, speed = _wipe_speed;
    if (_wipe_dir) { // grow: 14e0
        pos += speed;
        if (pos > 0xC7u) pos = 0xC7u;
        if (speed == 0u) {
            _wipe_dir = 0;
            _wipe_speed = 0u;
            _wipe_pos = pos;
            return;
        }
        speed--;
    } else { // shrink: 14f6
        int32_t diff = (int32_t)pos - (int32_t)speed; // SUB AX,CX flags
        uint32_t r = (uint32_t)diff;
        if (diff <= 0 || diff > 0xFF) { // negative or exact zero
            _wipe_pos = 0u;
            _wipe_dir = 1;
            _wipe_speed = speed >> 1;
            return;
        }
        pos = (uint8_t)(r);
        if (speed < 0x20u) speed++;
    }
    _wipe_pos = pos;
    _wipe_speed = speed;
}

/* --- the text helpers --------------------------------------------------- */

/** @brief _format_decimal (1000:0AEC): value DX:CX; the digits are written
 * least-significant first and the whole run is reversed once (0x0B3B), so
 * `>= 10000` is the quotient's digits followed by exactly four zero-padded
 * remainder digits, and below it the plain digits.
 *
 * @param v the value
 * @param out the output buffer
 */
static void _format_decimal(uint32_t v, char *out)
{
    uint8_t n = 0, m = 0;
    char t[8];

    if (v >= 10000u) {
        uint32_t hi = v / 10000u, lo = v % 10000u;

        for (uint16_t i = 0; i < 4u; i++) { // 1000:0B0E: four LSB-first
            t[m++] = (char)('0' + lo % 10u);
            lo /= 10u;
        }
        while (hi) {
            t[m++] = (char)('0' + hi % 10u);
            hi /= 10u;
        }
    } else {
        if (!v) t[m++] = '0';
        while (v) {
            t[m++] = (char)('0' + v % 10u);
            v /= 10u;
        }
    }
    while (m) out[n++] = t[--m]; // 1000:0B3B: one reversal of the lot
    out[n] = 0;
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
void attract_format_decimal_for_probe(uint32_t v, char *out)
{ _format_decimal(v, out); }
#endif /* HUMANSRE_TEST_PROBES */

/** @brief _draw_score_block (1000:0651): [0x36]=0x10, [0x38]=0x18 (set by
 * intro_frame_1); rank x=0x20, name x=0x82, score x=0x102.
 *
 * @param ar the 80x200 source arena
 */
static void _draw_score_block(uint8_t *ar)
{
    uint8_t row = 0x18u;
    for (uint16_t k = 0; k < 10u; k++) {
        char buf[12];
        _format_decimal(k + 1u, buf);
        font_draw_text(ar, buf, 0x20u, row);
        font_draw_text(ar, hiscore_name(k), 0x82u, row);
        _format_decimal(hiscore_score(k), buf);
        font_draw_text(ar, buf, 0x102u, row);
        row += 0x10u;
    }
}

/** @brief draw_number_hex's string builders (1000:1693..16D0): the 3-digit level
 * number, the minutes digit behind 0xC73's '0', the seconds nibbles.
 *
 * @param ar the 80x200 source arena
 * @param levelMinus1 the zero-based level
 * @param rec the record
 * @param row the row
 */
static void _draw_times_row(uint8_t *ar, uint32_t levelMinus1,
                            const uint8_t *rec, uint32_t row)
{
    uint16_t v = (uint16_t)(levelMinus1 + 1u);
    char num[5], mins[3], secs[3];
    num[0] = (char)('0' + v / 100u);
    num[1] = (char)('0' + (v / 10u) % 10u);
    num[2] = (char)('0' + v % 10u);
    num[3] = 0;
    mins[0] = '0';
    mins[1] = (char)('0' + rec[1]);
    mins[2] = 0;
    secs[0] = (char)('0' + (rec[0] >> 4));
    secs[1] = (char)('0' + (rec[0] & 0xFu));
    secs[2] = 0;
    font_draw_text(ar, game_string(GAME_STRING_ATTRACT_TIMES_LABEL), 0x92u,
                   row); // 16D3, redrawn per row
    font_draw_text(ar, num, 0x3Du, row);
    font_draw_text(ar, mins, 0x74u, row);
    font_draw_text(ar, secs, 0xD8u, row);
}

/* --- effect 2's present path -------------------------------------------- */

/**
 * @brief Splat the four bits of one 1bpp byte as four pixels.
 *
 * @param d the destination pixels
 * @param b the source byte
 * @param dl the colour
 */
static void _splat_bits(uint8_t *d, uint8_t b, uint32_t dl)
{
    for (uint16_t bit = 0; bit < 4u; bit++) {
        if (b & 1u) d[bit] = (uint8_t)dl;
        b >>= 1;
    }
}

/** @brief _blit_80x200 (1000:17FB): the arena's bits 0..3 (4 px per byte, bit 0 the
 * leftmost) splat colour over video_flat(), 320 B per row from dest offset 4.
 *
 * @param ar the 80x200 source arena
 */
static void _blit_80x200(const uint8_t *ar)
{
    uint8_t *flat = video_flat();
    uint8_t dl = 0xFDu;
    for (uint16_t row = 0; row < VIDEO_PAGE_H_MODE13H; row++) {
        uint8_t *d = flat + 4u + row * VIDEO_PAGE_W;
        const uint8_t *s = ar + row * 80u;
        /* The +4 offset is the original's +1 address byte: the last group
         * spills into the next row, and row 199's would go past the window
         * (the original lands it at A000 base+16000, never displayed). */
        uint16_t groups = (row + 1u < VIDEO_PAGE_H_MODE13H) ? 80u : 79u;

        for (uint16_t k = 0; k < groups; k++) _splat_bits(d + k * 4u, s[k], dl);
        uint16_t cx = VIDEO_PAGE_H_MODE13H - row; // the pushed row countdown
        if (cx > 0xB0u) dl = 0xF0u | ((dl + 1u) & 0xFu);
        else if (cx == 0xB0u) dl = _colour1881;
        else {
            dl = (dl + 1u) & 0xFFu;
            if (!dl) dl = 0x40u;
        }
    }
    /* the tail: [CS:0x1881] += 0xA0, +0x40 while < 0x40 (1862) */
    _colour1881 = (uint8_t)((_colour1881 + 0xA0u) & 0xFFu);
    if (_colour1881 < 0x40u) _colour1881 = (uint8_t)(_colour1881 + 0x40u);
}

/** @brief blit_sprite_planes (1000:1795): an 8x16 chunk of video_flat() planarised
 * into A000 at _page_draw + row*80 + x/4, plane (x+j)&3.  The source/dest read
 * past a row's edge into the next row exactly as the original's flat
 * offsets do (the right-sweeping columns start at 319).
 *
 * @param x the x coordinate
 * @param row the row
 */
static void _blit_strip8(uint32_t x, uint32_t row)
{
    const uint8_t *flat = video_flat();

    for (uint16_t j = 0; j < 8u; j++) {
        uint32_t px = x + j;
        for (uint16_t r = 0; r < 16u; r++) {
            uint16_t sy = (uint16_t)(row + r);
            uint8_t v = flat[sy * VIDEO_PAGE_W + px];
            video_vram_group(
                (uint16_t)(_page_draw + sy * 80u + (px >> 2)))[px & 3u] = v;
        }
    }
}

/** @brief _wipe_row (1000:1722): eleven 16-row chunks tiling rows 8..183,
 * alternating between the right-sweeping column (the arg_swap variant,
 * x = 319-4k) and the left-sweeping one (x = 4k) - the criss-cross.
 *
 * @param k the chunk index, 0..10
 */
static void _wipe_row(uint32_t k)
{
    static const uint32_t rows[11] = { 0x8u,  0x18u, 0x28u, 0x38u, 0x48u, 0x58u,
                                       0x68u, 0x78u, 0x88u, 0x98u, 0xA8u };
    uint16_t xs = u16(4u * k);
    uint16_t xd = (uint16_t)(0x13Fu - 4u * k);
    for (uint16_t i = 0; i < 11u; i++)
        _blit_strip8((i & 1u) ? xs : xd, rows[i]);
}

/* --- the per-effect loads ------------------------------------------- */

/** @brief Drop the current effect image. */
static void _unload_img(void)
{
    g_osys->memFree(_img_buf);
    _img_buf = NULL;
}

/**
 * @brief effect 1 (1000:1406): the load + the wipe setup.  Returns 0 = skip.
 *
 * @return 0 when the effect is skipped, 1 when it runs
 */
static int32_t _e1_load(void)
{
    video_vram_clear();
    uint32_t sz = 0;
    _unload_img();
    _img_buf =
        res_load_chunk(game_resource_name(GAME_RES_LBM_GAMEINT_BIT), &sz, NULL);
    if (!_img_buf || sz < VIDEO_PAGE_BYTES + 768u) {
        _unload_img();
        return 0;
    }

    palette_load_working(_img_buf + VIDEO_PAGE_BYTES);
    video_vram_planarise(_img_buf, UINT32_MAX, _page_draw, 80u, 80u,
                         VIDEO_PAGE_H_MODE13H);
    _flip_page(); // the image at 0 and
    video_vram_planarise(_img_buf, UINT32_MAX, _page_draw, 80u, 80u,
                         VIDEO_PAGE_H_MODE13H);
    _flip_page(); // at 0x8000

    _page_draw = 0x3E80u; // 142F/1435: the blank
    _page_disp = 0xBE80u; // start, past the copy
    _flip_page();

    _wipe_pos = 0xC7u;
    _wipe_dir = 0;
    _wipe_speed = 0x20u; // 143e: 199/shrink/32
    palette_build_fade(0u, 256u, 0x46u, palette_working());
    return 1;
}

/**
 * @brief effect 3 (1000:18CA + intro_setup 1000:1903).  Returns 0 = skip.
 *
 * @return 0 when the effect is skipped, 1 when it runs
 */
static int32_t _e3_load(void)
{
    video_vram_clear();
    uint32_t sz = 0;
    _unload_img();
    _img_buf =
        res_load_chunk(game_resource_name(GAME_RES_LBM_PCTITLE_RAW), &sz, NULL);
    if (!_img_buf || sz < 112768u) {
        _unload_img();
        return 0;
    }

    video_vram_planarise(_img_buf, UINT32_MAX, 0x3E80u, 80u, 80u,
                         VIDEO_PAGE_H_MODE13H); // the title: rows 200-400
    video_vram_planarise(_img_buf + 48000u, UINT32_MAX, 0x6D60u, 80u, 80u,
                         VIDEO_PAGE_H_MODE13H); // the promo: rows 350-550
    palette_load_working(_img_buf + 112000u);
    _wait_page(_d3b); // the stale [0xD3B]
    palette_build_fade(0u, 256u, 0x23u, palette_working());
    return 1;
}

/**
 * @brief effect 2 (1000:1514): the greyscale gameint + scorepal.  Returns 0 = skip.
 *
 * @return 0 when the effect is skipped, 1 when it runs
 */
static int32_t _e2_load(void)
{
    _page_draw = 0u;
    _page_disp = 0x8000u;
    uint32_t sz = 0;
    _unload_img();
    _img_buf =
        res_load_chunk(game_resource_name(GAME_RES_LBM_GAMEINT_BIT), &sz, NULL);
    if (!_img_buf || sz < VIDEO_PAGE_BYTES + 768u) {
        _unload_img();
        return 0;
    }

    palette_load_working(_img_buf + VIDEO_PAGE_BYTES);

    /* blit_fullscreen 1000:15B1: two 75-pixel, 200-row strips */
    for (uint16_t r = 0; r < VIDEO_PAGE_H_MODE13H; r++) {
        memcpy(_img_buf + r * VIDEO_PAGE_W, _img_buf + 0x9Eu + r * VIDEO_PAGE_W,
               75u);
        memcpy(_img_buf + 0xF5u + r * VIDEO_PAGE_W,
               _img_buf + 0x55u + r * VIDEO_PAGE_W, 75u);
    }
    /* decode_page_in_place 1000:15E6 + hash_three_bytes 1000:43D5: the
     * luma conversion through gameint's own palette */
    {
        const uint8_t *pal = palette_working();
        for (uint16_t i = 0; i < VIDEO_PAGE_BYTES; i++) {
            const uint8_t *p = pal + _img_buf[i] * 3u;
            /* hash_three_bytes' CH = (77R+151G+28B)>>8, then SHR CH,1 */
            _img_buf[i] =
                (uint8_t)((77u * p[0] + 151u * p[1] + 28u * p[2]) >> 9);
        }
    }
    video_vram_planarise(_img_buf, UINT32_MAX, _page_draw, 80u, 80u,
                         VIDEO_PAGE_H_MODE13H);
    _flip_page();
    video_vram_planarise(_img_buf, UINT32_MAX, _page_draw, 80u, 80u,
                         VIDEO_PAGE_H_MODE13H);
    _flip_page();

    uint8_t *palBuf = res_load_chunk(
        game_resource_name(GAME_RES_LBM_SCOREPAL_PAL), &sz, NULL);
    if (palBuf) {
        palette_load_working(palBuf);
        g_osys->memFree(palBuf);
    }
    palette_build_fade(0u, 256u, 0x1Eu, palette_working());
    return 1;
}

/** @brief _fill_pattern (1000:1AFC) - draw_char_advance's blank path: the tile's 16
 * rows of [sig 0xFF, mask 0x20]; the data bytes keep the zeroed strip.
 *
 * @param tile the tile's base row in the strip
 */
static void _fill_pattern(uint8_t *tile)
{
    for (uint16_t r = 0; r < 16u; r++) {
        tile[r * 80u] = 0xFFu;
        tile[r * 80u + 1u] = 0x20u;
    }
}

/** @brief _fill_pattern_clip (1000:1B13) - the text path: rows 0..3 the [0xFF, 0xE0]
 * band above the text (the presenter clamps the background to index & 0x1F
 * there, erasing stale ink), rows 4..14 the [0..10, 0x20] per-row
 * signatures (adjacent glyph rows always mismatch the cache, so ink rows
 * are always redrawn).  The original's dead MOV BX,0xD2E (1b1a - BX pushed
 * and never read) is dropped.
 *
 * @param tile the tile's base row in the strip
 */
static void _fill_pattern_clip(uint8_t *tile)
{
    for (uint16_t r = 0; r < 15u; r++) {
        tile[r * 80u] = (r < 4u) ? 0xFFu : (uint8_t)(r - 4u);
        tile[r * 80u + 1u] = (r < 4u) ? 0xE0u : 0x20u;
    }
}

/** @brief the screenful of 1A07: clear_bitmaps_6 (the 192-KB zero), [0xD39]:=0,
 * the fill loop - strcpy_cae (1AA4) + draw_char_advance (1AB9) per line
 * until the next _credtext byte is 0xFF or _rows_built >= 0x989 (the max
 * build 2456 rows * 80 B fits the 0x30000-byte strip) - and the INC SI at
 * 1a37 that walks the cursor onto the terminator (the shipped file's
 * four-byte 0xFF run absorbs it).
 */
static void _e4_fill(void)
{
    char scratch[1024]; // 0xCAE; bounded only in the port
    if (!_e4strip) return;
    memset(_e4strip, 0, E4_STRIP_BYTES);
    _rows_built = 0;
    do {
        uint16_t n = 0; // strcpy_cae: line -> scratch
        while (_cred_cursor < _credtext_size) {
            uint8_t c = _credtext[_cred_cursor++];
            if (n + 1u < sizeof scratch) scratch[n++] = (char)c;
            if (c == 0u) break; // the NUL is copied too
        }
        scratch[n] = 0;

        uint16_t tile = _rows_built; // BP: the tile's base row
        if (scratch[0] == 0) { // 1ac1: the empty-line test
            _rows_built = (uint16_t)(_rows_built + 0x10u);
            _fill_pattern(_e4strip + tile * 80u);
        } else { // 1adf: the text path
            _rows_built = (uint16_t)(_rows_built + 0xFu);
            _fill_pattern_clip(_e4strip + tile * 80u);
            /* 1ae9..1af4: [0x1E20] := BP, draw_text(scratch, 0xC8, row 0)
             * - the tile is video_arena() at row `tile`, so blit_glyph's 16th
             * row lands on the NEXT tile's row 0 exactly as the spill does */
            font_draw_text(_e4strip, scratch, 0xC8u, tile);
        }
        if (_cred_cursor < _credtext_size && _credtext[_cred_cursor] == 0xFFu)
            break; // 1a28
    } while (_rows_built < 0x989u); // 1a2f
    _cred_cursor++; // 1a37: INC SI
}

/**
 * @brief _blit_scroll (1000:1B58): present strip row `row` - the 200-row window
 * written into the draw page's scanline bytes 20..79 (screen x 80..319),
 * VGA write mode 2 with the row's own bit mask: each 1-bpp source byte
 * sets 4 pixels, plane j := (latch & ~M) | (bit j of the byte ? M : 0).
 * A row whose signature matches the cache at CS:0x1CF7 is skipped whole.
 * The source modulo is the port's one bound: the original's source
 * segment walks past the 192-KB strip into other heap memory only for
 * malformed _credtext (a scroll target below 200 rows), unreachable with
 * the shipped data (_rows_built = 1579).  The original's trailing [0x5AC8]
 * spin (one ISR tick) and JMP flip_page_swap are this port's tick
 * boundary and the caller's _flip_page().
 *
 * @param row the row
 */
static void _blit_scroll(uint32_t row)
{
    if (!_e4strip) return;
    uint32_t src = (row * 80u) % E4_STRIP_BYTES;
    uint16_t di = _page_draw + 0x14u;
    for (uint16_t i = 0; i < VIDEO_PAGE_H_MODE13H; i++) {
        uint8_t sig = _e4strip[src];
        if (sig != _row_sig[i]) {
            _row_sig[i] = sig;
            uint8_t mask = _e4strip[src + 1u];
            for (uint16_t k = 0; k < 60u; k++) {
                video_vram_wm2((uint16_t)(di + k),
                               _e4strip[(src + 0x14u + k) % E4_STRIP_BYTES],
                               mask);
            }
        }
        src = (src + 80u) % E4_STRIP_BYTES;
        di += 80u;
    }
}

/** @brief effect 4 (1000:19AF): the loads, the fades, and the first screenful's
 * fill (the 1a01..1a38 stretch runs inside the load instant, with the
 * fade-in animating on the tick, exactly as the original's straight-line
 * code).  Returns 0 = skip.
 *
 * @return 0 when the effect is skipped, 1 when it runs
 */
static int32_t _e4_load(void)
{
    /* 19b9/19be: [0xD3D]:=0, [0x1E7C]:=8 - both write-only (no reader is
     * traced; see the note by the module state) */
    g_osys->memFree(_credtext);
    _credtext = NULL;
    _credtext = res_load_chunk(game_resource_name(GAME_RES_CREDTEXT_BIN),
                               &_credtext_size, NULL);
    if (!_credtext) return 0;
    uint32_t sz = 0;
    _unload_img();
    _img_buf = res_load_chunk(game_resource_name(GAME_RES_LBM_INTCRED4_BIT),
                              &sz, NULL);
    if (!_img_buf || sz < VIDEO_PAGE_BYTES + 768u) {
        _unload_img();
        return 0;
    }

    palette_load_working(_img_buf + VIDEO_PAGE_BYTES);
    video_vram_planarise(_img_buf, UINT32_MAX, _page_draw, 80u, 80u,
                         VIDEO_PAGE_H_MODE13H);
    _flip_page();
    video_vram_planarise(_img_buf, UINT32_MAX, _page_draw, 80u, 80u,
                         VIDEO_PAGE_H_MODE13H);
    _flip_page();
    palette_build_fade(0u, 256u, 0x1Eu, palette_working());

    /* 1a01/1a07: the cursor at 0, then the first screenful.  The signature
     * cache mirrors the shipped CS:0x1CF7 sentinel (0x80 x200, a value no
     * real signature matches) and - like the original's CS bytes - is
     * never reset after the first fill: it persists across cycles. */
    _cred_cursor = 0;
    if (!_e4strip) {
        _e4strip = g_osys->memAlloc(E4_STRIP_BYTES);
        if (!_e4strip) return 0;
    }
    if (!_row_sig_init) {
        memset(_row_sig, 0x80, sizeof _row_sig);
        _row_sig_init = 1;
    }
    _e4_fill();
    return 1;
}

/**
 * @brief game_loop's prologue + LAB_000E's disc reset: run at every (re)entry.
 */
static void _attract_restart(void)
{
    video_set_mode(VIDEO_MODE_13H); // 132f: vga_set_mode_13h (1000:8300)
    res_set_disc(0u); // LAB_000E: [0xF7E] := 0
    g_osys->memFree(_text_spr);
    _text_spr =
        res_load_chunk(game_resource_name(GAME_RES_APL_TEXT_SPR), NULL, NULL);
    font_publish(_text_spr); // [0x1E1C] - may be NULL (headless)

    _saved_draw = _page_draw;
    _saved_disp = _page_disp; // 1387: the PUSH pair
    _page_draw = 0u;
    _page_disp = 0x8000u; // 138F/1395
    _phase = ATTRACT_E1_LOAD;
}

/**
 * @brief Advance the attract cycle's 1->3->2->4 effect.
 */
static void _next_effect(void)
{
    /* the cycle boundary (13AB/13BE in the original) also sets the
     * write-only [0x1E7C]:=0x0A - see the note by the module state */
    switch (_phase) {
    case ATTRACT_E1_OUT: _phase = ATTRACT_E3_LOAD; break;
    case ATTRACT_E3_OUT: _phase = ATTRACT_E2_LOAD; break;
    case ATTRACT_E2_OUT: _phase = ATTRACT_E4_LOAD; break;
    case ATTRACT_E4_OUT: _phase = ATTRACT_E1_LOAD; break;
    default: _phase = ATTRACT_E1_LOAD; break;
    }
    /* the next effect's LOAD runs on the following tick */
}

/** @brief the local poll (the skip tests inside the slide 198C..1998 and effect
 * 4's scroll 1A58..1A67): the same bits as the escape, a different
 * consequence - it skips the current hold, it does NOT exit the attract.
 *
 * @param in the input state
 * @return 1 when a key or fire is held, 0 otherwise
 */
static int32_t _local_key(const InputState *in)
{
    return in->keyHeld != 0u || in->lastKey != 0u ||
           (in->joyBits & JOYBITS_FIRE) != 0u;
}

/** @brief attract_exit (1000:13E9): the escape test - the configured action key
 * ([0x5ACE] bit 7) or any held key ([0x48A3]).  Polled ONLY at the
 * original's poll points: the fade waits (fade_wait 1000:13E0) and the
 * holds (fade_step_frames 1000:13FB), once per tick of those phases.
 *
 * @param in the input state
 * @return 1 when the escape action is pressed, 0 otherwise
 */
static int32_t _escape_test(const InputState *in)
{
    return (in->joyBits & JOYBITS_FIRE) != 0u || in->keyHeld != 0u ||
           in->lastKey != 0u;
}

/** @brief the longjmp 1000:13B2: restore the saved page pair, the one fade of all
 * 256 entries to the zeroed 0x48BC ramp (black) div 0x23 - the running
 * fade/state is abandoned exactly as the longjmp does - then wait without
 * further polling (wait_palette_fade_done 1000:8B8B).
 */
static void _latch_escape(void)
{
    _page_draw = _saved_draw;
    _page_disp = _saved_disp; // 13b6/13ba
    palette_build_fade(0u, 256u, 0x23u, _ramp0); // 13c3..13cf
    _phase = ATTRACT_ESCAPE;
}

/**
 * @brief Release the attract's heap blocks (the effect images, the 192-KB
 * credit strip and the published font).  The escape path calls it, and so does
 * the session teardown.
 */
static void _attract_shutdown(void)
{
    _unload_img();
    g_osys->memFree(_credtext);
    _credtext = NULL;
    _credtext_size = 0;
    if (_text_spr) font_publish(NULL); // the glyph table dies with it
    g_osys->memFree(_text_spr);
    _text_spr = NULL;
    g_osys->memFree(_e4strip);
    _e4strip = NULL;
    video_arena_release(); // the attract's [0xC9D] arena
}

uint8_t attract_tick(const InputState *in)
{
    if (_phase == ATTRACT_DONE || _phase == ATTRACT_IDLE)
        _attract_restart(); // a new epoch: game_loop's prologue

    switch (_phase) {
    /* --- effect 1: the gameint wipe (1000:1406) ------------------------ */
    case ATTRACT_E1_LOAD:
        if (!_e1_load()) {
            /* the port-only skip path (see the header): consume the escape
             * so the headless flow tests stay drivable */
            if (_escape_test(in)) {
                _latch_escape();
                break;
            }
            _phase = ATTRACT_E3_LOAD;
        } else {
            _phase = ATTRACT_E1_FADEWAIT;
        }
        break;
    case ATTRACT_E1_FADEWAIT: // fade_wait 145f
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _phase = ATTRACT_E1_WIPE;
        break;
    case ATTRACT_E1_WIPE: { // the loop at 1462
        _wipe_step();
        uint16_t bx = u16(_wipe_pos * 80u);
        if (_page_draw < 0x8000u) {
            _page_draw = bx;
            _page_disp = (uint16_t)(0x8000u + bx);
        } else {
            _page_draw = (uint16_t)(0x8000u + bx);
            _page_disp = bx;
        }
        _flip_page();
        if (_wipe_pos != 0u || _wipe_dir != 0) break; // 149c/14a4
        _counter = 700u; // 14aa: fade_step_frames
        _phase = ATTRACT_E1_HOLD;
        break;
    }
    case ATTRACT_E1_HOLD: // fade_step_frames 14aa
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (--_counter != 0u) break;
        palette_build_fade(0u, 256u, 0x1Eu, _ramp0); // 14b0: fade to black
        _phase = ATTRACT_E1_OUT;
        break;
    case ATTRACT_E1_OUT: // fade_wait 14bf
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _next_effect();
        break;

    /* --- effect 3: the PCTITLE scroller (1000:18CA) --------------------- */
    case ATTRACT_E3_LOAD:
        if (!_e3_load()) {
            if (_escape_test(in)) {
                _latch_escape();
                break;
            }
            _phase = ATTRACT_E2_LOAD;
        } else {
            _phase = ATTRACT_E3_FADEWAIT;
        }
        break;
    case ATTRACT_E3_FADEWAIT: // fade_wait 18e5
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _d3b = 600u; // 1980
        _phase = ATTRACT_E3_SLIDE;
        break;
    case ATTRACT_E3_SLIDE: // pctitle_slide_hold 1986
        _wait_page(_d3b);
        if (_local_key(in)) { // 198c: the skip poll
            /* 19a9: the original sets [0xD3D]:=0xFF (the write-only
             * skip flag) and returns - the hold follows, the slide is cut  */
            _counter = 70u;
            _phase = ATTRACT_E3_HOLD;
            break;
        }
        if (--_d3b != 0u) break;
        _phase = ATTRACT_E3_TAIL; // 19a1: the normal exit
        break;
    case ATTRACT_E3_TAIL:
        _wait_page(_d3b); // 19a1: _d3b == 0
        _counter = 70u; // 18eb: the 70-tick hold
        _phase = ATTRACT_E3_HOLD;
        break;
    case ATTRACT_E3_HOLD: // fade_step_frames 18eb
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (--_counter != 0u) break;
        palette_build_fade(0u, 256u, 0x23u, _ramp0); // 18f1: fade to black
        _phase = ATTRACT_E3_OUT;
        break;
    case ATTRACT_E3_OUT: // JMP fade_wait 1900
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _next_effect();
        break;

    /* --- effect 2: the hi-score + times screens (1000:1514) ------------- */
    case ATTRACT_E2_LOAD:
        if (!_e2_load()) {
            if (_escape_test(in)) {
                _latch_escape();
                break;
            }
            _phase = ATTRACT_E4_LOAD;
        } else {
            _frame = 0u;
            _phase = ATTRACT_E2_FADEWAIT;
        }
        break;
    case ATTRACT_E2_FADEWAIT: // fade_wait 1581
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _phase = ATTRACT_E2_FRAME;
        break;
    case ATTRACT_E2_FRAME: { // one intro_frame body
        uint8_t *ar = video_arena();

        memcpy(video_flat(), _img_buf, VIDEO_PAGE_BYTES); // copy_page 18a4
        memset(ar, 0, 16000u); // clear_screen_buf 1882
        if (_frame == 0u) { // intro_frame_1 1625
            font_draw_text(ar, game_string(GAME_STRING_ATTRACT_SCORE_HEADER),
                           0xA0u, 4u);
            _draw_score_block(ar);
        } else { // intro_frame_2 1652
            uint16_t base = (_frame - 1u) * 10u;
            font_draw_text(ar, game_string(GAME_STRING_ATTRACT_TIMES_HEADER),
                           0xA0u, 4u);
            for (uint16_t r = 0; r < 10u; r++) {
                uint8_t rec[2];

                _fastest_time(base + r, rec);
                _draw_times_row(ar, base + r, rec, 0x18u + r * 0x10u);
            }
        }
        _blit_80x200(ar);
        _wipe_iter = 0u;
        _phase = ATTRACT_E2_WIPE;
        break;
    }
    case ATTRACT_E2_WIPE: { // wipe_diagonal 16fa
        _wipe_row(_wipe_iter);
        _flip_page();
        if (++_wipe_iter < 79u) break;
        video_vram_planarise(video_flat(), UINT32_MAX, _page_draw, 80u, 80u,
                             VIDEO_PAGE_H_MODE13H); // 171c: the full present
        _flip_page();
        _counter = 490u; // 164c/168a: the hold
        _phase = ATTRACT_E2_HOLD;
        break;
    }
    case ATTRACT_E2_HOLD: // fade_step_frames 164c/168a
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (--_counter != 0u) break;
        if (++_frame <= 16u) {
            _phase = ATTRACT_E2_FRAME;
            break;
        }
        palette_build_fade(0u, 256u, 0x1Eu, _ramp0); // 159f: fade to black
        _phase = ATTRACT_E2_OUT;
        break;
    case ATTRACT_E2_OUT: // JMP fade_wait 15ae
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _next_effect();
        break;

    /* --- effect 4: the credits (1000:19AF) -------------------------------- */
    case ATTRACT_E4_LOAD:
        if (!_e4_load()) {
            if (_escape_test(in)) {
                _latch_escape();
                break;
            }
            _phase = ATTRACT_E1_LOAD;
        } else {
            _phase = ATTRACT_E4_FADEWAIT;
        }
        break;
    case ATTRACT_E4_FADEWAIT: // fade_wait 1a3c
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _scroll_row = 0; // 1a3f: [0xD78] := 0
        _phase = ATTRACT_E4_SCROLL;
        break;
    case ATTRACT_E4_SCROLL: // the loop at 1a45
        /* _blit_scroll + its trailing one-tick spin + the JMP flip: one
         * strip row per display period, the page pair alternating        */
        _blit_scroll(_scroll_row);
        _flip_page();
        _scroll_row++;
        if (_scroll_row == (uint16_t)(_rows_built - VIDEO_PAGE_H_MODE13H)) {
            /* 1a72: the terminator check; more _credtext -> the next
             * screenful in the same instant (the clear + the fill + the
             * fade_wait's one poll, the fade being done), then row 0 on
             * the next tick.  The bound mirrors the original's heap-read
             * past the buffer only for data without the 0xFF terminator,
             * which the shipped file always has. */
            uint8_t next = (_cred_cursor < _credtext_size)
                               ? _credtext[_cred_cursor]
                               : 0xFFu;
            if (next != 0xFFu) {
                _e4_fill();
                if (_escape_test(in)) {
                    _latch_escape();
                    break;
                }
                _scroll_row = 0;
                break;
            }
            /* 1a82: the tail - [0x1E7C]:=0x0A (write-only) */
            palette_build_fade(0u, 256u, 0x1Eu, _ramp0);
            _phase = ATTRACT_E4_OUT;
            break;
        }
        /* 1a58: the LOCAL poll - a key ends the effect via the tail
         * ([0xD3D]:=0xFF, write-only); it is not the attract escape      */
        if (_local_key(in)) {
            palette_build_fade(0u, 256u, 0x1Eu, _ramp0);
            _phase = ATTRACT_E4_OUT;
        }
        break;
    case ATTRACT_E4_OUT: // fade_wait 1a96
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        /* effect 4's work area and file: [0xD76] and [0xC9F] are dead once
         * the credits have finished; the next cycle re-carves them */
        g_osys->memFree(_e4strip);
        _e4strip = NULL;
        g_osys->memFree(_credtext);
        _credtext = NULL;
        _credtext_size = 0;
        _next_effect(); // -> effect 1: the cycle
        break;

    /* --- the escape: the longjmp 13B2's fade, waited without polling ---- */
    case ATTRACT_ESCAPE:
        if (palette_fade_active()) break; // 8b8b: wait_palette_fade_done
        _attract_shutdown();
        _phase = ATTRACT_DONE;
        break;

    case ATTRACT_DONE:
    case ATTRACT_IDLE: break;
    }

    _compose_window();
    return _phase != ATTRACT_DONE;
}
