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

/* --- src/font.c — glyphs/packed ASCII (subsystem 12, §9) -------------- */
int  font_init(const OSystem *osys);

/* --- src/statusbar.c — the bottom bar (subsystem 13, §12.7) ----------- */
int  statusbar_init(const OSystem *osys);
void statusbar_tick(void);

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

#endif /* HUMANSRE_GAME_H */
