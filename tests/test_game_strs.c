/* test_game_strs.c — the text table's invariants across every language column.
 *
 * The text itself is verified against the shipped images by
 * `tools/extract_game_strs.py` (which derives each column from its build);
 * this test guards the structure the port's call sites rely on: the printf
 * markers, the in-place digit fields, the dialog line counts, and the two
 * columns whose seventh map line is their header string.
 */
#include <stdio.h>
#include <string.h>

#include "game_strs.h"

static int failures;

static void check(int cond, const char *what, enum GameLang lang)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s (lang %d)\n", what, (int)lang);
        failures++;
    }
}

static unsigned count(const char *s, char c)
{
    unsigned n = 0;

    for (; *s; s++)
        if (*s == c) n++;
    return n;
}

/* length of the digit run starting at `off`, 0 when that byte is not a digit */
static unsigned digit_run(const char *s, uint8_t off)
{
    unsigned n = 0;

    if (s[off] < '0' || s[off] > '9') return 0;
    while (s[off + n] >= '0' && s[off + n] <= '9') n++;
    return n;
}

int main(void)
{
    /* the four party templates the briefing patches in place, with the width
     * of the digit field the shipped template carries */
    static const struct {
        enum GameStr id;
        unsigned width;
    } field[] = {
        { GAME_STR_PARTY_COMING_UP, 3 },
        { GAME_STR_PARTY_NEED, 1 },
        { GAME_STR_PARTY_HAS, 2 },
        { GAME_STR_PARTY_SCORE, 11 },
    };

    for (int l = 0; l < GAME_LANG_COUNT; l++) {
        enum GameLang lang = (enum GameLang)l;

        for (int s = 0; s < GAME_STR_COUNT; s++)
            check(game_strs[l][s] != NULL, "string is NULL", lang);

        /* the two printf formats keep exactly one conversion each */
        check(count(game_strs[l][GAME_STR_GAMEFLOW_LEVEL_DONE], '%') == 1 &&
                  strstr(game_strs[l][GAME_STR_GAMEFLOW_LEVEL_DONE],
                         "%03u") != NULL,
              "level-done format", lang);
        check(count(game_strs[l][GAME_STR_LEVEL_INTO_DRIVE], '%') == 1 &&
                  strstr(game_strs[l][GAME_STR_LEVEL_INTO_DRIVE], "%c") != NULL,
              "into-drive format", lang);

        /* the dialogs: 7 / 6 / 5 lines (6 / 5 / 4 CRs) in every build */
        check(count(game_strs[l][GAME_STR_MENU_PAUSE_BLOCK], '\r') == 6,
              "pause block lines", lang);
        check(count(game_strs[l][GAME_STR_MENU_GRID_BLOCK], '\r') == 5,
              "grid block lines", lang);
        check(count(game_strs[l][GAME_STR_MENU_TIME_UP_BLOCK], '\r') == 4,
              "time-up block lines", lang);

        /* every in-place field starts on a digit of the shipped width */
        for (unsigned f = 0; f < sizeof field / sizeof field[0]; f++) {
            uint8_t off = game_strs_num_offset[l][field[f].id];

            check(off != GAME_STR_NUM_NONE, "party field offset", lang);
            check(off != GAME_STR_NUM_NONE &&
                      digit_run(game_strs[l][field[f].id], off) ==
                          field[f].width,
                  "party field width", lang);
        }

        /* the map page: seven lines, and the header is a string of its own
         * unless the translation fits the sentence in six (FRE, GER), where
         * the shipped build reuses the seventh line as the header */
        for (int i = 0; i < 7; i++)
            check(game_strs[l][GAME_STR_MAP_CONGRATS_0 + i][0] != '\0',
                  "map line is empty", lang);
        if (lang == GAME_LANG_FRE || lang == GAME_LANG_GER)
            check(strcmp(game_strs[l][GAME_STR_MAP_TOP_TEN],
                         game_strs[l][GAME_STR_MAP_CONGRATS_6]) == 0,
                  "reused map header", lang);

        /* scene 8 draws nothing, the other nine headlines are text */
        check(game_strs[l][GAME_STR_SCENE_HEADLINE_8][0] == '\0',
              "headline 8 is not empty", lang);
        for (int i = 0; i < 10; i++)
            if (i != 8)
                check(game_strs[l][GAME_STR_SCENE_HEADLINE_0 + i][0] != '\0',
                      "headline is empty", lang);

        /* the two caveated names: real file names in the intro\ directory,
         * unused today (see game_strs.h) */
        for (int i = 0; i < 2; i++) {
            const char *n = game_strs[l][GAME_STR_INTRO_LANG_BIT + i];

            check(strncmp(n, "intro\\", 6) == 0 &&
                      strlen(n) > 4 && strcmp(n + strlen(n) - 4, ".bit") == 0,
                  "intro asset name", lang);
        }
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("game_strs: %d languages x %d strings OK\n", GAME_LANG_COUNT,
           GAME_STR_COUNT);
    return 0;
}
