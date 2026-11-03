/* test_endflow.c — the level-end transition (draw_level_text 1000:33A3) and
 * check_highscore's outcome gate (1000:41AE), driven through game_main.
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * argv[2] = 1 for a won level, 0 for a lost one.
 *
 * The script reaches level 1 exactly like test_smoke; the present hook forces
 * the outcome at poll 7900 and a key dismisses the caption.  The hook latches
 * the three palette states (draw_level_text 0x33E2/0x33FD/0x3419):
 *   - grey:  entries 0..0x7F are the grey luma (r==g==b, non-zero) AND
 *            entries 0x80..0x83 are the shipped gradient (3C283C 3C263C ...);
 *   - black: the whole DAC is zero (the 0x48BC fade);
 *   - card:  entry 0xC0 is SCORE.BIT's palette entry 0 (00 20 3C) — the card
 *            bank installed by reblit 1000:4403.
 * A won level must reach all three and then the card; a lost level must reach
 * grey and black but NEVER the card (check_highscore's 0x41AE gate jumps to
 * 0x438A instead). */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;
extern void osys_null_inputScript(const InputState *states, uint32_t n);
extern const uint8_t *osys_null_palette(void);
extern void osys_null_onPresent(void (*fn)(uint32_t poll));

enum {
    FORCE_POLL = 7900u,
    SCRIPT_N = 9000u
};

static int32_t outcome;
static int32_t greySeen, blackSeen, cardSeen, seenCardAfterBlack;

static void hook(uint32_t poll)
{
    const uint8_t *pal = osys_null_palette();

    if (poll == FORCE_POLL) {
        /* mode 2 = the pause menu's RESTART THIS GAME (1000:390F), which must
         * run the lost-level caption and fades before the restart */
        if (outcome == 2) {
            int32_t q = 0;
            (void)gameflow_pauseAction(2, &q);
        } else {
            level_requestEnd(outcome);
        }
    }
    if (poll < FORCE_POLL) return;

    {
        uint32_t grey = 1, nz = 0, black = 1;

        for (uint32_t e = 0; e < 0x80u; e++) {
            if (!(pal[e * 3u] == pal[e * 3u + 1u] &&
                  pal[e * 3u] == pal[e * 3u + 2u]))
                grey = 0;
            if (pal[e * 3u]) nz = 1;
        }
        if (grey && nz && pal[0x80 * 3u] == 0x3C &&
            pal[0x80 * 3u + 1u] == 0x28 && pal[0x80 * 3u + 2u] == 0x3C &&
            pal[0x81 * 3u] == 0x3C && pal[0x81 * 3u + 1u] == 0x26)
            greySeen = 1;

        for (uint32_t i = 0; i < 768u; i++)
            if (pal[i]) black = 0;
        if (black) blackSeen = 1;

        /* the card bank: reblit copies SCORE.BIT+0xFA00 (entry 0 = 00 20 3C) */
        if (pal[0xC0 * 3u] == 0x00 && pal[0xC0 * 3u + 1u] == 0x20 &&
            pal[0xC0 * 3u + 2u] == 0x3C) {
            cardSeen = 1;
            if (blackSeen) seenCardAfterBlack = 1;
        }
    }
}

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    char path[512];
    FILE *f;
    static InputState script[SCRIPT_N];
    int32_t bad = 0;

    outcome = argc > 2 ? atoi(argv[2]) : 1;

    snprintf(path, sizeof path, "%s/HUMANS.002", vol_dir);
    f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume files at %s (fixture data absent)\n", vol_dir);
        return 0;
    }
    fclose(f);
    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }

    /* the test_smoke script: attract escape, the room's right exit, the
     * party's key, the goal page's held key, then force the level's end at
     * FORCE_POLL and dismiss the caption. */
    memset(script, 0, sizeof script);
    script[6600].joyBits = 0x80u;
    for (uint32_t i = 6700; i <= 6950u; i++) script[i].joyBits = 0x08u;
    script[7150].lastKey = 1u;
    for (uint32_t i = 7250u; i <= 7700u; i++) script[i].lastKey = 1u;
    for (uint32_t i = FORCE_POLL + 50u; i <= FORCE_POLL + 160u; i++)
        script[i].lastKey = 1u;              /* the caption's key */
    script[SCRIPT_N - 1].quit = 1u;

    osys_null_inputScript(script, SCRIPT_N);
    osys_null_onPresent(hook);
    if (game_main(&osys_null) != 0) {
        fprintf(stderr, "FAIL game_main returned nonzero\n");
        return 1;
    }

    if (!greySeen) {
        fprintf(stderr,
                "FAIL 1000:33E2: the grey fade + gradient bank was not seen\n");
        bad = 1;
    }
    if (!blackSeen) {
        fprintf(stderr, "FAIL 1000:3419: the black fade was not seen\n");
        bad = 1;
    }
    if (outcome == 1) {
        if (!cardSeen || !seenCardAfterBlack) {
            fprintf(stderr, "FAIL a won level did not open the score card\n");
            bad = 1;
        }
    } else if (cardSeen) {
        fprintf(stderr, "FAIL 1000:41AE: a lost level opened the score card\n");
        bad = 1;
    }

    if (!bad)
        printf("endflow(%s): OK\n",
               outcome == 2 ? "restart" : (outcome ? "won" : "lost"));
    return bad;
}
