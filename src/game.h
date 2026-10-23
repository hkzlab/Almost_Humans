/* game.h — the game core's internal module boundaries.
 *
 * One header so the module boundaries stay honest: each module below maps to
 * one subsystem of game_structure.md §1, and the init/step pairs are the
 * translation of the original's ISRs and main-loop stages
 * (port_structure.md §3).
 */
#ifndef HUMANSRE_GAME_H
#define HUMANSRE_GAME_H

#include "osystem.h"

/* --- src/res.c — resource I/O (subsystem 2, re_formats.md §8) ---------
 * The volume file (named chunks), the LZW decode, the offset-based pool
 * arena standing in for the original's segment pool. */
int  res_init(const OSystem *osys);

/* --- src/video.c — pages/scroll/dirty cells (subsystem 3, §5/§7) ------ */
int  video_init(const OSystem *osys);
void video_tick(void);

/* --- src/palette.c — fades/DAC (subsystem 4, §6) ---------------------- */
int  palette_init(const OSystem *osys);
void palette_tick(void);

/* --- src/font.c — glyphs/packed ASCII (subsystem 12, §9) -------------- */
int  font_init(const OSystem *osys);

/* --- src/statusbar.c — the bottom bar (subsystem 13, §12.7) ----------- */
int  statusbar_init(const OSystem *osys);
void statusbar_tick(void);

/* --- src/scene.c — .ANI/.RSC interpreter (subsystem 11, §8.5-8.7a) ---- */
int  scene_init(const OSystem *osys);
int  scene_playLevelIntro(unsigned levelIndex);   /* the cd_file_op body   */

/* --- src/level.c — headers/maps/defs (subsystem 9, §2) ---------------- */
int  level_init(const OSystem *osys);
int  level_load(unsigned levelIndex);

/* --- src/entity.c — entity table/bytecode (subsystem 10, §12) --------- */
int  entity_init(const OSystem *osys);
void entity_tick(void);

/* --- src/script.c — interpreter skeleton (re_scripts.md) -------------- */
int  script_init(const OSystem *osys);

/* --- src/audio.c — tune/sfx state machine (subsystem 7) --------------- */
int  audio_init(const OSystem *osys);

/* --- src/frontend.c — map/menu/code entry (subsystem 8, §10) ---------- */
int  frontend_init(const OSystem *osys);
int  frontend_tick(void);                          /* 0 = quit requested    */

/* --- src/save.c — the save-game format (byte-compatible) -------------- */
int  save_init(const OSystem *osys);

/* --- src/game_main.c — the top-level flow (the original's main loop) --
 *
 * The original's foreground loop is one flow-step per display period: the
 * ISR does the fixed work (flip apply, palette step, music tick) and the
 * foreground advances the flow once per tick boundary.  The states mirror
 * the original's top-level control (game_structure.md §1):
 *
 *     ATTRACT --(5 s dwell; the real card cycle comes later)--> MENU
 *     MENU    --(fire = the +0x5ACE bit-7 confirm)-----------> LEVEL
 */
typedef enum GameFlow {
    FLOW_ATTRACT = 0,      /* the idle cycle (intro cards / hi-scores)    */
    FLOW_MENU,             /* the map/level-select room                   */
    FLOW_LEVEL             /* a level is installed and running            */
} GameFlow;

/* The tick rate the port assumes: the original MEASURES it at boot
 * (init_crtc_timer calibrates the PIT divisor [0x5ABE] against the display
 * and the ISR re-spins to the next vsync edge each tick), so one tick = one
 * display period.  Verified live in DOSBox: the ISR tick counter runs at
 * 70.19-70.5 Hz, and the calibrated divisor 0x3E59 = PIT 74.76 Hz with the
 * bit-3-of-3DA wait making up the remainder to one display period. */
#define GAME_TICK_HZ 70

/* Advance the flow one step with the polled input; the loop calls it once
 * per tick boundary.  Returns the new state. */
GameFlow gameflow_step(GameFlow state, const InputState *input);

#endif /* HUMANSRE_GAME_H */
