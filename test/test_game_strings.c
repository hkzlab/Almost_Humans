/* test_game_strings.c - the text table's invariants across every language column.
 *
 * The text itself is verified against the shipped images, column by column;
 * this test guards the structure the port's call sites rely on: the printf
 * markers, the in-place digit fields, the dialog line counts, and the two
 * columns whose seventh map line is their header string.
 */
#include "game_strings.h"
#include "test_support.h"

#include <stdint.h>
#include <string.h>

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

/**
 * @brief Run the game-string table checks.
 */
static void test_game_strings(void)
{
    /* the four party templates the briefing patches in place, with the width
     * of the digit field the shipped template carries */
    static const struct {
        enum GameString id;
        unsigned width;
    } field[] = {
        { GAME_STRING_PARTY_COMING_UP, 3 },
        { GAME_STRING_PARTY_NEED, 1 },
        { GAME_STRING_PARTY_HAS, 2 },
        { GAME_STRING_PARTY_SCORE, 11 },
    };

    for (int l = 0; l < GAME_LANGUAGE_COUNT; l++) {
        enum GameLanguage lang = (enum GameLanguage)l;

        game_language_set(lang);
        for (int s = 0; s < GAME_STRING_COUNT; s++)
            TEST_ASSERT_TRUE_MESSAGE(game_string((enum GameString)s) != NULL,
                                     "string is NULL");

        /* the two printf formats keep exactly one conversion each */
        TEST_ASSERT_TRUE_MESSAGE(
            count(game_string(GAME_STRING_GAMEFLOW_LEVEL_DONE), '%') == 1 &&
                strstr(game_string(GAME_STRING_GAMEFLOW_LEVEL_DONE), "%03u") !=
                    NULL,
            "level-done format");
        TEST_ASSERT_TRUE_MESSAGE(
            count(game_string(GAME_STRING_LEVEL_INTO_DRIVE), '%') == 1 &&
                strstr(game_string(GAME_STRING_LEVEL_INTO_DRIVE), "%c") != NULL,
            "into-drive format");

        /* the dialogs: 7 / 6 / 5 lines (6 / 5 / 4 CRs) in every build */
        TEST_ASSERT_TRUE_MESSAGE(
            count(game_string(GAME_STRING_MENU_PAUSE_BLOCK), '\r') == 6,
            "pause block lines");
        TEST_ASSERT_TRUE_MESSAGE(
            count(game_string(GAME_STRING_MENU_GRID_BLOCK), '\r') == 5,
            "grid block lines");
        TEST_ASSERT_TRUE_MESSAGE(
            count(game_string(GAME_STRING_MENU_TIME_UP_BLOCK), '\r') == 4,
            "time-up block lines");

        /* every in-place field starts on a digit of the shipped width */
        for (unsigned f = 0; f < sizeof field / sizeof field[0]; f++) {
            uint8_t off = game_string_num_offset(field[f].id);

            TEST_ASSERT_TRUE_MESSAGE(off != GAME_STRING_NUM_NONE,
                                     "party field offset");
            TEST_ASSERT_TRUE_MESSAGE(
                off != GAME_STRING_NUM_NONE &&
                    digit_run(game_string(field[f].id), off) == field[f].width,
                "party field width");
        }

        /* the map page: seven lines, and the header is a string of its own
         * unless the translation fits the sentence in six (FRE, GER), where
         * the shipped build reuses the seventh line as the header */
        for (int i = 0; i < 7; i++)
            TEST_ASSERT_TRUE_MESSAGE(game_string((enum GameString)(
                                         GAME_STRING_MAP_CONGRATS_0 + i))[0] !=
                                         '\0',
                                     "map line is empty");
        if (lang == GAME_LANGUAGE_FRE || lang == GAME_LANGUAGE_GER)
            TEST_ASSERT_TRUE_MESSAGE(
                strcmp(game_string(GAME_STRING_MAP_TOP_TEN),
                       game_string(GAME_STRING_MAP_CONGRATS_6)) == 0,
                "reused map header");

        /* scene 8 draws nothing, the other nine headlines are text */
        TEST_ASSERT_TRUE_MESSAGE(game_string(GAME_STRING_SCENE_HEADLINE_8)[0] ==
                                     '\0',
                                 "headline 8 is not empty");
        for (int i = 0; i < 10; i++)
            if (i != 8)
                TEST_ASSERT_TRUE_MESSAGE(
                    game_string((enum GameString)(GAME_STRING_SCENE_HEADLINE_0 +
                                                  i))[0] != '\0',
                    "headline is empty");

        /* the two caveated names: real file names in the intro\ directory,
         * unused today (see game_strings.h) */
        for (int i = 0; i < 2; i++) {
            const char *n =
                game_string((enum GameString)(GAME_STRING_INTRO_LANG_BIT + i));

            TEST_ASSERT_TRUE_MESSAGE(strncmp(n, "intro\\", 6) == 0 &&
                                         strlen(n) > 4 &&
                                         strcmp(n + strlen(n) - 4, ".bit") == 0,
                                     "intro asset name");
        }
    }
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_game_strings);
    return UNITY_END();
}
