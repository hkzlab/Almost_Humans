/* frontend.h — the interface of src/frontend.c: the front-end room (subsystem 8, §10)
 *
 * The walkable level-select / game-start menu (frontend_room_setup
 * 1000:A48E + the frame web + level_select_event_loop 1000:AA2C —
 * re_map.md → "The front-end room"): the room the attract escape hands
 * into (entry LAB_002B).  One tick per call, exactly the original's
 * per-display-period frame.  1 = the room runs; 0 = the room exited
 * (the confirm: the walk off the right edge at x=0x280, or Enter on the
 * name screen with a codes.bin match) — both run the exit fade first.
 * ESC stays in the room (0x5F11's act0 clears the typed name); there is
 * no key path back to the attract. */

#ifndef HUMANSRE_FRONTEND_H
#define HUMANSRE_FRONTEND_H

#include "osystem.h"
/* The four option counters — the descriptor tables' +7 bytes at
 * 202F:0x5E5B/0x5E65/0x5E6F/0x5E79, where the badge's frame IS the
 * setting.  The read side uses this accessor: level.c takes LEVEL, the room
 * hands MUSIC to audio_musicGate, scene.c takes ANIMATIONS, and game_main
 * hands the RESTORE GAME slot to save_loadSlot (entry 1000:004C's load). */
enum {
    FRONTEND_OPT_MUSIC = 0, /* ON/OFF: the frame web's music gate      */
    FRONTEND_OPT_LEVEL = 1, /* EASY/NORMAL/HARD: time A/B/C, 12/10/8    */
    FRONTEND_OPT_GAME = 2, /* OFF/1..20: the save slot                */
    FRONTEND_OPT_ANIM = 3 /* ON/OFF: cd_file_op off (no cutscenes)   */
};

int32_t frontend_init(const OSystem *osys);
int32_t frontend_tick(const InputState *in);
/* [0x5F09]: the code match's 1-based index = the level the room starts
 * (string i of levels\codes.bin = level i; #1 "DARWIN"; a full miss
 * leaves 161 — the original's own edge).  entry's PUSH AX/POP AX move
 * it to [0x22]; game_main does the same into its level counter. */
uint32_t frontend_selectedLevel(void);
/* entry's JMP LAB_002B (the pause's RESTART THIS GAME): the room's setup
 * runs again; the option counters persist. */
void frontend_restart(void);
uint32_t frontend_option(uint32_t idx);
uint32_t frontend_cellForProbe(void); /* [0x4220]: the walk's rail node  */
int32_t frontend_nameScreenForProbe(void); /* the name screen's phase      */
const char *frontend_nameForProbe(void); /* the typed name (0x61E0)      */

#endif /* HUMANSRE_FRONTEND_H */
