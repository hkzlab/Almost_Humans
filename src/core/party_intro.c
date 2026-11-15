#include "party_intro.h"
#include "audio.h"
#include "font.h"
#include "game_strings.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "game_resources.h"
#include "video.h"
#include "byteorder.h"
#include "width.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SCRIPT_COUNT 7

/* === the 8-slot script machine ============================================== */
struct Slot { // 16 B at [0x11fe]:0xFA8 + 0x10*i
    uint16_t dst; // +0: the stamp's dest word
    uint8_t count; // +2: the 8-tick countdown
    uint8_t script; // +3: the running id (1-indexed)
    uint8_t fpos; // +4: the frame position
    uint8_t loopc; // +5: the sub-list counter
    uint8_t sub[10]; // +6..15: the sub-list
};
struct Script {
    uint16_t len;
    const uint8_t *b;
};

/* the machine state (the ticker's + 2bda's _phase-split) */
enum {
    PH_MUSIC = 0, // the timing_loop at 2564 blocks the bake
    PH_FADEWAIT, // 25ec
    PH_SWAPWAIT, // 2bda's {28e9; 8b9a; JC}
    PH_SET1,
    PH_SET1_WAIT, // 2600..2699
    PH_SET2,
    PH_SET2_WAIT, // 269b..2703
    PH_GAMEOVER, // 2739: the need>members page
    PH_EXITWAIT,
    PH_PRESENT,
    PH_DONE // 2711..272b
};

/* 2bda's internal split: its own fade-wait (2bec..2bf2), THEN the 0xE0
 * mask reset (2bf4) + clear + band redraws (2c01..2c1c) - the tail is
 * what wipes the previous text and re-bakes the arena-empty band. */
enum {
    SW_FADEWAIT = 0,
    SW_TAIL
};

/* === the shared A000 model + the CRTC pair ================================== */
static uint16_t _page_draw = 0x1200; // [0x5ac4]: the stamp target
static uint16_t _page_disp = 0x8100; // [0x5ac6]: the displayed page
static uint16_t _stamp_dest; // [0x1204]
static uint16_t _marq_phase; // [0xf9e], 0..0x15F
static uint16_t _marq_cell; // [0xf9c], 0..0xB
static uint16_t _marq_hold; // [0xfa6]

/* === the [0x11fe] heap-segment buffers ====================================== *
 * Both are carved from the heap block the level_intro_screen setup takes at
 * [0x11fe] (253f/2546), so they are allocated by party_intro_start and
 * released when the screen exits. */
#define PARTY_SHEET_BYTES 2784u // [0x1202]: the 29x96 party sprites
#define PARTY_STRIP_BYTES 15360u // [0x1206]: the rescaled birdy
static uint8_t *_sheet;
static uint8_t *_strip;
static uint8_t _workpal[768]; // 202F:0x4bbc
/* 0x4d3c is not a buffer of its own: it is 0x4bbc + 0x180, the .PAL working
 * palette's own upper half (28d0 = 4 x 0x60 B from .PAL[0..31]). */
static uint8_t *const _pal_upper = _workpal + 0x180; // 202F:0x4d3c
static uint8_t _prevpal[0x180]; // 202F:0x1208: the party-write DAC snapshot
static uint8_t _ramp_tbl[150]; // CS:0x2DFE..0x2E93 (0x96 bytes)
static uint8_t *_bit_img; // the loaded lbm\party1.bit
static uint8_t *_font_bytes; // [0x1e1c]: the published text.spr

/* === the DS values the screen reads ========================================= */
static uint16_t _g_level; // [0x1f41]
static uint16_t _g_need; // [0x17fb]
static uint16_t _g_members; // [0x11fc]
static uint16_t _g_score; // [0x592]
static char _g_tribe[16]; // 0x1189

/* === the string table (verbatim templates; the digit _slots rewritten) =======
 * Sizes cover the widest of the five language columns (game_strings.h): the
 * largest template is 38 bytes (GER "ES WERDEN 0 HUMANS BENOETIGT, UM DIESE"),
 * and a wider number spills into the tail exactly as the original's does, so
 * each buffer keeps room for the digits the field can grow into. */
static char _s_all[32]; // 0x105F: "COMING UP.... LEVEL 000"  (3 _slots)
static char _s_need[48]; // 0x1077: "YOU WILL NEED 0 HUMANS TO COMPLETE"
static char _s_this[24]; // 0x109A: "THIS LEVEL"
static char _s_has[40]; // 0x10A5: "YOUR TRIBE NOW HAS 00 MEMBERS!" (2)
static char _s_code[40]; // 0x10CE: "THE CODE FOR THIS LEVEL IS....."
static char _s_score[36]; // 0x10EE: "YOUR SCORE IS 00000000000"
static char _s_fire[48]; // 0x1108: "PRESS FIRE TO START LEVEL..."

static const uint8_t _ramp10[10] = // 2e94's ramp -> CS:[0x2DFE+i]
    { 0x80, 0xA0, 0xC0, 0xE0, 0xE0, 0xE0, 0xE0, 0xC0, 0xA0, 0x80 };
static struct Slot _slots[8];
static struct Script _scripts[SCRIPT_COUNT];

static uint8_t *_script_store; // the seeded bodies (memcpy'd)

/* The file-image script seed: the tables are static bytes of the MZ image
 * (202F:0xFA8 / 0x1028) and no traced code rewrites the sub-lists.  The
 * countdowns {10,8,6,4,5,0,7,9} phase the slots; slot 5's 0 counts up to 8
 * on its first tick (2909's DEC-then-test). */
static const uint8_t _slots_file[128] = {
    0xf0, 0x2f, 10, 1, 0, 0, 1, 1, 1, 3, 6, 3, 6, 1, 1, 1,
    0xf8, 0x31, 8,  1, 0, 0, 6, 6, 6, 6, 4, 5, 4, 5, 1, 1,
    0x50, 0x30, 6,  1, 0, 0, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4,
    0x98, 0x31, 4,  1, 0, 0, 1, 6, 2, 5, 3, 4, 4, 3, 4, 1,
    0x08, 0x31, 5,  1, 0, 0, 6, 6, 5, 4, 4, 3, 7, 4, 1, 3,
    0x48, 0x33, 0,  1, 0, 0, 1, 3, 7, 3, 7, 6, 3, 3, 1, 5,
    0x20, 0x33, 7,  1, 0, 0, 6, 3, 4, 3, 5, 4, 3, 4, 1, 4,
    0x48, 0x34, 9,  1, 0, 0, 3, 7, 3, 7, 3, 7, 6, 4, 3, 2
};
static const uint8_t _scripts_file[41] = {
    /* DS:0x1036..0x105E */
    0x01, 0x02, 0x03, 0x02, 0xFF, // 0x1036 s1
    0x05, 0x06, 0x07, 0x08, 0x09, 0x06, 0x05, 0x04, 0xFF, // 0x103b s2
    0x0a, 0x0b, 0xFF, // 0x1044 s3
    0x0a, 0x0d, 0x0e, 0x0f, 0x0e, 0x0d, 0xFF, // 0x1047 s4
    0x11, 0x12, 0x11, 0x0d, 0xFF, // 0x104e s5
    0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0xFF, // 0x1053 s6
    0x0a, 0x0c, 0xFF // 0x105c s7
};

static uint8_t _phase = PH_FADEWAIT; // the machine _phase
static uint8_t _step; // the in-_phase op counter
static uint8_t _wait_left; // 297f's AX
static uint8_t _key_caught; // the JNS's source
static uint8_t _present_left; // 835a's 0x4B cycles
static uint8_t _swap_next; // the 2bda's tail's next _phase
static uint8_t _game_over_mode; // level_intro_screen 2739's variant
static uint8_t _sw_phase = SW_FADEWAIT;

void party_intro_seed_scripts(const uint8_t *bytes, uint32_t len)
{
    /* the [live] pointer table: the DS offsets into the first body 0x1036 */
    uint32_t offs[SCRIPT_COUNT] = { 0x1036u, 0x103bu, 0x1044u, 0x1047u,
                                    0x104eu, 0x1053u, 0x105cu };
    g_osys->memFree(_script_store);
    _script_store = (uint8_t *)g_osys->memAlloc(len ? len : 1u);
    if (_script_store) memcpy(_script_store, bytes, len);
    for (uint16_t i = 0; i < SCRIPT_COUNT; i++) {
        uint16_t rel =
            (offs[i] >= 0x1036u) ? ((uint16_t)(offs[i] - 0x1036u)) : 0u;
        _scripts[i].len = (rel < len) ? ((uint16_t)(len - rel)) : 0u;
        _scripts[i].b = _script_store ? (_script_store + rel) : NULL;
    }
}

void party_intro_seed_slots(const uint8_t *raw) // the 128 B at DS:0xFA8
{
    if (!raw) {
        memset(_slots, 0, sizeof _slots);
        return;
    }
    for (uint16_t i = 0; i < 8u; i++) {
        const uint8_t *r = raw + i * 16u;
        struct Slot *s = &_slots[i];
        s->dst = rd_le_u16(r);
        s->count = r[2];
        s->script = r[3];
        s->fpos = r[4];
        s->loopc = r[5];
        memcpy(s->sub, r + 6u, 10u);
    }
}

/**
 * @brief Seed the intro slots from the live party state.
 */
static void _seed_slots_live(void)
{
    party_intro_seed_slots(_slots_file);
    party_intro_seed_scripts(_scripts_file, sizeof _scripts_file);
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
void party_intro_format_score(uint32_t v, char *out)
{
    static const uint32_t divs[8] = { 100000000u, 10000000u, 1000000u, 100000u,
                                      10000u,     1000u,     100u,     10u };
    uint8_t sig = 0; // 2b70's DI
    for (uint16_t i = 0; i < 8u; i++) {
        uint32_t q = v / divs[i]; // the 2b72 subcount
        if (q != 0) {
            *out++ = (char)('0' + q);
            sig = 0x30u;
        } else if (sig != 0) {
            *out++ = '0';
        }
        v %= divs[i];
    }
    *out++ = (char)('0' + (v % 10u)); // 2b68
    *out = '\0';
}
#endif /* HUMANSRE_TEST_PROBES */

/** @brief the party's in-place number formatter (1000:3F1B).
 *
 * Writes the digits at `dst` and returns past them; the template's tail after
 * the digits is untouched, so a wider number spills into it exactly as the
 * original's "HUMANS TO COMPLETE" / " MEMBERS!" tails do.
 *
 * @param dst the destination buffer
 * @param v the value
 * @return the new end of the string
 */
static char *_put_number(char *dst, uint32_t v)
{
    char buf[10];
    uint8_t n = 0;
    do {
        buf[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && n < 10u);
    for (uint16_t i = 0; i < n; i++) dst[i] = buf[n - 1u - i];
    return dst + n; // the formatter's SI
}

/** @brief 0x2660: append `src`, replacing the template's tail after the digits
 * (" MEMBERS!" for the tribe line).
 *
 * @param dst the destination buffer
 * @param src the source buffer
 * @return the new end of the string
 */
static char *_append_str(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = '\0';
    return dst;
}

/* --- the A000 helpers ------------------------------------------------ */

/** @brief 2AF5: clear arena rows 20..87 (0x640..0x1B80). */
static void _clear_canvas(void) // 2af5: rows 20..87 = 0x640..0x1B80
{ memset(video_arena() + 0x640u, 0, 68u * 80u); }

/**
 * @brief The intro's page flip.
 */
static void _flip_page(void) // 87f8
{
    uint16_t t = _page_draw;
    _page_draw = _page_disp;
    _page_disp = t;
}

/**
 * @brief 2ed7 deplanarise_intro_page: the .BIT rows plane-baked at pitch 88 +8
 *
 * @param bit the bit index
 * @param pagebase the page base
 */
static void _bake_page(const uint8_t *bit, uint32_t pagebase)
{
    video_vram_planarise(bit, UINT32_MAX, (uint16_t)(pagebase + 8u), 88u, 80u,
                         VIDEO_PAGE_H_MODE13H);
}

/**
 * @brief 2c1d draw_intro_frame - the closed funnel
 *
 * @param pagebase the page base
 */
static void _band_stamp(uint32_t pagebase)
{
    for (uint16_t row = 0; row < 68u; row++) {
        uint8_t m = _ramp_tbl[row + 16u]; // 2c1d's OUT 3CF = CS:0x2E0E+row
        uint16_t a = (uint16_t)(pagebase + 0x6E8u + row * 88u);
        const uint8_t *src = video_arena() + 0x640u + row * 80u;
        for (uint16_t k = 0; k < 80u; k++) {
            /* LODSB [BX+0x640+80+k] */
            video_vram_wm2((uint16_t)(a + k), src[k], m);
        }
    }
}

/**
 * @brief 29d9 blit_slot_colour_block: 24 rows x 4 bytes; frame clamps 0x1C
 *
 * @param frame the frame index
 */
static void _blit_slot(uint32_t frame)
{
    if (frame > 0x1Cu) frame = 0x1Cu;
    const uint8_t *s = _sheet + frame * 96u;
    uint32_t di = (uint32_t)_stamp_dest + (uint32_t)_page_draw + 8u;
    enum {
        MASK = 0x40u
    }; // 28e9's 299b(AH=0x40) live pin
    for (uint16_t r = 0; r < 24u; r++) {
        for (uint16_t w = 0; w < 4u; w++) {
            uint8_t c = s[r * 4u + w];
            /* write mode 2 reads AL's bits 0..3 only - the _sheet's nibble
             * is the plane value itself; no palette bias applies to the
             * byte (the +0xC0 story lives in the palette table, not here). */
            video_vram_wm2((uint16_t)(di + w), c, MASK);
        }
        di += 88u; // 4 written + ADD DI,0x54 = 88
    }
}

/**
 * @brief 2A76..2A95: the marquee's source offset and destination row for a
 * phase/cell pair.
 *
 * Factored out so the 0x160-phase word can be tested without a full screen.
 *
 * @param phase the [0xf9e] phase (0..0x15F)
 * @param cell the [0xf9c] cell (0..0xB)
 * @param src the source offset (out)
 * @param dhi the destination high byte (out)
 */
static void _marquee_offset(uint16_t phase, uint16_t cell, uint16_t *src,
                            uint16_t *dhi)
{
    uint16_t v = (uint16_t)(cell * 4u + (phase & 3u)); // 2a76..2a83
    if (src) *src = u16(v * 320u); // 2a85..2a8d: v*256 + v*64
    if (dhi) *dhi = (uint16_t)(0x2260u + phase / 4u); // 2a8f..2a95
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
void party_intro_marquee_for_probe(uint16_t phase, uint16_t cell, uint16_t *src,
                                   uint16_t *dhi)
{ _marquee_offset(phase, cell, src, dhi); }
#endif /* HUMANSRE_TEST_PROBES */

/**
 * @brief 2a3a draw_scrolling_marquee
 */
static void _marquee(void)
{
    _marq_phase++;
    if (_marq_phase == 0x160u) _marq_phase = 0u; // INC; CMP 0x160
    _marq_hold--; // 2a4c: DEC first, then test
    if (_marq_hold == 0u) {
        _marq_hold = 5u;
        _marq_cell = (_marq_cell + 1u) % 0xCu;
    }

    uint16_t src, dhi;
    _marquee_offset(_marq_phase, _marq_cell, &src, &dhi);

    enum {
        MASK = 0x40u
    }; // 28e9's 299b(AH=0x40) covers 2a3a
    for (uint16_t r = 0; r < 32u; r++) {
        uint16_t di = _page_draw + dhi + r * 88u;
        for (uint16_t k = 0; k < 10u; k++) {
            uint8_t c = _strip[src + r * 10u + k]; // 5 LODSW = 10 B/row
            video_vram_wm2((uint16_t)(di + k), c, MASK);
        }
    }
}

/**
 * @brief 28e9 animate_entities - verbatim: every tick, per slot
 */
static void _anim_entities(void)
{
    for (uint16_t i = 0; i < 8u; i++) {
        struct Slot *s = &_slots[i];
        uint8_t dl = 0;
        s->count = (uint8_t)(s->count - 1u);
        if (s->count == 0u) {
            s->count = 8u;
            dl = 1u;
        } // 2909..2912
        for (uint16_t pass = 0; pass < 40u; pass++) { // the 2914 walk
            if (s->script == 0u || s->script > SCRIPT_COUNT) break;
            const struct Script *sc = &_scripts[s->script - 1u];
            if (sc->b == NULL || sc->len == 0u) break;
            uint8_t bx = s->fpos; // 2920: BX = [SI+4]
            s->fpos = (uint8_t)(bx + dl); // 2925: ADD before the read
            uint8_t dh = sc->b[bx]; // 2928
            if (dh != 0xFFu) { // 292a: the stamp
                _stamp_dest = s->dst;
                _blit_slot(dh);
                break; // 295f: next slot
            }
            uint8_t cl = ((uint32_t)s->loopc + 1u) % 10u; // 292f..293b
            s->loopc = cl;
            s->script = s->sub[cl]; // the sub-list ids
            s->fpos = 0u; // 294a, redo 2914
            if (s->script == 0u || s->script > SCRIPT_COUNT) break;
        }
    }
    _marquee();
    _flip_page();
}

/** @brief 2E94 _draw_text_ramped: the glyph stroke lands in the canvas only.
 *
 * The per-call 28E9 animates the slots/marquee and flips, but the text does
 * not reach the display until the next 2C1D (inside 2B9C/2BDA): all text
 * fades in together at the first band redraw.
 *
 * @param s the text line
 * @param centreX the centring x
 * @param row the row
 */
static void _draw_text_ramped(const char *s, uint32_t centreX, uint32_t row)
{
    memcpy(_ramp_tbl + row, _ramp10, 10u); /* 2e94: the ramp slides with the
                                            * row param - CS:0x2DFE + row */
    if (_font_bytes) font_draw_text(video_arena(), s, centreX, row);
    _anim_entities(); /* 28e9: _slots + _marquee + flip -
                                            * the arena glyph is NOT rendered */
}

/** @brief 28d0 palette_copy_low_to_upper: 4 x 0x60 bytes .PAL[0..31] ->
 * .PAL[128..255] (so the window fade targets the blues, not the file's grey
 * upper half).
 */
static void _copy_palette(void)
{
    for (uint16_t p = 0; p < 4u; p++)
        memcpy(_pal_upper + p * 0x60u, _workpal, 0x60u);
}

/** @brief 2b9c _intro_anim: the backdrop band twice, then the snapshot restore and
 * the 0x80..0xFF window fade toward it - 0x1208 (25bc) is the .PAL's own
 * upper half, so the text window dissolves from the 28d0 blues into the
 * file's grey ramp (the fade-in).
 */
static void _intro_anim(void)
{
    _band_stamp(_page_draw);
    _anim_entities();
    _band_stamp(_page_draw);
    _anim_entities();
    memcpy(_pal_upper, _prevpal, 0x180u); // 2bbf..2bc8: 0x1208 -> 0x4d3c
    palette_build_fade(0x80u, 0x80u, 0x28u, _pal_upper); // 2bca: BX=0x80
}

/** @brief 2bda fade_and_animate - verbatim _phase-split: 2bda runs [28d0]; builds
 * the window fade; THEN BLOCKS ON ITS OWN FADE ({28e9; 8b9a; JC} at
 * 2bec..2bf2) before the 0xE0 mask reset (2bf4), the 2af5 clear (2c01)
 * and the two 2c1d band redraws (2c04..2c1c).  The clear/redraw tail is
 * therefore separated from the next set's stamps by the fade's own
 * completion - the original's "text fades in at the next set" rhythm:
 * every 2c1d renders the arena under the fade state CURRENT AT THAT
 * MOMENT, and the holds are the built fades' converged end-states.
 *
 * @param next the next index
 */
static void _swap_begin(uint32_t next)
{
    _copy_palette(); // 2bda (28d0)
    palette_build_fade(0x80u, 0x80u, 0x28u, _pal_upper);
    _swap_next = (uint8_t)next; // 0xFF = no next phase
    _sw_phase = SW_FADEWAIT; // 2bec: the fade runs
    _phase = PH_SWAPWAIT;
}

/* === the asset prep ========================================================== */

/**
 * @brief 27eb expand_nibbles_to_bytes
 *
 * @param src the source buffer
 * @param n the count
 * @param dst the destination buffer
 * @return 1 when the output table is large enough, 0 otherwise
 */
static int32_t _expand_nibbles(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    if (n < 0x570u) return 0;
    uint16_t di = 0;
    for (uint16_t i = 0; i < 0x570u; i++) {
        uint8_t al = src[i], ah = al;
        for (uint16_t t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        dst[di + 1u] = al & 0xFu;
        for (uint16_t t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        dst[di] = al & 0xFu;
        di += 2u;
    }
    return 1;
}

/**
 * @brief 28c2 shift_right_1: shift a 0xA0-byte row right by one bit.
 *
 * @param d the row
 */
static void _shift_right1(uint8_t *d)
{
    uint8_t c = 0;
    for (uint16_t i = 0; i < 0xA0u; i++) {
        uint8_t c2 = d[i] & 1u;
        d[i] = (uint8_t)(((d[i] >> 1) | (c << 7)) & 0xFFu);
        c = c2;
    }
}

/**
 * @brief 288d _expand_pixels into ES:DI
 *
 * @param src the source buffer
 * @param dip the switch dip
 */
static void _expand_pixels(const uint8_t *src, uint16_t *dip)
{
    uint16_t di = *dip;
    for (uint16_t i = 0; i < 0xA0u; i++) {
        uint8_t al = src[i], ah = al;
        for (uint16_t t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        _strip[di + 1u] = al & 0xFu;
        for (uint16_t t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        _strip[di] = al & 0xFu;
        di += 2u;
    }
    *dip = di;
}

/**
 * @brief 283e rescale_bitmap_rows
 *
 * @param src the source buffer
 */
static void _rescale_rows(const uint8_t *src)
{
    uint16_t di = 0;
    for (uint16_t g = 0; g < 12u; g++) {
        uint8_t work[0xA0u];
        memcpy(work, src + g * 0xA0u, 0xA0u);
        for (uint16_t t = 0; t < 4u; t++) _shift_right1(work);
        _expand_pixels(work, &di);
        for (uint16_t k = 0; k < 4u; k++) {
            _shift_right1(work);
            if (k < 3u) _expand_pixels(work, &di);
        }
    }
}

void party_intro_set_values(uint16_t level, uint16_t need, uint16_t members,
                            uint16_t score, const char *tribe)
{
    _g_level = level;
    _g_need = need;
    _g_members = members;
    _g_score = score;
    memset(_g_tribe, 0, sizeof _g_tribe);
    if (tribe) strncpy(_g_tribe, tribe, sizeof _g_tribe - 1u);
    /* the image's verbatim strings (202F:0x105F..; the digit fields are
     * fixed-width and the formatter rewrites them in place, 0x3F1B) */
    strncpy(_s_all, game_string(GAME_STRING_PARTY_COMING_UP),
            sizeof _s_all - 1u);
    strncpy(_s_need, game_string(GAME_STRING_PARTY_NEED), sizeof _s_need - 1u);
    strncpy(_s_this, game_string(GAME_STRING_PARTY_THIS_LEVEL),
            sizeof _s_this - 1u);
    strncpy(_s_has, game_string(GAME_STRING_PARTY_HAS), sizeof _s_has - 1u);
    strncpy(_s_code, game_string(GAME_STRING_PARTY_CODE), sizeof _s_code - 1u);
    strncpy(_s_score, game_string(GAME_STRING_PARTY_SCORE),
            sizeof _s_score - 1u);
    strncpy(_s_fire, game_string(GAME_STRING_PARTY_FIRE), sizeof _s_fire - 1u);
}

void party_intro_set_game_over(uint8_t on) { _game_over_mode = on ? 1 : 0; }

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
uint16_t party_intro_score(void) { return _g_score; }
#endif /* HUMANSRE_TEST_PROBES */

/**
 * @brief Load one intro asset by name.
 *
 * @param name the name
 * @return the pointer, or NULL when unavailable
 */
static uint8_t *_load(const char *name)
{
    uint32_t sz = 0;
    uint16_t ck = 0;
    return res_load_chunk(name, &sz, &ck);
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
const uint8_t *party_intro_workpal(void) { return _workpal; }
#endif /* HUMANSRE_TEST_PROBES */

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
const uint8_t *party_intro_pal_upper(void) { return _pal_upper; }
#endif /* HUMANSRE_TEST_PROBES */

uint8_t party_intro_start(void)
{
    char name[24];
    uint8_t *b;

    video_set_mode(VIDEO_MODE_X); /* level_intro_screen 1000:2508 calls
                                        * reset_display_mode (1000:82c6)  */
    /* level_intro_screen is a self-contained blocking routine in the
     * original: every level re-enters it at 1000:2508.  The port's split
     * start/tick keeps the machine's statics, so a second briefing would
     * inherit the first one's final PH_DONE and exit on its first tick. */
    _phase = PH_FADEWAIT;
    _step = 0u;
    _wait_left = 0u;
    _key_caught = 0;
    _swap_next = 0u;
    _present_left = 0u;
    _sw_phase = SW_FADEWAIT;
    _page_draw = 0x1200u;
    _page_disp = 0x8100u; // 2519/2523
    /* 2a3a's counters are static DS data: [0xf9c]=0, [0xf9e]=0, [0xfa6]=1
     * (the MZ image; a 0 hold DEC-wraps and freezes the flap on run 1) */
    _marq_phase = 0u;
    _marq_cell = 0u;
    _marq_hold = 1u;
    g_osys->memFree(_sheet);
    _sheet = g_osys->memAlloc(PARTY_SHEET_BYTES);
    g_osys->memFree(_strip);
    _strip = g_osys->memAlloc(PARTY_STRIP_BYTES);
    if (!_sheet || !_strip) return 0;
    video_vram_clear();
    memset(_ramp_tbl, 0xE0u, sizeof _ramp_tbl); // the table's clean state
    _clear_canvas();
    _seed_slots_live(); // the DS:0xFA8's [live]

    b = _load(game_resource_name(GAME_RES_APL_PARTY_SPR)); // 2570
    if (!b) return 0;
    {
        uint8_t ok = (uint8_t)(_expand_nibbles(b, 0x570u, _sheet));
        g_osys->memFree(b);
        if (!ok) return 0;
    }

    b = _load(game_resource_name(GAME_RES_APL_BIRDY_SPR)); // 257c
    if (!b) return 0;
    _rescale_rows(b);
    g_osys->memFree(b);

    {
        uint8_t d = (uint8_t)(_g_level & 7u) + 1u; // 2519: '1'+(lvl&7)
        snprintf(name, sizeof name,
                 game_resource_name(GAME_RES_LBM_PARTY_PAL_FMT), d);
    }
    b = _load(name); // 2588
    if (!b) return 0;
    palette_load_working(b); // 2591 (8d24): -> 0x4bbc
    memcpy(_workpal, palette_working(), 768u);
    g_osys->memFree(b);

    b = _load(game_resource_name(GAME_RES_LBM_PARTY1_BIT)); // 259a
    if (!b) return 0;
    g_osys->memFree(_bit_img);
    _bit_img = b;

    b = _load(game_resource_name(GAME_RES_APL_TEXT_SPR)); // 25af
    g_osys->memFree(_font_bytes);
    _font_bytes = b;
    if (b) {
        font_publish(b);
    } else {
        _font_bytes = NULL;
        font_publish(NULL);
    }

    /* 25bc: 0x1208 <- DS:0x4d3c (the .PAL's upper half, verbatim) */
    memcpy(_prevpal, _pal_upper, 0x180u);

    /* 1000:2564-256d: play_tune("mus\party") / music_resume / timing_loop.
     * The original blocks inside the fade-in before the page bake; the port
     * holds in PH_MUSIC (party_intro_tick) instead. */
    audio_play_tune(audio_tune_name(AUDIO_TUNE_PARTY));
    audio_music_resume();
    audio_fade_in();
    _phase = PH_MUSIC;
    return 1;
}

/** @brief the bake + palette the fade held back (1000:25ca..25dd). */
static void _party_begin(void)
{
    _bake_page(_bit_img, _page_draw); // 25ca..25d8: the bake x2
    _anim_entities();
    _bake_page(_bit_img, _page_draw);

    _copy_palette(); // 25da (28d0)
    palette_build_fade(0u, 0x100u, 0x23u, _workpal); // 25dd
}

/* === the ticker =============================================================== */

uint8_t party_intro_tick(const InputState *in)
{
    uint8_t fire = (uint8_t)(in->joyBits & JOYBITS_FIRE);
    uint8_t anyk = (in->lastKey != 0u);

    switch (_phase) {
    case PH_MUSIC: // 2564-256d's timing_loop: the bake waits for it
        if (audio_fade_busy()) return 1;
        _party_begin();
        _phase = PH_FADEWAIT;
        break;

    case PH_FADEWAIT:
        _anim_entities();
        if (!palette_fade_active()) {
            _phase = _game_over_mode ? PH_GAMEOVER : PH_SET1;
            _step = 0u;
        }
        break;

    case PH_SWAPWAIT: /* 2bec..2c1c: the fade wait,
                                                * THEN the tail (verbatim the
                                                * order inside 2bda)         */
        _anim_entities(); // 2bec's 28e9
        if (_sw_phase == SW_FADEWAIT) {
            if (!palette_fade_active()) _sw_phase = SW_TAIL; // 2bf2's JC out
            break; // no tail before the fade ends
        }
        /* the 2bf4 tail: the mask reset + clear + band redraws, once */
        memset(_ramp_tbl, 0xE0u,
               sizeof _ramp_tbl); // 2bf4: the 0x96 fill
        _clear_canvas(); // 2c01 (2af5)
        _band_stamp(_page_draw);
        _anim_entities();
        _band_stamp(_page_draw);
        _anim_entities();
        if (_swap_next == PH_SET1) {
            _phase = PH_SET1;
            _step = 0u;
        } else if (_swap_next == PH_SET2) {
            _phase = PH_SET2;
            _step = 0u;
        } else { // the exit: 270e's tail
            _clear_canvas(); // 270e (2af5)
            {
                static uint8_t black[768];
                palette_build_fade(0u, 0x100u, 0x23u, black); // 2711
            }
            _phase = PH_EXITWAIT;
        }
        _sw_phase = SW_FADEWAIT; // the tail ran once
        break;

    case PH_SET1: // 2600..268d
        switch (_step) {
        case 0u: // 2600..2611
            _clear_canvas();
            *_put_number(
                _s_all + game_string_num_offset(GAME_STRING_PARTY_COMING_UP),
                _g_level) = '\0'; // 260e
            _draw_text_ramped(_s_all, 0xA0u, 0x14u);
            break;
        case 1u: /* 2623..263a: no NUL - the
                                                * " HUMANS TO COMPLETE" tail
                                                * survives the patch        */
            (void)_put_number(
                _s_need + game_string_num_offset(GAME_STRING_PARTY_NEED),
                _g_need);
            _draw_text_ramped(_s_need, 0xA0u, 0x28u);
            break;
        case 2u:
            _draw_text_ramped(_s_this, 0xA0u, 0x34u); // 2640
            break;
        case 3u: // 2652..268a
        {
            char *q = _append_str(
                _put_number(_s_has +
                                game_string_num_offset(GAME_STRING_PARTY_HAS),
                            _g_members),
                game_string(GAME_STRING_PARTY_MEMBERS_TAIL)); // 2660
            if (_g_members == 1u) { // 266d..267a
                q -= 3;
                *q = '!';
                *(q + 1u) = '\0';
            }
        }
            _draw_text_ramped(_s_has, 0xA0u, 0x48u);
            break;
        }
        if (_step < 3u) _step++;
        else {
            _intro_anim();
            _phase = PH_SET1_WAIT;
            _wait_left = 0xC8u;
            _key_caught = 0;
        }
        break;

    case PH_SET1_WAIT: // 2693..2699
        _anim_entities();
        if (fire || anyk) {
            _key_caught = 1;
            _wait_left = 0u;
        } else if (_wait_left) _wait_left--;
        if (!_wait_left) {
            if (_key_caught) _swap_begin((uint32_t)-1); // 270b
            else _swap_begin(PH_SET2);
        }
        break;

    case PH_SET2: // 269e..26f7
        switch (_step) {
        case 0u:
            _clear_canvas(); // 269e (2af5)
            _draw_text_ramped(_s_code, 0xA0u, 0x14u); // 26a1
            break;
        case 1u:
            _draw_text_ramped(_g_tribe, 0xA0u, 0x20u); // 26b3
            break;
        case 2u:
            *_put_number(_s_score +
                             game_string_num_offset(GAME_STRING_PARTY_SCORE),
                         _g_score) = '\0'; // 26d3
            _draw_text_ramped(_s_score, 0xA0u, 0x34u);
            break;
        case 3u:
            _draw_text_ramped(_s_fire, 0xA0u, 0x48u); // 26e8
            break;
        }
        if (_step < 3u) _step++;
        else {
            _intro_anim();
            _phase = PH_SET2_WAIT;
            _wait_left = 0xC8u;
            _key_caught = 0;
        }
        break;

    case PH_SET2_WAIT:
        _anim_entities();
        if (fire || anyk) {
            _key_caught = 1;
            _wait_left = 0u;
        } else if (_wait_left) _wait_left--;
        if (!_wait_left) {
            if (_key_caught) _swap_begin((uint32_t)-1); // 270b
            else _swap_begin(PH_SET1);
        }
        break;

    case PH_GAMEOVER: /* 2739..27e9: the GAME OVER
                                                * variant of the briefing  */
        if (_step == 0u) {
            _clear_canvas();
            _draw_text_ramped(game_string(GAME_STRING_PARTY_GAME_OVER), 0xA0u,
                              0x14u);
            _draw_text_ramped(game_string(GAME_STRING_PARTY_TOO_SMALL), 0xA0u,
                              0x28u);
            _draw_text_ramped(game_string(GAME_STRING_PARTY_IN_RACE), 0xA0u,
                              0x34u);
            _draw_text_ramped(game_string(GAME_STRING_PARTY_TRY_AGAIN), 0xA0u,
                              0x48u);
            _intro_anim(); /* 2b9c: the band redraws carry
                                            * the arena text to the display */
            _step = 1u;
        }
        _anim_entities();
        if (fire || anyk) _swap_begin((uint32_t)-1);
        break;

    case PH_EXITWAIT: // 2720: {28e9; 8b9a; JC}
        _anim_entities();
        if (!palette_fade_active()) {
            _phase = PH_PRESENT;
            _present_left = 0x4Bu;
            audio_fade_out(); // 1000:2728 present_frames_75
        }
        break;

    case PH_PRESENT: // 2728 (835a), 272b
        if (_present_left) _present_left--;
        else if (audio_fade_busy()) return 1; // the original's 2x wait
        else {
            /* 1000:272B: the music_stop that ends present_frames_75. */
            audio_music_stop();
            _phase = PH_DONE;
        }
        break;

    case PH_DONE:
    default:
        /* the briefing's carve at [0x11fe] (1000:253f/2546) and the arena it
         * draws through are dead once the screen has exited */
        g_osys->memFree(_sheet);
        _sheet = NULL;
        g_osys->memFree(_strip);
        _strip = NULL;
        g_osys->memFree(_bit_img);
        _bit_img = NULL;
        video_arena_release();
        break;
    }

    /* the compose (the attract's contract): the display page into the
     * flat 320x200 page 0 for videoPresent */
    party_intro_compose(video_page(0));

    if (_phase == PH_DONE) return 0; // the screen exited
    return 1;
}

void party_intro_compose(uint8_t *pg)
{
    video_vram_present(pg, (uint16_t)(_page_disp + 8u), 88u, 0u,
                       VIDEO_PAGE_H_MODE13H);
}
