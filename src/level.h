/* level.h — the interface of src/level.c: the level definitions (subsystem 9, §2)
 *
 * LEVELS\LA.BIN..LP.BIN: 16 files x 10 records = 160 levels.  The record:
 * the 13-byte header, the goal/author ASCIIZ text, the 8-byte editor
 * metadata, the 640-byte tile map (32x20).
 */

#ifndef HUMANSRE_LEVEL_H
#define HUMANSRE_LEVEL_H

#include "osystem.h"

typedef struct {
    uint32_t levelId; /* 0..159                                    */
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

int32_t level_init(const OSystem *osys);
int32_t level_load(uint32_t levelId); /* 0..159; 0 = ok            */
const LevelData *level_data(void); /* NULL before a load        */
const uint8_t *level_map(void); /* == level_data()->map      */
uint32_t level_style(void); /* [0x17EE]: the record's style */
/* the working palette 202F:0x14D3 (the fade target of the claim, the level's
 * own fades and the rotate hook) */
const uint8_t *level_palette(void);
/* draw_level_text 0x33E2/0x33F2: seal the level palette's 0..0x7F to grey
 * and copy the shipped gradient into 0x80..0xBF (the 0x14D3 buffer). */
void level_endPalette(void);
/* draw_level_text 0x344C/0x3450: re-render the world page (blit_bitmap_planes
 * + render_objects), erasing the caption before check_highscore's card. */
void level_redrawPage(void);
/* page_offset_to_ptr (1000:F123): 1 = the tile id's page is free (the ACTION
 * op 3 gate) — the BD file's page mask + its 16-word item record. */
int32_t level_pageFree(uint32_t id);
/* The two level maps the walk reads: `0x92A0`'s page bytes (bit 7 = solid, the
 * low 7 bits the step, 1000:E389) and `[0x1FD6]`'s 16-word item records
 * (1000:F25F's occupancy bit). */
/* Field K's first word: the level's two special tile ids ([0x6F20]/[0x6F21]). */
uint32_t level_specialTile(uint32_t i);
uint32_t level_pageByte(uint32_t id, uint32_t subcol);
uint32_t level_itemWord(uint32_t id, uint32_t row);
uint32_t level_itemByte(uint32_t id, uint32_t off);
int32_t level_tick(void); /* 1 = the level is running  */
/* 1000:3DC0: the view's world origin (the CRTC start's source). */
void level_setView(uint32_t x, uint32_t y);
/* 1000:5CB6: the camera follows the acting record (update_world's first call,
 * 4 px/frame, clamped to the 512x320 page's pan range). */
void level_followView(void);
/* [0xC]: update_world's gate (set by level_fade_in 1000:336C, cleared by the
 * claim at 1000:3B19 and restored at 1000:3CAB). */
void level_setUpdates(int32_t on);
/** [0x2D8E]: the follower's +-1 px offset walk enabler (1000:DAC7 / 0xF8CE). */
void level_set_offsetWord(uint16_t v);
uint16_t level_offsetWord(void);
/* 1000:5D37's walk pointer ([0x2D90]) back to the table's first pair. */
void level_reset_offsetWalk(void);
int32_t level_updates(void); /* [0xC] */
uint32_t level_viewX(void);
uint32_t level_viewY(void);
/* The claim's particles (1000:3E73/0x3FB3/0x3F4A): begin at the appended record
 * with the 16x16 shade table object.c builds from the recipe sprite. */
void level_sparkBegin(uint32_t ox, uint32_t oy, const uint8_t *shade);
int32_t level_sparkActive(void);
int32_t level_sparkPeek(uint32_t *x, uint32_t *y, uint32_t *value);
/* 1000:8526's [0x5ACE] for the play loop's command dispatch (1000:E3F6). */
void level_noteInput(const InputState *in);
/* level_fade_in (1000:3334): the start chain — fade out of the pre-level
 * screen, the eight update_world frames, the fade in to the working palette,
 * then the bar's ticking gate.  level_begin arms it once; the phases advance
 * in level_tick, so level_starting() is true until the level is playable. */
void level_begin(void);
int32_t level_starting(void);
/* load_sprite_pal's LOADING screen (1000:1DC0): ten walkers for 0xA0 frames,
 * then the "LOADING!" letters for 0x56.  level_loadingScreen() starts it and
 * reports whether it is showing; level_loadingTick draws one frame. */
int32_t level_loadingScreen(void);
void level_loadingTick(void);

/* level_run 3060/3062: the level's required humans ([0x17FB], the map's
 * 0xAA count) against the tribe ([0x11FC]).  A tribe that is too small plays
 * the briefing's GAME OVER variant and the level is SKIPPED; [0x1809] then
 * takes entry 1000:00DC -> 002B (the restart-game path), not the next level. */
uint32_t level_required(void);
uint32_t level_members(void);
void level_addMember(void); /* 1000:DE1F: [0x11FC]++ */

/* level_restore (1000:35AF): [0x11FC] := [0x1970] — the members the level
 * started with, before the level's own joins (level_start 1000:3032 then
 * re-picks the time; the port re-runs level_load, so the snapshot is put
 * back afterwards). */
void level_restoreMembers(void);

/* level_run's end: the play loop exits when object_test_dispatch
 * (1000:35D6) returns negative, and [0x138E] (0xFF won / 0 extinct) then
 * selects draw_level_text's page.  The six test handlers need the placed-
 * object array (0x3E75), which object.c holds; level_endTest evaluates the
 * six predicates through object_testDispatch.  level_requestEnd(1) = the
 * level was completed. */
void level_requestEnd(int32_t won);
/* draw_status_indicator (1000:1FEF): the disc probe + the "PUT THE DISK
 * INTO DRIVE n / PRESS FIRE WHEN READY" screen.  Begin returns 0 when the
 * volume is present (the normal, no-screen path); Tick returns 1 while the
 * screen is up. */
int32_t level_diskStatusBegin(uint32_t disc);
int32_t level_diskStatusTick(const InputState *in);
void level_diskStatusReset(void); /* test/lifecycle: drop the screen */
/* the level-end test (object_test_dispatch 1000:35D6's negative return).
 * level_goalReached is the win hook the discovery/claim path sets; a lost
 * tribe member decrements the live count and ends the level at zero. */
void level_goalReached(void);
void level_memberLost(void);
int32_t level_endTest(void);
int32_t level_ended(void);
int32_t level_endHolding(void);
void level_setEnd(int32_t won); /* 1000:F66D: [0x138E] = 0 */
int32_t level_outcome(void); /* [0x138E]: 1 won, 0 extinct */

/* [0x5D31] / [0x11FC] after level_load's LEVEL-board pick: the record's
 * timeA/B/C word and the 12/10/8 starting tribe (entry 1000:009F,
 * render_level_frame 1000:9589). */
uint16_t level_timeLimit(void);
uint32_t level_tribeMembers(void);
/* the stamped background image and the BD P block, for the exit/igloo test */
const uint8_t *level_bgForProbe(void);
const uint8_t *level_pBlockForProbe(void);
uint32_t level_bgIndexForProbe(void);
const uint8_t *level_cBlockForProbe(void);
/* render_objects' tail (1000:5F54): the 0xA9 background sprites */
void level_drawBgSprites(uint8_t *page, int32_t scrollX, int32_t scrollY);
/* the level's [0x6532] (BD field F, 0 when absent). */
uint32_t level_animFrames(void);
/* 1000:1FD4 field A: the per-tile signed slope profile (behaviour 4). */
int32_t level_slopeByte(uint32_t id, uint32_t sub);

#endif /* HUMANSRE_LEVEL_H */
