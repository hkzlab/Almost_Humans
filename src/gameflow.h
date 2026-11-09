/* gameflow.h - the flow helpers of src/game_main.c: the top-level flow
 * (the original's main loop).  game_main() itself, the core's entry the arch
 * layer calls, is declared in osystem.h - that header is the whole
 * arch↔core handshake.
 *
 * The original's foreground loop is one flow-step per display period: the
 * ISR does the fixed work (flip apply, palette step, music tick) and the
 * foreground advances the flow once per tick boundary.  The states mirror
 * the original's top-level control (game_structure.md §1):
 *
 *     ATTRACT --(the escape: the action key or any held key, after the
 *                13B2 fade completes -- attract_tick)--> MENU
 *     MENU    --(the room's confirm: the walk off the right edge, or
 *                Enter with a codes.bin match -- frontend_tick)--> LEVEL
 */

#ifndef HUMANSRE_GAMEFLOW_H
#define HUMANSRE_GAMEFLOW_H

#include "osystem.h"

/* The tick rate the port assumes: the original MEASURES it at boot
 * (init_crtc_timer calibrates the PIT divisor [0x5ABE] against the display
 * and the ISR re-spins to the next vsync edge each tick), so one tick = one
 * display period.  Verified live in DOSBox: the ISR tick counter runs at
 * 70.19-70.5 Hz, and the calibrated divisor 0x3E59 = PIT 74.76 Hz with the
 * bit-3-of-3DA wait making up the remainder to one display period.
 *
 * This rate is the SOLE pacer: game_main's loop gates on osys->tickSync
 * (the [0x5AC8] counter gate - re_map.md "Timer / INT 8"),
 * so the host display's refresh rate must never enter the pacing.
 * The foreground runs exactly one flow step per tick boundary and drops 
 * missed boundaries (the original's delta-spin semantics), it never catches up.
 */
#define GAME_TICK_HZ 70

typedef enum GameFlow {
    FLOW_ATTRACT = 0, /* game_loop 1000:131E: intro/attract mode    */
    FLOW_MENU,        /* the map/level-select room                  */
    FLOW_LEVEL,       /* a level is installed and running           */
    FLOW_ENDING       /* entry 1512: cd_file_op(6 lost / 7 won),
                       * draw_status_indicator, menu_loop (the MAP) */
} GameFlow;

/**
 * @brief One flow step: run the current state and return the next.
 *
 * Advance the flow one step with the polled input; the loop calls it
 * once per tick boundary.
 * Returns the new state.
 *
 * @param state the flow state
 * @param input the input state
 * @return the next flow state
 */
GameFlow gameflow_step(GameFlow state, const InputState *input);

/**
 * @brief The current level, 0-based: the original's level number
 *        ([0x22] / [0x1F41], 1..160) minus 1.
 *
 * The MENU exit stores (room selection - 1); a loaded slot's header
 * level overrides it, also - 1.  gameflow_after_level advances it after
 * a won level, up to level 160.
 *
 * Core code reads the original's 1-based id back as
 * gameflow_level_current() + 1 (score.c's [0x1F41] terms and the level
 * tune).
 *
 * @return the current 0-based level index
 */
uint16_t gameflow_level_current(void);

/** @brief dispatch_by_result_a167 (1000:38EA): the pause menu's five 0-based
 * results.  Returns the flow the action leaves (FLOW_MENU for RESTART THIS
 * GAME) and sets *quit for QUIT THIS GAME.
 *
 * @param choice the selected entry: the A167 result in AX (1000:A1FD,
 *               `MOV AX, word ptr [0x5e3b]`), so an `int16_t`
 * @param quit the quit flag: 0/1 out-flag, the port's deferred game_exit
 *             (1000:8DD6), honoured by game_main after the frame
 * @return the next flow state
 */
GameFlow gameflow_pause_action(int16_t choice, uint8_t *quit);

/** @brief The briefing's YOUR SCORE IS value: the low word of [0x592]/[0x594],
 * which score_prepare maintains across levels.
 *
 * @return the party score
 */
uint16_t gameflow_party_score(void);

/**
 * @brief test seam: the restart left the party screen pending (1000:3073).
 *
 * @return 1 when the party screen is still pending, 0 otherwise
 */
uint8_t gameflow_party_pending(void);

/** @brief 1000:35CA `wait_palette_fade_done`: 1 while the restart's fade
 * runs, 0 when it is over (the level has then been reloaded).
 *
 * 1000:35AF/35CA: 1 while a RESTART THIS LEVEL's fade to black runs; the level
 * reloads and the party screen starts when it returns 0.
 *
 * @return 1 while the fade is still stepping, 0 otherwise
 */
uint8_t gameflow_restart_fade_tick(void);

/** @brief The briefing's code line (0x1189): load_level (1000:30BC) copies the level
 * record's author there (1000:312C), and level_intro_screen draws it.
 *
 * @return the pointer, or NULL when unavailable
 */
const char *gameflow_party_tribe(void);

/* scan_id_list 1000:386E event 4: ESC opens the 0x5BBC pause block. */

/** @brief 1000:386E.  Returns 1 when an entry ran (a non-bit7 entry whose gate [0xC]
 * is closed returns 0 before the dispatch, 1000:38AD).
 *
 * scan_id_list 1000:386E: the DS:0x1989 key list.
 * Handles ESC (the 0x5BBC pause block), F1..F8 (select the record by
 * scancode), ','/'.' (previous / next record), SPACE (the action-icon step /
 * home state) and F10 (the key wait).
 *
 * @param in the input state
 * @return 1 when an entry ran, 0 otherwise
 */
uint8_t gameflow_key_list(const InputState *in);

/**
 * @brief Accessor: the goal page is showing.
 *
 * [0x5BBA]: the goal page (scroll_map 1000:9E51) is up.
 *
 * @return The goal page is showing.
 */
uint8_t gameflow_goal_page_up(void);

/** @brief Test seam: level_setup armed the goal/title page ([0x5BBA]).
 *
 * Test seam: level_setup armed the goal/title page ([0x5BBA]) but the first
 * play frame has not shown it yet.
 *
 * @return 1 when the page is pending its first play frame.
 */
uint8_t gameflow_goal_armed(void);

/**
 * @brief Accessor: a scripted key wait is active.
 *
 * 1000:3954's key wait (F10) owns the input and blocks the world.
 *
 * @return A scripted key wait is active.
 */
uint8_t gameflow_key_wait_active(void);

/** @brief input_dispatch_loop 34F9's inline time-up dispatch: 0 restart this level
 * (level_restore), 1 restart this game (the room), 2 quit.
 *
 * @param choice the selected entry: the A167 result in AX (1000:A1FD,
 *               `MOV AX, word ptr [0x5e3b]`), so an `int16_t`
 * @param quit the quit flag: 0/1 out-flag, the port's deferred game_exit
 *             (1000:8DD6), honoured by game_main after the frame
 * @return the next flow state
 */
GameFlow gameflow_time_up_action(int16_t choice, uint8_t *quit);

/**
 * @brief test seam: the pause/time-up RESTART THIS GAME arm set [0x1809] = 0xFF.
 *
 * @return 1 when a restart is pending, 0 otherwise
 */
uint8_t gameflow_restart_pending(void);

/** @brief entry LAB_00CD's post-level step: [0x22]++ / [0x26]-- (0xA0 - [0x22]).
 * While the window has levels left the PLAY arm runs again - the next level
 * of the 160.  When they run out, COMPLETE: cd_file_op([0x1E]) (the ending
 * scene), draw_status_indicator, menu_loop (the MAP screen), then the
 * attract.  The ending/MAP arm is game_main's FLOW_ENDING (scene 6/7, the
 * disc-0 probe, map_begin/map_tick).  Known deviations: port_deviations.md.
 *
 * @param levelCurrent the current level number
 * @return the next flow state
 */
GameFlow gameflow_after_level(uint16_t *levelCurrent);

/** @brief entry 14E6/14FF: the post-level newspaper.  A lost level goes to the
 * ending ([0x138E]==0 at 14E6); a won one plays its header +1 index unless
 * that byte is negative (0xFF = none; cd_file_op's own CMP AL,0x80 path).
 *
 * entry 14E6/14FF and 150C/1512: the post-level newspaper index (0xFFFF =
 * none: lost, or the header byte 0xFF) and the ending scene (6 lost, 7
 * COMPLETE).
 *
 * @param won the won flag
 * @param introIndex the intro scene index
 * @return the newspaper index, or 0xFFFF when none
 */
uint32_t gameflow_post_level_scene(uint8_t won, uint32_t introIndex);

/** @brief entry 150C/1512: [0x1e]=7 when the levels run out (COMPLETE); the lost
 * path reaches 1512 with [0x1e]=6 (the restart-game arm set it at 14BE).
 *
 * @param won the won flag
 * @return the ending scene index (6 or 7)
 */
uint32_t gameflow_ending_scene(uint8_t won);

/** @brief level_run 3060/3062: load_level leaves [0x17FB] (the humans needed) and
 * [0x11FC] (the tribe); need > members takes the briefing's GAME OVER
 * variant and the level is NOT started.
 *
 * @param need the required count
 * @param members the member count
 * @return 1 when the level needs more members, 0 otherwise
 */
uint8_t gameflow_tribe_too_small(uint16_t need, uint16_t members);

#endif /* HUMANSRE_GAMEFLOW_H */
