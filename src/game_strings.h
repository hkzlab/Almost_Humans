/* Text: the localisable strings, one column per shipped build.
 *
 * `game_string(id)` is the only source of displayed text; adding a
 * language means adding its column to the two tables in game_strings.c.
 * Every column is byte-for-byte from that build's image (202F:0x5F..,
 * 0x7BB.., 0x105F.., ...).
 *
 * Resource and file names stay out: they name bytes on the disc, not text the
 * player reads, and are identical in every build.  The two per-build intro
 * asset names sit at the end of the enum, marked unused.
 */

#ifndef HUMANSRE_GAME_STRINGS_H
#define HUMANSRE_GAME_STRINGS_H

#include <stdint.h>

enum GameLanguage {
    GAME_LANGUAGE_EN = 0, // English
    GAME_LANGUAGE_FRE,    // French
    GAME_LANGUAGE_GER,    // German
    GAME_LANGUAGE_ITA,    // Italian
    GAME_LANGUAGE_SPA,    // Spanish
    GAME_LANGUAGE_COUNT
};

enum GameString {
    /* attract */
    GAME_STRING_ATTRACT_TIMES_HEADER = 0,
    GAME_STRING_ATTRACT_TIMES_LABEL,
    GAME_STRING_ATTRACT_SCORE_HEADER,

    /* gameflow */
    GAME_STRING_GAMEFLOW_LEVEL_DONE, // snprintf format, one %03u
    GAME_STRING_GAMEFLOW_EXTINCT,

    /* level: the insert-disc page (not localised in any build) */
    GAME_STRING_LEVEL_PUT_DISK,
    GAME_STRING_LEVEL_INTO_DRIVE, // snprintf format, one %c
    GAME_STRING_LEVEL_PRESS_FIRE,
    GAME_STRING_LEVEL_WHEN_READY,

    /* map: the qualifying page's seven lines, then the top-ten header.
     * FRE and GER fit the sentence in six lines, so their seventh line is
     * their header string - that is what `menu_loop` draws (1000:0252). */
    GAME_STRING_MAP_CONGRATS_0,
    GAME_STRING_MAP_CONGRATS_1,
    GAME_STRING_MAP_CONGRATS_2,
    GAME_STRING_MAP_CONGRATS_3,
    GAME_STRING_MAP_CONGRATS_4,
    GAME_STRING_MAP_CONGRATS_5,
    GAME_STRING_MAP_CONGRATS_6,
    GAME_STRING_MAP_TOP_TEN,

    /* menu: the three dialogs, each a CR-separated block */
    GAME_STRING_MENU_PAUSE_BLOCK,
    GAME_STRING_MENU_GRID_BLOCK,
    GAME_STRING_MENU_TIME_UP_BLOCK,

    /* party intro: the kept buffers and the GAME OVER variant */
    GAME_STRING_PARTY_COMING_UP,
    GAME_STRING_PARTY_NEED,
    GAME_STRING_PARTY_THIS_LEVEL,
    GAME_STRING_PARTY_HAS,
    GAME_STRING_PARTY_CODE,
    GAME_STRING_PARTY_SCORE,
    GAME_STRING_PARTY_FIRE,
    GAME_STRING_PARTY_MEMBERS_TAIL, // appended after the member count
    GAME_STRING_PARTY_GAME_OVER,
    GAME_STRING_PARTY_TOO_SMALL,
    GAME_STRING_PARTY_IN_RACE,
    GAME_STRING_PARTY_TRY_AGAIN,

    /* scene: the newspaper headlines, scene index N picks N */
    GAME_STRING_SCENE_HEADLINE_0,
    GAME_STRING_SCENE_HEADLINE_1,
    GAME_STRING_SCENE_HEADLINE_2,
    GAME_STRING_SCENE_HEADLINE_3,
    GAME_STRING_SCENE_HEADLINE_4,
    GAME_STRING_SCENE_HEADLINE_5,
    GAME_STRING_SCENE_HEADLINE_6,
    GAME_STRING_SCENE_HEADLINE_7,
    GAME_STRING_SCENE_HEADLINE_8, // the empty one: scene 8 draws nothing
    GAME_STRING_SCENE_HEADLINE_9,

    /* The two intro asset names that change per build.  Nothing loads them:
     * the language bitmap's only loader site (ENG 1000:BDF5) is inside
     * intro_flags_screen (1000:BDD8..BE38), which has no caller, and no
     * volume ships either file (all five carry INTRO\GAMEPROT.BIT).  They are
     * kept because they are the only file names that localise. */
    GAME_STRING_INTRO_LANG_BIT,
    GAME_STRING_INTRO_PROT_BIT,

    GAME_STRING_COUNT
};

/* The active column: ver_detect_run() stores the language of the installed
 * volume set at boot; every consumer reads it through game_language(), so a
 * call site never names a language. */
enum GameLanguage game_language(void);

/**
 * @brief Select the text column.
 *
 * @param lang the language
 */
void game_language_set(enum GameLanguage lang);

/**
 * @brief One string in the active column.
 *
 * @param id the string
 * @return the text, or NULL for an id out of range
 */
const char *game_string(enum GameString id);

/* Byte offset of each string's in-place digit field, parallel to the text
 * above: the briefing's `_put_number` calls patch digits into a copied
 * template, and a translation may move the field, so the offset is language
 * data too.  GAME_STRING_NUM_NONE marks a string with no in-place field
 * (printed verbatim, or formatted through printf into a scratch buffer). */
#define GAME_STRING_NUM_NONE 0xFFu

/**
 * @brief The in-place digit field's byte offset for one active-column string.
 *
 * @param id the string
 * @return the byte offset, GAME_STRING_NUM_NONE when the string has no field
 */
uint8_t game_string_num_offset(enum GameString id);

#endif
