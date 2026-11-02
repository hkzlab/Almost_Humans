/* statusbar.c — the in-level status bar (game_structure.md §1 subsystem 13,
 * re_map.md → "The in-level status bar, decoded end to end",
 * re_formats.md §12.7/§12.8).
 *
 * The original's mechanism, translated to the flat 320x200x8 page:
 *
 * - The bar = the bottom 40 screen rows (y 152..191): panel4.bit's top 40
 *   rows, one art byte per pixel, +0xC0-tagged.  The original draws it once
 *   (blit_area_if_dirty's first-frame pass, 1000:9A18, after shift_screen_up
 *   1000:99D8 ADDs the 0xC0C0 tag and copies the rows to +0x3200) into the
 *   dedicated low-VRAM band at A000:0x0000..0xDBF, shown at the screen
 *   bottom through the CRTC line compare ([0x5B0A] = 152).  The port's page
 *   is recomposed every tick, so the band is redrawn every tick instead.
 *
 * - The +0xC0 tag selects the DAC's 0xC0 bank: level_video_init
 *   (1000:32CD) copies panel4's own palette's first 0xC0 bytes to 202F:0x1713
 *   = the live 256-entry palette's entries 0xC0..0xFF (0x14D3 + 0xC0*3).
 *   statusbar_palette() exposes that source; level.c installs it per tick.
 *
 * - Five digit marks (y 172..179), the bar's whole content: the countdown's
 *   minutes, seconds-tens, seconds-ones and the tribe counter's tens and
 *   ones.  A mark shows an 8x8 glyph cut from the PANELN1+PANELN2 strip:
 *   the phase words are (digit+1)*16 glyph slots (build_statusbar_layout
 *   1000:961B) and mark row r is the strip's 8 consecutive bytes at
 *   4*(phase + 2*r) — the seconds-tens mark reads at +0xC0 (PANELN2 = the
 *   strip's second half).  The stamp (1000:9846) runs every frame.
 *
 * - statusbar_tick ports the INT 8 hook at 1000:96AD (armed by level_fade_in
 *   patching CS:[0x8B1C] := 0x339D): the tribe odometer
 *   (tick_state_machine_5b22 1000:9767), the 70-frame second ([0x5B14]),
 *   the packed-decimal countdown with the odometer slide animations
 *   ([0x5B10] = 2: 2 phase-bytes per 2 frames), and the time-up
 *   ([0x5B12] := 0, [0x5E44] := 0xFF).
 *
 * The elapsed clock [0x5B0C..0E] is ported: one step per second in the same
 * gate (1000:96C6: ones 0..9, tens 0..5, minutes), reset by level_start
 * (1000:95B5), exposed as statusbar_elapsedWord() in the packing
 * check_highscore uses for the per-level best-time compare (1000:42CE).
 *
 * The line-compare reveal ([0x5B0A] 0xC9→0x98, one per tick) is ported:
 * `barSplit` offsets the band, marks and action row; the time-up dialog
 * ([0x5E44] → the input_dispatch_loop pause branch) is menu_openTimeUp /
 * menu_tick.  draw_level_text (0x342B) parks [0x5B0A] at 0xC9 for the level
 * end, which the port models as `statusbar_hide` (no composition at all,
 * since the bar is a display overlay there and the page shows the world).
 */
#include "game.h"
#include <stdlib.h>
#include <string.h>

#define STRIP_LEN 1280u      /* PANELN1 (768) + PANELN2 (512), §12.7a    */
#define BAR_FIRST_ROW 152u   /* the line-compare split, [0x5B0A] = 0x98  */
#define BAR_ROWS 40u        /* panel4's top 40 rows                      */
#define MARK_Y 172u         /* bar row 20 (the destinations 0x6E2..)     */

/* [0x5B0A]: the bar's first screen row.  The original's line-compare reveal
 * starts at 201 (hidden) and the INT 8 hook 1000:331E decrements it to 0x98
 * (152) over 48 ticks; crtc_set_scroll sets the line compare to 2*row-1. */
static unsigned barSplit = 152u;

static const OSystem *sb_osys;
static uint8_t *strip;        /* the four-plane panel strip                */
static uint8_t *panel4;       /* LBM\PANEL4.BIT (the art + the palette)   */
static uint32_t p4Size;       /* its length: the charge bar reads 0x3EE0+  */
static uint8_t *icons3;       /* APL\ICONS3.SPR: 12 x 16x24 (0x180 each)  */

static int active;            /* [0x5B12]: the bar's ticking gate         */
/* The bar is a display overlay in the original (CRTC line compare); the
 * port composes it into the page, so [0x5B0A] beyond the screen has to mean
 * "do not compose".  draw_level_text (0x342B) parks it at 0xC9 for the level
 * end, and blit_bitmap_planes re-renders the world over those rows. */
static int barHidden;
static int timeUp;            /* [0x5E44]: the countdown reached zero     */

/* the countdown: values [0x5B19..1B], phases [0x5B1C/1E/20], slides
 * [0x5B16..18] — indices 0 = minutes, 1 = seconds-tens, 2 = seconds-ones */
static uint8_t cd[3];
static uint16_t cdPhase[3];
static uint8_t cdSlide[3];
static uint8_t secDiv;        /* [0x5B14], seeded 0x46 = 70 frames       */
static uint8_t slideDiv;      /* [0x5B10], seeded 2                      */

/* the elapsed clock [0x5B0C..0E] = seconds ones (0..9), tens (0..5), minutes
 * (1000:96C6, one step per second; reset by level_start at 1000:95B5).  The
 * level-end score packs it as minutes<<8 | tens<<4 | ones for the per-level
 * best-time comparison (check_highscore 1000:42CE). */
static uint8_t elapsed[3];

/* the tribe odometer: values [0x5B25/26], phases [0x5B27/29], state
 * [0x5B22], slides [0x5B23/24] — indices 0 = tens, 1 = ones */
static uint8_t td[2];
static uint16_t tdPhase[2];
static uint8_t odoState;
static uint8_t odoSlide[2];
/* 1000:F5E3: the "anything collected" test ([0x5B25] | [0x5B26] != 0). */
int statusbar_tribeActive(void) { return (td[0] | td[1]) != 0; }
/* The two digits as the number the check_highscore arithmetic reads (1000:41B8:
 * `[0x5B25]*10 + [0x5B26]`). */
unsigned statusbar_tribeValue(void) { return (unsigned)td[0] * 10u + td[1]; }
unsigned statusbar_tribeDigit(unsigned i) { return td[i & 1u]; }
/* The odometer's digits are [0x5B25]/[0x5B26]; the game only writes them through
 * statusbar_start (members - required) and the roll animation.  This setter is
 * a test seam for the object machine's level-end probe, whose two branches key
 * on that word. */
void statusbar_setTribeDigits(unsigned tens, unsigned ones)
{
    td[0] = (uint8_t)(tens % 10u);
    td[1] = (uint8_t)(ones % 10u);
}

/* 0x5B22..0x5B29, the save's +0x17F0 block: state, the two slide
 * countdowns, the two digits, the two phase bytes and the 0x5B28 gap. */
void statusbar_saveParams(uint8_t *out)
{
    if (!out) return;
    out[0] = odoState;
    out[1] = odoSlide[0];
    out[2] = odoSlide[1];
    out[3] = td[0];
    out[4] = td[1];
    out[5] = (uint8_t)tdPhase[0];
    out[6] = 0u;
    out[7] = (uint8_t)tdPhase[1];
}

void statusbar_loadParams(const uint8_t *in)
{
    if (!in) return;
    odoState = in[0];
    odoSlide[0] = in[1];
    odoSlide[1] = in[2];
    td[0] = (uint8_t)(in[3] % 10u);
    td[1] = (uint8_t)(in[4] % 10u);
    tdPhase[0] = in[5];
    tdPhase[1] = in[7];
}

int statusbar_init(const OSystem *osys) { sb_osys = osys; return 0; }

/* The mark x positions: the five destinations 0x6E2/0x6E6/0x6E8/0x728/0x72A
 * are bar row 20's bytes 2/6/8/72/74 at the 88-byte pitch; the flat page's
 * x = 4 * byte.  The seconds-tens mark (index 1) reads its glyphs at
 * +0xC0 — PANELN2, the strip's cells 12..19. */
static const uint16_t markX[5] = { 8, 24, 32, 288, 296 };
static const uint16_t markBias[5] = { 0, 0xC0, 0, 0, 0 };

/* stamp_statusbar_plane_bits (1000:9846) + copy_8rows_2bytes_to_vga
 * (1000:98D1): one mark = 16 consecutive strip bytes laid out 8 px per
 * row, 8 rows.  The strip values are the low nibbles {03,07,0E} = the
 * paper/antialias/ink greys; the +0xC0 tag carries them into the bank
 * level_video_init loaded. */
static void stamp_marks(uint8_t *page)
{
    const uint16_t ph[5] = {
        cdPhase[0], cdPhase[1], cdPhase[2], tdPhase[0], tdPhase[1]
    };
    for (unsigned m = 0; m < 5u; m++) {
        unsigned base = 4u * (unsigned)(ph[m] + markBias[m]);
        for (unsigned r = 0; r < 8u; r++) {
            unsigned at = barSplit + 20u + r;
            if (at >= 200u) continue;
            for (unsigned j = 0; j < 8u; j++)
                page[at * 320u + markX[m] + j] =
                    (uint8_t)(0xC0u | strip[base + 8u * r + j]);
        }
    }
}

/* The bar's background: panel4's top 40 rows, +0xC0-tagged (shift_screen_up
 * 1000:99D8 + blit_area_if_dirty 1000:9A18).  The page's rows 192..199 stay
 * the world's — the original's visible mode is 192 rows and its page holds
 * unpainted memory below the band; the flat page has no such band, so the
 * present's crop decides (the 200-row page also serves the boot intro's
 * rows 182..197). */
static void draw_band(uint8_t *page)
{
    for (unsigned y = 0; y < BAR_ROWS; y++) {
        unsigned at = barSplit + y;
        uint8_t *dst;
        const uint8_t *src = panel4 + y * 320u;

        if (at >= 200u) continue;
        dst = page + at * 320u;
        for (unsigned x = 0; x < 320u; x++)
            dst[x] = (uint8_t)(0xC0u | src[x]);
    }
}

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
 * keeps one copy of those bytes (`object_programHeader`), so the highlighted
 * cell and the executed op cannot drift apart. */
static const uint8_t actionGold[40] = {   /* 202F:0x5B57 */
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,
    0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0xE0,0xE1,0xE2,0xE3,
    0xE3,0xE2,0xE1,0xE0,0xDF,0xDE,0xDD,0xDC,0xDB,0xDA,
    0xD9,0xD8,0xD7,0xD6,0xD5,0xD4,0xD3,0xD2,0xD1,0xD0,
};
/* 1000:3747: the jump/throw charge bar, drawn into the bar band from the
 * acting record's `+0xE`.  Two halves, differing in both art and position:
 *
 *  - `+0xE` < 0x100 (0x374B): a generated 2-px-wide, 10-row stroke at band row
 *    15, byte column `(0x60 + (+0xE >> 1)) >> 2`, plane `(0x60 + v>>1) & 3`,
 *    colours `DF E0 E1 E2 E3 E3 E2 E1 E0 DF`; the plane bit walks one further
 *    (wrapping into the next byte column) for the second of the two passes
 *    (`0x376D: CX = 2`).
 *  - `+0xE` >= 0x100 (0x37C3): the folded length `len = (0x1FF - v) >> 1`
 *    selects a 2-px-wide, 15-row slice of **panel4's own art** at
 *    `0x3EE0 + len` (source rows 0x140 apart, the two pixels 4 apart), placed
 *    at band row 10, column `24 + (len >> 2)`, plane `len & 3`.
 *
 * The port is per-pixel, so a planar byte becomes the pixel `4*col + plane`. */
void statusbar_chargeBar(unsigned v)
{
    static const uint8_t shade[10] = {
        0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE3, 0xE2, 0xE1, 0xE0, 0xDF
    };
    uint8_t *page = sb_osys ? sb_osys->videoPage(0) : NULL;
    unsigned i, r;

    if (!page || !panel4) return;              /* no bar art: nothing to draw */
    if (v < 0x100u) {
        unsigned ax = 0x60u + (v >> 1);        /* 0x372E's `SHR AX,1`        */
        unsigned col = ax >> 2, p = ax & 3u;

        for (i = 0; i < 2u; i++) {             /* 0x376D: CX = 2 passes      */
            unsigned x = 4u * col + p;

            for (r = 0; r < 10u; r++) {
                unsigned y = barSplit + 15u + r;

                if (x < 320u && y < 200u) page[y * 320u + x] = shade[r];
            }
            if (++p == 4u) { p = 0; col++; }
        }
        return;
    }
    {
        unsigned len = (0x1FFu - v) >> 1;      /* 0x3726-0x372E              */
        unsigned col = 24u + (len >> 2), p = len & 3u;
        unsigned src = 0x3EE0u + len;

        if (src + 0x140u * 15u + 4u > p4Size) return;   /* past the art     */
        for (i = 0; i < 2u; i++) {
            unsigned x = 4u * col + p;

            for (r = 0; r < 15u; r++) {
                unsigned y = barSplit + 10u + r;
                uint8_t a = panel4[src + r * 0x140u + i];
                uint8_t b = panel4[src + r * 0x140u + i + 4u];

                if (x < 320u && y < 200u) page[y * 320u + x] = a;
                if (x + 4u < 320u && y < 200u) page[y * 320u + x + 4u] = b;
            }
            if (++p == 4u) { p = 0; col++; }
        }
    }
}

/* 1000:3933's tail: step the selected cell - the program's cursor byte - with
 * the wrap to 1 past the count.  The bar is redrawn every tick, so the store is
 * the whole effect. */
void statusbar_cycleAction(void)
{
    object_stepActionCell();               /* 1000:3941 */
}

/* [0x5B56] itself, for the tests: the group the bar's next draw uses. */
unsigned statusbar_actionGroup(void) { return object_actionProgram(); }

/* the group's icon count (its header's first byte), for the tests. */
unsigned statusbar_actionCount(unsigned group)
{
    const uint8_t *h = object_programHeader(group);

    return h ? (unsigned)h[0] : 0u;
}

/* the group's selected-cell byte, for the tests (0 past the table). */
unsigned statusbar_actionCell(unsigned group)
{
    const uint8_t *h = object_programHeader(group);

    return h ? (unsigned)h[1] : 0u;
}

/* 0xAFFE: one frame pixel — the mask byte when it is non-zero, else the
 * destination shaded by `d` and clamped to the bar bank. */
static void frame_px(uint8_t *page, unsigned x, unsigned y, int d,
                     uint8_t mask)
{
    unsigned at;
    int v;
    if (x >= 320u || y >= 200u) return;
    at = y * 320u + x;
    if (mask) { page[at] = mask; return; }
    v = (int)page[at] + d;
    if (v >= 0xD0) v = 0xCF;
    if (v < 0xC0) v = 0xC0;
    page[at] = (uint8_t)v;
}

/* 0xAF7D / 0xAF6E: the four edges in the original's order (top -4, left +4,
 * bottom +4, right -4).  Only the vertical runs advance the mask (0xAFF6's
 * INC SI per row); a horizontal run re-uses one byte for the whole edge
 * (0xAFF5 loops without touching SI), and the bottom edge reads the byte at
 * base+h because BX was stepped by the height first (0xAFAE). */
static void frame_box(uint8_t *page, unsigned x, unsigned y, unsigned w,
                      unsigned h, const uint8_t *mask, unsigned base)
{
    unsigned i;
    uint8_t top = mask ? mask[base] : 0u;
    uint8_t bottom = mask ? mask[base + h] : 0u;

    for (i = 0; i < w; i++) frame_px(page, x + i, y, -4, top);
    for (i = 0; i < h; i++)
        frame_px(page, x, y + i, 4, mask ? mask[base + i] : 0u);
    for (i = 0; i < w; i++) frame_px(page, x + i, y + h, 4, bottom);
    for (i = 0; i < h; i++)
        frame_px(page, x + w, y + i, -4, mask ? mask[base + i] : 0u);
}

static void draw_actionIcon(uint8_t *page, unsigned x, unsigned y,
                            unsigned cell)
{
    const uint8_t *src;
    if (!icons3 || cell >= 12u) return;
    src = icons3 + cell * 0x180u;
    for (unsigned r = 0; r < 24u; r++)
        for (unsigned c = 0; c < 16u; c++) {
            uint8_t v = src[r * 16u + c];
            if (!v) continue;
            if (x + c < 320u && y + r < 200u)
                page[(y + r) * 320u + x + c] = (uint8_t)(v + 0xC0u);
        }
}

static void draw_actions(uint8_t *page)
{
    const uint8_t *g = object_actionHeader();
    unsigned count;

    if (!g) return;                        /* [0x5B56] is 1..6, see 1000:32DC */
    count = g[0];
    unsigned span = count * 23u + (count - 1u) * 12u;
    unsigned x = (0x140u - span) / 2u;
    unsigned y = barSplit + 5u;

    for (unsigned i = 0; i + 1u < count + 1u && i < 4u; i++) {
        frame_box(page, x, y, 23u, 31u, NULL, 0u);
        /* 1000:990A: the gold 21x29 inner frame sits on the cell the header's
         * second byte counts down to - the cell `g[1] - 1`, not always the
         * first (SPACE steps that byte, 1000:3933). */
        if (g[1] != 0u && i == (unsigned)(g[1] - 1u))
            frame_box(page, x + 1u, y + 1u, 21u, 29u, actionGold, 6u);
        draw_actionIcon(page, x + 4u, y + 4u, (unsigned)(g[i + 2u] - 1u));
        x += 4u + 0x1Fu;      /* the icon's +4 is never undone (0xAD6A/7C) */
    }
}

/* tick_state_machine_5b22 (1000:9767): the tribe odometer.  The value
 * changes are instant; the phases then slide one 16-byte cell over 8
 * frames so the new digit rolls in.  [0x5B22]: 0 idle, 0xFF roll down
 * (armed by object_record_init 1000:F682 when a follower joins), 1 roll
 * up (no reachable arm), 2/3 slide in progress. */
static void odometer_tick(void)
{
    /* the guard tests the word at [0x5B25]: both digits zero -> stop */
    uint16_t both = (uint16_t)((td[1] << 8) | td[0]);

    switch (odoState) {
    case 0:
        return;
    case 2:
        if (odoSlide[1]) { tdPhase[1] = (uint16_t)(tdPhase[1] - 2u); odoSlide[1]--; }
        if (odoSlide[0]) { tdPhase[0] = (uint16_t)(tdPhase[0] - 2u); odoSlide[0]--; }
        if (!(odoSlide[0] | odoSlide[1])) odoState = 0;
        return;
    case 3:
        if (odoSlide[1]) { tdPhase[1] = (uint16_t)(tdPhase[1] + 2u); odoSlide[1]--; }
        if (odoSlide[0]) { tdPhase[0] = (uint16_t)(tdPhase[0] + 2u); odoSlide[0]--; }
        if (!(odoSlide[0] | odoSlide[1])) odoState = 0;
        return;
    case 1:
        if (both == 0x0909u) { odoState = 0; return; }  /* 99: capped    */
        odoState = 3;
        odoSlide[1] = 8;
        if (td[1]++ == 9) {
            td[1] = 0;
            tdPhase[1] = 0;                    /* roll up from the blank    */
            odoSlide[0] = 8;
            if (td[0]++ == 9) {
                odoState = 0;
                odoSlide[0] = 0;
            }
        }
        return;
    default:                                  /* 0xFF: the roll down      */
        if (both == 0) { odoState = 0; return; }
        odoState = 2;
        odoSlide[1] = 8;
        if (td[1]-- == 0) {
            td[1] = 9;
            tdPhase[1] = 0xB0;                /* slide down from cell 11  */
            odoSlide[0] = 8;
            if (td[0]-- == 0) {
                odoState = 0;                 /* 00: stop                 */
                odoSlide[0] = 0;
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
/* 1000:96C6: the elapsed clock's per-second step (ones 0..9, tens 0..5). */
static void elapsed_tick(void)
{
    if (++elapsed[0] == 10u) {
        elapsed[0] = 0u;
        if (++elapsed[1] == 6u) {
            elapsed[1] = 0u;
            elapsed[2]++;
        }
    }
}

static void clock_tick(void)
{
    cdSlide[2] = 8;
    if (cd[2]-- == 0) {
        cd[2] = 9;
        cdPhase[2] = 0xB0;
        cdSlide[1] = 8;
        if (cd[1]-- == 0) {
            cd[1] = 5;
            cdPhase[1] = 0x70;
            cdSlide[0] = 8;
            if (cd[0]-- == 0) {
                /* TIME UP: [0x5B16..18] := 0, [0x5B12] := 0, [0x5E44] := 0xFF */
                cdSlide[0] = cdSlide[1] = cdSlide[2] = 0;
                active = 0;
                timeUp = 1;
            }
        }
    }
}

/* The INT 8 hook at 1000:96AD (gated on [0x5B12]) + the JMP to the stamp:
 * the marks are stamped every frame — frozen at their last state once the
 * bar goes inactive, exactly as the original's page keeps them. */
void statusbar_tick(void)
{
    uint8_t *page;

    if (!sb_osys) return;
    if (barHidden) return;             /* 1000:342B: [0x5B0A] = 0xC9, no split */
    if (barSplit > 152u) barSplit--;   /* 1000:331E, one per display period */
    page = sb_osys->videoPage(0);
    if (!strip || !panel4 || !page) return;

    draw_band(page);
    draw_actions(page);

    if (active) {
        int died = 0;

        odometer_tick();

        if (--secDiv == 0) {
            secDiv = 70;                       /* one second at 70 Hz      */
            elapsed_tick();                    /* 1000:96C6                */
            clock_tick();
            died = timeUp;                     /* the original skips the
                                                  slide block on time-up  */
        }
        if (!died && --slideDiv == 0) {
            slideDiv = 2;
            if (cdSlide[2]) { cdPhase[2] = (uint16_t)(cdPhase[2] - 2u); cdSlide[2]--; }
            if (cdSlide[1]) { cdPhase[1] = (uint16_t)(cdPhase[1] - 2u); cdSlide[1]--; }
            if (cdSlide[0]) { cdPhase[0] = (uint16_t)(cdPhase[0] - 2u); cdSlide[0]--; }
        }
    }

    stamp_marks(page);
}

/* build_statusbar_layout (1000:961B) + level_start's time pick (1000:9589):
 * `packedTimeBcd` = the level's chosen limit, AH = minutes, AL = BCD
 * seconds (the header's timeA/B/C picked by the difficulty byte [0x5E65];
 * the port passes timeA — no difficulty select yet).  The tribe digits =
 * [0x11FC] - [0x17FB] (members - required), DIV/MOD 10 — the caller owns
 * both counts: the members start at 12/10/8 by difficulty (entry
 * 1000:00B1) and the required = the map's 0xAA-tile count (load_level
 * 1000:315F). */
void statusbar_start(uint16_t packedTimeBcd, unsigned tribeMembers,
                     unsigned tribeRequired)
{
    uint32_t n1Size = 0, n2Size = 0, i3Size = 0;
    uint16_t chk = 0;
    uint8_t *n1, *n2;
    uint8_t m = (uint8_t)(packedTimeBcd >> 8);
    uint8_t s = (uint8_t)(packedTimeBcd & 0xFFu);
    uint16_t diff;

    n1 = res_loadChunk("APL\\PANELN1.SPR", &n1Size, &chk);
    if (!n1 || n1Size != 768u) { free(n1); return; }
    n2 = res_loadChunk("APL\\PANELN2.SPR", &n2Size, &chk);
    if (!n2 || n2Size != 512u) {
        free(n1); free(n2); return;
    }
    free(strip);
    strip = malloc(STRIP_LEN);
    if (!strip) { free(n1); free(n2); return; }
    memcpy(strip, n1, 768u);
    memcpy(strip + 768u, n2, 512u);
    free(n1);
    free(n2);

    /* level_video_init's load (1000:32AD): the +0xC0 paragraph stride puts
     * PANELN2 right after PANELN1 — one 1280-byte strip (§12.7a)         */
    free(panel4);
    p4Size = 0;
    panel4 = res_loadChunk("LBM\\PANEL4.BIT", &p4Size, &chk);
    if (!panel4 || p4Size < 0xFA00u + 768u) {
        free(panel4);
        panel4 = NULL;
        return;
    }

    free(icons3);
    icons3 = res_loadChunk("APL\\ICONS3.SPR", &i3Size, &chk);
    if (!icons3 || i3Size != 4608u) {
        free(icons3);
        icons3 = NULL;
    }

    barSplit = 0xC9u;                  /* 1000:3040: hidden at level start */
    barHidden = 0;
    cd[0] = m;
    cd[1] = (uint8_t)(s >> 4);
    cd[2] = (uint8_t)(s & 0xFu);
    cdPhase[0] = (uint16_t)((m + 1u) * 16u);
    cdPhase[1] = (uint16_t)(((s >> 4) + 1u) * 16u);
    cdPhase[2] = (uint16_t)(((s & 0xFu) + 1u) * 16u);
    cdSlide[0] = cdSlide[1] = cdSlide[2] = 0;

    diff = (uint16_t)(tribeMembers - tribeRequired);
    td[0] = (uint8_t)(diff / 10u);
    td[1] = (uint8_t)(diff % 10u);
    tdPhase[0] = (uint16_t)((td[0] + 1u) * 16u);
    tdPhase[1] = (uint16_t)((td[1] + 1u) * 16u);
    odoState = 0;
    odoSlide[0] = odoSlide[1] = 0;

    elapsed[0] = elapsed[1] = elapsed[2] = 0u;  /* level_start 1000:95B5  */
    secDiv = 70;                              /* [0x5B14] = 0x46          */
    slideDiv = 2;                             /* [0x5B10] = 2             */
    /* active stays 0: [0x5B12] is level_fade_in's, set when the start chain
     * completes — the countdown must not run during the fade (see level.c) */
    timeUp = 0;
}

/* The live +0xC0 palette bank's source (level_video_init 1000:32CD's copy
 * of panel4's own palette): install at DAC entries 0xC0..0xFF. */
const uint8_t *statusbar_palette(void)
{
    return panel4 ? panel4 + 0xFA00u : NULL;
}

/* object_record_init's arm (1000:F682: [0x5B22] := 0xFF): a follower
 * joined — the displayed counter rolls down by one. */
void statusbar_armOdometer(void)
{
    odoState = 0xFF;
}

int statusbar_timeUp(void) { return timeUp; }

/* [0x5B12]: the ticking gate.  The in-level dialogs clear it (A167's head)
 * and restore it on return; the band keeps drawing while it is clear. */
void statusbar_setActive(int on) { active = on ? 1 : 0; }
void statusbar_setSplit(unsigned row)
{
    barSplit = row > 200u ? 200u : row;
    barHidden = 0;                     /* an explicit split re-composes it */
}
/* draw_level_text 0x342B: park the split at 0xC9 (off-screen) so the bar is
 * not composed; the level's ISR hook (0x331E) is already uninstalled, so it
 * stays hidden. */
void statusbar_hide(void) { barHidden = 1; barSplit = 0xC9u; }
unsigned statusbar_split(void) { return barSplit; }

/* The countdown word the save writes at +0x07: minutes << 8 | BCD seconds
 * (the original's [0x5B19] and [0x5B1A]*16 + [0x5B1B]). */
uint16_t statusbar_timeWord(void)
{
    return (uint16_t)(((unsigned)cd[0] << 8) |
                      (((unsigned)cd[1] & 0x0Fu) << 4) |
                      ((unsigned)cd[2] & 0x0Fu));
}

/* [0x1E84] after check_highscore's repack (1000:42CE): the *elapsed* clock as
 * minutes<<8 | tens<<4 | ones — the value the per-level best compares. */
uint16_t statusbar_elapsedWord(void)
{
    return (uint16_t)(((unsigned)elapsed[2] << 8) |
                      (((unsigned)elapsed[1] & 0x0Fu) << 4) |
                      ((unsigned)elapsed[0] & 0x0Fu));
}
