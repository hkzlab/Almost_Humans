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

#endif /* HUMANSRE_GAME_H */
