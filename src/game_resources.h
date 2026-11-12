/* Resource names: the volume directory's chunk names.
 *
 * Every resource the game loads by name comes from `game_resources[id]`; the
 * loaders carry no literals.  The text the player reads is localisable and
 * lives in game_strings.h; these names are not - they are identical in every
 * build.
 *
 * A `_FMT` entry is the snprintf() template for a per-level/-style family;
 * the loader fills its one field.
 */

#ifndef HUMANSRE_GAME_RESOURCES_H
#define HUMANSRE_GAME_RESOURCES_H

enum GameResource {
    /* ANM */
    GAME_RES_ANM_ANI_FMT, // %02u: the intro movie index

    /* APL */
    GAME_RES_APL_ALPHA_SPR,
    GAME_RES_APL_BAND_SPR,
    GAME_RES_APL_BIRDY_SPR,
    GAME_RES_APL_BOX_SPR,
    GAME_RES_APL_CODEB1_SPR,
    GAME_RES_APL_CODEB2_SPR,
    GAME_RES_APL_CODEHUM_SPR,
    GAME_RES_APL_DISKIMG1_DAT,
    GAME_RES_APL_ICONS3_SPR,
    GAME_RES_APL_LOADING_APL,
    GAME_RES_APL_PANELN1_SPR,
    GAME_RES_APL_PANELN2_SPR,
    GAME_RES_APL_PARTY_SPR,
    GAME_RES_APL_RECORD_SPR,
    GAME_RES_APL_RESTORE_SPR,
    GAME_RES_APL_REX_DAT,
    GAME_RES_APL_SCOOPS_APL,
    GAME_RES_APL_SCORE_SPR,
    GAME_RES_APL_SCORENUM_SPR,
    GAME_RES_APL_SPRITES_DAT,
    GAME_RES_APL_SPRPAL_APL,
    GAME_RES_APL_TER_DAT,
    GAME_RES_APL_TEXT_SPR,

    /* BACKS */
    GAME_RES_BACKS_BG_FMT,   // %u: the background index
    GAME_RES_BACKS_ICON_FMT, // %c: the level letter

    /* DEFS */
    GAME_RES_DEFS_BD_FMT, // %u: the level style

    /* INTRO */
    GAME_RES_INTRO_APPLE_SPR,
    GAME_RES_INTRO_BAC_BIT,
    GAME_RES_INTRO_FIRST_BIT,
    GAME_RES_INTRO_GAMETEK_RSC,
    GAME_RES_INTRO_IDI_BIT,
    GAME_RES_INTRO_INTRO_RSC,
    GAME_RES_INTRO_ITB_BIT,
    GAME_RES_INTRO_TREEBOT_BIT,
    GAME_RES_INTRO_TREETOP_BIT,
    GAME_RES_INTRO_WORDS_BIT,

    /* LBM */
    GAME_RES_LBM_CODE2_RAW,
    GAME_RES_LBM_DAILY_BIT,
    GAME_RES_LBM_GAMEINT_BIT,
    GAME_RES_LBM_HIGH_BIT,
    GAME_RES_LBM_INTCRED4_BIT,
    GAME_RES_LBM_PANEL4_BIT,
    GAME_RES_LBM_PARTY1_BIT,
    GAME_RES_LBM_PARTY_PAL_FMT, // %u: the party disc number
    GAME_RES_LBM_PCTITLE_RAW,
    GAME_RES_LBM_SCORE_BIT,
    GAME_RES_LBM_SCOREPAL_PAL,

    /* LEVELS */
    GAME_RES_LEVELS_BS_FMT, // %u: the level style
    GAME_RES_LEVELS_CODES_BIN,
    GAME_RES_LEVELS_L_FMT, // %c: the level file letter

    /* root */
    GAME_RES_CREDTEXT_BIN,

    GAME_RES_COUNT
};

/* The chunk name of one resource, as the volume directory stores it. */
extern const char *const game_resources[GAME_RES_COUNT];

#endif /* HUMANSRE_GAME_RESOURCES_H */
