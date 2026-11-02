/* score.c — check_highscore (1000:41AE), the score/record screen between the
 * level's end page and the post-level newspaper.
 *
 * The screen, in the original's order:
 *   - the score arithmetic (41B8..4235): the member recount, the level term and
 *     the placed-object bonus, the time term;
 *   - the four resource loads (4257..4276): LBM\SCORE.BIT, APL\SCORE.SPR,
 *     APL\SCORENUM.SPR and APL\RECORD.SPR;
 *   - or_region_c0, build_highscore_table, reblit_framebuffer_88 (the card),
 *     crtc_write(0x8F) - the 320x200 field;
 *   - the NEW RECORD test (the best-time table at DS:0x38A + level*2, all
 *     0x0999 sentinels shipped), the running total and its displayed copy;
 *   - draw_scorenum_three_passes twice (the elapsed and best times), then
 *     draw_score_columns (the three counters);
 *   - the fade to 0x14D3 and the loop: tick_score_counters + wait_vsync +
 *     poll_input, ending on the fire bit, [0x48A3] or [0x1DC8]'s 0x15E
 *     countdown; then the fade to 0x48BC and vga_set_mode_13h.
 *
 * The high-score file, the top-10 insert and the name entry live in hiscore.c
 * (HISCORES.HUM) and map.c (the insert screen).  The stamp's underlay is saved
 * and restored by score_stampDraw (0x4697/0x46A3).
 */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- check_highscore's data (1000:41B8..4352) ---------------------------
 *
 * `1000:41B8` recomputes the member count `[0x11FC]` from the bar's odometer
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
 * `[0x1E84]` (minutes<<8 | BCD seconds, `statusbar_elapsedWord()`), compared with
 * `<`; a better time is stored back and `[0x1DD6] = 0xFF`.  The level's two terms
 * are then added into the running total `[0x592]/[0x594]`, with the displayed
 * copy `[0x1DD2]/[0x1DD4]` holding the *old* total - `1000:475D` then counts the
 * display up 10 at a time.
 */
static uint16_t scoreLevelTerm;    /* [0x1DCA] */
static uint16_t scoreTimeTerm;     /* [0x1DCE] */
static uint16_t scoreTotalLo, scoreTotalHi;       /* [0x592]/[0x594]  */
static uint16_t scoreShownLo, scoreShownHi;       /* [0x1DD2]/[0x1DD4] */
static uint16_t scoreDelay;                       /* [0x1DC6] */
static uint16_t scoreScreenTimer;                 /* [0x1DC8]: 0x15E, the screen's
                                                   * own countdown - exits when the
                                                   * sign bit sets */
static uint16_t scoreBestTime;                    /* [0x1E88] */
static int scoreIsRecord;                         /* [0x1DD6] */
static uint8_t scoreBlink = 0x19;                 /* [0x1DD7] */
static int scoreBlinkPhase;                       /* [0x1DC4]: the RECORD's on/off */

/* 1000:41B8..41E6: the member count the score uses. */
unsigned score_members(void)
{
    unsigned n = statusbar_tribeValue();
    unsigned live = object_liveCount();

    (void)live;
    for (unsigned i = 0; i < object_staticSlots(); i++) {
        const uint8_t *r = object_slot(i);

        if (!r || (r[6] & 0x80u)) continue;
        {
            unsigned ty = (unsigned)(r[8] | (r[9] << 8));

            if (ty == 0x1Du || ty == 0x1Eu) continue;
        }
        n++;
    }
    return n;
}

/* 1000:41EA..4235: the level's two terms. */
static void score_terms(void)
{
    unsigned levelNo = gameflow_levelForProbe() + 1u;

    scoreLevelTerm = (uint16_t)(levelNo * score_members() * 10u);
    for (unsigned i = 0; i < object_staticSlots(); i++) {
        unsigned cell = object_recordMapCell(i);

        if (cell == 0xA8u) scoreLevelTerm = (uint16_t)(scoreLevelTerm + 1000u);
        else if (cell == 0xA7u) scoreLevelTerm = (uint16_t)(scoreLevelTerm + 2000u);
    }
    {
        uint16_t t = statusbar_timeWord();
        unsigned mins = (unsigned)(t >> 8);
        unsigned secs = (unsigned)(((t >> 4) & 0xFu) * 10u + (t & 0xFu));

        scoreTimeTerm = (uint16_t)((mins * 60u + secs) * 10u);   /* 1000:429B.. */
    }
}

/* 1000:42CE..4352: the best-time comparison, the NEW RECORD flag and the
 * running total. */
void score_setTotal(unsigned lo, unsigned hi)
{
    scoreTotalLo = (uint16_t)lo;
    scoreTotalHi = (uint16_t)hi;
}

void score_prepare(void)
{
    unsigned levelNo = gameflow_levelForProbe() + 1u;
    uint16_t elapsed = statusbar_elapsedWord();     /* [0x1E84] */
    unsigned idx = levelNo;

    scoreLevelTerm = 0;                             /* [0x1DCA] starts at 0 */
    score_terms();
    scoreIsRecord = 0;                              /* 1000:42E4 */
    if (elapsed < hiscore_best(idx)) {              /* 1000:42F9 */
        hiscore_setBest(idx, elapsed);              /* 1000:42FD */
        scoreBestTime = elapsed;                    /* 1000:4301 */
        scoreIsRecord = 1;                          /* 1000:4305 */
    }
    scoreShownLo = scoreTotalLo;                    /* 1000:433A */
    scoreShownHi = scoreTotalHi;
    {
        uint32_t total = ((uint32_t)scoreTotalHi << 16) | scoreTotalLo;

        total += scoreLevelTerm;                    /* 1000:4341 */
        total += scoreTimeTerm;                     /* 1000:4348 */
        scoreTotalLo = (uint16_t)(total & 0xFFFFu);
        scoreTotalHi = (uint16_t)(total >> 16);
    }
    scoreDelay = 0x50u;                             /* 1000:4327 */
    scoreScreenTimer = 0x15Eu;                      /* 1000:432C [0x1DC8] */
}

/* 1000:467A blink_tick: while [0x1DD6] (NEW RECORD) is set, a 0x19-frame
 * counter toggles [0x1DC4], the RECORD sprite's on/off phase (its two drawing
 * halves at 0x46A3 / 0x46FC are not ported: they copy a 16x24 mask from the
 * scratch at 0x3CA0 to the page). */
/* the stamp's current phase ([0x1DC4]): 1 = drawn */
int scoreBlinkOn(void) { return scoreBlinkPhase; }

int score_blinkTick(void)
{
    if (!scoreIsRecord) return 0;
    if (--scoreBlink) return 0;
    scoreBlink = 0x19;
    scoreBlinkPhase ^= 1;
    return scoreBlinkPhase;
}

/* 1000:475D tick_score_counters: one 10-point step of the displayed total. */
int score_countersRun(void)
{
    if (scoreDelay) { scoreDelay--; return 1; }
    if (scoreLevelTerm) {
        uint32_t shown = ((uint32_t)scoreShownHi << 16) | scoreShownLo;

        if (scoreLevelTerm < 10u) {
            shown += scoreLevelTerm;                /* 1000:478B..4798 */
            scoreLevelTerm = 0;
        } else {
            scoreLevelTerm = (uint16_t)(scoreLevelTerm - 10u);
            shown += 10u;                           /* 1000:479E */
        }
        scoreShownLo = (uint16_t)(shown & 0xFFFFu);
        scoreShownHi = (uint16_t)(shown >> 16);
        return 1;
    }
    if (scoreTimeTerm) {
        uint32_t shown = ((uint32_t)scoreShownHi << 16) | scoreShownLo;

        if (scoreTimeTerm < 10u) {
            shown += scoreTimeTerm;
            scoreTimeTerm = 0;
        } else {
            scoreTimeTerm = (uint16_t)(scoreTimeTerm - 10u);
            shown += 10u;
        }
        scoreShownLo = (uint16_t)(shown & 0xFFFFu);
        scoreShownHi = (uint16_t)(shown >> 16);
        return 1;
    }
    /* both terms spent: the screen's own countdown runs.  It exits when the
     * sign bit sets, i.e. after 0x15E + 1 ticks (1000:47D2's [0x1DC8]--). */
    scoreScreenTimer--;
    return (scoreScreenTimer & 0x8000u) ? 0 : 1;
}

int      score_isRecord(void) { return scoreIsRecord; }
uint16_t score_bestFor(unsigned level) { return hiscore_best(level); }
uint16_t score_bestTime(void) { return scoreBestTime; }
unsigned score_totalLo(void) { return scoreTotalLo; }
unsigned score_totalHi(void) { return scoreTotalHi; }
unsigned score_shownLo(void) { return scoreShownLo; }
unsigned score_shownHi(void) { return scoreShownHi; }


#define SCORE_PAGE (320u * 200u)
enum { SCORE_OFF, SCORE_WAIT, SCORE_FADE, SCORE_HOLD };

static uint8_t *scoreBit;
static uint32_t scoreSize;
static uint8_t *scoreFont;
static uint8_t *scoreDigits;      /* apl\SCORE.SPR (the 4x16 digit blocks) */
static uint8_t *scoreRecord;      /* apl\RECORD.SPR (the NEW RECORD stamp)  */
static uint8_t *scoreNum;         /* apl\SCORENUM.SPR (20 slots of 0x40)    */
static int scorePhase;
static int scoreArmed;
static int scoreArmFade;   /* the entry fade is still running */
static uint16_t scoreHold;

/* check_highscore's score terms (1000:41AE).  [0x1F41] = the level number
 * (1-based, the same value draw_level_text prints), [0x11FC] = the members. */
unsigned score_levelScore(void)
{
    uint16_t t;
    unsigned mins, secs, remaining, levelNo, bonus;

    if (!level_outcome()) return 0u;         /* 55B3: a lost level adds none */

    t = statusbar_timeWord();                /* [0x5B19/1A/1B] */
    mins = (unsigned)(t >> 8);
    secs = (unsigned)(((t >> 4) & 0xFu) * 10u + (t & 0xFu));
    remaining = mins * 60u + secs;

    levelNo = gameflow_levelForProbe() + 1u; /* [0x1F41] */
    /* the original multiplies by the *recounted* [0x11FC] (1000:41B8: the
     * odometer digits plus the live records), not by the level-start tribe */
    bonus = levelNo * score_members() * 10u + remaining * 10u;

    /* 55FA..5635: the eight static records' cells, 0xA8 (+1000) / 0xA7 (+2000) */
    for (unsigned i = 0; i < object_staticSlots(); i++) {
        unsigned cell = object_recordMapCell(i);
        if (cell == 0xA8u) bonus += 1000u;
        else if (cell == 0xA7u) bonus += 2000u;
    }
    (void)remaining;
    return bonus;
}



/* --- 1000:46FC / 0x4697 / 0x46A3: the NEW RECORD stamp -------------------
 *
 * `blink_tick` toggles `[0x1DC4]` and jumps on the new phase: 0 -> `0x46A3`,
 * 1 -> `0x46FC`.  `0x46FC` draws `apl\record.spr` ([0x1DDE]) into the page at
 * byte `0x3CA0 + [0x4106]`: 0x18 rows x 64 bytes, `LODSB` + `ADD SI,3` (the
 * `p::4` pass de-interleave), a non-zero byte drawn as `0xC0 + v`, four
 * Map-Mask passes.  A page address is a 4-pixel group (plane p = pixel 4j+p),
 * so the four passes read `src[4j+p]` into pixel `4j+p`: a 1:1 copy of the
 * chunky 64 x 24 sprite.  `0x3CA0` is not a multiple of the 88-address row:
 * 0x3CA0 = 176*88 + 32, so the stamp sits at row 176, address column 32 -
 * a 64 x 24 cell at px 128..191 on rows 176..199.
 *
 * 0x4697/0x46A3 are one body at 0x46AF with source and destination swapped.
 * Its segment setup is `B8 00 A0 / 8E C0 / 8E D8`: ES and DS are both A000,
 * so the scratch is **A000:0000**, not DS:0.  The on phase (`0x46FC`) copies
 * the 16 x 24 cell to page 0 and then draws the sprite; the off phase
 * (`0x46A3`) copies page 0 back.  The copy runs with Map Mask 0x0F and read
 * map 0, so it carries one page byte per 4-px group (plane 0's pixel) and
 * writes it to all four; the port keeps that shape (`stampCopy`).
 */
#define STAMP_ROW 176u
#define STAMP_COL 32u

/* 1000:46AF: 24 rows x 16 page bytes at the 88-byte stride, Map Mask 0x0F
 * and read map 0.  A page address is a 4-pixel group (plane p = pixel 4j+p),
 * so the copy carries the group's plane-0 pixel (the first of the four) and
 * writes it back to all four.
 *
 * The scratch itself is A000:0000, while the screen's page base ([0x4106])
 * is elsewhere (the live screen had 0x1230), so the scratch is NOT part of
 * the visible page.  The port keeps it in a private buffer of the same shape
 * instead of the flat page's top-left. */
static uint8_t stampUnder[24u * 16u];

void score_stampDraw(uint8_t *page, int on)
{
    unsigned r, j;

    if (!page) return;
    if (!on) {
        /* 1000:46A3: A000:0000 -> the cell. */
        for (r = 0; r < 24u; r++)
            for (j = 0; j < 16u; j++) {
                unsigned v = stampUnder[r * 16u + j];
                unsigned py = STAMP_ROW + r;
                unsigned px = (STAMP_COL + j) * 4u;

                if (py >= 200u || px + 3u >= 320u) continue;
                page[py * 320u + px + 0u] = (uint8_t)v;
                page[py * 320u + px + 1u] = (uint8_t)v;
                page[py * 320u + px + 2u] = (uint8_t)v;
                page[py * 320u + px + 3u] = (uint8_t)v;
            }
        return;
    }
    /* 1000:4697: the cell -> A000:0000, then draw the sprite. */
    for (r = 0; r < 24u; r++)
        for (j = 0; j < 16u; j++) {
            unsigned py = STAMP_ROW + r;
            unsigned px = (STAMP_COL + j) * 4u;

            stampUnder[r * 16u + j] =
                (py < 200u && px < 320u) ? page[py * 320u + px] : 0u;
        }
    /* 1000:46FC: apl\RECORD.SPR is chunky 64x24; the four passes read
     * src[4j+p] into plane p at address j, i.e. pixel 4j+p = src[4j+p].
     * A zero byte leaves the saved underlay alone. */
    for (r = 0; r < 24u; r++)
        for (j = 0; j < 64u; j++) {
            unsigned v = scoreRecord ? scoreRecord[r * 64u + j] : 0u;
            unsigned py = STAMP_ROW + r;
            unsigned px = STAMP_COL * 4u + j;

            if (!v || py >= 200u || px >= 320u) continue;
            page[py * 320u + px] = (uint8_t)(v + 0xC0u);
        }
}

/* --- 1000:462D / 1000:4578: the score screen's digit columns ------------
 *
 * `blit_score_block` (0x462D): `SI = digit*256` into SCORE.SPR (ten 0x100-byte
 * slots).  The page address is a 4-pixel group (plane p = pixel 4j+p).  Each
 * plane pass reads 16 rows x 4 bytes and pushes/pops only DI (and CX), NOT SI,
 * so pass p starts 64 bytes on and reads the slot's quarter p: the glyph is a
 * 16 x 16 chunky image stored plane-major, pixel 4k+p = src[p*64 + r*4 + k],
 * drawn as 0xC0 | v (`or_region_c0` 0x4278 biases the sheet).
 * `statusbar_segment` (0x4578): a 32-bit
 * value from [0x4228]/[0x422A] drawn as DX digits at (AX, BX), right to left
 * in 4-address (16 px) steps; the value is divided by 10000 first, then by 10,
 * and an exhausted value keeps drawing block 0 (zero-padded).
 */
static void score_digitBlock(uint8_t *page, unsigned digit, int x, int y)
{
    const uint8_t *src;

    if (!scoreDigits || digit > 9u) return;   /* ten 0x100-byte slots */
    src = scoreDigits + digit * 256u;         /* SI = AX = digit*0x100 */
    /* 1000:462D: 16 rows x 4 bytes per plane pass.  The pass pushes/restores
     * only DI, NOT SI, so pass p starts 64 bytes on and reads the slot's
     * quarter p: the glyph is a 16x16 chunky image stored plane-major,
     * pixel 4k+p = src[p*64 + r*4 + k].  The runtime OR of 0xC0 into the
     * sheet (0x4278) biases the colour, and every byte is stored (opaque). */
    for (unsigned r = 0; r < 16u; r++)
        for (unsigned px = 0; px < 16u; px++) {
            uint8_t v = (uint8_t)(src[(px & 3u) * 64u + r * 4u + (px >> 2)] | 0xC0u);
            int dx = x + (int)px;
            int dy = y + (int)r;

            if (dx >= 0 && dx < 320 && dy >= 0 && dy < 200)
                page[(unsigned)dy * 320u + (unsigned)dx] = v;
        }
}

void score_column(uint8_t *page, uint32_t value, int x, int y, unsigned digits)
{
    unsigned d = digits;

    if (!d) return;
    for (unsigned i = 0; i < d; i++) {
        score_digitBlock(page, (unsigned)(value % 10u), x + (int)(4u * (d - 1u - i)) * 4,
                         y);
        value /= 10u;
    }
}

/* 1000:4471 draw_score_columns: the level term, the time term and the running
 * total.  Column x is the pixel origin; each digit is 16 px wide. */
void score_drawColumns(uint8_t *page)
{
    score_column(page, scoreLevelTerm, 0xE0, 0x10, 5u);   /* (0xE0, 0x10) */
    score_column(page, scoreTimeTerm,  0xE0, 0x42, 5u);   /* (0xE0, 0x42) */
    score_column(page, ((uint32_t)scoreShownHi << 16) | scoreShownLo,
                 0xA0, 0x74, 9u);                          /* (0xA0, 0x74) */
}

/* --- 1000:44BB / 1000:450F: the elapsed and best times -------------------
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
 */
static void score_numBlock(uint8_t *page, unsigned digit, unsigned bp, unsigned off)
{
    unsigned row = off / 88u, col = off % 88u;
    unsigned px = col * 4u;                  /* one address = 4 px */

    if (!page || !scoreNum || digit > 19u || row >= 200u) return;
    /* 1000:450F: 8 rows x 2 addresses.  Pass p takes src[slot + r*8 + p] and
     * src[slot + r*8 + 4 + p], so pixel x of the row is src[r*8 + x]: the
     * slot is a chunky 8x8 image, stored as v | 0xC0. */
    for (unsigned r = 0; r < 8u; r++) {
        unsigned y = row + r;

        if (y >= 200u) break;
        for (unsigned x = 0; x < 8u; x++) {
            if (px + x >= 320u) continue;
            page[y * 320u + px + x] =
                (uint8_t)(scoreNum[digit * 0x40u + bp + r * 8u + x] | 0xC0u);
        }
    }
}

void score_drawNum3(uint8_t *page, unsigned ax, uint16_t value, unsigned bx, unsigned bp)
{
    unsigned off = bx * 88u + (ax >> 2);

    score_numBlock(page, (unsigned)((value >> 8) & 0x0Fu), bp, off + 6u);
    score_numBlock(page, (unsigned)((value >> 4) & 0x0Fu), bp, off + 8u);
    score_numBlock(page, (unsigned)(value & 0x0Fu),        bp, off + 10u);
}


/* 1000:4403 reblit_framebuffer_88's card pass: SCORE.BIT is a chunky 320x200
 * image (one byte per pixel); the four plane passes read `src[4j+p]` into
 * plane p at address j, i.e. pixel 4j+p = src[4j+p] - a plain 1:1 copy.  A
 * non-zero byte is drawn as 0xC0 + v, a zero byte is LEFT ALONE (the page
 * keeps the level caption).  The file's first 64 palette entries are the
 * card's own DAC 0xC0..0xFF bank. */
void score_drawCard(uint8_t *page)
{
    if (!page || !scoreBit || scoreSize < 320u * 200u) return;
    /* 1000:4403: SI walks the chunky 320-byte row at stride 4 (one byte per
     * plane pass) and DI is the 4-px page group, so pixel 4j+p = src[4j+p]:
     * a plain 1:1 copy.  A zero byte is LEFT ALONE (the level caption). */
    for (unsigned i = 0; i < 320u * 200u; i++) {
        unsigned v = scoreBit[i];

        if (v) page[i] = (uint8_t)(v + 0xC0u);
    }
}

int score_begin(unsigned value)
{
    if (scorePhase != SCORE_OFF) return 1;

    score_prepare();          /* 1000:41B8..4352: the terms, best, totals */

    /* 1000:428D: `MOV BX,0x128F; CALL 0x88E9` (crtc_write) sets CRTC 0x12 =
     * 0x8F, the 400-scanline vertical display, i.e. the 320x200 field - the
     * score card's.  (The full mode set at 0x8300 is called at the *end*,
     * 0x439F, for the newspaper.) */
    video_setMode(VIDEO_MODE_13H);
    (void)value;
    res_setDisc(1u);                          /* the score assets' disc */
    free(scoreBit); scoreBit = NULL; scoreSize = 0;
    scoreBit = res_loadChunk("LBM\\SCORE.BIT", &scoreSize, NULL);
    uint8_t *page = video_page(0);
    /* The page is NOT cleared: `check_highscore` draws the card straight over
     * whatever the level's caption left on it (`draw_level_text` 0x33A3 ends
     * with the page drawn and the DAC faded to black), and
     * `reblit_framebuffer_88` skips a zero source byte.  The live stamp cell
     * shows exactly that: bytes 1..0xBF (level-palette indices) inside the card
     * where the sprite is transparent.  An earlier revision memset the page,
     * which made those spots black instead - corrected. */
    /* 1000:4278..4284 or_region_c0(10, 0x100, 0) runs with ES = [0x1DDA] =
     * APL\SCORE.SPR, i.e. it ORs 0xC0 into the DIGIT SHEET's first 4096 bytes,
     * not into the page.  The bias only sets bits 4..7, which this page's 4-px
     * groups never read, so score_digitBlock's 0x0F ink is the same result. */
    if (scoreBit) {
        score_drawCard(page);
        if (scoreSize >= SCORE_PAGE + 768u) {
            /* 1000:4333 palette_build_fade(0x100, 0, 0x23, 0x14D3): the screen
             * FADES IN from the level's (black) palette to 0x14D3, whose
             * 0xC0..0xFF bank is the card's own 64 colours - the file's first
             * 64 entries.  The fade is stepped by the tick and waited out
             * before the input loop. */
            static uint8_t fadeTarget[768];

            /* 0x4333 targets 0x14D3, the LEVEL palette (sealed to grey), not
             * the image working palette 0x4BBC. */
            memcpy(fadeTarget, level_palette(), sizeof fadeTarget);
            memcpy(fadeTarget + 0xC0u * 3u, scoreBit + SCORE_PAGE, 192u);
            palette_buildFade(0u, 0x100u, 0x23u, fadeTarget);
        }
        scoreArmFade = 1;
    }
    free(scoreFont);
    scoreFont = res_loadChunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(scoreFont);
    free(scoreDigits);
    scoreDigits = res_loadChunk("APL\\SCORE.SPR", NULL, NULL);   /* 1000:4264 */
    free(scoreRecord);
    scoreRecord = res_loadChunk("APL\\RECORD.SPR", NULL, NULL);  /* 1000:4276 */
    free(scoreNum);
    scoreNum = res_loadChunk("APL\\SCORENUM.SPR", NULL, NULL);   /* 1000:4269 */

    /* 1000:4310/0x4320 draw_scorenum_three_passes: the elapsed time at
     * (0x50, 0xB5) and the best time at (0xF0, 0xB5), both row 181. */
    score_drawNum3(page, 0x50u, statusbar_elapsedWord(), 0xB5u, 0u);
    score_drawNum3(page, 0xF0u, scoreBestTime,          0xB5u, 0x280u);

    /* 1000:4471: the three counter columns (the level term, the time term and
     * the running total) in SCORE.SPR's digits. */
    score_drawColumns(page);
    /* The NEW RECORD stamp is apl\record.spr at row 176, px 128..191
     * (0x3CA0 = 176*88 + 32) - blink_tick redraws it.  An earlier revision
     * also drew the words with textpage_drawLine at row 0x90; the live screen
     * (2026-11-01) shows only the sprite, so that stand-in is gone. */

    scorePhase = SCORE_WAIT;
    scoreArmed = 0;
    return 1;
}

int score_tick(const InputState *in)
{
    static const uint8_t black[768];
    switch (scorePhase) {
    case SCORE_WAIT:
        /* The port's caller hands in the same InputState that dismissed the end
         * page, so the first tick skips the key test; the original calls
         * poll_input inside its loop and so never sees that key. */
        if (scoreArmFade) {                  /* 1000:4333's wait_palette_fade_done */
            if (palette_fadeActive()) return 1;
            scoreArmFade = 0;
        }
        if (!scoreArmed) { scoreArmed = 1; return 1; }
        {
            uint8_t *page = video_page(0);
            int alive;

            /* tick_score_counters (1000:475D): draw_score_columns, blink_tick,
             * then one 10-point step.  The drawing happens every frame. */
            if (page) score_drawColumns(page);
            /* 1000:467A: the expiry toggles [0x1DC4] and runs the *new*
             * phase's handler - 1000:46A3 restores the cell (off), 1000:46FC
             * saves it and draws the sprite (on).  Both halves run. */
            {
                int was = scoreBlinkOn();

                (void)score_blinkTick();
                if (page && scoreBlinkOn() != was)
                    score_stampDraw(page, scoreBlinkOn());
            }
            alive = score_countersRun();

            /* check_highscore's loop: while ([0x1DC8] & 0x8000) == 0, and it
             * also breaks on the fire bit ([0x5ACE] & 0x80) or [0x48A3].  The
             * screen ends by itself after 0x15E ticks. */
            if (!alive || in->lastKey || (in->joyBits & 0x80u)) {
                palette_buildFade(0u, 0x100u, 0x23u, black); /* 1000:438A */
                scorePhase = SCORE_FADE;
            }
        }
        return 1;
    case SCORE_FADE:
        if (palette_fadeActive()) return 1;
        scorePhase = SCORE_HOLD;
        scoreHold = 0x4Bu;                    /* present_frames_75 */
        return 1;
    case SCORE_HOLD:
        if (scoreHold > 0u) { scoreHold--; return 1; }
        scorePhase = SCORE_OFF;
        /* 1000:439F's tail: [0x4105] = 0 and [0x1DC4] = 0 - the stamp's phase
         * is cleared as the mode goes back to 13h. */
        scoreBlinkPhase = 0;
        free(scoreBit); scoreBit = NULL; scoreSize = 0;
        free(scoreFont); scoreFont = NULL;
        free(scoreRecord); scoreRecord = NULL;
        free(scoreNum); scoreNum = NULL;
        font_publish(NULL);
        return 0;
    default:
        return 0;
    }
}
