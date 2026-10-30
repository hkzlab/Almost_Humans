/* score.c — check_highscore (1000:41AE), the score/record screen between the
 * level's end page and the post-level newspaper.
 *
 * START (this pass): LBM\SCORE.BIT's card and palette, the level score and a
 * key/ACTION exit with the original's fade to 0x48BC (div 0x23).  The
 * record table (HISCORES.HUM), the SCORENUM/RECORD sprites and the
 * NEW RECORD detection are the next pass; the score value the port owns is
 * the statusbar's running score word (0 until the accumulation lands).
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
