/* score.c — check_highscore (1000:41AE), the score/record screen between the
 * level's end page and the post-level newspaper.
 *
 * START (this pass): LBM\SCORE.BIT's card and palette, the level score and a
 * key/ACTION exit with the original's fade to 0x48BC (div 0x23).  The
 * record table (HISCORES.HUM), the SCORENUM/RECORD sprites, the per-level
 * best-time table ([0x138A + level*2]) and the NEW RECORD detection are the
 * next pass (re_map.md, "The level-end score").
 *
 * The score is `check_highscore`'s arithmetic (1000:41AE), all of it here:
 *   [0x1DCA] += [0x1F41] * [0x11FC] * 10      (55EA: level x members x 10)
 *   [0x1DCA] += the placed-object bonuses     (55FA..5635: for each of the 8
 *                                              records at 0x3E75, the map cell
 *                                              under it via 0xF761(AX=8,BX=0xC):
 *                                              0xA8 -> +1000, 0xA7 -> +2000)
 *   [0x1DCE]  = remaining_seconds * 10        (569B..56CB, from the countdown)
 *   total     = [0x1DCA] + [0x1DCE]           (5733)
 * A lost level adds nothing at all (55B3 -> 578A).  The cross-level running
 * total ([0x592]/594) is not modelled, so the value is this level's.
 */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCORE_PAGE (320u * 200u)
enum { SCORE_OFF, SCORE_WAIT, SCORE_FADE, SCORE_HOLD };

static uint8_t *scoreBit;
static uint32_t scoreSize;
static uint8_t *scoreFont;
static int scorePhase;
static int scoreArmed;
static uint16_t scoreHold;
static unsigned scoreValue;

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
    bonus = levelNo * level_members() * 10u + remaining * 10u;

    /* 55FA..5635: the eight static records' cells, 0xA8 (+1000) / 0xA7 (+2000) */
    for (unsigned i = 0; i < object_staticSlots(); i++) {
        unsigned cell = object_recordMapCell(i);
        if (cell == 0xA8u) bonus += 1000u;
        else if (cell == 0xA7u) bonus += 2000u;
    }
    (void)remaining;
    return bonus;
}

int score_begin(unsigned value)
{
    if (scorePhase != SCORE_OFF) return 1;

    scoreValue = value;
    res_setDisc(1u);                          /* the score assets' disc */
    free(scoreBit); scoreBit = NULL; scoreSize = 0;
    scoreBit = res_loadChunk("LBM\\SCORE.BIT", &scoreSize, NULL);
    uint8_t *page = video_page(0);
    memset(page, 0, SCORE_PAGE);
    if (scoreBit) {
        memcpy(page, scoreBit, scoreSize < SCORE_PAGE ? scoreSize : SCORE_PAGE);
        if (scoreSize >= SCORE_PAGE + 768u)
            palette_setDac(scoreBit + SCORE_PAGE, 0, 256);
    }
    free(scoreFont);
    scoreFont = res_loadChunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(scoreFont);

    char line[40];
    snprintf(line, sizeof line, "YOUR SCORE IS %04u", scoreValue & 0xFFFFu);
    textpage_drawLine(page, line, 0x50u, 0x0Fu);
    textpage_drawLine(page, "PRESS FIRE", 0x70u, 0x0Fu);

    scorePhase = SCORE_WAIT;
    scoreArmed = 0;
    return 1;
}

int score_tick(const InputState *in)
{
    static const uint8_t black[768];
    switch (scorePhase) {
    case SCORE_WAIT:
        if (!scoreArmed) { scoreArmed = 1; return 1; }
        if (in->lastKey || (in->joyBits & 0x80u)) {
            palette_buildFade(0u, 0x100u, 0x23u, black); /* 1000:438A */
            scorePhase = SCORE_FADE;
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
        free(scoreBit); scoreBit = NULL; scoreSize = 0;
        free(scoreFont); scoreFont = NULL;
        font_publish(NULL);
        return 0;
    default:
        return 0;
    }
}
