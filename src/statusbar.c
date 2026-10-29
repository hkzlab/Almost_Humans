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
 * Not ported, with the original's references: the elapsed 1/60s clock
 * [0x5B0C..0E] (consumed by check_highscore's score arithmetic, unported);
 * the line-compare reveal ([0x5B0A] 200→152 over 48 ticks, driven by the
 * pre-level party screens — the port has no party phase yet, so the bar
 * starts fully revealed); the time-up dialog ([0x5E44] → the
 * input_dispatch_loop pause branch — the frontend's; the bar itself just
 * stops, as the original's does).
 */
#include "game.h"
#include <stdlib.h>
#include <string.h>

#define STRIP_LEN 1280u      /* PANELN1 (768) + PANELN2 (512), §12.7a    */
#define BAR_FIRST_ROW 152u   /* the line-compare split, [0x5B0A] = 0x98  */
#define BAR_ROWS 40u        /* panel4's top 40 rows                      */
#define MARK_Y 172u         /* bar row 20 (the destinations 0x6E2..)     */

static const OSystem *sb_osys;
static uint8_t *strip;        /* the four-plane panel strip                */
static uint8_t *panel4;       /* LBM\PANEL4.BIT (the art + the palette)   */

static int active;            /* [0x5B12]: the bar's ticking gate         */
static int timeUp;            /* [0x5E44]: the countdown reached zero     */

/* the countdown: values [0x5B19..1B], phases [0x5B1C/1E/20], slides
 * [0x5B16..18] — indices 0 = minutes, 1 = seconds-tens, 2 = seconds-ones */
static uint8_t cd[3];
static uint16_t cdPhase[3];
static uint8_t cdSlide[3];
static uint8_t secDiv;        /* [0x5B14], seeded 0x46 = 70 frames       */
static uint8_t slideDiv;      /* [0x5B10], seeded 2                      */

/* the tribe odometer: values [0x5B25/26], phases [0x5B27/29], state
 * [0x5B22], slides [0x5B23/24] — indices 0 = tens, 1 = ones */
static uint8_t td[2];
static uint16_t tdPhase[2];
static uint8_t odoState;
static uint8_t odoSlide[2];

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
        for (unsigned r = 0; r < 8u; r++)
            for (unsigned j = 0; j < 8u; j++)
                page[(MARK_Y + r) * 320u + markX[m] + j] =
                    (uint8_t)(0xC0u | strip[base + 8u * r + j]);
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
        uint8_t *dst = page + (BAR_FIRST_ROW + y) * 320u;
        const uint8_t *src = panel4 + y * 320u;
        for (unsigned x = 0; x < 320u; x++)
            dst[x] = (uint8_t)(0xC0u | src[x]);
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
    page = sb_osys->videoPage(0);
    if (!strip || !panel4 || !page) return;

    draw_band(page);

    if (active) {
        int died = 0;

        odometer_tick();

        if (--secDiv == 0) {
            secDiv = 70;                       /* one second at 70 Hz      */
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
    uint32_t n1Size = 0, n2Size = 0, p4Size = 0;
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
    panel4 = res_loadChunk("LBM\\PANEL4.BIT", &p4Size, &chk);
    if (!panel4 || p4Size < 0xFA00u + 768u) {
        free(panel4);
        panel4 = NULL;
        return;
    }

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

    secDiv = 70;                              /* [0x5B14] = 0x46          */
    slideDiv = 2;                             /* [0x5B10] = 2             */
    active = 1;                               /* level_fade_in's [0x5B12] */
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

/* The countdown word the save writes at +0x07: minutes << 8 | BCD seconds
 * (the original's [0x5B19] and [0x5B1A]*16 + [0x5B1B]). */
uint16_t statusbar_timeWord(void)
{
    return (uint16_t)(((unsigned)cd[0] << 8) |
                      (((unsigned)cd[1] & 0x0Fu) << 4) |
                      ((unsigned)cd[2] & 0x0Fu));
}
