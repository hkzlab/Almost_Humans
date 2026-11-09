/* frontend.h - the interface of src/frontend.c: the front-end room (subsystem 8, §10)
 *
 * The walkable level-select / game-start menu (frontend_room_setup
 * 1000:A48E + the frame web + level_select_event_loop 1000:AA2C -
 * re_map.md → "The front-end room"): the room the attract escape hands
 * into (entry LAB_002B).  One tick per call, exactly the original's
 * per-display-period frame.  1 = the room runs; 0 = the room exited
 * (the confirm: the walk off the right edge at x=0x280, or Enter on the
 * name screen with a codes.bin match) - both run the exit fade first.
 * ESC stays in the room (0x5F11's act0 clears the typed name); there is
 * no key path back to the attract. */

#ifndef HUMANSRE_FRONTEND_H
#define HUMANSRE_FRONTEND_H

#include "osystem.h"

/* The four option counters - the descriptor tables' +7 bytes at
 * 202F:0x5E5B/0x5E65/0x5E6F/0x5E79, where the badge's frame IS the
 * setting.  The read side uses this accessor: level.c takes LEVEL, the room
 * hands MUSIC to audio_music_gate, scene.c takes ANIMATIONS, and game_main
 * hands the RESTORE GAME slot to save_load_slot (entry 1000:004C's load). */
enum {
    FRONTEND_OPT_MUSIC = 0, /* ON/OFF: the frame web's music gate      */
    FRONTEND_OPT_LEVEL = 1, /* EASY/NORMAL/HARD: time A/B/C, 12/10/8    */
    FRONTEND_OPT_GAME = 2, /* OFF/1..20: the save slot                */
    FRONTEND_OPT_ANIM = 3 /* ON/OFF: cd_file_op off (no cutscenes)   */
};

/**
 * @brief Bind the frontend module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t frontend_init(const OSystem *osys);

/**
 * @brief One frontend tick: the room, the name screen or the load.
 *
 * @param in the input state
 * @return 1 while the frontend keeps running
 */
uint8_t frontend_tick(const InputState *in);

/**
 * @brief Accessor: the level-code cursor.
 *
 * [0x5F09]: the code match's 1-based index = the level the room starts (string
 * i of levels\codes.bin = level i; #1 "DARWIN"; a full miss leaves 161 - the
 * original's own edge).
 * entry's PUSH AX/POP AX move it to [0x22]; game_main does the same into its
 * level counter.
 *
 * @return The level-code cursor.
 */
uint16_t frontend_selected_level(void);

/** @brief entry's JMP LAB_002B: the room's setup runs again from the top (the
 * counters persist - frontend_room_setup seeds none of them).  The next
 * frontend_tick re-loads the assets (FE_LOAD).
 *
 * entry's JMP LAB_002B (the pause's RESTART THIS GAME): the room's setup runs
 * again; the option counters persist.
 */
void frontend_restart(void);

/** @brief The four option counters - the read side's one accessor.  Consumers:
 * level.c (LEVEL: the time-limit pick + the 12/10/8 tribe), audio.c (MUSIC:
 * the _frame web's gate), scene.c (ANIMATIONS: cd_file_op's early RET).  The
 * RESTORE GAME _slot is carried out here and consumed by entry 1000:004C's
 * save load, which the port's save.c does not have yet (M6).
 *
 * @param idx the index
 * @return the option counter
 */
uint8_t frontend_option(uint32_t idx);

/**
 * @brief The room's write side, for the audio/sfx gate's tests.
 *
 * The room's write side, exposed for the audio/sfx gate's tests (the room
 * itself writes the counters from its input).
 *
 * @param idx the index
 * @param value the value
 */
void frontend_option_set_for_probe(uint32_t idx, uint32_t value);

/** @brief [0x4220] - the rail node, exposed for the option read-side test's walk
 * legs (the same role as attract_phase_for_probe).
 *
 * @return the current option cell
 */
uint16_t frontend_cell_for_probe(void);

/**
 * @brief the name screen's _phase and typed buffer, for the input-law tests.
 *
 * @return 1 when the name screen is up, 0 otherwise
 */
uint8_t frontend_name_screen_for_probe(void);

/**
 * @brief Accessor: the typed high-score name (0x61E0).
 *
 * @return The typed high-score name.
 */
const char *frontend_name_for_probe(void);

#endif /* HUMANSRE_FRONTEND_H */
