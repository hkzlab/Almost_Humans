/* gameflow.h — the interface of src/game_main.c: the top-level flow
 * (the original's main loop).
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
 * (the [0x5AC8] counter + [0x48BA] abort pair — re_map.md "Timer / INT 8"),
 * so the host display's refresh rate must never enter the pacing.  The
 * foreground runs exactly one flow step per tick boundary and drops missed
 * boundaries (the original's delta-spin semantics), it never catches up. */
#define GAME_TICK_HZ 70

/* --- src/game_main.c — the top-level flow (the original's main loop) --
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
typedef enum GameFlow {
    FLOW_ATTRACT = 0, /* game_loop 1000:131E: the effect cycle      */
    FLOW_MENU, /* the map/level-select room                   */
    FLOW_LEVEL, /* a level is installed and running            */
    FLOW_ENDING /* entry 1512: cd_file_op(6 lost / 7 won),
                            * draw_status_indicator, menu_loop (the MAP)   */
} GameFlow;

/* Advance the flow one step with the polled input; the loop calls it once
 * per tick boundary.  Returns the new state. */
GameFlow gameflow_step(GameFlow state, const InputState *input);
/* The level index the MENU exit chose (0-based; [0x22]-1) — the save's
 * +0x00 when a slot loaded, otherwise the room's code match.  Probe for the
 * save/restore test (the same role as attract_phase_for_probe). */
uint32_t gameflow_level_for_probe(void);
/* The pause menu's five 0-based results (dispatch_by_result_a167 1000:38EA);
 * returns the flow the action leaves, sets *quit for QUIT THIS GAME. */
GameFlow gameflow_pause_action(int32_t choice, int32_t *quit);
uint32_t gameflow_party_score(void); /* the briefing's [0x592] low word */
int32_t gameflow_party_pending(void); /* a restart leaves the party to run */
/* 1000:35AF/35CA: 1 while a RESTART THIS LEVEL's fade to black runs; the
 * level reloads and the party screen starts when it returns 0. */
int32_t gameflow_restart_fade_tick(void);
const char *gameflow_party_tribe(void); /* 0x1189: the level's author   */
/* scan_id_list 1000:386E event 4: ESC opens the 0x5BBC pause block. */
/* scan_id_list 1000:386E: the DS:0x1989 key list.  Handles ESC (the 0x5BBC
 * pause block), F1..F8 (select the record by scancode), ','/'.' (previous /
 * next record), SPACE (the action-icon step / home state) and F10 (the key
 * wait).  Returns 1 when an entry ran; a non-bit7 entry whose gate [0xC] is
 * closed returns 0 (1000:38AD). */
int32_t gameflow_key_list(const InputState *in);
/* [0x5BBA]: the goal page (scroll_map 1000:9E51) is up. */
int32_t gameflow_goal_page_up(void);
/* Test seam: level_setup armed the goal/title page ([0x5BBA]) but the first
 * play frame has not shown it yet. */
int32_t gameflow_goal_armed(void);
/* 1000:3954's key wait (F10) owns the input and blocks the world. */
int32_t gameflow_key_wait_active(void);
/* The time-up dialog's 3 results (1000:34F9's inline dispatch): 0 restart
 * this level (level_restore), 1 restart this game (the room), 2 quit. */
GameFlow gameflow_time_up_action(int32_t choice, int32_t *quit);
int32_t gameflow_restart_pending(void); /* [0x1809] = 0xFF: restart game */
/* entry LAB_00CD's post-level step: [0x22]++ / [0x26]-- -> the next level of
 * the 160; when they run out this returns FLOW_ATTRACT (the caller starts
 * FLOW_ENDING: scene 7 -> the disc probe -> the MAP -> the attract). */
GameFlow gameflow_after_level(uint32_t *levelCurrent);
/* entry 14E6/14FF and 150C/1512: the post-level newspaper index (0xFFFF =
 * none: lost, or the header byte 0xFF) and the ending scene (6 lost,
 * 7 COMPLETE). */
uint32_t gameflow_post_level_scene(int32_t won, uint32_t introIndex);
uint32_t gameflow_ending_scene(int32_t won);
/* level_run 3060/3062's gate: the level's required humans vs the tribe. */
int32_t gameflow_tribe_too_small(uint32_t need, uint32_t members);

#endif /* HUMANSRE_GAMEFLOW_H */
