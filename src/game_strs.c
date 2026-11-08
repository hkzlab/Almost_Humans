/* game_strs.c — the text table; see game_strs.h.
 *
 * The English column is verbatim from the shipped image, so a pointer here is
 * behaviour-identical to the literal it replaced.
 */
#include "game_strs.h"

const char *const game_strs[GAME_LANG_COUNT][GAME_STR_COUNT] = {
    [GAME_LANG_EN] = {
        /* attract */
        "FASTEST LEVEL TIMES!",
        "LEVEL         MINUTES,    SECONDS",
        "TOP TEN TRIBES",

        /* gameflow */
        "WELL DONE! YOU FINISHED LEVEL %03u",
        "YOUR TRIBE IS EXTINCT!!",

        /* level: the insert-disc page */
        "PUT THE DISK",
        "INTO DRIVE %c",
        "PRESS FIRE",
        "WHEN READY",

        /* map: the qualifying page */
        "WELL DONE!",
        "YOUR TRIBE HAS EVOLVED TO BECOME",
        "ONE OF THE TEN MOST ADVANCED IN",
        "THE ENTIRE WORLD!",
        "THE NAME OF YOUR TRIBE WILL NOW",
        "BE CARVED INTO THE ROCK THAT MARKS",
        "THE TEN MOST CIVILISED TRIBES!",
        "THE TOP TEN TRIBES",

        /* menu */
        "PLEASE SELECT YOUR OPTION\r"
        "\r"
        "CONTINUE THIS LEVEL\r"
        "RESTART THIS LEVEL\r"
        "RESTART THIS GAME\r"
        "SAVE GAME \r"
        "QUIT THIS GAME",
        "SAVE GAME NUMBER\r"
        "\r"
        " 01 05 09 13 17 \r"
        " 02 06 10 14 18 \r"
        " 03 07 11 15 19 \r"
        " 04 08 12 16 20 ",
        "YOU RAN OUT OF TIME!!\r"
        " \r"
        "RESTART THIS LEVEL\r"
        "RESTART THIS GAME\r"
        "QUIT THIS GAME",

        /* party intro */
        "COMING UP.... LEVEL 000",
        "YOU WILL NEED 0 HUMANS TO COMPLETE",
        "THIS LEVEL",
        "YOUR TRIBE NOW HAS 00 MEMBERS!",
        "THE CODE FOR THIS LEVEL IS......",
        "YOUR SCORE IS 0000",
        "PRESS FIRE TO START LEVEL....",
        " MEMBERS!",
        "GAME OVER!",
        "YOUR TRIBE IS TOO SMALL TOO CONTINUE",
        "IN THE RACE FOR EVOLUTION!",
        "PRESS FIRE TO TRY AGAIN!",

        /* scene headlines */
        "HUMANS INVENT WEAPON! THE SPEAR!",
        "HUMANS RESCUE BABY DINOSAUR!",
        "REVOLUTION IN TRANSPORT! THE WHEEL!",
        "BRAVE HUMAN RESCUES THE QUEEN!",
        "SPECIAL REPORT ON NEW DISCOVERY! FIRE!",
        "UNIDENTIFIED FLYING HUMAN SPOTTED!",
        "HUMANS FAIL IN RACE FOR EVOLUTION!",
        "HUMANS WIN THE RACE FOR EVOLUTION!",
        "",
        "HUMANS RECOVER THE STOLEN IDOL!"
    }
};

/* One row per language, one column per string, in the same order as
 * game_strs: the party templates' digit fields sit at fixed byte offsets in
 * the original's string block, and the other strings have no in-place field. */
const uint8_t game_strs_num_offset[GAME_LANG_COUNT][GAME_STR_COUNT] = {
    [GAME_LANG_EN] = {
        /* attract */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* gameflow (printf formats) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* level (INTO DRIVE is a printf format) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE,
        /* map */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* menu */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* party intro: the in-place digit fields (202F:0x105F..) */
        0x14u,             /* COMING UP.... LEVEL nnn                     */
        0x0Eu,             /* YOU WILL NEED n HUMANS TO COMPLETE          */
        GAME_STR_NUM_NONE, /* THIS LEVEL                                  */
        0x13u,             /* YOUR TRIBE NOW HAS nn MEMBERS!              */
        GAME_STR_NUM_NONE, /* THE CODE FOR THIS LEVEL IS......            */
        0x0Eu,             /* YOUR SCORE IS nnnn                          */
        GAME_STR_NUM_NONE, /* PRESS FIRE TO START LEVEL....               */
        GAME_STR_NUM_NONE, /* " MEMBERS!" tail                            */
        GAME_STR_NUM_NONE, /* GAME OVER!                                  */
        GAME_STR_NUM_NONE, /* YOUR TRIBE IS TOO SMALL TOO CONTINUE        */
        GAME_STR_NUM_NONE, /* IN THE RACE FOR EVOLUTION!                  */
        GAME_STR_NUM_NONE, /* PRESS FIRE TO TRY AGAIN!                    */
        /* scene headlines */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE
    }
};
