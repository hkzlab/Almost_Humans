/* level.h - the interface of src/level.c: the level definitions (subsystem 9, §2)
 *
 * LEVELS\LA.BIN..LP.BIN: 16 files x 10 records = 160 levels.  The record:
 * the 13-byte header, the goal/author ASCIIZ text, the 8-byte editor
 * metadata, the 640-byte tile map (32x20).
 */

#ifndef HUMANSRE_LEVEL_H
#define HUMANSRE_LEVEL_H

#include "osystem.h"

typedef struct {
    uint16_t levelId; /* 0..159                                    */
    uint8_t style; /* +0x00 low: the tile style 1..6            */
    uint8_t introIndex; /* +0x01 high: the intro-movie index (0xFF=none) */
    uint8_t goalCount; /* +0x02 low: the solved-flag threshold      */
    uint8_t entityLife; /* +0x02 high: the entity lifetime seed      */
    uint16_t objectTest; /* +0x04: the object-test mode word          */
    uint8_t unused06; /* +0x06: 0 in all 160 shipped records       */
    uint16_t timeA; /* +0x07: the BCD MMSS limit (difficulty A)  */
    uint16_t timeB; /* +0x09: difficulty B                       */
    uint16_t timeC; /* +0x0B: difficulty C                       */
    uint8_t map[640]; /* 32 columns x 20 rows, row-major           */
    uint32_t mapFileOff; /* the map's offset inside the L*.BIN image  */
    char goal[128]; /* the goal text (CR = a line break)         */
    char author[128]; /* the author text                           */
} LevelData;

/**
 * @brief Bind the level module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t level_init(const OSystem *osys);

/**
 * @brief Load one level record and its terrain from the volume (0..159; 0 = ok).
 *
 * @param levelId the level id
 * @return 0 on success, -1 on failure
 */
int32_t level_load(uint16_t levelId);

/**
 * @brief Accessor: the loaded LevelData record (NULL before a load).
 *
 * @return The loaded LevelData record.
 */
const LevelData *level_data(void);

/**
 * @brief Accessor: the 640-cell map buffer (== level_data()->map).
 *
 * @return The 640-cell map buffer.
 */
const uint8_t *level_map(void);

/**
 * @brief The record's style byte (header +0, `[0x17EE]`): 1000:5662 tests it against 6.
 *
 * @return the level style
 */
uint32_t level_style(void);

/**
 * @brief the level's working palette 202F:0x14D3 (_level_pal above)
 *
 * the working palette 202F:0x14D3 (the fade target of the claim, the level's
 * own fades and the rotate hook)
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *level_palette(void);

/** @brief check_highscore's palette prep (1000:4287/0x428A): seal the working
 * palette's entries 0x80..0xDF to grey (`0x43BC` at `0x1653` = entry 0x80,
 * CX = 0x60), then copy the score card's own 64 entries over 0xC0..0xFF
 * (`0x4403`'s `0xFA00` -> `0x1713`, `0xC0` bytes).
 *
 * @param cardPalette the card's 192 palette bytes, or NULL
 */
void level_palette_score_prepare(const uint8_t *cardPalette);

/** @brief draw_level_text 0x33E2 + 0x33F2: the level palette's 0..0x7F becomes grey
 * (seal_triples), then the shipped gradient fills 0x80..0xBF.  The 0x33FD
 * fade targets this palette.
 *
 * draw_level_text 0x33E2/0x33F2: seal the level palette's 0..0x7F to grey and
 * copy the shipped gradient into 0x80..0xBF (the 0x14D3 buffer).
 */
void level_end_palette(void);

/** @brief draw_level_text 0x344C/0x3450: blit_bitmap_planes + render_objects render
 * the _world into the page after the caption has been dismissed, so the page
 * behind check_highscore's card is the _world, not the caption.
 *
 * draw_level_text 0x344C/0x3450: re-render the world page (blit_bitmap_planes
 * + render_objects), erasing the caption before check_highscore's card.
 */
void level_redraw_page(void);

/** @brief draw_level_text 1000:3437: the end page clamps [0x2] to 0x78 before its
 * two page frames; the clamped value persists in the original's view.
 */
void level_end_redraw_page(void);

/** @brief 1000:3D5E: the claim's pan frames recolour every page byte below 0x80 to
 * 0x1E before the present (the original's four plane passes are one flat
 * page here). The planar shadow is recoloured too: it is what the spark's
 * strip save and the frozen frames compose from.
 */
void level_claim_recolour(void);

/** @brief the claim's frozen page (1000:3D04 -> 1000:3CB5): the original draws
 * the world once, recolours it black on the pan, and then only presents the
 * spark strip plus the selected/claimed records.  This is that frame in the
 * flat model: the spark step, the plane->page present, 1000:40CA's two records
 * during the animation, and the bar (which lives outside the recoloured page).
 *
 * the claim's frozen page: 1000:40CA (the two records) + 1000:3F4A (the spark)
 * over the recoloured page, never a world re-compose.
 */
void level_claim_frame(void);

/** @brief page_offset_to_ptr (1000:F123): 0 when the tile id's 16-byte page mask is
 * all-negative (every sub-cell empty) AND its 16-word item record sums to 0
 * (16-bit wrap, as DX).  The caller (op 3) bails on a non-zero return.
 *
 * @param id the identifier
 * @return 1 when the tile's page bytes are free, 0 otherwise
 */
uint8_t level_page_free(uint32_t id);

/* The two level maps the walk reads: `0x92A0`'s page bytes (bit 7 = solid, the
 * low 7 bits the step, 1000:E389) and `[0x1FD6]`'s 16-word item records
 * (1000:F25F's occupancy bit). */

/** @brief Field K, the level's four special tile ids: `[0x6F20]`/`[0x6F21]` (the walk
 * states compare the cell under the feet with them) and the second word
 * `[0x6F0B]`/`[0x6F0C]` (`1000:4E99`, which the fall machine's `1000:F7CD`
 * type compare indexes with `type - 0x19`, and `1000:E30E` reads directly).
 *
 * Field K, both words: i = 0/1 are the first word's two tile ids
 * ([0x6F20]/[0x6F21]), i = 2/3 the second word's ([0x6F0B]/[0x6F0C]).
 *
 * @param i the index
 * @return the special tile id, or 0xFFFF when out of range
 */
uint32_t level_special_tile(uint32_t i);

/** @brief The `0x92A0` page mask's byte for a tile id and sub-column: bit 7 = solid,
 * the low 7 bits are the step 1000:E389 follows.  `level_page_free` reads the
 * same table for op 3.
 *
 * @param id the identifier
 * @param subcol the sub-column
 * @return the page mask byte
 */
uint32_t level_page_byte(uint32_t id, uint32_t subcol);

/**
 * @brief The map cell's item word (tile row and sub-column).
 *
 * @param id the identifier
 * @param row the row
 * @return the item word, or 0xFFFF when out of range
 */
uint32_t level_item_word(uint32_t id, uint32_t row);

/** @brief The item record's raw byte at `off` from the tile's offset, i.e. from field
 * B.  (`1000:EC27` reads field A, the slope profiles, through
 * `level_slope_byte`; this accessor is the item record's own field-B read.)
 *
 * @param id the identifier
 * @param off the offset
 * @return the item byte, or 0x80 when out of range
 */
uint32_t level_item_byte(uint32_t id, uint32_t off);

/**
 * @brief One play frame: update the world, render and pump the effects.
 *
 * @return 1 while the level is running
 */
uint8_t level_tick(void);

/** @brief [0x0]/[0x2]: the display window into the 512x320 level page, in pixels and
 * rows - `set_display_ptr` (1000:882D) starts the CRTC at
 * `pagebase + 0x58*[0x2] + [0x0]/4`, and the object pass subtracts the same
 * pair (`draw_object_scaled_x1` 1000:66EC clips `x - [0x0]` against
 * [-0x10,0x140]).  The two stores are level_reset_view's (`1000:319D`), and in
 * play `level_follow_view` below is their only writer.
 *
 * 1000:3DC0: the view's world origin (the CRTC start's source).
 *
 * @param x the x coordinate
 * @param y the y coordinate
 */
void level_set_view(uint16_t x, uint16_t y);

/** @brief 1000:5CB6 - the first call of `update_world` (`1000:80AD`), guarded there by
 * `[0xC] != 0`: the camera follows the **acting record** (`get_object_ptr`,
 * `1000:79F4`, walks the 29-word table at `0x3E24` with `[0x3E22]`, i.e.
 * `object_slot(object_action_entity())`), or the record `[0x2D8C]` names while
 * menu_flag_tick's goal pan holds it (`1000:5CB9..5CC0`).
 *
 *   target = record + the offset pair at `[0x2D90]` - (0x88, 0x44)
 *   clamped to the pan range: 0xC0 = 512-320 px, 0xA8 = 320-152 rows (the
 *   play area; the bar owns the last 40 rows), then each axis moves at most
 *   `[0x2DA6]` = 4 px per frame (0x5D09 / 0x5D21), stored at 0x5D2F/0x5D33.
 *
 * The offset pair is **{0,0} at rest**: `[0x2D90]` = 0x2D92 (the first pair of
 * the 0x2D92..0x2DA2 table) and `[0x2D8E]` = 0xFFFF, whose bit 15 idles the
 * 4-entry +-1 px walk at 0x5D37.  Both enablers (`0xDAC7` the entity path and
 * `0xF8CE` the fall machine) are ported; _level_offset_walk advances the pair.
 * `update_view` (1000:80F1) then scrolls the page by the difference; the flat
 * compose reads the window directly, which is the same visible result.
 */
void level_follow_view(void);

/** @brief [0xC]: the claim clears it at its start (1000:3B19) and restores it before
 * the closing page frames (1000:3CAB).
 *
 * [0xC]: update_world's gate (set by level_fade_in 1000:336C, cleared by the
 * claim at 1000:3B19 and restored at 1000:3CAB).
 *
 * @param on the on/off flag
 */
void level_set_updates(uint8_t on);

/** [0x2D8E]: the follower's +-1 px offset walk enabler (1000:DAC7 / 0xF8CE). */
void level_set_offset_word(uint16_t v);

/**
 * @brief Accessor: the record's offset word.
 *
 * @return The record's offset word.
 */
uint16_t level_offset_word(void);

/**
 * @brief test seam: the walk's pointer back to the table's first pair
 *
 * 1000:5D37's walk pointer ([0x2D90]) back to the table's first pair.
 */
void level_reset_offset_walk(void);

/**
 * @brief Accessor: the world-update gate ([0xC]).
 *
 * @return The world-update gate.
 */
uint8_t level_updates(void);

/**
 * @brief Accessor: the camera's x.
 *
 * @return The camera's x.
 */
uint16_t level_view_x(void);

/**
 * @brief Accessor: the camera's y.
 *
 * @return The camera's y.
 */
uint16_t level_view_y(void);

/* menu_flag_tick's goal pan is blocked (the original's pan loop): a test seam
 * for the input latch/marker-mode sequence. */
uint8_t level_goal_pan_running_for_probe(void);

/**
 * @brief 1000:3E73's tail (`JMP 0x3DC0` at (x - 0x20, y - 0x20)): clear the
 * list, take the shade table and the spark origin, and save the strip.
 *
 * The claim's particles (1000:3E73/0x3FB3/0x3F4A): begin at the appended
 * record with the 16x16 shade table object.c builds from the recipe sprite.
 *
 * @param ox the spark origin x
 * @param oy the spark origin y
 * @param shade the 16x16 shade table
 */
void level_spark_begin(uint16_t ox, uint16_t oy, const uint8_t *shade);

/**
 * @brief Accessor: the spark effect is running.
 *
 * @return The spark effect is running.
 */
uint8_t level_spark_active(void);

/**
 * @brief Test seam: one byte of 1000:3DC0's scratch strip.
 *
 * @param col the strip address column (0..0x11)
 * @param row the strip row (0..0x3F)
 * @param plane the plane byte (0..3)
 * @return the byte, or 0 out of range
 */
uint8_t level_spark_strip_at(uint32_t col, uint32_t row, uint32_t plane);

/**
 * @brief the first live slot, for the tests: writes its _world position and value.
 *
 * @param x the x coordinate
 * @param y the y coordinate
 * @param value the value
 * @return 1 when a live spark slot was found, 0 otherwise
 */
int32_t level_spark_peek(uint32_t *x, uint32_t *y, uint32_t *value);

/**
 * @brief the same queue bytes the original keeps at 0x48BC, for object.c's DS window
 *
 * the 0x48BC queue as the original's 0x2C0-byte window, for object.c's DS
 * record model (offset 0 = 0x48BC).
 *
 * @param off the offset
 * @return the spark byte
 */
uint8_t level_spark_byte(uint32_t off);

/**
 * @brief Test seam: poke one spark buffer byte.
 *
 * @param off the offset
 * @param value the value
 */
void level_spark_set_byte(uint32_t off, uint8_t value);

/** @brief The play loop's input (1000:8526's output; the backend already produces the
 * same shape in InputState.joyBits).  Kept out of level_tick's signature so the
 * front-end screens call it the same way as the original's shared sampler.
 *
 * 1000:8526's [0x5ACE] for the play loop's command dispatch (1000:E3F6).
 *
 * @param in the input state
 */
void level_note_input(const InputState *in);

/**
 * @brief level_fade_in: start the level's fade and first play frame.
 *
 * level_fade_in (1000:3334): the start chain - fade out of the pre-level
 * screen, the eight update_world frames, the fade in to the working palette,
 * then the bar's ticking gate.
 * level_begin arms it once; the phases advance in level_tick, so
 * level_starting() is true until the level is playable.
 */
void level_begin(void);

/** @brief still inside level_fade_in's chain?  game_main holds the goal page and the
 * play loop until it is done, exactly like the original's foreground block.
 *
 * @return 1 while the fade-in chain runs, 0 otherwise
 */
uint8_t level_starting(void);

/** @brief level_run (1000:3054) runs level_video_init's two fades (3083) BEFORE
 * load_sprite_pal (3086), so the loading screen exists only once the chain has
 * poured the palette to black.
 *
 * load_sprite_pal's LOADING screen (1000:1DC0): ten walkers for 0xA0 frames,
 * then the "LOADING!" letters for 0x56.
 * level_loading_screen() starts it and reports whether it is showing;
 * level_loading_tick draws one frame.
 *
 * @return 1 while the loading screen is up, 0 otherwise
 */
uint8_t level_loading_screen(void);

/** @brief particle_frame (1000:1E78) + update_particles (1000:1E98): one frame of
 * the list - the counter reload, the per-entry step and sprite cycle - then
 * the draw and the present.
 */
void level_loading_tick(void);

/**
 * @brief --- level_run's end and the entry state machine's spine --------------
 *
 * level_run 3060/3062: the level's required humans ([0x17FB], the map's 0xAA
 * count) against the tribe ([0x11FC]).
 * A tribe that is too small plays the briefing's GAME OVER variant and the
 * level is SKIPPED; [0x1809] then takes entry 1000:00DC -> 002B (the
 * restart-game path), not the next level.
 *
 * @return the members the level needs
 */
uint16_t level_required(void);

/**
 * @brief [0x17F0]: the level header's +0x02 low byte, the goal scan's threshold.
 *
 * @return the level's goal count
 */
uint32_t level_goal_count(void);

/**
 * @brief Accessor: the tribe's current member count.
 *
 * @return The tribe's current member count.
 */
uint16_t level_members(void);

/**
 * @brief 1000:DE1F: the join tail increments the tribe count ([0x11FC]).
 */
void level_add_member(void);

/**
 * @brief 1000:41E6 (check_highscore): store the tribe counter + live-record recount
 * into [0x11FC].
 *
 * The only per-level write besides the difficulty pick/restore/join; the
 * port's earlier per-death decrement is not in the image.
 *
 * @param n the recounted member count
 */
void level_set_members(uint32_t n);

/** @brief level_restore (1000:35AF): the members go back to the level's own start
 * value, then level_start re-picks the time (the port re-runs level_load, so
 * this is called after it).
 *
 * level_restore (1000:35AF): [0x11FC] := [0x1970] - the members the level
 * started with, before the level's own joins (level_start 1000:3032 then
 * re-picks the time; the port re-runs level_load, so the snapshot is put back
 * afterwards).
 */
void level_restore_members(void);

/**
 * @brief The level's end request with the win/lose outcome.
 *
 * level_run's end: the play loop exits when object_test_dispatch (1000:35D6)
 * returns negative, and [0x138E] (0xFF won / 0 extinct) then selects
 * draw_level_text's page.
 * The six test handlers need the placed- object array (0x3E75), which object.c
 * holds; level_end_test evaluates the six predicates through
 * object_test_dispatch.
 * level_request_end(1) = the level was completed.
 *
 * @param won the won flag
 */
void level_request_end(uint8_t won);

/**
 * @brief Start the disc-presence indicator for one disc.
 *
 * draw_status_indicator (1000:1FEF): the disc probe + the "PUT THE DISK INTO
 * DRIVE n / PRESS FIRE WHEN READY" screen.
 * Begin returns 0 when the volume is present (the normal, no-screen path);
 * Tick returns 1 while the screen is up.
 *
 * @param disc the disc number
 * @return 1 when the prompt is up, 0 when the disc is present
 */
uint8_t level_disk_status_begin(uint16_t disc);

/**
 * @brief Tick the disc indicator; 1 = still waiting.
 *
 * @param in the input state
 * @return 1 while the prompt is still up
 */
uint8_t level_disk_status_tick(const InputState *in);

/**
 * @brief Clear the disc indicator's state (test/lifecycle: drop the screen).
 */
void level_disk_status_reset(void);

/** @brief the level-end test: object_test_dispatch (1000:35D6) returning negative
 * ends the play loop; [0x138E] stays 0xFF for a win and is cleared for the
 * extinct/time-out paths.  The six handlers all exist now: object_test_dispatch
 * maps the level's own mode word onto them (the array predicates, the entity
 * table and the two data conditions this file owns - the goal hook and the
 * tribe count).  level_end_test runs from level_tick's play phase, like the
 * original's call in input_dispatch_loop.
 *
 * level_goal_reached is the win hook the discovery/claim path sets; a lost
 * tribe member decrements the live count and ends the level at zero.
 */
void level_goal_reached(void);

/**
 * @brief One tribe member was lost: decrement the counter.
 */
void level_member_lost(void);

/**
 * @brief The level-end test (object_test_dispatch's negative return).
 *
 * @return 1 when the level-end test fires, 0 otherwise
 */
uint8_t level_end_test(void);

/**
 * @brief Accessor: the play loop's end reached and its hold finished.
 *
 * @return The play loop's end reached and its hold finished.
 */
uint8_t level_ended(void);

/** @brief 1000:34E6: the play loop's exit - after the end test's negative return the
 * loop plays sound 0xC (1000:83B7) and waits 15 vsyncs (1000:8B56) on the
 * frozen _world before returning to draw_level_text.
 *
 * @return 1 while the exit hold runs, 0 otherwise
 */
uint8_t level_end_holding(void);

/** @brief 1000:F66D: the goal-record tail's "no survivor" exit sets [0x138E] = 0 and
 * ends the play loop (the same state object_test_dispatch's negative return
 * produces, reached from the object machine instead).
 *
 * @param won the won flag
 */
void level_set_end(uint8_t won);

/**
 * @brief Accessor: 1 when the level was won ([0x138E]: 1 won, 0 extinct).
 *
 * @return 1 when the level was won.
 */
uint8_t level_outcome(void);

/** @brief [0x5D31] and [0x11FC] after the LEVEL board's pick (the bar's seed; the
 * party screen reads the members too).
 *
 * [0x5D31] / [0x11FC] after level_load's LEVEL-board pick: the record's
 * timeA/B/C word and the 12/10/8 starting tribe (entry 1000:009F,
 * render_level_frame 1000:9589).
 *
 * @return the time limit word
 */
uint16_t level_time_limit(void);

/**
 * @brief Accessor: the tribe's member count.
 *
 * @return The tribe's member count.
 */
uint16_t level_tribe_members(void);

/**
 * @brief the stamped background and the P block, for the exit/igloo test
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *level_bg_for_probe(void);

/** @brief 1000:54A9's stamp y for the exit cell at (col, row): the handler's
 * `row*16`, then 0x5662's style-6 nudge, then `ADD DX,0x10 / SUB DX,0x40`.
 *
 * @param col the cell column
 * @param row the cell row
 * @return the stamped block's top y
 */
int32_t level_exit_stamp_y(uint32_t col, uint32_t row);

/**
 * @brief Accessor: the record's P block (exit art).
 *
 * @return The record's P block (exit art).
 */
const uint8_t *level_p_block_for_probe(void);

/**
 * @brief Accessor: the loaded background index.
 *
 * @return The loaded background index.
 */
uint16_t level_bg_index_for_probe(void);

/**
 * @brief Accessor: the record's C block (0xA9 background art).
 *
 * @return The record's C block (0xA9 background art).
 */
const uint8_t *level_c_block_for_probe(void);

/** @brief 1000:5F54 - render_objects' tail: for each 0x32B9 entry it blits the two
 * 32 x 24 mode-2 C-block sprites at (x, y) and (x + 0x20, y) (0x5FBE ADDs
 * 0x300 to the source and 0x20 to x for the second).  The queue holds one
 * entry per 0xA9 cell, and all 14 shipped levels with a 0xA9 have exactly
 * one.  The original's drawer steps DI by 8 (not the record's 12) at
 * 1000:5FD2, so with more than one entry it reads the first record's +8/+A
 * as x/y; nothing shipped reaches that.  The page here is the flat-compose
 * window, so the view is subtracted exactly as the object passes do.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
void level_draw_bg_sprites(uint8_t *page, int32_t scrollX, int32_t scrollY);

/**
 * @brief Accessor: the loaded record's animation frame count.
 *
 * the level's [0x6532] (BD field F, 0 when absent).
 *
 * @return The loaded record's animation frame count.
 */
uint32_t level_anim_frames(void);

/**
 * @brief 1000:1FD4's per-tile slope profile at `sub` (signed -4..+4).
 *
 * 1000:1FD4 field A: the per-tile signed slope profile (behaviour 4).
 *
 * @param id the identifier
 * @param sub the sub-column
 * @return the signed slope byte
 */
int32_t level_slope_byte(uint32_t id, uint32_t sub);

#endif /* HUMANSRE_LEVEL_H */
