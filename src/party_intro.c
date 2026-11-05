/* party_intro.c — the level-intro party screen: level_intro_screen
 * (1000:2508–27EA) with every helper its body reaches, ported
 * listing-for-listing from re_map.md → "The level-intro party screen
 * decoded end to end (2026-10-29)".  Both render gates have been
 * measurement-closed this session:
 *
 *   • the 2C1D funnel: hardware VGA write-mode-2 semantics — per plane p,
 *     post_p = (pre_p & ~mask) | ((src>>p)&1 replicated over the masked
 *     positions) — fitted 21760/21760 against parked-state dumps.
 *   • the A000↔.BIT↔screen layout law: the GC-4=p view byte at offset n is
 *     the .BIT pixel (y = 20+q, x = 4n+p); an A000 byte = 4 pixels; the
 *     band = screen/.BIT rows 20..87 (83.5% .BIT-exact, 0 mismatches off
 *     the stamp bands); the composed page = the .BIT verbatim 1:1 per row
 *     (the PNG law peaks at sy = 20 with the same x-law).
 *
 * Memory model (attract.c's, with the party's CRTC numbers):
 *   _vram[65536][4]: one A000 byte = a 4-pixel group; plane p = x&3; the
 *   row pitch = 88 bytes (CRTC[13] = 0x2C words: 80 written + 8 hidden);
 *   the page pair 0x1200/0x8100 (A000 byte addresses); the screen row y's
 *   byte base = pagebase + 8 + y*88; the band base = pagebase + 0x6E8.
 * The heap _canvas ([0x1e20] = the [0x11fe] segment) is the 200-row × 80-B
 * draw arena: the draw anchors are absolute arena rows (y = 0x14 → the
 * byte 0x640), 2af5 clears exactly rows 20..87 (0x640..0x1B80 as words)
 * before each stamp set, and 2c1d reads those rows to stamp the band.
 * The identity 20·80 = 0x640 marshals the three layers.
 *
 * Machine (one op = one tick where the original iterates):
 *   the 5 loads + consumers (2570..25b6); 0x1208 <- 0x4D3C (25bc);
 *   the bake ×2 with the 28e9 flip between (25ca..25d8); 28d0 (25da);
 *   palette_build_fade(0, 0x100, 0x23, 0x4bbc) (25dd); the 25ec wait; the
 *   2600 loop:
 *     2600: 2af5; 4× stanza (number-slot fmt + 2e94 draw + anim);
 *           _intro_anim 2b9c; wait_frames_or_key 0xC8; JNS→exit;
 *           2bda fade_and_animate;
 *     269e: 2af5; the second set; _intro_anim; wait 0xC8; JNS→exit;
 *           2bda; JMP 2600;
 *   and the 270b exit: 2bda; 2af5; whole-256 fade 35 steps toward the
 *   zeroed ramp 0x48BC; wait; present_frames_75 (0x4B frames), music_stop,
 *   RET.
 * The GAME OVER variant (2739..27e9) is level-A-unreachable (the need >
 * members branch); the port keeps the normal branch's shape and documents
 * the variant's absence at P_DONE exactly like the scene player's.
 *
 * The animators' slot seeding is CLOSED (2026-10-30 pass-3): the 8 slot
 * structs at DS:0xFA8, the 7 script pointers at DS:0x1028 and the script
 * bodies DS:0x1036..0x105E are STATIC in the MZ image (Ghidra 202F:0xFA8
 * hexdump == the live DS:0xFA8 read-back, sub-lists byte-identical) — no
 * seed writer exists.  The tables below are the file's verbatim bytes.
 * The interpreter is 28e9 verbatim: per slot per tick, DEC the countdown;
 * when it hits zero reload 8 and set the advance _step; ALWAYS walk the
 * script at [SI+4] (BX = the pre-increment position), stamp the frame it
 * holds every tick (the 2a3a _marquee passes over the dancers and the next
 * stamp restores them); on 0xFF switch to sub-list entry (loopc+1) mod 10,
 * fpos = 0, and re-walk from the new script's byte 0 in the same tick.
 * The bird _marquee's source group is (cell*4 + (_phase&3)) at a 320-byte
 * stride [0x1206] — exactly the rescale group grid (47*320+319 = 15359 =
 * the _strip's last byte; the numbers close).
 *
 * The palette buffers (2026-10-30): 0x4bbc is the .PAL working palette, and
 * 0x4d3c is NOT a separate buffer — it is 0x4bbc + 0x180, the file's own
 * upper half (entries 128..255). 28d0 replicates .PAL[0..31] four times
 * over it, 25bc snapshots it to 0x1208, 2b9c fades the text window from the
 * 28d0 blues into that grey snapshot (the fade-in), and 2bda re-runs 28d0
 * and fades back (the text then equals the background and vanishes).
 *
 * Port-only caveats (port_deviations.md D-01/D-19): with no disc the loads
 * fail and the screen is
 * skipped — the _load-failure path the scene player established; the
 * music/marker calls (play_tune 8437, music_resume 83f7, timing_loop 84a5,
 * music_stop 8389, INT 60 service 0xA inside present_frames_75) ride the
 * OSystem audio contract, which the null build stubs.
 */
#include "party_intro.h"
#include "font.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "video.h"
#include "byteorder.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SCRIPT_COUNT 7

/* === the 8-slot script machine ============================================== */
struct slot { /* 16 B at [0x11fe]:0xFA8 + 0x10*i    */
    uint16_t dst; /* +0: the stamp's dest word          */
    uint8_t count; /* +2: the 8-tick countdown           */
    uint8_t script; /* +3: the running id (1-indexed)     */
    uint8_t fpos; /* +4: the frame position             */
    uint8_t loopc; /* +5: the sub-list counter           */
    uint8_t sub[10]; /* +6..15: the sub-list               */
};
struct script {
    uint32_t len;
    const uint8_t *b;
};

/* the machine state (the ticker's + 2bda's _phase-split) */
enum {
    PH_FADEWAIT = 0, /* 25ec                   */
    PH_SWAPWAIT, /* 2bda's {28e9; 8b9a; JC} */
    PH_SET1,
    PH_SET1_WAIT, /* 2600..2699             */
    PH_SET2,
    PH_SET2_WAIT, /* 269b..2703             */
    PH_GAMEOVER, /* 2739: the need>members page */
    PH_EXITWAIT,
    PH_PRESENT,
    PH_DONE /* 2711..272b             */
};

/* 2bda's internal split: its own fade-wait (2bec..2bf2), THEN the 0xE0
 * mask reset (2bf4) + clear + band redraws (2c01..2c1c) — the tail is
 * what wipes the previous text and re-bakes the _canvas-empty band. */
enum {
    SW_FADEWAIT = 0,
    SW_TAIL
};

/* === the A000 _vram + the CRTC pair ========================================== */
static uint8_t _vram[65536][4]; /* [address][plane]                   */
static uint16_t _page_draw = 0x1200; /* [0x5ac4]: the stamp target         */
static uint16_t _page_disp = 0x8100; /* [0x5ac6]: the displayed page       */
static uint16_t _stamp_dest; /* [0x1204]                           */
static uint16_t _marq_phase; /* [0xf9e], 0..0x15F                  */
static uint16_t _marq_cell; /* [0xf9c], 0..0xB                    */
static uint16_t _marq_hold; /* [0xfa6]                            */

/* === the [0x11fe] heap-segment buffers ====================================== */
static uint8_t _canvas[16000]; /* [0x1e20]: the 200x80 draw arena    */
static uint8_t _sheet[2784]; /* [0x1202]: the 29x96 party sprites  */
static uint8_t _strip[15360]; /* [0x1206]: the rescaled birdy       */
static uint8_t _workpal[768]; /* 202F:0x4bbc                        */
/* 0x4d3c is not a buffer of its own: it is 0x4bbc + 0x180, the .PAL working
 * palette's own upper half (28d0 = 4 × 0x60 B from .PAL[0..31]). */
static uint8_t *const _pal_upper = _workpal + 0x180; /* 202F:0x4d3c       */
static uint8_t _prevpal[0x180]; /* 202F:0x1208: the party-write DAC snapshot */
static uint8_t _ramp_tbl[150]; /* CS:0x2DFE..0x2E93 (0x96 bytes)      */
static uint8_t *_bit_img; /* the loaded lbm\party1.bit          */
static uint8_t *_font_bytes; /* [0x1e1c]: the published text.spr   */

/* === the DS values the screen reads ========================================= */
static uint16_t _g_level; /* [0x1f41]                           */
static uint16_t _g_need; /* [0x17fb]                           */
static uint16_t _g_members; /* [0x11fc]                           */
static uint16_t _g_score; /* [0x592]                            */
static char _g_tribe[16]; /* 0x1189                             */

/* === the string table (verbatim templates; the digit _slots rewritten) ======= */
static char _s_all[28]; /* 0x105F: "COMING UP.... LEVEL 000"  (3 _slots)    */
static char _s_need[40]; /* 0x1077: "YOU WILL NEED 0 HUMANS TO COMPLETE"    */
static char _s_this[16]; /* 0x109A: "THIS LEVEL"                            */
static char _s_has[32]; /* 0x10A5: "YOUR TRIBE NOW HAS 00 MEMBERS!" (2)    */
static char _s_code[36]; /* 0x10C6: "THE CODE FOR THIS LEVEL IS......"      */
static char _s_score[20]; /* 0x10E0: "YOUR SCORE IS 0000"    (4 _slots)       */
static char _s_fire[32]; /* 0x1108: "PRESS FIRE TO START LEVEL...."         */

static const uint8_t _ramp10[10] = /* 2e94's ramp -> CS:[0x2DFE+i]  */
    { 0x80, 0xA0, 0xC0, 0xE0, 0xE0, 0xE0, 0xE0, 0xC0, 0xA0, 0x80 };
static struct slot _slots[8];
static struct script _scripts[SCRIPT_COUNT];

static uint8_t *_script_store; /* the seeded bodies (memcpy'd)       */

/* === the file-image script seed (2026-10-30: the tables are STATIC bytes
 * of the MZ image — the 202F:0xFA8 / 0x1028 hexdumps == the live DS
 * read-back; the sub-lists are never rewritten by traced code, so no seed
 * writer exists.  The countdowns slug the _slots' _phase: {10,8,6,4,5,0,7,9};
 * a count of 0 (slot 5) counts up to 8 on its first tick per 2909
 * DEC-then-test.) */
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
    0x01, 0x02, 0x03, 0x02, 0xFF, /* 0x1036 s1 */
    0x05, 0x06, 0x07, 0x08, 0x09, 0x06, 0x05, 0x04, 0xFF, /* 0x103b s2 */
    0x0a, 0x0b, 0xFF, /* 0x1044 s3 */
    0x0a, 0x0d, 0x0e, 0x0f, 0x0e, 0x0d, 0xFF, /* 0x1047 s4 */
    0x11, 0x12, 0x11, 0x0d, 0xFF, /* 0x104e s5 */
    0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0xFF, /* 0x1053 s6 */
    0x0a, 0x0c, 0xFF /* 0x105c s7 */
};

static uint32_t _phase = PH_FADEWAIT; /* the machine _phase                   */
static uint32_t _step; /* the in-_phase op counter             */
static uint32_t _wait_left; /* 297f's AX                           */
static int32_t _key_caught; /* the JNS's source                    */
static uint32_t _present_left; /* 835a's 0x4B cycles                  */
static uint32_t _swap_next; /* the 2bda's tail's next _phase        */
static int32_t _game_over_mode; /* level_intro_screen 2739's variant   */
static uint32_t _sw_phase = SW_FADEWAIT;

void party_intro_seed_scripts(const uint8_t *bytes, uint32_t len)
{
    /* the [live] pointer table: the DS offsets into the first body 0x1036 */
    uint32_t offs[SCRIPT_COUNT] = { 0x1036u, 0x103bu, 0x1044u, 0x1047u,
                                    0x104eu, 0x1053u, 0x105cu };
    free(_script_store);
    _script_store = (uint8_t *)malloc(len ? len : 1u);
    if (_script_store) memcpy(_script_store, bytes, len);
    for (uint32_t i = 0; i < SCRIPT_COUNT; i++) {
        uint32_t rel = (offs[i] >= 0x1036u) ? (offs[i] - 0x1036u) : 0u;
        _scripts[i].len = (rel < len) ? (len - rel) : 0u;
        _scripts[i].b = _script_store ? (_script_store + rel) : NULL;
    }
}

void party_intro_seed_slots(const uint8_t *raw) /* the 128 B at DS:0xFA8 */
{
    if (!raw) {
        memset(_slots, 0, sizeof _slots);
        return;
    }
    for (uint32_t i = 0; i < 8u; i++) {
        const uint8_t *r = raw + i * 16u;
        struct slot *s = &_slots[i];
        s->dst = rd_le_u16(r);
        s->count = r[2];
        s->script = r[3];
        s->fpos = r[4];
        s->loopc = r[5];
        memcpy(s->sub, r + 6u, 10u);
    }
}

static void _seed_slots_live(void)
{
    party_intro_seed_slots(_slots_file);
    party_intro_seed_scripts(_scripts_file, sizeof _scripts_file);
}

/* === 2b1b format_score + 2b70 ================================================ */
void party_intro_format_score(uint32_t v, char *out)
{
    static const uint32_t divs[8] = { 100000000u, 10000000u, 1000000u, 100000u,
                                      10000u,     1000u,     100u,     10u };
    uint32_t sig = 0; /* 2b70's DI           */
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t q = v / divs[i]; /* the 2b72 subcount   */
        if (q != 0) {
            *out++ = (char)('0' + q);
            sig = 0x30u;
        } else if (sig != 0) {
            *out++ = '0';
        }
        v %= divs[i];
    }
    *out++ = (char)('0' + (v % 10u)); /* 2b68                */
    *out = '\0';
}

/* the party's in-place number formatter (1000:3F1B): writes `width`
 * characters at `dst` (zero- or space-padded) and returns; the string's
 * tail is untouched, and a number wider than the field spills into it —
 * both exactly as the original does (its "HUMANS TO COMPLETE" and
 * " MEMBERS!" tails survive the patch). */
static char *_put_number(char *dst, uint32_t v)
{
    char buf[10];
    uint32_t n = 0;
    do {
        buf[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && n < 10u);
    for (uint32_t i = 0; i < n; i++) dst[i] = buf[n - 1u - i];
    return dst + n; /* the formatter's SI          */
}

/* the string appender at 0x2660: the tail replaces whatever the template
 * had after the digits (" MEMBERS!" for the tribe line) */
static char *_append_str(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = '\0';
    return dst;
}

/* === the _vram-layer helpers ================================================== */
static void _clear_canvas(void) /* 2af5: rows 20..87 = 0x640..0x1B80 */
{ memset(_canvas + 0x640u, 0, 68u * 80u); }

static void _flip_page(void) /* 87f8                                */
{
    uint16_t t = _page_draw;
    _page_draw = _page_disp;
    _page_disp = t;
}

/* 2ed7 deplanarise_intro_page: the .BIT rows plane-baked at pitch 88 +8 */
static void _bake_page(const uint8_t *bit, uint32_t pagebase)
{
    for (uint32_t r = 0; r < 200u; r++) {
        uint32_t a = pagebase + 8u + r * 88u;
        const uint8_t *s = bit + r * 320u;
        for (uint32_t c = 0; c < 80u; c++) {
            _vram[a + c][0] = s[c * 4u + 0u];
            _vram[a + c][1] = s[c * 4u + 1u];
            _vram[a + c][2] = s[c * 4u + 2u];
            _vram[a + c][3] = s[c * 4u + 3u];
        }
    }
}

/* 2c1d draw_intro_frame — the closed funnel */
static void _band_stamp(uint32_t pagebase)
{
    for (uint32_t row = 0; row < 68u; row++) {
        uint32_t m = _ramp_tbl[row + 16u]; /* 2c1d's OUT 3CF = CS:0x2E0E+row */
        uint32_t a = pagebase + 0x6E8u + row * 88u;
        const uint8_t *src = _canvas + 0x640u + row * 80u;
        for (uint32_t k = 0; k < 80u; k++) {
            uint8_t c = src[k]; /* LODSB [BX+0x640+80+k]      */
            for (uint32_t p = 0; p < 4u; p++) {
                uint32_t spread = (((c >> p) & 1u) != 0u) ? m : 0u;
                uint8_t pre = _vram[a + k][p];
                _vram[a + k][p] =
                    (uint8_t)(((pre & (uint32_t)(~m & 0xFFu)) | spread) &
                              0xFFu);
            }
        }
    }
}

/* 29d9 blit_slot_colour_block: 24 rows × 4 bytes; frame clamps 0x1C */
static void _blit_slot(uint32_t frame)
{
    if (frame > 0x1Cu) frame = 0x1Cu;
    const uint8_t *s = _sheet + frame * 96u;
    uint32_t di = (uint32_t)_stamp_dest + (uint32_t)_page_draw + 8u;
    enum {
        MASK = 0x40u
    }; /* 28e9's 299b(AH=0x40) live pin */
    for (uint32_t r = 0; r < 24u; r++) {
        for (uint32_t w = 0; w < 4u; w++) {
            uint8_t c = s[r * 4u + w];
            /* write mode 2 reads AL's bits 0..3 only — the _sheet's nibble
             * is the plane value itself; no palette bias applies to the
             * byte (the +0xC0 story lives in the palette table, not here). */
            for (uint32_t p = 0; p < 4u; p++) {
                uint32_t spread = (((c >> p) & 1u) != 0u) ? MASK : 0u;
                uint8_t pre = _vram[di + w][p];
                _vram[di + w][p] =
                    (uint8_t)(((pre & (uint32_t)(~MASK & 0xFFu)) | spread) &
                              0xFFu);
            }
        }
        di += 88u; /* 4 written + ADD DI,0x54 = 88      */
    }
}

/* 2a3a draw_scrolling_marquee */
static void _marquee(void)
{
    _marq_phase++;
    if (_marq_phase == 0x160u) _marq_phase = 0u; /* INC; CMP 0x160          */
    _marq_hold--; /* 2a4c: DEC first, then test */
    if (_marq_hold == 0u) {
        _marq_hold = 5u;
        _marq_cell = (_marq_cell + 1u) % 0xCu;
    }

    uint32_t cell = _marq_cell, ph = _marq_phase;
    uint32_t v = cell * 4u + (ph & 3u); /* 2a76..2a83 */
    uint32_t src = v * 320u; /* 2a85..2a8d: v*256 + v*64   */

    uint32_t dhi = 0x2260u + ph / 4u; /* 2a8f..2a95 + the page add  */

    enum {
        MASK = 0x40u
    }; /* 28e9's 299b(AH=0x40) covers 2a3a  */
    for (uint32_t r = 0; r < 32u; r++) {
        uint32_t di = (uint32_t)_page_draw + dhi + r * 88u;
        for (uint32_t k = 0; k < 10u; k++) {
            uint8_t c = _strip[src + r * 10u + k]; /* 5 LODSW = 10 B/row    */
            for (uint32_t p = 0; p < 4u; p++) {
                uint32_t spread = (((c >> p) & 1u) != 0u) ? MASK : 0u;
                uint8_t pre = _vram[di + k][p];
                _vram[di + k][p] =
                    (uint8_t)(((pre & (uint32_t)(~MASK & 0xFFu)) | spread) &
                              0xFFu);
            }
        }
    }
}

/* 28e9 animate_entities — verbatim: every tick, per slot */
static void _anim_entities(void)
{
    for (uint32_t i = 0; i < 8u; i++) {
        struct slot *s = &_slots[i];
        uint8_t dl = 0;
        s->count--;
        if (s->count == 0u) {
            s->count = 8u;
            dl = 1u;
        } /* 2909..2912 */
        for (uint32_t pass = 0; pass < 40u; pass++) { /* the 2914 walk */
            if (s->script == 0u || s->script > SCRIPT_COUNT) break;
            const struct script *sc = &_scripts[s->script - 1u];
            if (sc->b == NULL || sc->len == 0u) break;
            uint8_t bx = s->fpos; /* 2920: BX = [SI+4] */
            s->fpos = (uint8_t)(bx + dl); /* 2925: ADD before the read */
            uint8_t dh = sc->b[bx]; /* 2928 */
            if (dh != 0xFFu) { /* 292a: the stamp */
                _stamp_dest = s->dst;
                _blit_slot(dh);
                break; /* 295f: next slot */
            }
            uint32_t cl = ((uint32_t)s->loopc + 1u) % 10u; /* 292f..293b */
            s->loopc = (uint8_t)cl;
            s->script = s->sub[cl]; /* the sub-list ids */
            s->fpos = 0u; /* 294a, redo 2914 */
            if (s->script == 0u || s->script > SCRIPT_COUNT) break;
        }
    }
    _marquee();
    _flip_page();
}

/* 2e94 _draw_text_ramped — _canvas-resident per the original: the glyph
 * stroke lands in the CANVAS only (the = "2c1d never runs here"); the
 * per-call 28e9 animates the _slots/_marquee and flips, but the TEXT does
 * not reach the display until the next 2C1D (inside 2b9c/2bda) — the
 * original's "all text fades in together at the first band redraw". */
static void _draw_text_ramped(const char *s, uint32_t centreX, uint32_t row)
{
    memcpy(_ramp_tbl + row, _ramp10, 10u); /* 2e94: the ramp slides with the
                                            * row param — CS:0x2DFE + row */
    if (_font_bytes) font_draw_text(_canvas, s, centreX, row);
    _anim_entities(); /* 28e9: _slots + _marquee + flip —
                                            * the _canvas glyph is NOT rendered */
}

/* 28d0 palette_copy_low_to_upper: 4 × 0x60 bytes .PAL[0..31] ->
 * .PAL[128..255] (so the window fade targets the blues, not the file's grey
 * upper half). */
static void _copy_palette(void)
{
    for (uint32_t p = 0; p < 4u; p++)
        memcpy(_pal_upper + p * 0x60u, _workpal, 0x60u);
}

/* 2b9c _intro_anim: the backdrop band twice, then the snapshot restore and
 * the 0x80..0xFF window fade toward it — 0x1208 (25bc) is the .PAL's own
 * upper half, so the text window dissolves from the 28d0 blues into the
 * file's grey ramp (the fade-in). */
static void _intro_anim(void)
{
    _band_stamp(_page_draw);
    _anim_entities();
    _band_stamp(_page_draw);
    _anim_entities();
    memcpy(_pal_upper, _prevpal, 0x180u); /* 2bbf..2bc8: 0x1208 -> 0x4d3c */
    palette_build_fade(0x80u, 0x80u, 0x28u, _pal_upper); /* 2bca: BX=0x80 */
}

/* 2bda fade_and_animate — verbatim _phase-split: 2bda runs [28d0]; builds
 * the window fade; THEN BLOCKS ON ITS OWN FADE ({28e9; 8b9a; JC} at
 * 2bec..2bf2) before the 0xE0 mask reset (2bf4), the 2af5 clear (2c01)
 * and the two 2c1d band redraws (2c04..2c1c).  The clear/redraw tail is
 * therefore separated from the next set's stamps by the fade's own
 * completion — the original's "text fades in at the next set" rhythm:
 * every 2c1d renders the _canvas under the fade state CURRENT AT THAT
 * MOMENT, and the holds are the built fades' converged end-states. */
static void _swap_begin(uint32_t next)
{
    _copy_palette(); /* 2bda (28d0)          */
    palette_build_fade(0x80u, 0x80u, 0x28u, _pal_upper);
    _swap_next = next;
    _sw_phase = SW_FADEWAIT; /* 2bec: the fade runs  */
    _phase = PH_SWAPWAIT;
}

/* === the asset prep ========================================================== */

/* 27eb expand_nibbles_to_bytes */
static int32_t _expand_nibbles(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    if (n < 0x570u) return 0;
    uint32_t di = 0;
    for (uint32_t i = 0; i < 0x570u; i++) {
        uint8_t al = src[i], ah = al;
        for (uint32_t t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        dst[di + 1u] = al & 0xFu;
        for (uint32_t t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        dst[di] = al & 0xFu;
        di += 2u;
    }
    return 1;
}

/* 28c2 shift_right_1 */
static void _shift_right1(uint8_t *d)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < 0xA0u; i++) {
        uint32_t c2 = d[i] & 1u;
        d[i] = (uint8_t)(((d[i] >> 1) | (c << 7)) & 0xFFu);
        c = c2;
    }
}

/* 288d _expand_pixels into ES:DI */
static void _expand_pixels(const uint8_t *src, uint32_t *dip)
{
    uint32_t di = *dip;
    for (uint32_t i = 0; i < 0xA0u; i++) {
        uint8_t al = src[i], ah = al;
        for (uint32_t t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        _strip[di + 1u] = al & 0xFu;
        for (uint32_t t = 0; t < 4u; t++) {
            uint8_t cf = ah & 1u;
            ah >>= 1;
            al = (uint8_t)(((al << 1) | cf) & 0xFFu);
        }
        _strip[di] = al & 0xFu;
        di += 2u;
    }
    *dip = di;
}

/* 283e rescale_bitmap_rows */
static void _rescale_rows(const uint8_t *src)
{
    uint32_t di = 0;
    for (uint32_t g = 0; g < 12u; g++) {
        uint8_t work[0xA0u];
        memcpy(work, src + g * 0xA0u, 0xA0u);
        for (uint32_t t = 0; t < 4u; t++) _shift_right1(work);
        _expand_pixels(work, &di);
        for (uint32_t k = 0; k < 4u; k++) {
            _shift_right1(work);
            if (k < 3u) _expand_pixels(work, &di);
        }
    }
}

/* === the API ================================================================= */
void party_intro_set_values(uint32_t level, uint32_t need, uint32_t members,
                            uint32_t score, const char *tribe)
{
    _g_level = (uint16_t)level;
    _g_need = (uint16_t)need;
    _g_members = (uint16_t)members;
    _g_score = (uint16_t)score;
    memset(_g_tribe, 0, sizeof _g_tribe);
    if (tribe) memcpy(_g_tribe, tribe, sizeof _g_tribe - 1u);
    /* the image's verbatim strings (202F:0x105F..; the digit fields are
     * fixed-width and the formatter rewrites them in place, 0x3F1B) */
    memcpy(_s_all, "COMING UP.... LEVEL 000", 23);
    memcpy(_s_need, "YOU WILL NEED 0 HUMANS TO COMPLETE", 35);
    memcpy(_s_this, "THIS LEVEL", 11);
    memcpy(_s_has, "YOUR TRIBE NOW HAS 00 MEMBERS!", 30);
    memcpy(_s_code, "THE CODE FOR THIS LEVEL IS......", 32);
    memcpy(_s_score, "YOUR SCORE IS 0000", 18);
    memcpy(_s_fire, "PRESS FIRE TO START LEVEL....", 30);
}

void party_intro_set_game_over(int32_t on) { _game_over_mode = on ? 1 : 0; }
uint32_t party_intro_score(void) { return _g_score; }

static uint8_t *_load(const char *name)
{
    uint32_t sz = 0;
    uint16_t ck = 0;
    return res_load_chunk(name, &sz, &ck);
}

/* the palette buffers, exposed for the fade-ladder harness: the port's
 * 0x4bbc/0x4d3c(+0x180)/0x1208 stand-ins (read-only diagnostics, see
 * test_fadeladder) */
const uint8_t *party_intro_workpal(void) { return _workpal; }
const uint8_t *party_intro_pal_upper(void) { return _pal_upper; }

int32_t party_intro_start(void)
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
    _page_disp = 0x8100u; /* 2519/2523               */
    /* 2a3a's counters are static DS data: [0xf9c]=0, [0xf9e]=0, [0xfa6]=1
     * (the MZ image; a 0 hold DEC-wraps and freezes the flap on run 1) */
    _marq_phase = 0u;
    _marq_cell = 0u;
    _marq_hold = 1u;
    memset(_vram, 0, sizeof _vram);
    memset(_ramp_tbl, 0xE0u, sizeof _ramp_tbl); /* the table's clean state */
    _clear_canvas();
    _seed_slots_live(); /* the DS:0xFA8's [live]   */

    b = _load("APL\\PARTY.SPR"); /* 2570                    */
    if (!b) return 0;
    {
        int32_t ok = _expand_nibbles(b, 0x570u, _sheet);
        free(b);
        if (!ok) return 0;
    }

    b = _load("APL\\BIRDY.SPR"); /* 257c                    */
    if (!b) return 0;
    _rescale_rows(b);
    free(b);

    {
        uint32_t d = (uint32_t)(_g_level & 7u) + 1u; /* 2519: '1'+(lvl&7) */
        sprintf(name, "LBM\\PARTY%u.PAL", d);
    }
    b = _load(name); /* 2588                    */
    if (!b) return 0;
    palette_load_working(b); /* 2591 (8d24): -> 0x4bbc  */
    memcpy(_workpal, palette_working(), 768u);
    free(b);

    b = _load("LBM\\PARTY1.BIT"); /* 259a                    */
    if (!b) return 0;
    _bit_img = b;

    b = _load("APL\\TEXT.SPR"); /* 25af                    */
    if (b) {
        _font_bytes = b;
        font_publish(b);
    } else {
        _font_bytes = NULL;
        font_publish(NULL);
    }

    /* 25bc: 0x1208 <- DS:0x4d3c (the .PAL's upper half, verbatim) */
    memcpy(_prevpal, _pal_upper, 0x180u);

    _bake_page(_bit_img, _page_draw); /* 25ca..25d8: the bake x2 */
    _anim_entities();
    _bake_page(_bit_img, _page_draw);

    _copy_palette(); /* 25da (28d0)             */
    palette_build_fade(0u, 0x100u, 0x23u, _workpal); /* 25dd              */
    return 1;
}

/* === the ticker =============================================================== */

/* the game_main hook's per-tick compose into the flat page (the attract's
 * contract: the tick draws, videoPresent(0) shows) */
int32_t party_intro_tick(const InputState *in)
{
    int32_t fire = (int32_t)(in->joyBits & 0x80u);
    int32_t anyk = (int32_t)(in->lastKey != 0u);

    switch (_phase) {
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
        _anim_entities(); /* 2bec's 28e9                */
        if (_sw_phase == SW_FADEWAIT) {
            if (!palette_fade_active())
                _sw_phase = SW_TAIL; /* 2bf2's JC out  */
            break; /* no tail before the fade ends */
        }
        /* the 2bf4 tail: the mask reset + clear + band redraws, once */
        memset(_ramp_tbl, 0xE0u,
               sizeof _ramp_tbl); /* 2bf4: the 0x96 fill     */
        _clear_canvas(); /* 2c01 (2af5)         */
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
        } else { /* the exit: 270e's tail   */
            _clear_canvas(); /* 270e (2af5)            */
            {
                static uint8_t black[768];
                palette_build_fade(0u, 0x100u, 0x23u, black); /* 2711 */
            }
            _phase = PH_EXITWAIT;
        }
        _sw_phase = SW_FADEWAIT; /* the tail ran once          */
        break;

    case PH_SET1: /* 2600..268d             */
        switch (_step) {
        case 0u: /* 2600..2611              */
            _clear_canvas();
            *_put_number(_s_all + 0x14u, _g_level) = '\0'; /* 260e    */
            _draw_text_ramped(_s_all, 0xA0u, 0x14u);
            break;
        case 1u: /* 2623..263a: no NUL — the
                                                * " HUMANS TO COMPLETE" tail
                                                * survives the patch        */
            (void)_put_number(_s_need + 0x0Eu, _g_need);
            _draw_text_ramped(_s_need, 0xA0u, 0x28u);
            break;
        case 2u:
            _draw_text_ramped(_s_this, 0xA0u, 0x34u); /* 2640    */
            break;
        case 3u: /* 2652..268a              */
        {
            char *q = _append_str(_put_number(_s_has + 0x13u, _g_members),
                                  " MEMBERS!"); /* 2660    */
            if (_g_members == 1u) { /* 266d..267a              */
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

    case PH_SET1_WAIT: /* 2693..2699              */
        _anim_entities();
        if (fire || anyk) {
            _key_caught = 1;
            _wait_left = 0u;
        } else if (_wait_left) _wait_left--;
        if (!_wait_left) {
            if (_key_caught) _swap_begin((uint32_t)-1); /* 270b          */
            else _swap_begin(PH_SET2);
        }
        break;

    case PH_SET2: /* 269e..26f7              */
        switch (_step) {
        case 0u:
            _clear_canvas(); /* 269e (2af5)             */
            _draw_text_ramped(_s_code, 0xA0u, 0x14u); /* 26a1    */
            break;
        case 1u:
            _draw_text_ramped(_g_tribe, 0xA0u, 0x20u); /* 26b3    */
            break;
        case 2u:
            *_put_number(_s_score + 0x0Eu, _g_score) = '\0'; /* 26d3    */
            _draw_text_ramped(_s_score, 0xA0u, 0x34u);
            break;
        case 3u:
            _draw_text_ramped(_s_fire, 0xA0u, 0x48u); /* 26e8    */
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
            if (_key_caught) _swap_begin((uint32_t)-1); /* 270b          */
            else _swap_begin(PH_SET1);
        }
        break;

    case PH_GAMEOVER: /* 2739..27e9: the GAME OVER
                                                * variant of the briefing  */
        if (_step == 0u) {
            _clear_canvas();
            _draw_text_ramped("GAME OVER!", 0xA0u, 0x14u);
            _draw_text_ramped("YOUR TRIBE IS TOO SMALL TOO CONTINUE", 0xA0u,
                              0x28u);
            _draw_text_ramped("IN THE RACE FOR EVOLUTION!", 0xA0u, 0x34u);
            _draw_text_ramped("PRESS FIRE TO TRY AGAIN!", 0xA0u, 0x48u);
            _intro_anim(); /* 2b9c: the band redraws carry
                                            * the _canvas text to the display */
            _step = 1u;
        }
        _anim_entities();
        if (fire || anyk) _swap_begin((uint32_t)-1);
        break;

    case PH_EXITWAIT: /* 2720: {28e9; 8b9a; JC}  */
        _anim_entities();
        if (!palette_fade_active()) {
            _phase = PH_PRESENT;
            _present_left = 0x4Bu;
        }
        break;

    case PH_PRESENT: /* 2728 (835a), 272b       */
        if (_present_left) _present_left--;
        else _phase = PH_DONE; /* 8389 music_stop        */
        break;

    case PH_DONE:
    default: break;
    }

    /* the compose (the attract's contract): the display page into the
     * flat 320x200 page 0 for videoPresent */
    {
        uint8_t *pg = video_page(0);
        if (pg) {
            uint32_t base = _page_disp;
            for (uint32_t y = 0; y < 200u; y++) {
                uint32_t row = ((base + 8u + y * 88u) & 0xFFFFu);
                uint8_t *d = pg + y * 320u;
                for (uint32_t x = 0; x < 320u; x++)
                    d[x] = _vram[(row + (x >> 2)) & 0xFFFFu][x & 3u];
            }
        }
    }

    if (_phase == PH_DONE) return 0; /* the screen exited       */
    return 1;
}

/* the closed layout law, exposed for the byte-exact fixtures: the page's
 * pixel (x,y) = _vram[pagebase + 8 + y*88 + (x>>2)][x&3] of the DISPLAY
 * page (_page_disp — the just-flipped bake target). */
void party_intro_compose(uint8_t *pg)
{
    uint32_t base = (uint32_t)_page_disp + 8u;
    for (uint32_t y = 0; y < 200u; y++) {
        uint32_t row = (base + y * 88u) & 0xFFFFu;
        uint8_t *d = pg + y * 320u;
        for (uint32_t x = 0; x < 320u; x++)
            d[x] = _vram[(row + (x >> 2)) & 0xFFFFu][x & 3u];
    }
}
