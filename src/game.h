/* game.h — the game core's internal module boundaries.
 *
 * One header so the module boundaries stay honest: each module below maps to
 * one subsystem of game_structure.md §1, and the init/step pairs are the
 * translation of the original's ISRs and main-loop stages
 * (port_structure.md §3).
 *
 * Addresses (§4a): `1000:XXXX` is a Ghidra in-segment offset of a routine in
 * the original; `[0xXXXX]` / `202F:XXXX` is a game data address; anything else
 * (a file offset, a VGA page, a run-time segment) says so at the use.
 */
#ifndef HUMANSRE_GAME_H
#define HUMANSRE_GAME_H

#include "osystem.h"

/* --- src/res.c — resource I/O (subsystem 2, re_formats.md §8) ---------
 * The volume file (named chunks), the LZW decode, the offset-based pool
 * arena standing in for the original's segment pool. */
int  res_init(const OSystem *osys);
/* The [0xF7E] analog: the disc that name-based loads come from (0..2 =
 * HUMANS.000/.001/.002; the original opens ONE volume file built from the
 * [0xF7E] disc number and never scans across discs). */
void     res_setDisc(unsigned disc);
/* draw_status_indicator's disc probe (1000:20CF): opens the volume file for
 * `disc` and closes it; 0 = the disc is not available. */
int      res_volumePresent(unsigned disc);
/* The level-header intro-movie/disc index -> disc byte: the ported table
 * `202F:0x6287` (1000:AFF8's lookup; corpus-verified 10/10 — every
 * ANI<idx>.ANI ships on the disc the table names).  0xFF = no entry — the
 * analog of the original's CMP AL,0xFF/JE branch (keep the current disc). */
unsigned res_movieDisc(unsigned idx);
/* Load a named chunk case-insensitively out of the CURRENT disc's volume
 * (the name is the relative path inside the volume, e.g. "ANM\ANI03.ANI");
 * NULL when absent — the original's CF to the INT 63 caller (the
 * "PLEASE INSERT HUMANS DISK A/B/C" prompt territory, re_formats.md §8.3).
 * `chkOut` (nullable) receives the BP analog, the resource-level checksum
 * exactly as resource_load leaves it in BP (1000:9183 OR BP,BP / RET):
 * stored blocks XOR their flag + payload words in; LZ blocks RESET the
 * accumulator to the stream's checksum field and then XOR the bit-buffer
 * word and every refill word (1000:91b9..91ce, the 91c7 XOR is dead —
 * 91c9 MOV BP,AX overwrites it, so the size word never contributes).
 * The resource-level value is NOT a 0-gate in the original: every shipped
 * block decodes to a nonzero accumulator (corpus sweep 2026-10-23) yet
 * levels\CODES.BIN loads live through the same machine.  The gate verified
 * against 0 is the scene-object one (1000:B444) and belongs to the scene
 * player. */
uint8_t *res_loadChunk(const char *name, uint32_t *sizeOut, uint16_t *chkOut);
/* Decode one LZW object straight out of a memory buffer (an .ANI's chunk
 * image): [u16 decompressed_size][u16 checksum][u16 bit buffer][LZW bits].
 * Writes `size` bytes to `out`, returns the decompressed size or -1.
 * *chkOut mirrors the machine's final checksum accumulator (BP) — the
 * scene-object gate tests it against 0 (1000:B444). */
int res_decode_lzw_mem(const uint8_t *data, uint32_t data_size,
                       uint32_t off, uint8_t *out, uint32_t out_size,
                       uint16_t *chkOut);

/* --- src/video.c — pages/scroll/dirty cells (subsystem 3, §5/§7) ------ */
int  video_init(const OSystem *osys);
void video_tick(void);
uint8_t *video_page(int which);   /* 320x200x8 page 0 or 1                    */
/* the mode set, forwarded to the backend (osystem.h's VideoMode): the page
 * layout never changes, only which field the display scans out. */
void video_setMode(VideoMode mode);

/* --- src/palette.c — fades/DAC (subsystem 4, §6) ---------------------- */
int  palette_init(const OSystem *osys);
void palette_tick(void);   /* palette_fade_step 1000:8bfd, once per tick  */
/* palette_load_6bit (1000:8d24): the 202F:0x4BBC working palette := 768
 * 6-bit bytes.  The RAM copy only — the DAC moves through fades or
 * palette_setDac. */
void palette_loadWorking(const uint8_t *p6);
const uint8_t *palette_working(void);   /* the 0x4BBC buffer              */
/* seal_triples/hash_three_bytes (1000:43C2/43D5): grey the given palette
 * entries in place, value (77R+151G+28B) >> 9. */
void palette_sealTriples(uint8_t *p6, unsigned first, unsigned entries);
/* set_palette_dac (1000:8CEE): an immediate DAC write (the DAC shadow is
 * kept for the next fade's current-colour read). */
void palette_setDac(const uint8_t *p6, unsigned first, unsigned n);
/* palette_build_fade (1000:8bac): fade DAC entries first..first+entries-1
 * toward `target` (its triple for `first` at target[0]) in ~div ticks;
 * poll with palette_fadeActive. */
void palette_buildFade(unsigned first, unsigned entries, unsigned div,
                       const uint8_t *target);
int  palette_fadeActive(void);          /* palette_fade_active 1000:8b9a  */
const uint8_t *palette_dacShadow(void); /* the DAC shadow (read-only;
                                        * the off-line fade-ladder harness) */
const uint8_t *palette_fadeTarget(void); /* the active fade's target buffer */
unsigned palette_fadeFirst(void);        /* the active fade's first index   */
unsigned palette_fadeComps(void);        /* the active fade's component #   */

/* --- src/font.c — glyphs/packed ASCII (subsystem 12, §9) -------------- */
int  font_init(const OSystem *osys);
/* game_loop's publish ([0x1E1C] := the loaded apl\text.spr).  NULL clears
 * it (drawText then draws nothing) — the headless no-data case. */
void font_publish(const uint8_t *textSpr);
/* draw_text (1000:4989): centre `s` on `centreX` (the advance table,
 * re_formats.md §`TEXT.SPR`) and blit its glyphs into `arena` — the
 * caller's [0x1E20] text arena (200 rows x 80 B, 1 bpp, 4 px per byte,
 * bit 0 = leftmost; each 2-bpp glyph pixel becomes two 1-bpp pixels, so
 * the glyph doubles horizontally) — at `row`. */
void font_drawText(uint8_t *arena, const char *s, unsigned centreX,
                   unsigned row);
/* draw_text's own measure: the advance-table sum (stops at CR/NUL). */
unsigned font_textWidth(const char *s);

/* The shipped 202F:0x4DE top-10 (the attract's default table), for the
 * MAP/score screens' list. */
unsigned attract_hiscoreCount(void);
uint32_t attract_hiscoreScore(unsigned i);
const char *attract_hiscoreName(unsigned i);

/* --- src/attract.c — the ATTRACT loop (game_loop 1000:131E) -------------
 *
 * The four intro effects in the original's order (1 -> 3 -> 2 -> 4),
 * cycled forever; the only exit is attract_exit (1000:13E9): the
 * configured action key ([0x5ACE] bit 7) or any held key ([0x48A3]),
 * which runs the 13B2 escape — the page pair restored, one fade to black
 * whose completion hands control to the front-end.  Per tick, exactly like
 * the original's wait_vsync cadence: attract_tick() advances the machine
 * by one display period, composes the visible planar window into page 0
 * (game_main's videoPresent shows it) and returns 1 while the attract
 * plays, 0 once the escape's fade completed.  The details and the one
 * documented latency deviation are in attract.c's header and
 * port_deviations.md.
 */
int attract_init(const OSystem *osys);

/* The machine's phases — public for the flow tests (the cycle-order and
 * escape assertions in tests/test_loop.c). */
typedef enum AttractPhase {
    ATTRACT_IDLE = 0,
    ATTRACT_E1_LOAD, ATTRACT_E1_FADEWAIT, ATTRACT_E1_WIPE,
    ATTRACT_E1_HOLD, ATTRACT_E1_OUT,
    ATTRACT_E3_LOAD, ATTRACT_E3_FADEWAIT, ATTRACT_E3_SLIDE,
    ATTRACT_E3_TAIL, ATTRACT_E3_HOLD, ATTRACT_E3_OUT,
    ATTRACT_E2_LOAD, ATTRACT_E2_FADEWAIT, ATTRACT_E2_FRAME,
    ATTRACT_E2_WIPE, ATTRACT_E2_HOLD, ATTRACT_E2_OUT,
    ATTRACT_E4_LOAD, ATTRACT_E4_FADEWAIT, ATTRACT_E4_SCROLL,
    ATTRACT_E4_OUT,
    ATTRACT_ESCAPE, ATTRACT_DONE
} AttractPhase;

int      attract_tick(const InputState *in);   /* 1 = the attract is playing */
unsigned attract_phaseForProbe(void);          /* the tests' phase read      */

/* Effect 4's strip probe (the 192-KB work area at [0xD76] + the rows
 * built, [0xD39]): the byte-exact fixture test compares the fill against
 * tools/attract_scroll_sim.py's dump.  Valid from the first ATTRACT_E4
 * load until the next effect begins. */
const uint8_t *attract_stripForProbe(unsigned *rowsBuilt);

/* --- src/statusbar.c — the bottom bar (subsystem 13, §12.7) -----------
 *
 * The level's bottom 40 rows (y 152..191): panel4.bit's top 40 rows
 * +0xC0-tagged, plus five digit marks (y 172..179) cut from the
 * PANELN1+PANELN2 glyph strip — the countdown's minutes / sec-tens /
 * sec-ones and the tribe counter's tens / ones (re_map.md → "The in-level
 * status bar, decoded end to end").  statusbar_tick is the INT 8 hook at
 * 1000:96AD: the 70-frame second, the packed-decimal countdown with the
 * odometer slides, the tribe odometer, and the per-frame stamp.
 */
int  statusbar_init(const OSystem *osys);
/* build_statusbar_layout (1000:961B) + level_start's pick: packedTimeBcd =
 * AH minutes, AL BCD seconds (the header's timeA/B/C); the tribe digits =
 * members - required (DIV/MOD 10).  Loads its own assets from the CURRENT
 * disc — the level flow's HUMANS.001 (level_load's res_setDisc); a failed
 * load leaves the bar inactive (the headless no-data case). */
void statusbar_start(uint16_t packedTimeBcd, unsigned tribeMembers,
                     unsigned tribeRequired);
void statusbar_tick(void);
/* level_video_init's palette copy (1000:32CD): panel4's own palette —
 * install at DAC entries 0xC0..0xFF (the +0xC0 bank). */
const uint8_t *statusbar_palette(void);
/* object_record_init's arm (1000:F682): [0x5B22] := 0xFF — the displayed
 * tribe counter rolls down by one (a follower joined). */
void statusbar_armOdometer(void);
/* [0x5E44]: the countdown reached zero (the time-up dialog's flag). */
int  statusbar_timeUp(void);
/* 1000:F5E3: [0x5B25] | [0x5B26] != 0 - the odometer is showing a count. */
int  statusbar_tribeActive(void);
/* test seam: sets the odometer digits ([0x5B25]/[0x5B26]) that 1000:F5BC gates on. */
void statusbar_setTribeDigits(unsigned tens, unsigned ones);
unsigned statusbar_tribeDigit(unsigned i);   /* [0x5B25]/[0x5B26] by index */
void statusbar_saveParams(uint8_t *out);      /* the 0x5B22..0x5B29 block   */
void statusbar_loadParams(const uint8_t *in);
unsigned statusbar_tribeValue(void);     /* [0x5B25]*10 + [0x5B26] */
/* [0x5B12]: the ticking gate (the in-level dialogs clear it). */
void statusbar_setActive(int on);
void statusbar_setSplit(unsigned row);
/* the level end's [0x5B0A] = 0xC9 (1000:342B): stop composing the bar */
void statusbar_hide(void);   /* [0x5B0A], 201..152 reveal */
unsigned statusbar_split(void);
/* [0x5B56] (1..6): the current ACTION program — draws both the program and
 * the action row's group.  object.c owns it; the bar follows. */
/* 1000:3933's tail: advance the current action row's selected cell (the
 * 0x5B2B header's second byte) with the wrap to 1 past the count. */
void statusbar_cycleAction(void);
/* 1000:3747: the jump/throw charge bar, v = the acting record's +0xE. */
void statusbar_chargeBar(unsigned v);
/* the group's selected-cell byte (1-based), for tests; 0 past the table. */
unsigned statusbar_actionGroup(void);   /* [0x5B56]: the bar's current group */
/* the program/group header ([0x5B2B] -> 0x5B37): {count, cursor, icon...} */
const uint8_t *object_programHeader(unsigned group);
const uint8_t *object_actionHeader(void);
void object_stepActionCell(void);       /* 1000:3941: SPACE steps the cursor */
unsigned statusbar_actionCount(unsigned group);
unsigned statusbar_actionCell(unsigned group);
/* The countdown word for the save's +0x07: minutes<<8 | BCD seconds. */
uint16_t statusbar_timeWord(void);
/* the elapsed clock [0x5B0C..0E] repacked as check_highscore does (1000:42CE) */
uint16_t statusbar_elapsedWord(void);

/* --- src/scene.c — .ANI/.RSC interpreter (subsystem 11, §8.5-8.7a) ---- */
int  scene_init(const OSystem *osys);
/* introIndex = the level header's +0x01 byte: the intro-movie index
   (0..9; 0xFF = the level has no cutscene). */
int  scene_playLevelIntro(unsigned introIndex);   /* the cd_file_op body   */
/* [0x627C]: boot_intro_sequence's fast mode (its only writer).  The shared
 * scene player then fades 4 palette entries div 2 instead of 0x100 div 0x19
 * and waits 1 tick per object instead of 6.  The level's .ANI runs clear. */
void scene_setFastMode(int on);
/* Force the next scene_playLevelIntro call to load its .ANI even when the
 * index equals the last one (cd_file_op always reloads; the per-tick player
 * keeps a finished intro finished otherwise). */
void scene_resetIntro(void);
/* The boot intro (boot_intro_sequence, 1000:B590): the full four-stage
 * chain — the Gametek logo (GAMETEK.RSC's display list), the IDI and FIRST
 * cards, then the ITB card + INTRO.RSC's display list over the scrolled
 * bac.bit background — driven by the 202F:0x843C cue list.  Per-tick like
 * the level intro; plays once (a held key aborts the whole chain — every
 * stage's c0af/c0b8 wait polls via boot_abort_poll). */
int  scene_playBootIntro(void);
/* The main loop's per-tick input state; the boot's abort polls (c0c6's
 * [0x48A3] / +0x5ACE bit-7 equivalents) read this cached copy. */
void scene_noteInput(const InputState *in);
unsigned scene_boot_phaseForProbe(void);   /* probe: the boot machine's phase */

/* --- src/level.c — the level definitions (subsystem 9, §2) -------------
 *
 * LEVELS\LA.BIN..LP.BIN: 16 files x 10 records = 160 levels.  The record:
 * the 13-byte header, the goal/author ASCIIZ text, the 8-byte editor
 * metadata, the 640-byte tile map (32x20).
 */
typedef struct {
    unsigned levelId;       /* 0..159                                    */
    uint8_t  style;         /* +0x00 low: the tile style 1..6            */
    uint8_t  introIndex;    /* +0x01 high: the intro-movie index (0xFF=none) */
    uint8_t  goalCount;     /* +0x02 low: the solved-flag threshold      */
    uint8_t  entityLife;    /* +0x02 high: the entity lifetime seed      */
    uint16_t objectTest;    /* +0x04: the object-test mode word          */
    uint8_t  unused06;      /* +0x06: 0 in all 160 shipped records       */
    uint16_t timeA;         /* +0x07: the BCD MMSS limit (difficulty A)  */
    uint16_t timeB;         /* +0x09: difficulty B                       */
    uint16_t timeC;         /* +0x0B: difficulty C                       */
    uint8_t  map[640];      /* 32 columns x 20 rows, row-major           */
    uint32_t mapFileOff;    /* the map's offset inside the L*.BIN image  */
    char     goal[128];     /* the goal text (CR = a line break)         */
    char     author[128];   /* the author text                           */
} LevelData;

int  level_init(const OSystem *osys);
int  level_load(unsigned levelId);          /* 0..159; 0 = ok            */
const LevelData *level_data(void);          /* NULL before a load        */
const uint8_t *level_map(void);             /* == level_data()->map      */
unsigned level_style(void);                  /* [0x17EE]: the record's style */
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
int level_pageFree(unsigned id);
/* The two level maps the walk reads: `0x92A0`'s page bytes (bit 7 = solid, the
 * low 7 bits the step, 1000:E389) and `[0x1FD6]`'s 16-word item records
 * (1000:F25F's occupancy bit). */
/* Field K's first word: the level's two special tile ids ([0x6F20]/[0x6F21]). */
unsigned level_specialTile(unsigned i);
unsigned level_pageByte(unsigned id, unsigned subcol);
unsigned level_itemWord(unsigned id, unsigned row);
unsigned level_itemByte(unsigned id, unsigned off);
int  level_tick(void);                      /* 1 = the level is running  */
/* 1000:3DC0: the view's world origin (the CRTC start's source). */
void level_setView(unsigned x, unsigned y);
/* 1000:5CB6: the camera follows the acting record (update_world's first call,
 * 4 px/frame, clamped to the 512x320 page's pan range). */
void level_followView(void);
/* [0xC]: update_world's gate (set by level_fade_in 1000:336C, cleared by the
 * claim at 1000:3B19 and restored at 1000:3CAB). */
void level_setUpdates(int on);
/** [0x2D8E]: the follower's +-1 px offset walk enabler (1000:DAC7 / 0xF8CE). */
void     level_set_offsetWord(uint16_t v);
uint16_t level_offsetWord(void);
/* 1000:5D37's walk pointer ([0x2D90]) back to the table's first pair. */
void     level_reset_offsetWalk(void);
int  level_updates(void);            /* [0xC] */
unsigned level_viewX(void);
unsigned level_viewY(void);
/* The claim's particles (1000:3E73/0x3FB3/0x3F4A): begin at the appended record
 * with the 16x16 shade table object.c builds from the recipe sprite. */
void level_sparkBegin(unsigned ox, unsigned oy, const uint8_t *shade);
int  level_sparkActive(void);
int  level_sparkPeek(unsigned *x, unsigned *y, unsigned *value);
/* 1000:8526's [0x5ACE] for the play loop's command dispatch (1000:E3F6). */
void level_noteInput(const InputState *in);

/* menu_loop (1000:012A), the MAP/high-score screen after the ending.
 * START: the HIGH.BIT card + the title, key/ACTION exit; the full
 * map/cursor/score tables are the next pass. */
int  map_begin(void);                       /* 1 = the screen is up      */
int  map_tick(const InputState *in);        /* 1 = still up              */

/* check_highscore (1000:41AE), the score/record screen between the level's
 * end page and the post-level newspaper.  START: SCORE.BIT + the score line
 * + the key/ACTION exit and the 0x100-entry fade to 0x48BC (div 0x23). */
/* check_highscore's score terms (1000:41AE): level x members x 10 + the
 * remaining seconds x 10; a lost level is 0.  The placed-object bonuses are
 * the one term the object array (0x3E75) still owes. */
unsigned score_levelScore(void);
/* --- HISCORES.HUM (re_formats.md §11, menu_loop 1000:012A) ------------ */
void     hiscore_init(void);                 /* 1000:069F: load + decrypt  */
void     hiscore_reset(void);                /* the shipped defaults       */
void     hiscore_reload(void);               /* re-read the file           */
void     hiscore_obfuscate(uint8_t *p, unsigned n);  /* 1000:06DE          */
void     hiscore_write(void);                /* 1000:06CC: encrypt + save  */
uint16_t hiscore_best(unsigned level);       /* DS:0x38A + 2*level         */
void     hiscore_setBest(unsigned level, uint16_t value);
unsigned hiscore_count(void);                /* 10                         */
uint32_t hiscore_score(unsigned i);          /* the top-10, descending     */
const char *hiscore_name(unsigned i);
int      hiscore_qualifies(uint32_t score);
int      hiscore_insert(uint32_t score, const char *name);   /* 0x01CD    */
int      hiscore_nameApply(char *name, unsigned column, unsigned *len);

/* --- the MAP screen's letter wheel (apl\alpha.spr, 1000:0705/0F93/0CBE) --- */
void     alpha_expand(const uint8_t *src, uint8_t *dst);
void     alpha_cell(unsigned cell, unsigned angle, unsigned *src, unsigned *dst);
void     alpha_drawWheel(uint8_t *page, unsigned base);
void     alpha_drawName(uint8_t *page, const char *name);
/* --- the MAP screen's frame (apl\band.spr, 1000:12D3/0874/07E6) --------- */
extern unsigned map_boxBottom;               /* [0x8AE]                    */
extern unsigned map_panel;                   /* [0x8A2]: 1..6              */
void     map_bandFill(uint8_t *page);        /* 1000:12D3                  */
void     map_drawBoxEdges(uint8_t *page, unsigned row);   /* 1000:07E6    */
void     map_drawList(uint8_t *page);        /* draw_score_block 1000:0651 */
void     map_drawCongrats(uint8_t *page);    /* 1000:021E..0266            */
unsigned map_congratsDraws(void);            /* 1000:0251's two passes     */
void     map_drawRect(uint8_t *page, unsigned x, unsigned y, unsigned w,
                      unsigned h);          /* 1000:1251 outline          */
unsigned map_transitionStep(void);           /* 0..6, 1000:028C..030A      */
unsigned map_fadeStarts(void);               /* palette_build_fade calls   */
int      map_fadeAt(unsigned i, unsigned *first, unsigned *entries,
                    unsigned *div, unsigned *target);
void     map_drawName(uint8_t *page);        /* 1000:0CBE (ring + typed)   */

/* --- check_highscore's data (1000:41B8..4352) ------------------------- */
unsigned score_members(void);                /* [0x11FC]'s recompute      */
void     score_prepare(void);                /* terms, best, NEW RECORD, totals */
int      score_countersRun(void);            /* 1000:475D: one 10-point step */
int      score_isRecord(void);               /* [0x1DD6]                  */
int      score_blinkTick(void);              /* 1000:467A: 1 = phase flipped */
int      scoreBlinkOn(void);                 /* [0x1DC4]: 1 = the stamp is up */
void     score_stampDraw(uint8_t *page, int on);   /* 1000:46FC / 0x46A3 */
void     score_column(uint8_t *page, uint32_t value, int x, int y, unsigned digits);
void     score_drawColumns(uint8_t *page);   /* 1000:4471                 */
void     score_drawCard(uint8_t *page);      /* 1000:4403, transparent for 0 */
void     score_drawNum3(uint8_t *page, unsigned ax, uint16_t value,
                        unsigned bx, unsigned bp);   /* 1000:44BB / 0x450F */
uint16_t score_bestFor(unsigned level);      /* DS:0x38A + level*2        */
uint16_t score_bestTime(void);               /* [0x1E88]                  */
unsigned score_totalLo(void), score_totalHi(void);    /* [0x592]/[0x594]   */
void     score_setTotal(unsigned lo, unsigned hi); /* the save header's halves */
unsigned score_shownLo(void), score_shownHi(void);    /* [0x1DD2]/[0x1DD4] */
int  score_begin(unsigned value);           /* 1 = the screen is up      */
int  score_tick(const InputState *in);      /* 1 = still up              */
/* level_fade_in (1000:3334): the start chain — fade out of the pre-level
 * screen, the eight update_world frames, the fade in to the working palette,
 * then the bar's ticking gate.  level_begin arms it once; the phases advance
 * in level_tick, so level_starting() is true until the level is playable. */
void level_begin(void);
int  level_starting(void);
/* load_sprite_pal's LOADING screen (1000:1DC0): ten walkers for 0xA0 frames,
 * then the "LOADING!" letters for 0x56.  level_loadingScreen() starts it and
 * reports whether it is showing; level_loadingTick draws one frame. */
int  level_loadingScreen(void);
void level_loadingTick(void);

/* level_run 3060/3062: the level's required humans ([0x17FB], the map's
 * 0xAA count) against the tribe ([0x11FC]).  A tribe that is too small plays
 * the briefing's GAME OVER variant and the level is SKIPPED; [0x1809] then
 * takes entry 1000:00DC -> 002B (the restart-game path), not the next level. */
unsigned level_required(void);
unsigned level_members(void);
void     level_addMember(void);          /* 1000:DE1F: [0x11FC]++ */

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
void level_requestEnd(int won);
/* draw_status_indicator (1000:1FEF): the disc probe + the "PUT THE DISK
 * INTO DRIVE n / PRESS FIRE WHEN READY" screen.  Begin returns 0 when the
 * volume is present (the normal, no-screen path); Tick returns 1 while the
 * screen is up. */
int  level_diskStatusBegin(unsigned disc);
int  level_diskStatusTick(const InputState *in);
void level_diskStatusReset(void);          /* test/lifecycle: drop the screen */
/* the level-end test (object_test_dispatch 1000:35D6's negative return).
 * level_goalReached is the win hook the discovery/claim path sets; a lost
 * tribe member decrements the live count and ends the level at zero. */
void level_goalReached(void);
void level_memberLost(void);
int  level_endTest(void);
int  level_ended(void);
int  level_endHolding(void);
void level_setEnd(int won);             /* 1000:F66D: [0x138E] = 0 */
int  level_outcome(void);            /* [0x138E]: 1 won, 0 extinct */

/* [0x5D31] / [0x11FC] after level_load's LEVEL-board pick: the record's
 * timeA/B/C word and the 12/10/8 starting tribe (entry 1000:009F,
 * render_level_frame 1000:9589). */
uint16_t level_timeLimit(void);
unsigned level_tribeMembers(void);
/* the stamped background image and the BD P block, for the exit/igloo test */
const uint8_t *level_bgForProbe(void);
const uint8_t *level_pBlockForProbe(void);
unsigned level_bgIndexForProbe(void);
const uint8_t *level_cBlockForProbe(void);
/* render_objects' tail (1000:5F54): the 0xA9 background sprites */
void level_drawBgSprites(uint8_t *page, int scrollX, int scrollY);

/* --- src/entity.c — entity table/bytecode (subsystem 10, §12) -----------
 *
 * The ported, closed §12.6 system: tile 0xB3 spawns a dinosaur, a 0xB4 on
 * the same row supplies the far endpoint (entity_find_by_id), and the
 * game itself alternates the dispatched handler (the shared tail's +4 ^=
 * 1 flip) so the dino paces between the two endpoints at ±1 px per tick.
 * All field meanings and the per-tick machine are in re_formats §12.6.
 */
int  entity_init(const OSystem *osys);
void entity_tick(void);
/* 1000:5823 / 0x5894: the entity sprite pass over apl\rex.dat (mode 2, two
 * 0x20-px cells per entity), and the three big-endian frame-table reads it
 * uses (1000:58A5).  Returns 1 when the index is inside the bank. */
void entity_publishBank(const uint8_t *bank, uint32_t size);
/* DEFS field G: the per-level 3-layer 0x100 sprites the frame slots 0xC1+4L+i
 * point at (level_setup_screen writes their far pointers into DS:0x3CAC). */
void object_publishGSprites(const uint8_t *data, unsigned blocks, unsigned n);
/* DEFS fields E and H: the 0xA9 log sprite (frames 0x33..0x37) and the two
 * sprites the frame slots 0x26/0x27 point at. */
void object_publishDEHSprites(const uint8_t *e, const uint8_t *h);
/* 1000:C295 update_list: the placed records' behaviour walk (torch, stepper,
 * sequence, wanderer). */
void object_updateList(void);
/* the level's [0x6532] (BD field F, 0 when absent). */
unsigned level_animFrames(void);
void entity_render(uint8_t *page, int scrollX, int scrollY);
int  entity_frameInfo(unsigned a, unsigned *src, unsigned *rows, unsigned *yoff);

/* One entity record: the DS layout word-for-word (+0x00..+0x0E). */
typedef struct {
    uint16_t x0;        /* +0x0: endpoint A — handler 1's target        */
    uint16_t x1;        /* +0x2: endpoint B — handler 0's target        */
    uint16_t handler;   /* +0x4: the dispatch index (game: 0/1 only)    */
    uint16_t x;         /* +0x6: the live position                      */
    uint16_t y;         /* +0x8: row*16 - 0x30 + 1                      */
    uint16_t hold;      /* +0xA: the beat countdown, seeded 3           */
    uint16_t waypoint;  /* +0xC: the waypoint / animation index         */
    uint16_t life;      /* +0xE: the goal word's high byte              */
} Entity;

/* load_terrain_b (1000:5168): the 25-pair map walk in table order, one
 * handler call per matching cell (CX = col*16, DX = row*16; the spawn's
 * y is row*16 - 0x30 + 1, +3 under the style-6 prologue).  Resets the
 * count first (the level_init analog: the count only — the fixed-table
 * slots keep the exe image's values).  `style` = the record's +0x00 byte
 * (the prologue's [0x17EE]); `lifeSeed` = the goal word's high byte
 * ([0x17F1] -> the +0xE seed).  0 = ok, -1 = no map. */
int entity_spawnFromMap(const uint8_t *map, unsigned style,
                        unsigned lifeSeed);

const Entity *entity_at(unsigned i);   /* NULL past the count */
unsigned entity_count(void);
/* save +0x1656+0x28: [0x64C2] and the first seven 0x10-byte records. */
void entity_saveFixed(uint8_t *dst);
void entity_loadFixed(const uint8_t *src);

/* --- src/object.c — the placed-object array (0x3E75) ------------------
 *
 * 8 records of 0x12 bytes (check_highscore 1000:41D7 walks all eight); the
 * fields and the remaining machine are in object.c's header.  The predicates
 * are object_test_dispatch's (1000:35D6): -1 = true, 0 = false.
 */
void object_init(void);
void     object_blitSprite(uint8_t *page, const uint8_t *src, unsigned rows,
                           unsigned cols, int x, int y);
/* 1000:5C2F draw_objects_pass2: the two TER.DAT slots at DS:0x649A, disabled by
 * object_array_init (0x4CAB) through their +0x10 flag words. */
void     object_terSlotInit(void);
uint8_t *object_terSlotAt(unsigned i);           /* NULL past the two slots */
void     object_publishTer(const uint8_t *bank, uint32_t size);
void     object_renderTer(uint8_t *page, int scrollX, int scrollY);
uint8_t *object_slot(unsigned i);        /* NULL past object_slots()      */
unsigned object_slots(void);
unsigned object_frameSlots(void);       /* the sprite table bound (0x11E) */
void     object_frameForProbe(unsigned frame, unsigned *off, unsigned *rows);
/* the 0x32B9 background-sprite queue (1000:5F54's input) */
unsigned object_bgCount(void);
void     object_bgAt(unsigned i, unsigned *x, unsigned *y);
/* the SAVE GAME state ranges (re_formats.md §10/§12.3): object table
 * (0x3E20, 0x25F), pools (0x6F20, 0x13CA), programs (0x5B37, 0x20),
 * TER slots (0x649A, 0x28) and the 0x6F92 copy (0x54). */
void object_saveTable(uint8_t *dst);   void object_loadTable(const uint8_t *src);
void object_savePool(uint8_t *dst);    void object_loadPool(const uint8_t *src);
void object_savePrograms(uint8_t *dst);void object_loadPrograms(const uint8_t *src);
void object_saveTer(uint8_t *dst);     void object_loadTer(const uint8_t *src);
void object_saveSpans(uint8_t *dst);   void object_loadSpans(const uint8_t *src);
int  object_current(void);               /* [0x1AF2], -1 = none           */
void object_setCurrent(int slot);
int  object_player(void);                /* [0x180A], -1 = none           */
void object_setPlayer(int slot);
int  object_noneType1d(void);            /* 1000:3640                     */
int  object_classMatches(unsigned want); /* 1000:3659                     */
int  object_typeEquals(unsigned type);   /* 1000:3678                     */
int  object_entitiesFinished(void);      /* 1000:3688 (the entity table)  */
int  object_mapCellEquals(unsigned cell); /* 1000:36AB (the map cell)     */
unsigned object_staticSlots(void);       /* the 8 the predicates scan     */
unsigned object_liveCount(void);         /* [0x3E73]; 9 at the level start */
uint8_t  object_slotClass(unsigned slot);/* [0x3E5E + slot]               */
void     object_setSlotClassForProbe(unsigned slot, uint8_t cls);
void     object_setSlotClass(unsigned slot, unsigned cls);
int  object_testDispatch(uint16_t mode); /* 1000:35D6: 1 = goal met       */
/* render_objects' object pass (1000:5D56 -> 0x653C): the base sprite of every
 * live record, drawn at (x - scrollX, y - scrollY). */
void object_publishBank(const uint8_t *bank, uint32_t size);
/* load_terrain_b's per-tile object walk (1000:5168): [0x3E73] = 8, the 20
 * behaviour bytes cleared, then every map cell of the 25 table tiles in
 * table order (0xAA the human into the first free static slot, frame 0x10,
 * type 2; 0xAB the spear, appended, frame 0x60, +8 = 0xFFFF, behaviour 5;
 * the rest per re_map.md); returns [0x3E73]. */
unsigned object_loadTerrainObjects(uint8_t *map);
unsigned object_placedHumans(void);      /* [0x3E20]                       */
/* the map cell under a static record (0xF761 with AX = 8, BX = 0xC); the
 * score's object bonus reads it: 0xA8 -> +1000, 0xA7 -> +2000 (1000:55FA). */
unsigned object_recordMapCell(unsigned slot);
void object_render(uint8_t *page, int scrollX, int scrollY);
/* The adjacent-object op of the entity bytecode (1000:3A6A): the gate and the
 * scan around the *acting* record (`actor`, the entity's own slot), then the
 * red/white flash, the blinks, the animation, the append and the settle.
 * `recipe` = 1/0/3 for the three handlers.  object_tick drives the sequence
 * and supplies the level's palette (the fades return to it). */
/* 1000:E3F6's command dispatch: the acting record's handler for one input word
 * (`[0x5ACE]`).  `handler` is the in-segment address the original calls
 * (0 = none, so the row's `tail` applies); `command` is the 0..7 number the
 * type row was indexed by.  Index 2 is 1000:E609, whose tail holds the human
 * stack's push.  The handler bodies are ported except 0xE7C0 and 0xEBCA
 * (all rows are now ported). */
typedef struct {
    unsigned handler;
    unsigned tail;
    unsigned hook;      /* the row's +0xA: run 1000:D8BD before the tail */
    unsigned command;
} ObjCommand;
ObjCommand object_commandSelect(uint8_t *rec, uint8_t input);

/* 1000:E4B0's outcome for one command. */
#define OBJ_CMD_NONE      0   /* neither a handler nor a tail ran          */
#define OBJ_CMD_HANDLER   1   /* the type row's handler ran                */
#define OBJ_CMD_TAIL      2   /* the row's tail handler ran                */
#define OBJ_CMD_UNPORTED (-1) /* no port */
/* 1000:E4B0..E4F7: select and run one command for the acting record.  Only the
 * ported handlers are dispatched (the two movement states, the leaves, the snap,
 * the selector wrappers and the bare-RET tail). */
int  object_commandRun(uint8_t *actor, uint8_t input);
/* handler test seams: the two bodies and the one-shot gate. */
void object_streamCommand(uint8_t *actor);
void object_clearCountdown(uint8_t *actor);
void object_setCommandTimer(unsigned v);
void object_animatorTick(void);               /* 1000:D6B2 phase 3 */
uint8_t *object_groupAt(unsigned i);           /* 0x6F22 entry */
void object_animatorReset(void);

/* The human stack (`0x6F22`, 8 entries x 0x0E: +0/+2 x/y, bit 15 = free, +4 the
 * animator state, +5 the owner index, +6ff the bit-7-terminated member list).
 * `object_stackPush` is the push block of 1000:E609 (1000:E697) and
 * `object_stackGrab` the one of 1000:E4F8 (1000:E58B), `object_stackPop` is
 * 1000:FDED, the leaves are 1000:E860 / 1000:E8DD, the snap 1000:E963, the
 * selector 1000:F17C, and `object_stackScanDissolve` is 1000:F453 (the scan that
 * finds the group holding a record and dissolves it).  The `0x64AD`/`0x64C1`
 * in-a-stack masks (1000:F38F/F3CA) are written by the two pushes and read by
 * the selector. */
int  object_stackPush(uint8_t *actor);            /* 1 = a group took it      */
int  object_stackGrab(uint8_t *actor);            /* 1 = grabbed, 0 = registered */
/* -1 / 0 / 1 as 1000:FDED; `*below` = the record it leaves in DI and `*entry` =
 * the 0x6F22 entry it was scanning (1000:FDF7's DX, which 1000:F223 reads). */
int  object_stackPop(uint16_t ax, uint8_t **below, uint8_t **entry);
int  object_stackStepOff(uint8_t *actor);         /* 1000:E860: 1/0/-1        */
int  object_stackLeave(uint8_t *actor);           /* 1000:E8DD: 1/0/-1        */
int  object_stackSnap(uint8_t *actor);            /* 1000:E963: 1/0           */
int  object_stackStand(uint8_t *actor);           /* 1000:E99D: 1/0           */
/* 1000:F17C: 0 = done, -1 = nothing found.  The terrain branch is in
 * (1000:F1D7's step probe, 1000:F223's field-K special-tile test). */
int  object_selector(uint8_t *actor, uint16_t xoff, uint16_t frame, uint16_t type);
int  object_stackScanDissolve(unsigned recIndex); /* 1 = a group was dissolved */
/* 1000:FA13 object_group_dissolve, by group slot (the 0x6F22 entry the tail holds
 * in BP when it calls it). */
void object_groupDissolveAt(unsigned slot);
/* 1000:E4F8 / 1000:E609: the two movement states that feed the stack - the feet
 * cell's special tile (field K), a 0x6F92 span, else the grab / the push.
 * Codes: 1 tile rule, 2 span, 3 grabbed/pushed, 4 registered/nothing, -1 nothing. */
int  object_stepDown(uint8_t *actor);
int  object_stepUp(uint8_t *actor);
/* 1000:E93D (left) / 1000:E952 (right): the selector with the wrappers' constants. */
int  object_selectorWalk(uint8_t *actor, int right);

int  object_claimBegin(unsigned recipe, int actor);
int  object_claimActive(void);
void object_claimTick(const uint8_t *levelPal);
/* the scan at 4E91: the first record beside `actor` (|dx| <= 0x20,
 * |dy| <= 8, type <= 2, active) or -1. */
int  object_findNeighbour(int actor);
/* one level frame of the object machine (the frame counter + the claim) */
void object_tick(const uint8_t *levelPal);
int  object_lowPhaseMatch(void);          /* 1000:3600: -1 when met         */
/* 1000:F423's window, against the type row 1000:E467 hands the hook as DI:
 * the row's own words are the "rect", so it never matches on shipped data -
 * the test asserts that over every type and both halves. */
int  object_rowWindowMatches(unsigned type, unsigned half, int x, int y);
/* The ACTION program machine (script_op_dispatch, 1000:3979): ACTION with no
 * direction held runs the acting record's program.  The acting record is
 * [0x3E22]; object_actionRun() performs one press, object_cycleEntity(±1) is
 * the object selector (1000:410A/4147). */
void object_actionRun(void);
void object_pendingRun(void);
/* one frame of the event-4 charge loop (1000:3702); call after the frame */
int  object_chargeTick(void);      /* the [0x1979] dispatch, per frame */
int  object_cycleEntity(int step);
/* 1000:38C3: select record `index` (F1..F8 = scancode-0x3B), skipping a
 * disabled slot or type 0x1D.  Returns 1 when selected. */
int  object_selectByIndex(unsigned index);
/* 1000:3933 (SPACE): home-state the acting record, else step the action row's
 * selected cell.  Returns 1 (the make is consumed). */
int  object_spaceAction(void);
unsigned object_actionEntity(void);
/* level_fade_in's slot-8 init (1000:3344): frame 0x17, type 0x1A. */
void object_levelStartRecord(void);
/* 1000:D64F: the active-human marker (record 8) follows the acting record,
 * bobs through its type word and expires; update_world calls it every frame. */
void object_markerTick(void);
void object_setActionEntity(unsigned index);
unsigned object_actionProgram(void);
unsigned object_actionPending(void);
void object_actionSetCursor(unsigned cursor);
void object_actionSetProgram(unsigned index);   /* [0x5B56], 1..6        */
unsigned object_actionFault(void);
/* 1000:83B7 driver_call: the last sfx number a handler asked for (the audio
 * surface has no sfx entry until M6, so the request is only recorded). */
unsigned object_lastSfx(void);
/* [0x6FE6]: the marker 1000:EAE6/0xF1B2 write (no ported reader yet). */
int      object_flag6fE6(void);
void     object_driverCall(unsigned n);   /* 1000:83B7 from another module */
/* [0x13A4]: the ACTION press's latch as the command handlers see it. */
int  object_actionLatched(void);
void object_setActionLatched(int on);              /* 7 = op 7 was dispatched */

/* The arch's fatal route for the op-7 halt (NULL detaches it, the tests do
 * that on purpose); level_init attaches the game's OSystem. */
void object_attachOsys(const OSystem *osys);

/* --- src/script.c — interpreter skeleton (re_scripts.md) -------------- */
int  script_init(const OSystem *osys);

/* --- src/script.c — the frame-keyed cue interpreter (§8.7a) -------------
 * The boot intro's event script: 25 verbatim cues from 202F:0x843C,
 * dispatched on the exact frame (JE) through a 7-command dispatch. */
typedef struct {
    unsigned frame;         /* the current frame (the player advances it) */
    unsigned groupDwell;    /* cmd 7: frames per display-list group       */
    int      scrollOn;      /* cmd 3 on / cmd 2 off                       */
    unsigned scrollDelta;   /* cmd 6: the scroll advance per frame        */
    unsigned transitionIdx; /* cmd 1: run_screen_transition index         */
    int      quit;          /* cmd 5: the quit flag                       */
    int      done;          /* the frame target reached                   */
    int      seekPending;   /* cmd 4 fired: the player must seek          */
    unsigned seekFrame;     /*    ...to this frame                        */
    const uint16_t *cues;   /* [frame][cmd][arg] triples, verbatim        */
    unsigned cueCount;
    unsigned cursor;        /* the next cue to fire                       */
} CueScript;

/* The boot intro's verbatim cue list (25 cues, frame target 387). */
void     script_introStart(CueScript *s);
/* Fire every cue whose frame == `frame` (JE dispatch). */
void     script_fire(CueScript *s, unsigned frame);
int      script_quit(const CueScript *s);
int      script_done(const CueScript *s);
unsigned script_groupDwell(const CueScript *s);
int      script_scrollOn(const CueScript *s);
unsigned script_scrollDelta(const CueScript *s);
unsigned script_transitionIndex(const CueScript *s);
int      script_seekPending(const CueScript *s);
unsigned script_seekFrame(const CueScript *s);
void     script_seekDone(CueScript *s);

/* --- src/audio.c — tune/sfx state machine (subsystem 7) --------------- */
int  audio_init(const OSystem *osys);
/* The frame web's music gate (1000:A5E8): `off` = the MUSIC board's counter
 * [0x5E5B] non-zero -> music_stop, zero -> music_resume.  One call per room
 * tick.  The M5 stub's stop/start is the whole observable (M6 adds the tune
 * handle). */
void audio_musicGate(unsigned off);

/* --- src/frontend.c — the front-end room (subsystem 8, §10) ------------
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
int  frontend_init(const OSystem *osys);
int  frontend_tick(const InputState *in);
/* [0x5F09]: the code match's 1-based index = the level the room starts
 * (string i of levels\codes.bin = level i; #1 "DARWIN"; a full miss
 * leaves 161 — the original's own edge).  entry's PUSH AX/POP AX move
 * it to [0x22]; game_main does the same into its level counter. */
unsigned frontend_selectedLevel(void);
/* entry's JMP LAB_002B (the pause's RESTART THIS GAME): the room's setup
 * runs again; the option counters persist. */
void frontend_restart(void);
/* The four option counters — the descriptor tables' +7 bytes at
 * 202F:0x5E5B/0x5E65/0x5E6F/0x5E79, where the badge's frame IS the
 * setting.  The read side uses this accessor: level.c takes LEVEL, the room
 * hands MUSIC to audio_musicGate, scene.c takes ANIMATIONS, and game_main
 * hands the RESTORE GAME slot to save_loadSlot (entry 1000:004C's load). */
enum {
    FRONTEND_OPT_MUSIC = 0,     /* ON/OFF: the frame web's music gate      */
    FRONTEND_OPT_LEVEL = 1,     /* EASY/NORMAL/HARD: time A/B/C, 12/10/8    */
    FRONTEND_OPT_GAME  = 2,     /* OFF/1..20: the save slot                */
    FRONTEND_OPT_ANIM  = 3      /* ON/OFF: cd_file_op off (no cutscenes)   */
};
unsigned frontend_option(unsigned idx);
unsigned frontend_cellForProbe(void);   /* [0x4220]: the walk's rail node  */
int      frontend_nameScreenForProbe(void); /* the name screen's phase      */
const char *frontend_nameForProbe(void);    /* the typed name (0x61E0)      */

/* --- src/party_intro.c — the level-intro party screen (subsystem 14) --
 *
 * level_intro_screen (1000:2508): the fresh-load briefing.  The screen is
 * reached from FLOW_LEVEL's fresh-load path (level_run's 307a call; the
 * .ANI cutscene is not part of this screen — see next_step.md's note).
 * party_intro_start seeds and runs the setup (the 5 asset loads, the
 * double .BIT bake with the flip, the master-palette copy, the 35-step
 * fade-in); party_intro_tick is one display period of the machine, and
 * returns 1 while the screen plays, 0 after its exit's music_stop.
 * A headless run without the disc data skips the screen (start returns 0).
 *
 * Facts: re_map.md → "The level-intro party screen decoded end to end
 * (2026-10-29)" (all render gates closed); the memory model and the
 * machine notes are in party_intro.c's header.
 */
/* level_intro_screen 2739 (need > members): the GAME OVER briefing variant
 * (the same art; GAME OVER / YOUR TRIBE IS TOO SMALL / IN THE RACE / PRESS
 * FIRE, one key/fire exit). */
void party_intro_setGameOver(int on);
unsigned party_intro_score(void);            /* g_score [0x592] */
void party_intro_setValues(unsigned level, unsigned need, unsigned members,
                           unsigned score, const char *tribe);
void party_intro_seedScripts(const uint8_t *bytes, unsigned len);
void party_intro_seedSlots(const uint8_t *raw);   /* the 128 B at DS:0xFA8 */
int  party_intro_start(void);
int  party_intro_tick(const InputState *in);
void party_intro_formatScore(unsigned v, char *out);   /* 2b1b/2b70       */
void party_intro_compose(uint8_t *page320x200); /* the law compose for the fixtures */
const uint8_t *party_intro_workpal(void);   /* the 0x4bbc stand-in (diagnostics) */
const uint8_t *party_intro_palUpper(void); /* 0x4d3c = 0x4bbc + 0x180 (diagnostics) */
/* --- src/textpage.c — the in-level text page (draw_map_object 9EA3) ----
 *
 * The dialog box + its lines + the option markers, ported from
 * draw_scroll_text / draw_map_object / blit_sprite_planar / blit_text_tile
 * (re_map.md §"The in-level pause menu").  The box tiles are APL\BOX.SPR
 * (9 x 16x16, loaded with the level's assets).
 */
void textpage_publishBox(const uint8_t *box);
/* `text` = the block (CR-separated lines, NUL-terminated); cursorRow >= 0
 * draws the marker pair on that option row (grid = the cell pair). */
void textpage_draw(uint8_t *page, const char *text, int cursorRow,
                   unsigned cursorCol, int grid);
/* one centred line in a solid colour (no box underneath) — the disk prompt,
 * the score screen and the MAP's list use this. */
void textpage_drawLine(uint8_t *page, const char *line, unsigned row,
                       uint8_t colour);
/* draw_text's AX: the line centred on an arbitrary x. */
void textpage_drawLineAt(uint8_t *page, const char *line, unsigned centreX,
                         unsigned row, uint8_t colour);
/* blit_text_tile (1000:47D5) proper: `mask` OR'd into the page (the OR ALU at
 * 1000:9F76 + write mode 2).  The box text and the cutscene headline use it,
 * so their shading is the box fill tile's own EB..E4..EB ramp. */
void textpage_drawLineMask(uint8_t *page, const char *line, unsigned row,
                           uint8_t mask);
void textpage_drawLineMaskAt(uint8_t *page, const char *line, unsigned centreX,
                             unsigned row, uint8_t mask);

/* --- src/menu.c — the in-level dialogs (pause menu + SAVE GAME) -------
 *
 * The 5-option pause menu (202F:0x5BBC, A167) and the SAVE GAME number grid
 * (202F:0x5CD8, render_frame_loop's cursor): the behaviour is the original's
 * (0-based rows, wrap, ACTION/Enter selects; the countdown is suspended).
 * The dialog draws through draw_map_object (1000:9EA3); known deviations are
 * in port_deviations.md.
 */
void     menu_init(const OSystem *osys);
void     menu_openPause(void);          /* [0x5B12] := 0, cursor 0         */
void     menu_openTimeUp(void);         /* [0x5E44]'s 3-option block        */
int      menu_timeUp(void);             /* the dialog currently open        */
int      menu_active(void);             /* 1 while a dialog is up          */
int      menu_tick(const InputState *in);   /* 1 = still up; draws         */
void     menu_draw(void);
int      menu_result(void);             /* the 0-based pause choice, -1    */
unsigned menu_slot(void);               /* the grid's game number 1..20    */
void     menu_close(void);

/* --- src/save.c — the save-game format (load side) ---------------------
 *
 * re_formats.md §10/§10.0: the 0x1800 file `SAVE<'A'+n-1>` (the fs module's
 * name for the original's `\humans\cdrom\saveX`).  The load is entry:004C's
 * path: the slot is the front-end room's RESTORE GAME counter (GAME 1..20),
 * and a hit overrides the room's level, the time limit and the tribe.
 */
int  save_init(const OSystem *osys);
/* Load the slot (1..20; 0 = GAME OFF).  Resets the state first; returns 1
 * when the file's header was read (the original's "save present" [0x34]). */
int  save_loadSlot(unsigned slot);
int  save_present(void);          /* [0x34]                              */
uint16_t save_level(void);        /* +0x00, 1..160 -> [0x22]             */
uint16_t save_time(void);         /* +0x07 word -> [0x5D31]              */
unsigned save_members(void);      /* +0x0B -> [0x1970]                   */
/* The whole 0x1800 buffer ([0x1C92]): the header and the state ranges are
 * carried verbatim (the ranges have no port consumer yet). */
const uint8_t *save_blob(void);
void save_restoreState(void);   /* after level_load: the 0x1800 ranges */
/* level_run 1000:30B4: [0x34] := 0 after the level starts, so a later
 * "restart this level" takes the LEVEL board's time again. */
void save_consume(void);
/* SAVE GAME (save_progress 1FE7:0000): the live header and the five state
 * ranges, 0x1800 bytes; save_restoreState puts them back after level_load.
 * The +0x17F0 parameter block is the image's [0x5B22] defaults. */
int  save_writeSlot(unsigned slot, unsigned level, uint16_t timeWord,
                    unsigned members);

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
    FLOW_ATTRACT = 0,      /* game_loop 1000:131E: the effect cycle      */
    FLOW_MENU,             /* the map/level-select room                   */
    FLOW_LEVEL,            /* a level is installed and running            */
    FLOW_ENDING            /* entry 1512: cd_file_op(6 lost / 7 won),
                            * draw_status_indicator, menu_loop (the MAP)   */
} GameFlow;

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

/* Advance the flow one step with the polled input; the loop calls it once
 * per tick boundary.  Returns the new state. */
GameFlow gameflow_step(GameFlow state, const InputState *input);
/* The level index the MENU exit chose (0-based; [0x22]-1) — the save's
 * +0x00 when a slot loaded, otherwise the room's code match.  Probe for the
 * save/restore test (the same role as attract_phaseForProbe). */
unsigned gameflow_levelForProbe(void);
/* The pause menu's five 0-based results (dispatch_by_result_a167 1000:38EA);
 * returns the flow the action leaves, sets *quit for QUIT THIS GAME. */
GameFlow gameflow_pauseAction(int choice, int *quit);
unsigned gameflow_partyScore(void);          /* the briefing's [0x592] low word */
const char *gameflow_partyTribe(void);       /* 0x1189: the level's author   */
/* scan_id_list 1000:386E event 4: ESC opens the 0x5BBC pause block. */
/* scan_id_list 1000:386E: the DS:0x1989 key list.  Handles ESC (the 0x5BBC
 * pause block), F1..F8 (select the record by scancode), ','/'.' (previous /
 * next record), SPACE (the action-icon step / home state) and F10 (the key
 * wait).  Returns 1 when an entry ran; a non-bit7 entry whose gate [0xC] is
 * closed returns 0 (1000:38AD). */
int      gameflow_keyList(const InputState *in);
/* [0x5BBA]: the goal page (scroll_map 1000:9E51) is up. */
int      gameflow_goalPageUp(void);
/* 1000:3954's key wait (F10) owns the input and blocks the world. */
int      gameflow_keyWaitActive(void);
/* The time-up dialog's 3 results (1000:34F9's inline dispatch): 0 restart
 * this level (level_restore), 1 restart this game (the room), 2 quit. */
GameFlow gameflow_timeUpAction(int choice, int *quit);
int      gameflow_restartPending(void);   /* [0x1809] = 0xFF: restart game */
/* entry LAB_00CD's post-level step: [0x22]++ / [0x26]-- -> the next level of
 * the 160; when they run out this returns FLOW_ATTRACT (the caller starts
 * FLOW_ENDING: scene 7 -> the disc probe -> the MAP -> the attract). */
GameFlow gameflow_afterLevel(unsigned *levelCurrent);
/* entry 14E6/14FF and 150C/1512: the post-level newspaper index (0xFFFF =
 * none: lost, or the header byte 0xFF) and the ending scene (6 lost,
 * 7 COMPLETE). */
unsigned gameflow_postLevelScene(int won, unsigned introIndex);
unsigned gameflow_endingScene(int won);
/* level_run 3060/3062's gate: the level's required humans vs the tribe. */
int      gameflow_tribeTooSmall(unsigned need, unsigned members);

#endif /* HUMANSRE_GAME_H */
