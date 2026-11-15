/* The level module's shared declarations: the start-chain phases, the
 * accessors every translation unit uses for another theme's state, and the
 * helpers that cross a theme boundary.  Nothing here is part of the port's
 * public API; include level.h for that.
 */
#ifndef HUMANSRE_LEVEL_INTERNAL_H
#define HUMANSRE_LEVEL_INTERNAL_H

#include "level/level.h"

#include <stdint.h>

/* The record geometry and the visual sets' extents, shared by the theme
 * translation units. */
#define MAP_LEN   640u
#define MAP_COLS  32u
#define MAP_ROWS  20u
#define REC_HDR   13u
#define SIG_LEN   6u
#define FIELD_LEN 8u

#define WORLD_W     512u
#define WORLD_H     320u
#define ATLAS_TILES 169u

/* level_fade_in (1000:3334) - the level's start chain.  The original blocks
 * in the foreground for each fade (wait_palette_fade_done) while the ISR
 * steps it; the port's loop is tick-driven, so the chain advances one phase
 * per level_tick and the play loop only starts once it is done. */
enum {
    LVSTART_IDLE,
    LVSTART_VIDEO_BASE,
    LVSTART_VIDEO_BAR,
    LVSTART_LOADING,
    LVSTART_FADE_OUT,
    LVSTART_WARMUP,
    LVSTART_FADE_IN,
    LVSTART_PLAYING
};

/* --- shared state, reached through its theme's accessors --------------- */

/** @brief The loaded level's record (always valid once a load began).
 *
 * @return the record
 */
const LevelData *_level_record(void);

/**
 * @brief Whether a level record is loaded.
 *
 * @return 1 when loaded, 0 otherwise
 */
uint8_t _level_is_loaded(void);

/** @brief The tile atlas' palette slice. */
const uint8_t *_level_atlas_palette(void);

/** @brief The background image's own 64-entry palette. */
const uint8_t *_level_bg_palette(void);

/** @brief The BD row block (field I). */
const uint8_t *_level_row_block(void);

/** @brief The palette-rotation ranges (field M).
 *
 * @param count the entry count out-parameter
 * @return the 4-byte entry table, or NULL
 */
uint8_t *_level_anim_ranges(uint16_t *count);

/** @brief The static 512x320 world composite. */
const uint8_t *_level_world(void);

/** @brief The level's start-chain phase (level_fade_in 1000:3334). */
uint8_t _level_phase(void);

/** @brief Arm a fresh start chain. */
void _level_phase_reset(void);

/**
 * @brief Set the level's bar words for a fresh load.
 *
 * @param time the time limit
 * @param members the tribe's member count
 * @param required the map's 0xAA count
 */
void _level_tribe_configure(uint16_t time, uint16_t members, uint16_t required);

/** @brief The play loop's end request. */
uint8_t _level_end_requested(void);

/** @brief 1000:34E6's exit hold: one tick, 1 while it runs. */
uint8_t _level_exit_hold_step(void);

/** @brief The end sound is due (the request is set and not yet played). */
uint8_t _level_end_sound_pending(void);

/** @brief Arm 1000:34E6's sound latch and 15-frame hold. */
void _level_end_hold_start(void);

/* --- shared helpers (defined in their theme's translation unit) -------- */

/** @brief Load the style's BD set, atlas and background, and build the world.
 *
 * @param style the level style
 * @return 0 on success, -1 when an asset is missing
 */
int32_t _level_assets_load(uint32_t style);

/** @brief 1000:5159/0x54A9: stamp every 0xB9 cell's P block into the background. */
void _level_stamp_exits(void);

/** @brief Build the static 512x320 world composite (BG + terrain) once per load. */
void _level_world_build(void);

/** @brief Build the working palette 0x14D3 from the loaded assets. */
void _level_palette_build(void);

/** @brief Step palette_rotate_table (1000:8C78) for one tick. */
void _level_palette_rotate(void);

/** @brief Drop the loading screen's bank and arm the screen for the next load. */
void _level_loading_reset(void);

/** @brief Reset the goal flag, the pan and its follow override (1000:3540). */
void _level_view_reset(void);

/** @brief Reset the end request and the outcome for a fresh level load. */
void _level_tribe_reset(void);

/** @brief Present the shared A000 model into page 0 (the flat compose). */
void _level_page_present(void);

/** @brief Bake page 0 back into the shared A000 model. */
void _level_page_bake(void);

/** @brief 1000:3F4A's per-frame particle pass: restore, spawn, age and draw. */
void _level_spark_frame(void);

/** @brief menu_flag_tick's per-tick head (1000:3540). */
void _level_goal_pan_tick(void);

/** @brief menu_flag_tick's per-tick accounting after the world update. */
void _level_goal_pan_after(void);


#endif /* HUMANSRE_LEVEL_INTERNAL_H */
