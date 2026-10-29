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
/* The [0xF7E] analog: the disc that name-based loads come from (0..2 =
 * HUMANS.000/.001/.002; the original opens ONE volume file built from the
 * [0xF7E] disc number and never scans across discs). */
void     res_setDisc(unsigned disc);
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

/* --- src/palette.c — fades/DAC (subsystem 4, §6) ---------------------- */
int  palette_init(const OSystem *osys);
void palette_tick(void);   /* palette_fade_step 1000:8bfd, once per tick  */
/* palette_load_6bit (1000:8d24): the 202F:0x4BBC working palette := 768
 * 6-bit bytes.  The RAM copy only — the DAC moves through fades or
 * palette_setDac. */
void palette_loadWorking(const uint8_t *p6);
const uint8_t *palette_working(void);   /* the 0x4BBC buffer              */
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
 * documented latency deviation are in attract.c's header and next_step.md.
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
/* [0x5B12]: the ticking gate (the in-level dialogs clear it). */
void statusbar_setActive(int on);
/* The countdown word for the save's +0x07: minutes<<8 | BCD seconds. */
uint16_t statusbar_timeWord(void);

/* --- src/scene.c — .ANI/.RSC interpreter (subsystem 11, §8.5-8.7a) ---- */
int  scene_init(const OSystem *osys);
/* introIndex = the level header's +0x01 byte: the intro-movie index
   (0..9; 0xFF = the level has no cutscene). */
int  scene_playLevelIntro(unsigned introIndex);   /* the cd_file_op body   */
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
int  level_tick(void);                      /* 1 = the level is running  */
/* level_fade_in (1000:3334): the start chain — fade out of the pre-level
 * screen, the eight update_world frames, the fade in to the working palette,
 * then the bar's ticking gate.  level_begin arms it once; the phases advance
 * in level_tick, so level_starting() is true until the level is playable. */
void level_begin(void);
int  level_starting(void);

/* level_run 3060/3062: the level's required humans ([0x17FB], the map's
 * 0xAA count) against the tribe ([0x11FC]).  A tribe that is too small plays
 * the briefing's GAME OVER variant and the level is SKIPPED (the entry
 * state machine's [0x22]++/[0x26]-- runs instead of the level). */
unsigned level_required(void);
unsigned level_members(void);

/* level_restore (1000:35AF): [0x11FC] := [0x1970] — the members the level
 * started with, before the level's own joins (level_start 1000:3032 then
 * re-picks the time; the port re-runs level_load, so the snapshot is put
 * back afterwards). */
void level_restoreMembers(void);

/* level_run's end: the play loop exits when object_test_dispatch
 * (1000:35D6) returns negative, and [0x138E] (0xFF won / 0 extinct) then
 * selects draw_level_text's page.  That dispatch is not ported yet (the
 * object/command system is pass 11's), so this is the stub hook the object
 * system will call; level_requestEnd(1) = the level was completed. */
void level_requestEnd(int won);
int  level_ended(void);
int  level_outcome(void);            /* [0x138E]: 1 won, 0 extinct */

/* [0x5D31] / [0x11FC] after level_load's LEVEL-board pick: the record's
 * timeA/B/C word and the 12/10/8 starting tribe (entry 1000:009F,
 * render_level_frame 1000:9589). */
uint16_t level_timeLimit(void);
unsigned level_tribeMembers(void);

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

/* --- src/menu.c — the in-level dialogs (pause menu + SAVE GAME) -------
 *
 * The 5-option pause menu (202F:0x5BBC, A167) and the SAVE GAME number grid
 * (202F:0x5CD8, render_frame_loop's cursor): the behaviour is the original's
 * (0-based rows, wrap, ACTION/Enter selects; the countdown is suspended).
 * The dialog's exact renderer (draw_map_object 1000:9EA3) is not ported yet;
 * the port draws the block with TEXT.SPR on the flat page (see menu.c).
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
/* level_run 1000:30B4: [0x34] := 0 after the level starts, so a later
 * "restart this level" takes the LEVEL board's time again. */
void save_consume(void);
/* SAVE GAME (save_progress 1FE7:0000): the header from the live state, the
 * 0x1800 file.  The object/entity ranges are written ZERO (unported) — the
 * port's saves are header-complete, NOT blob-compatible. */
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
    FLOW_LEVEL             /* a level is installed and running            */
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
/* The time-up dialog's 3 results (1000:34F9's inline dispatch): 0 restart
 * this level (level_restore), 1 restart this game (the room), 2 quit. */
GameFlow gameflow_timeUpAction(int choice, int *quit);
/* entry LAB_00CD's post-level step: [0x22]++ / [0x26]-- -> the next level of
 * the 160, or COMPLETE (the ending + the MAP screen, then the attract). */
GameFlow gameflow_afterLevel(unsigned *levelCurrent);
/* level_run 3060/3062's gate: the level's required humans vs the tribe. */
int      gameflow_tribeTooSmall(unsigned need, unsigned members);

#endif /* HUMANSRE_GAME_H */
