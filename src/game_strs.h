/* game_strs.h - the localisable text of the game core.
 *
 * `game_strs[language][id]` is the only source of displayed text: the first
 * index picks the language, the second the string a routine needs. 
 * Adding a language means filling a row in `game_strs` `and game_strs_num_offset`
 * used to overwrite some chars in the drawn strings.
 *
 * Every column is taken byte-for-byte from that language's shipped image
 * (`202F:0x5F..`, `0x7BB..`, `0x105F..`, ... per build).
 *
 * Resource and file names (APL\TEXT.SPR, mus\title, ...) are not here: they
 * name bytes on the discs, not text the player reads, and they are the same
 * in every build.  The exception is the pair of intro asset names that do
 * change per build; they sit at the end of the enum, marked unused.
 */

#ifndef HUMANSRE_GAME_STRS_H
#define HUMANSRE_GAME_STRS_H

#include <stdint.h>

enum GameLang {
    GAME_LANG_EN = 0, // English
    GAME_LANG_FRE,    // French
    GAME_LANG_GER,    // German
    GAME_LANG_ITA,    // Italian
    GAME_LANG_SPA,    // Spanish
    GAME_LANG_COUNT
};

enum GameStr {
    /* attract */
    GAME_STR_ATTRACT_TIMES_HEADER = 0,
    GAME_STR_ATTRACT_TIMES_LABEL,
    GAME_STR_ATTRACT_SCORE_HEADER,

    /* gameflow */
    GAME_STR_GAMEFLOW_LEVEL_DONE, // snprintf format, one %03u
    GAME_STR_GAMEFLOW_EXTINCT,

    /* level: the insert-disc page (not localised in any build) */
    GAME_STR_LEVEL_PUT_DISK,
    GAME_STR_LEVEL_INTO_DRIVE, // snprintf format, one %c
    GAME_STR_LEVEL_PRESS_FIRE,
    GAME_STR_LEVEL_WHEN_READY,

    /* map: the qualifying page's seven lines, then the top-ten header.
     * FRE and GER fit the sentence in six lines, so their seventh line is
     * their header string - that is what `menu_loop` draws (1000:0252). */
    GAME_STR_MAP_CONGRATS_0,
    GAME_STR_MAP_CONGRATS_1,
    GAME_STR_MAP_CONGRATS_2,
    GAME_STR_MAP_CONGRATS_3,
    GAME_STR_MAP_CONGRATS_4,
    GAME_STR_MAP_CONGRATS_5,
    GAME_STR_MAP_CONGRATS_6,
    GAME_STR_MAP_TOP_TEN,

    /* menu: the three dialogs, each a CR-separated block */
    GAME_STR_MENU_PAUSE_BLOCK,
    GAME_STR_MENU_GRID_BLOCK,
    GAME_STR_MENU_TIME_UP_BLOCK,

    /* party intro: the kept buffers and the GAME OVER variant */
    GAME_STR_PARTY_COMING_UP,
    GAME_STR_PARTY_NEED,
    GAME_STR_PARTY_THIS_LEVEL,
    GAME_STR_PARTY_HAS,
    GAME_STR_PARTY_CODE,
    GAME_STR_PARTY_SCORE,
    GAME_STR_PARTY_FIRE,
    GAME_STR_PARTY_MEMBERS_TAIL, // appended after the member count
    GAME_STR_PARTY_GAME_OVER,
    GAME_STR_PARTY_TOO_SMALL,
    GAME_STR_PARTY_IN_RACE,
    GAME_STR_PARTY_TRY_AGAIN,

    /* scene: the newspaper headlines, scene index N picks N */
    GAME_STR_SCENE_HEADLINE_0,
    GAME_STR_SCENE_HEADLINE_1,
    GAME_STR_SCENE_HEADLINE_2,
    GAME_STR_SCENE_HEADLINE_3,
    GAME_STR_SCENE_HEADLINE_4,
    GAME_STR_SCENE_HEADLINE_5,
    GAME_STR_SCENE_HEADLINE_6,
    GAME_STR_SCENE_HEADLINE_7,
    GAME_STR_SCENE_HEADLINE_8, // the empty one: scene 8 draws nothing
    GAME_STR_SCENE_HEADLINE_9,

    /* The two intro asset names that change per build.  NOTHING LOADS THEM
     * TODAY: the language bitmap's only loader site (ENG `1000:BDF5`) sits
     * inside `intro_flags_screen` (`1000:BDD8..BE38`), which has no caller
     * in the database and never fires in the documented runtime capture -
     * and a fresh analysis of the translated builds creates no function
     * around their site at all.  The protection bitmap never reaches a
     * loader (in ENG its name appears only as a `MOV CX` operand inside
     * `save_progress`, not as a request), and no volume ships either file:
     * all five carry `INTRO\GAMEPROT.BIT`.  They are here because they are
     * the only file names that localise, so the table is complete for a
     * build that ever reaches them. Supposedly, they're used in the floppy
     * versions of the game. */
    GAME_STR_INTRO_LANG_BIT,
    GAME_STR_INTRO_PROT_BIT,

    GAME_STR_COUNT
};

/* The active column.  ver_detect_run() stores the language of the installed
 * volume set here at boot; every consumer indexes the tables below with it,
 * so a call site never names a language. */
extern uint32_t g_game_lang;

extern const char *const game_strs[GAME_LANG_COUNT][GAME_STR_COUNT];

/* Byte offset of each string's in-place digit field, parallel to game_strs:
 * the briefing's `_put_number` calls patch digits into a copied template, and
 * a translation may move the field, so the offset is language data too.
 * GAME_STR_NUM_NONE marks a string with no in-place field (printed verbatim,
 * or formatted through printf into a scratch buffer). */
#define GAME_STR_NUM_NONE 0xFFu

extern const uint8_t game_strs_num_offset[GAME_LANG_COUNT][GAME_STR_COUNT];

#endif
