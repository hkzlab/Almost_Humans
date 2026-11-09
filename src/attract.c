/* attract.c — the ATTRACT loop: game_loop (1000:131E) + the four intro
 * effects, ported from re_map.md § "The ATTRACT loop decoded end to end
 * (2026-10-25)"; the data formats and the shipped tables are in
 * re_formats.md §9.4.
 *
 * The machine model: the original's 64-KB mode-X address space (the CRTC
 * byte mode bios_set_mode_13h installed — one address byte = 4 pixels, one
 * per plane; 80 bytes per scanline; the start address a byte offset) becomes
 * _vram[addr][plane]; the CRTC start becomes _display_start; every present
 * composes the 200-row window at _display_start into the flat page 0
 * (game_main's per-tick videoPresent shows it — the same contract
 * scene_play_boot_intro uses).
 *
 * attract_tick() advances the machine by exactly ONE display period (the
 * original's wait_vsync cadence).  The exit (attract_exit 1000:13E9: the
 * configured action key [0x5ACE bit 7] or any held key [0x48A3]) is polled
 * EXACTLY where the original polls it — at the fade waits (fade_wait
 * 1000:13E0) and the holds (fade_step_frames 1000:13FB), once per tick of
 * those phases; the no-poll windows (the two wipes, the _frame draws, the
 * slide's tail) see no key until their next poll, as in the listing.  The
 * local polls (effect 3's slide skip, effect 4's scroller skip) stay local
 * exactly as in the listing — they skip the current hold, they do NOT exit
 * the attract.
 *
 * The one port-only escape point is the ASSET-LOAD-FAILURE skip
 * (port_deviations.md D-19): the
 * original has no such path (a failed INT 63h prompts for the disc), and
 * the port's headless tests run without data — so the skip consumes the
 * escape to stay drivable.  With data present that path never runs and the
 * polling is the original's.  (Load failures otherwise skip the effect —
 * the scene player's established pattern.)
 *
 * The escape itself (the longjmp 1000:13B2): the saved page pair is
 * restored, [0x1E7C]:=0x0A, one fade of all 256 DAC entries to the zeroed
 * 0x48BC ramp (black) div 0x23, waited WITHOUT further polling
 * (wait_palette_fade_done 1000:8B8B); its completion is the hand-off to
 * the front-end (FLOW_MENU — the original's RET into entry LAB_002B).
 *
 * Effect 4's credits scroller is the real machine (decoded 2026-10-25,
 * re_map.md "Effect 4" items 3-6): the fill builds the 192-KB strip at
 * [0xD76] (clear_bitmaps_6) as a vertical bitmap of 80-byte rows — [0]
 * the signature, [1] the write-mode-2 mask, [20..79] the presented 60
 * bytes — one 15-row tile per text line (BP += 75 paras) or 16-row blank
 * tile (BP += 80 paras), [0xD39] counting rows; _blit_scroll (1000:1B58)
 * then presents ONE strip row per tick into the draw page's scanline
 * bytes 20..79 (screen x 80..319) in write mode 2 with the row's own bit
 * mask (0x20: the ink sets palette bit 5 over the background; 0xE0: the
 * background clamped to index & 0x1F), skipping rows whose signature
 * matches the 200-byte cache at CS:0x1CF7 (shipped 0x80), until
 * [0xD78] == [0xD39] - 0xC8; more _credtext re-fills the next screenful,
 * else the tail ([0x1E7C]:=0x0A, fade to black div 0x1E).
 */
#include "attract.h"
#include "font.h"
#include "game_strs.h"
#include "hiscore.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "video.h"
#include "byteorder.h"

#include <stdlib.h>
#include <string.h>

/* --- the machine --------------------------------------------------------- */

/* --- the planar address space (A000, unchained, byte mode) ------------- */
static uint8_t _vram[65536][4];   /* [address][plane]: 1 byte = 4 pixels   */
static uint32_t _page_draw, _page_disp; /* [0x5AC4]/[0x5AC6]                */
static uint32_t _display_start; /* [0x4106]: the CRTC start (bytes) */
static uint32_t _saved_draw, _saved_disp; /* game_loop's PUSH pair (1387)     */

/* the strings at 202F:0xC38/0xC4D/0xC73/0xC79 (re_formats.md §9.4) are
 * game_strs' ATTRACT entries */

/* the 0x48BC ramp analog: it enters the attract zeroed (the boot's
 * clear_fade_ramp) and nothing in the attract ever writes it, so every
 * "fade to 0x48BC" is a fade to black. */
static const uint8_t _ramp0[768];

/* --- the module state --------------------------------------------------- */
static AttractPhase _phase = ATTRACT_IDLE;
static uint32_t _counter; /* the multi-tick _phase countdown        */
static uint32_t _frame; /* effect 2's _frame: 0 = scores, 1..16   */
static uint32_t _wipe_iter; /* effect 2's 79-step wipe iteration     */
static uint32_t _d3b; /* [0xD3B]: the slide's display row      */
static uint8_t _arena[16000]; /* [0xC9D]: the text _arena (200 x 80)    */
static uint8_t _page_b[64000]; /* [0xC9B]: the composed display page    */
static uint8_t *_img_buf; /* [0xC32]: the loaded image             */
static uint8_t *_text_spr; /* [0x1E1C] analog: the loaded font      */
static uint8_t _colour1881 = 0x43; /* CS:0x1881: _blit_80x200's colour base  */

/* --- effect 4's credits strip (re_map.md "Effect 4") ----------------------
 * The 192-KB work area [0xD76] (clear_bitmaps_6 1000:1B3B, re-zeroed per
 * screenful): a vertical bitmap of 80-byte rows — [0] the row signature,
 * [1] the write-mode-2 bit mask, [2..19] pad, [20..79] the presented 60
 * bytes = _arena pixels 80..319. */
static uint8_t _e4strip[196608];
static uint8_t _row_sig[200]; /* CS:0x1CF7: the row-signature cache    */
static int32_t _row_sig_init; /* the shipped 0x80 fill runs once       */
static uint8_t *_credtext; /* [0xC9F]: the loaded _credtext.bin      */
static uint32_t _credtext_size;
static uint32_t _cred_cursor; /* [0xD6C]: the _credtext cursor          */
static uint16_t _rows_built; /* [0xD39]: the strip rows built         */
static uint16_t _scroll_row; /* [0xD78]: the scroll window's start row */

/* --- effect 1's wipe: intro_wipe_step (1000:14C3) -----------------------
 * The damped oscillator on [0xC35]=pos, [0xC36]=dir (0=shrink), [0xC37]=speed:
 * trace 199->0 (speed 32), grow 16 -> peak 136, shrink -> 0, grow 8 -> 36,
 * -> 0, 4 -> 10, 2 -> 3, 1 -> 1, 0 -> exit (pos==0 && dir==0). */
static uint32_t _wipe_pos, _wipe_speed;
static int32_t _wipe_dir;

/* --- the shipped DS tables (re_formats.md §9.4, extracted 2026-10-25) -- */

uint32_t attract_hiscore_count(void) { return 10u; }
uint32_t attract_hiscore_score(uint32_t i) { return hiscore_score(i); }
const char *attract_hiscore_name(uint32_t i) { return hiscore_name(i); }

/* 202F:0x38C — 160 per-level records of [seconds][minutes] (two
 * pseudo-decimal nibbles / one digit), shipped as 99 09 ("9:99") for every
 * level. */
/** @brief 202F:0x38C — the per-level bests: `hiscore_best(level)` (the same 160 u16
 * the score screen compares against).
 *
 * @param levelMinus1 the zero-based level
 * @param rec the record
 */
static void _fastest_time(uint32_t levelMinus1, uint8_t rec[2])
{
    uint16_t v = hiscore_best(levelMinus1 + 1u);

    wr_le_u16(rec, v); /* [seconds][minutes] */
}

int32_t attract_init(const OSystem *osys)
{
    (void)osys;
    return 0;
}

uint32_t attract_phase_for_probe(void) { return (uint32_t)_phase; }

const uint8_t *attract_strip_for_probe(uint32_t *rowsOut)
{
    if (rowsOut) *rowsOut = _rows_built;
    return _e4strip;
}

/* --- the VRAM primitives ------------------------------------------------ */

/**
 * @brief _clear_vram (1000:833A): map mask 0xF, 64 KB zeroed.
 */
static void _clear_vram(void) { memset(_vram, 0, sizeof _vram); }

/** @brief blit_planar_to_vga (1000:888E): one 320x200 chunky image planarised into
 * the 16000-byte block at `dest` — dest[base + r*80 + c] plane p
 * = src[r*320 + c*4 + p].
 *
 * @param src the source buffer
 * @param dest the destination
 */
static void _blit_planar(const uint8_t *src, uint32_t dest)
{
    for (uint32_t r = 0; r < 200u; r++) {
        uint32_t a = dest + r * 80u;
        if (a + 79u >= 65536u) break; /* never taken: dest is safe */
        const uint8_t *s = src + r * 320u;
        for (uint32_t c = 0; c < 80u; c++) {
            _vram[a + c][0] = s[c * 4u];
            _vram[a + c][1] = s[c * 4u + 1u];
            _vram[a + c][2] = s[c * 4u + 2u];
            _vram[a + c][3] = s[c * 4u + 3u];
        }
    }
}

/** @brief begin_page_flip (1000:8821) + the swap (1000:87C7): the display target is
 * the drawing page, then the pair swaps.  The original's spin until the ISR
 * applies [0x4106] is implicit: the port composes from _display_start at the
 * tick's present.
 */
static void _flip_page(void)
{
    uint32_t t = _page_draw;
    _display_start = _page_draw;
    _page_draw = _page_disp;
    _page_disp = t;
}

/**
 * @brief _wait_page (1000:195B): _display_start := [0xD3B]*80, no page-pair change.
 *
 * @param row the row
 */
static void _wait_page(uint32_t row) { _display_start = row * 80u; }

/**
 * @brief The present: the 200-row window at _display_start, flat.
 */
static void _compose_window(void)
{
    uint8_t *page = video_page(0);
    if (!page) return;
    for (uint32_t y = 0; y < 200u; y++) {
        const uint32_t row = (_display_start + y * 80u) & 0xFFFFu;
        uint8_t *d = page + y * 320u;
        for (uint32_t x = 0; x < 320u; x++)
            d[x] = _vram[(row + (x >> 2)) & 0xFFFFu][x & 3u];
    }
}

/**
 * @brief One step of the attract wipe effect.
 */
static void _wipe_step(void)
{
    uint32_t pos = _wipe_pos, speed = _wipe_speed;
    if (_wipe_dir) { /* grow: 14e0 */
        pos += speed;
        if (pos > 0xC7u) pos = 0xC7u;
        if (speed == 0u) {
            _wipe_dir = 0;
            _wipe_speed = 0u;
            _wipe_pos = pos;
            return;
        }
        speed--;
    } else { /* shrink: 14f6 */
        uint32_t r = pos - speed; /* SUB AX,CX flags */
        if (r > 0xFFu || r == 0u) { /* negative or exact zero */
            _wipe_pos = 0u;
            _wipe_dir = 1;
            _wipe_speed = speed >> 1;
            return;
        }
        pos = r;
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
    uint32_t n = 0, m = 0;
    char t[8];

    if (v >= 10000u) {
        uint32_t hi = v / 10000u, lo = v % 10000u;

        for (uint32_t i = 0; i < 4u; i++) { /* 1000:0B0E: four LSB-first */
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
    while (m) out[n++] = t[--m]; /* 1000:0B3B: one reversal of the lot */
    out[n] = 0;
}

void attract_format_decimal_for_probe(uint32_t v, char *out)
{ _format_decimal(v, out); }

/** @brief _draw_score_block (1000:0651): [0x36]=0x10, [0x38]=0x18 (set by
 * intro_frame_1); rank x=0x20, name x=0x82, score x=0x102.
 *
 * @param ar the 80x200 source arena
 */
static void _draw_score_block(uint8_t *ar)
{
    uint32_t row = 0x18u;
    for (uint32_t k = 0; k < 10u; k++) {
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
    uint32_t v = levelMinus1 + 1u;
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
    font_draw_text(ar, game_strs[g_game_lang][GAME_STR_ATTRACT_TIMES_LABEL],
                   0x92u,
                   row); /* 16D3, redrawn per row */
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
    for (uint32_t bit = 0; bit < 4u; bit++) {
        if (b & 1u) d[bit] = (uint8_t)dl;
        b >>= 1;
    }
}

/** @brief _blit_80x200 (1000:17FB): the _arena's bits 0..3 (4 px per byte, bit 0 the
 * leftmost) splat colour over _page_b, 320 B per row from dest offset 4.
 *
 * @param ar the 80x200 source arena
 */
static void _blit_80x200(const uint8_t *ar)
{
    uint32_t dl = 0xFDu;
    for (uint32_t row = 0; row < 200u; row++) {
        uint8_t *d = _page_b + 4u + row * 320u;
        const uint8_t *s = ar + row * 80u;
        for (uint32_t k = 0; k < 80u; k++) _splat_bits(d + k * 4u, s[k], dl);
        uint32_t cx = 200u - row; /* the pushed row countdown */
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

/** @brief blit_sprite_planes (1000:1795): an 8x16 chunk of _page_b planarised into
 * _vram at _page_draw + row*80 + x/4, plane (x+j)&3.  The source/dest read
 * past a row's edge into the next row exactly as the original's flat
 * offsets do (the right-sweeping columns start at 319).
 *
 * @param x the x coordinate
 * @param row the row
 */
static void _blit_strip8(uint32_t x, uint32_t row)
{
    for (uint32_t j = 0; j < 8u; j++) {
        uint32_t px = x + j;
        for (uint32_t r = 0; r < 16u; r++) {
            uint32_t sy = row + r;
            uint8_t v = _page_b[sy * 320u + px];
            _vram[(_page_draw + sy * 80u + (px >> 2)) & 0xFFFFu][px & 3u] = v;
        }
    }
}

/** @brief _wipe_row (1000:1722): eleven 16-row chunks tiling rows 8..183,
 * alternating between the right-sweeping column (the arg_swap variant,
 * x = 319-4k) and the left-sweeping one (x = 4k) — the criss-cross.
 *
 * @param k the loop counter
 */
static void _wipe_row(uint32_t k)
{
    static const uint32_t rows[11] = { 0x8u,  0x18u, 0x28u, 0x38u, 0x48u, 0x58u,
                                       0x68u, 0x78u, 0x88u, 0x98u, 0xA8u };
    uint32_t xs = 4u * k;
    uint32_t xd = 0x13Fu - 4u * k;
    for (uint32_t i = 0; i < 11u; i++)
        _blit_strip8((i & 1u) ? xs : xd, rows[i]);
}

/**
 * @brief --- the per-effect loads ------------------------------------------------
 */
static void _unload_img(void)
{
    free(_img_buf);
    _img_buf = NULL;
}

/**
 * @brief effect 1 (1000:1406): the load + the wipe setup.  Returns 0 = skip.
 *
 * @return 0 when the effect is skipped, 1 when it runs
 */
static int32_t _e1_load(void)
{
    _clear_vram();
    uint32_t sz = 0;
    _unload_img();
    _img_buf = res_load_chunk("LBM\\GAMEINT.BIT", &sz, NULL);
    if (!_img_buf || sz < 64000u + 768u) {
        _unload_img();
        return 0;
    }

    palette_load_working(_img_buf + 64000u);
    _blit_planar(_img_buf, _page_draw);
    _flip_page(); /* the image at 0 and   */
    _blit_planar(_img_buf, _page_draw);
    _flip_page(); /* at 0x8000           */

    _page_draw = 0x3E80u; /* 142F/1435: the blank */
    _page_disp = 0xBE80u; /* start, past the copy */
    _flip_page();

    _wipe_pos = 0xC7u;
    _wipe_dir = 0;
    _wipe_speed = 0x20u; /* 143e: 199/shrink/32 */
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
    _clear_vram();
    uint32_t sz = 0;
    _unload_img();
    _img_buf = res_load_chunk("LBM\\PCTITLE.RAW", &sz, NULL);
    if (!_img_buf || sz < 112768u) {
        _unload_img();
        return 0;
    }

    _blit_planar(_img_buf, 0x3E80u); /* the title: rows 200-400 */
    _blit_planar(_img_buf + 48000u, 0x6D60u); /* the promo: rows 350-550 */
    palette_load_working(_img_buf + 112000u);
    _wait_page(_d3b); /* the stale [0xD3B]       */
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
    _img_buf = res_load_chunk("LBM\\GAMEINT.BIT", &sz, NULL);
    if (!_img_buf || sz < 64000u + 768u) {
        _unload_img();
        return 0;
    }

    palette_load_working(_img_buf + 64000u);

    /* blit_fullscreen 1000:15B1: two 75-pixel, 200-row strips */
    for (uint32_t r = 0; r < 200u; r++) {
        memcpy(_img_buf + r * 320u, _img_buf + 0x9Eu + r * 320u, 75u);
        memcpy(_img_buf + 0xF5u + r * 320u, _img_buf + 0x55u + r * 320u, 75u);
    }
    /* decode_page_in_place 1000:15E6 + hash_three_bytes 1000:43D5: the
     * luma conversion through gameint's own palette */
    {
        const uint8_t *pal = palette_working();
        for (uint32_t i = 0; i < 64000u; i++) {
            const uint8_t *p = pal + _img_buf[i] * 3u;
            /* hash_three_bytes' CH = (77R+151G+28B)>>8, then SHR CH,1 */
            _img_buf[i] =
                (uint8_t)((77u * p[0] + 151u * p[1] + 28u * p[2]) >> 9);
        }
    }
    _blit_planar(_img_buf, _page_draw);
    _flip_page();
    _blit_planar(_img_buf, _page_draw);
    _flip_page();

    uint8_t *palBuf = res_load_chunk("LBM\\SCOREPAL.PAL", &sz, NULL);
    if (palBuf) {
        palette_load_working(palBuf);
        free(palBuf);
    }
    palette_build_fade(0u, 256u, 0x1Eu, palette_working());
    return 1;
}

/** @brief _fill_pattern (1000:1AFC) — draw_char_advance's blank path: the tile's 16
 * rows of [sig 0xFF, mask 0x20]; the data bytes keep the zeroed strip.
 *
 * @param tile the tile byte
 */
static void _fill_pattern(uint8_t *tile)
{
    for (uint32_t r = 0; r < 16u; r++) {
        tile[r * 80u] = 0xFFu;
        tile[r * 80u + 1u] = 0x20u;
    }
}

/** @brief _fill_pattern_clip (1000:1B13) — the text path: rows 0..3 the [0xFF, 0xE0]
 * band above the text (the presenter clamps the background to index & 0x1F
 * there, erasing stale ink), rows 4..14 the [0..10, 0x20] per-row
 * signatures (adjacent glyph rows always mismatch the cache, so ink rows
 * are always redrawn).  The original's dead MOV BX,0xD2E (1b1a — BX pushed
 * and never read) is dropped.
 *
 * @param tile the tile byte
 */
static void _fill_pattern_clip(uint8_t *tile)
{
    for (uint32_t r = 0; r < 15u; r++) {
        tile[r * 80u] = (r < 4u) ? 0xFFu : (uint8_t)(r - 4u);
        tile[r * 80u + 1u] = (r < 4u) ? 0xE0u : 0x20u;
    }
}

/** @brief the screenful of 1A07: clear_bitmaps_6 (the 192-KB zero), [0xD39]:=0,
 * the fill loop — strcpy_cae (1AA4) + draw_char_advance (1AB9) per line
 * until the next _credtext byte is 0xFF or _rows_built >= 0x989 (the max
 * build 2456 rows * 80 B fits the strip exactly) — and the INC SI at
 * 1a37 that walks the cursor onto the terminator (the shipped file's
 * four-byte 0xFF run absorbs it).
 */
static void _e4_fill(void)
{
    char scratch[1024]; /* 0xCAE; bounded only in the port */
    memset(_e4strip, 0, sizeof _e4strip);
    _rows_built = 0;
    do {
        uint32_t n = 0; /* strcpy_cae: line -> scratch    */
        while (_cred_cursor < _credtext_size) {
            uint8_t c = _credtext[_cred_cursor++];
            if (n + 1u < sizeof scratch) scratch[n++] = (char)c;
            if (c == 0u) break; /* the NUL is copied too          */
        }
        scratch[n] = 0;

        uint32_t tile = _rows_built; /* BP: the tile's base row          */
        if (scratch[0] == 0) { /* 1ac1: the empty-line test       */
            _rows_built = (uint16_t)(_rows_built + 0x10u);
            _fill_pattern(_e4strip + tile * 80u);
        } else { /* 1adf: the text path             */
            _rows_built = (uint16_t)(_rows_built + 0xFu);
            _fill_pattern_clip(_e4strip + tile * 80u);
            /* 1ae9..1af4: [0x1E20] := BP, draw_text(scratch, 0xC8, row 0)
             * — the tile is the _arena at row `tile`, so blit_glyph's 16th
             * row lands on the NEXT tile's row 0 exactly as the spill does */
            font_draw_text(_e4strip, scratch, 0xC8u, tile);
        }
        if (_cred_cursor < _credtext_size && _credtext[_cred_cursor] == 0xFFu)
            break; /* 1a28            */
    } while (_rows_built < 0x989u); /* 1a2f            */
    _cred_cursor++; /* 1a37: INC SI                    */
}

/**
 * @brief Mask four planes of one strip byte with one colour.
 *
 * @param px the x position
 * @param v the sheet byte
 * @param mask the mask
 */
static void _mask_four(uint8_t *px, uint8_t v, uint8_t mask)
{
    for (uint32_t j = 0; j < 4u; j++)
        px[j] =
            (uint8_t)((px[j] & (uint8_t)~mask) | (((v >> j) & 1u) ? mask : 0u));
}

/** @brief _blit_scroll (1000:1B58): present strip row `row` — the 200-row window
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
    uint32_t src = (row * 80u) % sizeof _e4strip;
    uint32_t di = _page_draw + 0x14u;
    for (uint32_t i = 0; i < 200u; i++) {
        uint8_t sig = _e4strip[src];
        if (sig != _row_sig[i]) {
            _row_sig[i] = sig;
            uint8_t mask = _e4strip[src + 1u];
            for (uint32_t k = 0; k < 60u; k++) {
                _mask_four(_vram[di + k],
                           _e4strip[(src + 0x14u + k) % sizeof _e4strip], mask);
            }
        }
        src = (src + 80u) % sizeof _e4strip;
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
    /* 19b9/19be: [0xD3D]:=0, [0x1E7C]:=8 — both write-only (no reader is
     * traced; see the note by the module state) */
    free(_credtext);
    _credtext = NULL;
    _credtext = res_load_chunk("CREDTEXT.BIN", &_credtext_size, NULL);
    if (!_credtext) return 0;
    uint32_t sz = 0;
    _unload_img();
    _img_buf = res_load_chunk("LBM\\INTCRED4.BIT", &sz, NULL);
    if (!_img_buf || sz < 64000u + 768u) {
        _unload_img();
        return 0;
    }

    palette_load_working(_img_buf + 64000u);
    _blit_planar(_img_buf, _page_draw);
    _flip_page();
    _blit_planar(_img_buf, _page_draw);
    _flip_page();
    palette_build_fade(0u, 256u, 0x1Eu, palette_working());

    /* 1a01/1a07: the cursor at 0, then the first screenful.  The signature
     * cache mirrors the shipped CS:0x1CF7 sentinel (0x80 x200, a value no
     * real signature matches) and — like the original's CS bytes — is
     * never reset after the first fill: it persists across cycles. */
    _cred_cursor = 0;
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
    video_set_mode(VIDEO_MODE_13H); /* 132f: vga_set_mode_13h (1000:8300) */
    res_set_disc(0u); /* LAB_000E: [0xF7E] := 0         */
    free(_text_spr);
    _text_spr = res_load_chunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(_text_spr); /* [0x1E1C] — may be NULL (headless) */

    _saved_draw = _page_draw;
    _saved_disp = _page_disp; /* 1387: the PUSH pair */
    _page_draw = 0u;
    _page_disp = 0x8000u; /* 138F/1395           */
    _phase = ATTRACT_E1_LOAD;
}

/**
 * @brief Advance the attract cycle's 1->3->2->4 effect.
 */
static void _next_effect(void)
{
    /* the cycle boundary (13AB/13BE in the original) also sets the
     * write-only [0x1E7C]:=0x0A — see the note by the module state */
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
 * consequence — it skips the current hold, it does NOT exit the attract.
 *
 * @param in the input state
 * @return 1 when a key or fire is held, 0 otherwise
 */
static int32_t _local_key(const InputState *in)
{
    return in->keyHeld != 0u || in->lastKey != 0u ||
           (in->joyBits & 0x80u) != 0u;
}

/** @brief attract_exit (1000:13E9): the escape test — the configured action key
 * ([0x5ACE] bit 7) or any held key ([0x48A3]).  Polled ONLY at the
 * original's poll points: the fade waits (fade_wait 1000:13E0) and the
 * holds (fade_step_frames 1000:13FB), once per tick of those phases.
 *
 * @param in the input state
 * @return 1 when the escape action is pressed, 0 otherwise
 */
static int32_t _escape_test(const InputState *in)
{
    return (in->joyBits & 0x80u) != 0u || in->keyHeld != 0u ||
           in->lastKey != 0u;
}

/** @brief the longjmp 1000:13B2: restore the saved page pair, the one fade of all
 * 256 entries to the zeroed 0x48BC ramp (black) div 0x23 — the running
 * fade/state is abandoned exactly as the longjmp does — then wait without
 * further polling (wait_palette_fade_done 1000:8B8B).
 */
static void _latch_escape(void)
{
    _page_draw = _saved_draw;
    _page_disp = _saved_disp; /* 13b6/13ba */
    palette_build_fade(0u, 256u, 0x23u, _ramp0); /* 13c3..13cf */
    _phase = ATTRACT_ESCAPE;
}

int32_t attract_tick(const InputState *in)
{
    if (_phase == ATTRACT_DONE || _phase == ATTRACT_IDLE)
        _attract_restart(); /* a new epoch: game_loop's prologue */

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
    case ATTRACT_E1_FADEWAIT: /* fade_wait 145f */
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _phase = ATTRACT_E1_WIPE;
        break;
    case ATTRACT_E1_WIPE: { /* the loop at 1462 */
        _wipe_step();
        uint32_t bx = _wipe_pos * 80u;
        if (_page_draw < 0x8000u) {
            _page_draw = bx;
            _page_disp = 0x8000u + bx;
        } else {
            _page_draw = 0x8000u + bx;
            _page_disp = bx;
        }
        _flip_page();
        if (_wipe_pos != 0u || _wipe_dir != 0) break; /* 149c/14a4 */
        _counter = 700u; /* 14aa: fade_step_frames */
        _phase = ATTRACT_E1_HOLD;
        break;
    }
    case ATTRACT_E1_HOLD: /* fade_step_frames 14aa */
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (--_counter != 0u) break;
        palette_build_fade(0u, 256u, 0x1Eu, _ramp0); /* 14b0: fade to black */
        _phase = ATTRACT_E1_OUT;
        break;
    case ATTRACT_E1_OUT: /* fade_wait 14bf */
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
    case ATTRACT_E3_FADEWAIT: /* fade_wait 18e5 */
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _d3b = 600u; /* 1980 */
        _phase = ATTRACT_E3_SLIDE;
        break;
    case ATTRACT_E3_SLIDE: /* pctitle_slide_hold 1986 */
        _wait_page(_d3b);
        if (_local_key(in)) { /* 198c: the skip poll   */
            /* 19a9: the original sets [0xD3D]:=0xFF (the write-only
             * skip flag) and returns — the hold follows, the slide is cut  */
            _counter = 70u;
            _phase = ATTRACT_E3_HOLD;
            break;
        }
        if (--_d3b != 0u) break;
        _phase = ATTRACT_E3_TAIL; /* 19a1: the normal exit */
        break;
    case ATTRACT_E3_TAIL:
        _wait_page(_d3b); /* 19a1: _d3b == 0       */
        _counter = 70u; /* 18eb: the 70-tick hold */
        _phase = ATTRACT_E3_HOLD;
        break;
    case ATTRACT_E3_HOLD: /* fade_step_frames 18eb */
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (--_counter != 0u) break;
        palette_build_fade(0u, 256u, 0x23u, _ramp0); /* 18f1: fade to black */
        _phase = ATTRACT_E3_OUT;
        break;
    case ATTRACT_E3_OUT: /* JMP fade_wait 1900 */
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
    case ATTRACT_E2_FADEWAIT: /* fade_wait 1581 */
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _phase = ATTRACT_E2_FRAME;
        break;
    case ATTRACT_E2_FRAME: { /* one intro_frame body   */
        memcpy(_page_b, _img_buf, 64000u); /* copy_page 18a4 */
        memset(_arena, 0, sizeof _arena); /* clear_screen_buf 1882 */
        if (_frame == 0u) { /* intro_frame_1 1625 */
            font_draw_text(_arena,
                           game_strs[g_game_lang][GAME_STR_ATTRACT_SCORE_HEADER],
                           0xA0u, 4u);
            _draw_score_block(_arena);
        } else { /* intro_frame_2 1652 */
            uint32_t base = (_frame - 1u) * 10u;
            font_draw_text(_arena,
                           game_strs[g_game_lang][GAME_STR_ATTRACT_TIMES_HEADER],
                           0xA0u, 4u);
            for (uint32_t r = 0; r < 10u; r++) {
                uint8_t rec[2];

                _fastest_time(base + r, rec);
                _draw_times_row(_arena, base + r, rec, 0x18u + r * 0x10u);
            }
        }
        _blit_80x200(_arena);
        _wipe_iter = 0u;
        _phase = ATTRACT_E2_WIPE;
        break;
    }
    case ATTRACT_E2_WIPE: { /* wipe_diagonal 16fa */
        _wipe_row(_wipe_iter);
        _flip_page();
        if (++_wipe_iter < 79u) break;
        _blit_planar(_page_b, _page_draw); /* 171c: the full present */
        _flip_page();
        _counter = 490u; /* 164c/168a: the hold    */
        _phase = ATTRACT_E2_HOLD;
        break;
    }
    case ATTRACT_E2_HOLD: /* fade_step_frames 164c/168a */
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (--_counter != 0u) break;
        if (++_frame <= 16u) {
            _phase = ATTRACT_E2_FRAME;
            break;
        }
        palette_build_fade(0u, 256u, 0x1Eu, _ramp0); /* 159f: fade to black */
        _phase = ATTRACT_E2_OUT;
        break;
    case ATTRACT_E2_OUT: /* JMP fade_wait 15ae */
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
    case ATTRACT_E4_FADEWAIT: /* fade_wait 1a3c */
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _scroll_row = 0; /* 1a3f: [0xD78] := 0    */
        _phase = ATTRACT_E4_SCROLL;
        break;
    case ATTRACT_E4_SCROLL: /* the loop at 1a45      */
        /* _blit_scroll + its trailing one-tick spin + the JMP flip: one
         * strip row per display period, the page pair alternating        */
        _blit_scroll(_scroll_row);
        _flip_page();
        _scroll_row++;
        if (_scroll_row == (uint16_t)(_rows_built - 0xC8u)) {
            /* 1a72: the terminator check; more _credtext -> the next
             * screenful in the same instant (the clear + the fill + the
             * fade_wait's one poll, the fade being done), then row 0 on
             * the next tick.  The bound mirrors the original's heap-read
             * past the buffer only for data without the 0xFF terminator,
             * which the shipped file always has. */
            uint32_t next = (_cred_cursor < _credtext_size)
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
            /* 1a82: the tail — [0x1E7C]:=0x0A (write-only) */
            palette_build_fade(0u, 256u, 0x1Eu, _ramp0);
            _phase = ATTRACT_E4_OUT;
            break;
        }
        /* 1a58: the LOCAL poll — a key ends the effect via the tail
         * ([0xD3D]:=0xFF, write-only); it is not the attract escape      */
        if (_local_key(in)) {
            palette_build_fade(0u, 256u, 0x1Eu, _ramp0);
            _phase = ATTRACT_E4_OUT;
        }
        break;
    case ATTRACT_E4_OUT: /* fade_wait 1a96 */
        if (_escape_test(in)) {
            _latch_escape();
            break;
        }
        if (palette_fade_active()) break;
        _next_effect(); /* -> effect 1: the cycle */
        break;

    /* --- the escape: the longjmp 13B2's fade, waited without polling ---- */
    case ATTRACT_ESCAPE:
        if (palette_fade_active()) break; /* 8b8b: wait_palette_fade_done */
        _unload_img();
        free(_credtext);
        _credtext = NULL; /* [0xC9F] released       */
        free(_text_spr);
        _text_spr = NULL;
        font_publish(NULL);
        _phase = ATTRACT_DONE;
        break;

    case ATTRACT_DONE:
    case ATTRACT_IDLE: break;
    }

    _compose_window();
    return _phase != ATTRACT_DONE;
}
