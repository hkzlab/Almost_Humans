/* level.c — the level definitions (game_structure.md §1 subsystem 9,
 * re_formats.md §2): LEVELS\LA.BIN … LP.BIN, 16 files x 10 records = 160
 * levels.
 *
 * Record layout (re_formats.md §2/§2.1a, census-verified over all 160):
 *
 *   +0x00  13-byte header:
 *            u16 style       low byte = the tile style 1..6 -> DEFS\BD<n>.BIN
 *                            high byte (+0x01) = the intro-movie index /
 *                            disc selector (0xFF = no cutscene)
 *            u16 goal count  low byte = the solved-flag threshold;
 *                            high byte (+0x03) = the entity lifetime seed
 *            u16 object-test mode (+0x04)
 *            u8  unused (+0x06; 0 in all 160 records)
 *            u16 time limit A (+0x07, BCD MMSS)
 *            u16 time limit B (+0x09), C (+0x0B)
 *   goal   ASCIIZ ("\r" = a line break)
 *   author ASCIIZ
 *   6      constant signature 00 20 00 14 00 00
 *   2      varying (the editor metadata tail)
 *   640    tile map, 32 columns x 20 rows, row-major
 *
 * The level id is NOT stored in the file: load_level takes it and builds
 * the name LEVELS\L<'A'+id/10>.BIN, record id%10 (1000:30BC).
 *
 * The map start is deterministic: the author's NUL + 8 bytes. The next
 * record begins at the first candidate at or after the map's end (the
 * census walk, tools/level_census.py) — some records carry a 0/1-byte
 * alignment trailer after the map.
 */
#include "level.h"
#include "entity.h"
#include "font.h"
#include "frontend.h"
#include "object.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "save.h"
#include "statusbar.h"
#include "textpage.h"
#include "video.h"
#include "byteorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_LEN   640u
#define MAP_COLS  32u
#define MAP_ROWS  20u
#define REC_HDR   13u
#define SIG_LEN   6u
#define FIELD_LEN 8u

/* --- the level's visual assets (re_formats.md §3-§5) -------------------
 *
 * DEFS\BD<style>.BIN: the defs walk — the loader steps the buffer
 * sequentially (array_max_word over the A/B offset tables, then the fixed
 * blocks); field L (the u16 background index) selects BACKS\BG<n>.RAW.
 * LEVELS\BS<style>.BLK: the 169x16x16 tile atlas behind a 0x400 APL header.
 * BACKS\BG<n>.RAW: 512x320 chunky + a 768-byte 6-bit palette; draw_map_digit
 * copies only its first 64 entries (0xC0 bytes) into the DAC. */
#define VIEW_W  320u
#define VIEW_H  200u
#define WORLD_W 512u
#define WORLD_H 320u

#define ATLAS_TILES 169u

/* level_fade_in (1000:3334) — the level's start chain.  The original blocks
 * in the foreground for each fade (wait_palette_fade_done) while the ISR
 * steps it; the port's loop is tick-driven, so the chain advances one phase
 * per level_tick and the play loop only starts once it is done. */
enum {
    LVSTART_IDLE,
    LVSTART_VIDEO_BASE,
    LVSTART_VIDEO_BAR,
    LVSTART_LOADING,
    LVSTART_FADE_OUT,
    LVSTART_WARMUP,
    LVSTART_FADE_IN,
    LVSTART_PLAYING
};
static struct {
    uint8_t live, value;
    uint16_t x, y, age;
} spark[0x58];

/* The BD walk (re_formats.md §4): returns the end offset (== the file size
 * for all six shipped files), 0 on a bounds failure.  The doc's "D" row —
 * the 5 far pointers — is loader-constructed, 0 file bytes; row J advances
 * by the LITERAL si += word[si] (the length word is part of the skip), then
 * 8 bytes; field M (a u16 count + 4-byte entries) is the palette-rotation
 * table, handed back to the caller.  Byte-exact over BD1..BD6 (2026-10-23). */
typedef struct BdPage {
    const uint8_t *aTable; /* field A: the per-tile slope profiles  */
    uint32_t aSize;
    const uint8_t *items; /* field B: 256 word offsets             */
    uint32_t itemsSize; /* maxB + 0x20: the record area          */
    const uint8_t *jBytes; /* field J: the 8 bytes at 0x1E98        */
    const uint8_t *rowBlock; /* field I: 0xC0 B (the 0x80..0xBF bank) */
    const uint8_t *nList; /* field N: count + 17-byte {id, 16}     */
    uint32_t nCount;
    const uint8_t *oList; /* field O: count + 16 id bytes          */
    uint32_t oCount;
    const uint8_t *cBlock; /* field C: the 0xA9 background sprites  */
    const uint8_t *eBlock; /* field E: the 0x80-byte A9 log sprite    */
    const uint8_t *hBlock; /* field H: the two 0x100-byte sprites     */
    const uint8_t *gBlock; /* field G: 3 layers x n x 0x100 sprites */
    uint32_t gCount; /* n (blocks per layer)                  */
    const uint8_t *pBlock; /* field P: the 0x1000-byte exit art     */
    uint32_t pSize;
} BdPage;

/* --- draw_status_indicator (1000:1FEF) ---------------------------------
 *
 * The disc-presence screen.  It is NOT a normal level screen: at the head it
 * opens the volume file for [0xF7E] (1000:20CF); if the file opens, it
 * returns at once.  Only a failed open draws the "PUT THE DISK INTO DRIVE n /
 * PRESS FIRE WHEN READY" page, fades its palette in (0x100 div 0x1E),
 * animates, retries the open on a key/ACTION, and on success fades to
 * [0x48BC] (black), holds 0x4B frames and returns.
 *
 * The art: APL\DISKIMG1.DAT is consumed by blit_sprite_to_page
 * (1000:2105): 4 planes, source stride 4, 0x1E40 = 7744 bytes per plane,
 * written to the page at [0x5AC4] (= 0 here).  The port converts those
 * planes to its chunky page with the same bit order.  The prompt's text is
 * drawn with the game's font (APL\TEXT.SPR ships on every disc); the
 * original draws the same two lines through the text renderer.
 */
enum {
    DISK_OFF,
    DISK_FADE_IN,
    DISK_WAIT,
    DISK_FADE_OUT,
    DISK_HOLD
};

static const uint8_t recSig[SIG_LEN] = { 0x00, 0x20, 0x00, 0x14, 0x00, 0x00 };

/* The 202F:0x6287-equivalent: the intro cue state lives in script.c; the
 * level's own state: */
static LevelData level_data_cur;
/* The writable map the game plays on: 1000:3138 copies the record's 640 bytes
 * to [0x1FA0], and 1000:51F9 (the 0xA9 handler) marks two cells of it.  Keeping
 * the copy means the parsed record stays byte-exact (tests/test_level). */
static uint8_t levelMapCopy[640];
static int32_t level_loaded;
static uint8_t *boxSpr;           /* apl\box.spr (the dialog tiles)      */
static uint8_t *objBank;          /* apl\sprites.dat (the object sprites) */
static uint8_t *entBank;          /* apl\rex.dat (the entity sprites)     */
static uint32_t entBankSize;
static uint8_t *terBank;          /* apl\TER.dat (the 0x649A slots)       */
static uint32_t terBankSize;
static uint32_t objBankSize;
static uint32_t boxSize;
static uint8_t levelPal[768];     /* the working palette 202F:0x14D3     */

/* --- load_sprite_pal's LOADING screen (1000:1DC0) ----------------------
 * 0xA0 frames of ten walkers (APL\LOADING.APL blocks 0..0x10, two 5-strong
 * groups converging at y=0x6E), then 0x56 frames of the eight "LOADING!"
 * letters (blocks 0x11..0x18) falling to y=0x56.  Both lists are the static
 * templates at 202F:0xD7E and 202F:0xDE4: [u16 count][{u16 x, u16 y,
 * s16 dx, s16 dy, u16 sprite}].  The sprite blit skips x outside
 * 0x40..0x1A0 (blit_sprite_scaled 1000:1F5A) and the templates' 0x25
 * sprite ids are past the 26-frame bank, so those entries draw nothing. */
static uint8_t *loadingBank;
static uint32_t loadingSize;
static uint8_t loadList[10u * 10u];
static uint8_t
    loadWalkerSnap[10u * 10u];   /* the walkers at the letters switch */
static uint32_t loadWalkerCount;
static uint8_t loadPal[768];          /* dicon+0x18: the screen's DAC    */
static uint8_t *loadDic;               /* BACKS\DICON.APL: the preview card */
static uint32_t loadDicSize;
static uint32_t loadEntries;
static int32_t loadCx, loadReload; /* the particle counter [0xD7A]/[0xD7C] */
static uint32_t loadLeft; /* particle_frame's CX             */
static int32_t loadPhase; /* 0 idle, 1 walkers, 2 letters, 3 done */

static const uint8_t loadWalkers[2u + 10u * 10u] = {
    0x0A, 0x00, 0x40, 0x00, 0x6E, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x00, 0x6E, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00,
    0x6E, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x6E, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6E, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x25, 0x00, 0x90, 0x01, 0x6E, 0x00, 0xFF, 0xFF, 0x00, 0x00,
    0x08, 0x00, 0xA0, 0x01, 0x6E, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x08, 0x00,
    0xB0, 0x01, 0x6E, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x08, 0x00, 0xC0, 0x01,
    0x6E, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x08, 0x00, 0xD0, 0x01, 0x6E, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x25, 0x00
};
static const uint8_t loadLetters[2u + 8u * 10u] = {
    0x08, 0x00, 0xB0, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x02, 0x00, 0x11, 0x00,
    0xC0, 0x00, 0x19, 0x00, 0x00, 0x00, 0x02, 0x00, 0x12, 0x00, 0xD0, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x02, 0x00, 0x13, 0x00, 0xE0, 0x00, 0x17, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x14, 0x00, 0xF0, 0x00, 0x16, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x15, 0x00, 0x00, 0x01, 0x15, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x16, 0x00, 0x10, 0x01, 0x14, 0x00, 0x00, 0x00, 0x02, 0x00, 0x17, 0x00,
    0x20, 0x01, 0x13, 0x00, 0x00, 0x00, 0x02, 0x00, 0x18, 0x00
};
static int32_t startPhase = LVSTART_IDLE;
static uint32_t warmupLeft;
/* the fade-to-black target: 0x48BC, the ramp the boot leaves zeroed */
static const uint8_t blackPal[768];
/* level_video_init's palette state at its fades (32F4/3309): 0x14D3 holds
 * only the bar's bank (0xC0..0xFF) at that point, everything else still
 * zeroed by level_setup — the screen goes black except the bar before
 * level_fade_in runs.  Installing it at the chain's start keeps the port's
 * first level frame off the pre-level screen's palette. */
static uint8_t palBarOnly[768];
static uint16_t timeLimit; /* [0x5D31]: the difficulty's BCD pick  */
static uint32_t tribeMembers; /* [0x11FC]: 12/10/8 by the difficulty  */
static const uint8_t zeroPal[768]; /* the zeroed 0x14D3 (1000:3170) */
static uint32_t viewX, viewY;
/* [0xC]: update_world's (1000:80AD) gate.  level_fade_in sets it at 1000:336C,
 * before its eight frames; the claim clears it (1000:3B19) and restores it
 * (1000:3CAB), so the world is frozen while the claim's blocking sequence
 * runs.  Zero until the level's own chain reaches that store. */
static int32_t worldUpdates;
/* [0x2D8E]: the follower's +-1 px offset walk enabler.  Its two writers are
 * 1000:DAC7 (the entity/collision path) and 0xF8CE (the fall machine), both
 * ported; the walk itself over 0x2D92..0x2DA2 is level_offsetWalk below. */
static uint16_t levelOffsetWord = 0xFFFFu; /* [0x2D8E]: bit 15 idles the walk */
/* DS:0x2D92..0x2DA4 - the follower's five offset pairs, read through the
 * pointer at [0x2D90] (0x2D92 at rest).  [0x2DA2]'s pair is (1, 1). */
static const int16_t levelOffsetPairs[10] = {
    0, 0, 1, -1, -1, 1, -1, -1, 1, 1,
};
static uint16_t levelOffsetPtr = 0x2D92u; /* [0x2D90] */
static uint32_t levelRequired; /* [0x17FB]: the map's 0xAA count        */
static uint32_t membersAtStart; /* [0x1970]: the members at level start  */
static int32_t endRequested; /* the play loop's exit (35D6's -1)      */
static int32_t endSoundSent; /* 1000:34E6's 0x83B7 call, once          */
static uint32_t playExitHold; /* 1000:34E6: the 15 x 1000:8B56 waits    */
static int32_t goalHit; /* the discovery/claim win hook          */
static int32_t endWon; /* [0x138E]: 1 won, 0 extinct            */
/* The chained screen the original draws into: one byte = 4 pixels, 88 bytes per
 * row (352 px, of which the visible window is 320 — that slack is what lets the
 * CRTC start sit on a 4-px column boundary, 1000:3DC0's `y*88 + x/4`). */
static uint8_t lvlPlane[VIEW_H * 88u][4];

/* --- the claim's particles (1000:3E73 init, 0x3FB3 step, 0x3F4A flush) ------
 * The list at 0x48BC is 0x58 slots of 8 bytes {live, value, x, y, age}.  The
 * tables are the image's: the age -> direction walk at 0x1B03, the eight
 * directions at 0x1B56 and the spawn nibbles at 0x1B76. */
static const uint8_t sparkPath[83] = {
    0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x01u, 0x01u,
    0x01u, 0x01u, 0x02u, 0x01u, 0x02u, 0x02u, 0x01u, 0x02u, 0x02u, 0x02u, 0x02u,
    0x02u, 0x02u, 0x03u, 0x02u, 0x02u, 0x03u, 0x02u, 0x03u, 0x03u, 0x03u, 0x03u,
    0x03u, 0x04u, 0x03u, 0x04u, 0x04u, 0x03u, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u,
    0x04u, 0x05u, 0x04u, 0x04u, 0x05u, 0x04u, 0x05u, 0x05u, 0x05u, 0x05u, 0x05u,
    0x06u, 0x05u, 0x06u, 0x06u, 0x05u, 0x06u, 0x06u, 0x06u, 0x06u, 0x06u, 0x06u,
    0x07u, 0x06u, 0x06u, 0x07u, 0x06u, 0x07u, 0x07u, 0x07u, 0x07u, 0x07u, 0x00u,
    0x07u, 0x00u, 0x00u, 0x07u, 0x00u, 0x00u,
};

static const uint8_t sparkStream[256] = {
    0x88u, 0x45u, 0xE1u, 0x08u, 0x6Bu, 0xDBu, 0x63u, 0xC4u, 0x3Bu, 0x8Eu, 0xFEu,
    0xA0u, 0xD8u, 0x1Eu, 0xA6u, 0x26u, 0xBBu, 0x32u, 0x67u, 0xF4u, 0x4Fu, 0x81u,
    0xC9u, 0x4Du, 0x8Bu, 0x61u, 0xF8u, 0x0Cu, 0x5Bu, 0x11u, 0x65u, 0xA2u, 0x38u,
    0x13u, 0x78u, 0xACu, 0x19u, 0xC1u, 0x6Eu, 0x94u, 0x4Au, 0xCDu, 0x74u, 0x2Eu,
    0x52u, 0x0Fu, 0x47u, 0xE9u, 0x92u, 0x7Cu, 0x2Au, 0x34u, 0x85u, 0xA9u, 0x31u,
    0xCBu, 0x1Bu, 0xB2u, 0xAEu, 0xE6u, 0x02u, 0x43u, 0xEBu, 0xB3u, 0x59u, 0xE3u,
    0x9Fu, 0x55u, 0xB8u, 0x0Au, 0x76u, 0xDCu, 0x05u, 0x56u, 0xD0u, 0x5Fu, 0xB6u,
    0x25u, 0xCAu, 0x21u, 0xA7u, 0x6Du, 0xFAu, 0x58u, 0x5Du, 0x9Du, 0x28u, 0xC8u,
    0x72u, 0x3Cu, 0xF9u, 0xA4u, 0x4Bu, 0x0Eu, 0x44u, 0x93u, 0xCCu, 0x64u, 0x50u,
    0xD2u, 0x3Eu, 0x98u, 0xC7u, 0x69u, 0x04u, 0xE8u, 0x77u, 0x8Au, 0x12u, 0x46u,
    0xC3u, 0x7Au, 0x2Du, 0x79u, 0x22u, 0x8Cu, 0xDEu, 0xFDu, 0x5Au, 0x42u, 0xA5u,
    0x29u, 0xAAu, 0xC5u, 0xA1u, 0x00u, 0x99u, 0x3Du, 0xD9u, 0xB1u, 0x24u, 0x5Cu,
    0xB5u, 0x3Fu, 0x80u, 0xFFu, 0x48u, 0x96u, 0x83u, 0x16u, 0x49u, 0x89u, 0xABu,
    0xD1u, 0x1Du, 0x41u, 0x68u, 0xFCu, 0x36u, 0xB7u, 0x73u, 0xD6u, 0x39u, 0xF6u,
    0x03u, 0x14u, 0x53u, 0xA8u, 0xFBu, 0xDAu, 0x57u, 0x66u, 0x23u, 0xD7u, 0x60u,
    0xC0u, 0x07u, 0x2Bu, 0x75u, 0x3Au, 0xEAu, 0xD5u, 0xF2u, 0x87u, 0x09u, 0x37u,
    0x27u, 0xBFu, 0x40u, 0x9Bu, 0x6Fu, 0x1Fu, 0x33u, 0x6Au, 0xDFu, 0x70u, 0x01u,
    0x62u, 0x86u, 0x2Fu, 0xB9u, 0x82u, 0x90u, 0x06u, 0x91u, 0x51u, 0xB4u, 0xEFu,
    0xE5u, 0xC2u, 0xE0u, 0x54u, 0x0Bu, 0x0Du, 0x15u, 0xD4u, 0xF1u, 0x7Du, 0x35u,
    0x10u, 0x84u, 0x7Eu, 0x8Fu, 0x1Au, 0x6Cu, 0xD3u, 0xF7u, 0x30u, 0xB0u, 0x5Eu,
    0x17u, 0x8Du, 0xC6u, 0x20u, 0x1Cu, 0xA3u, 0xE2u, 0x18u, 0x2Cu, 0xF0u, 0xBAu,
    0x97u, 0x71u, 0x7Fu, 0xAFu, 0x4Cu, 0x7Bu, 0x9Eu, 0xF5u, 0xCFu, 0x4Eu, 0x95u,
    0xEDu, 0xBCu, 0xADu, 0xEEu, 0xCEu, 0xF3u, 0xECu, 0xE4u, 0xDDu, 0xE7u, 0xBEu,
    0x9Au, 0xBDu, 0x9Cu,
};
static const int8_t sparkDir[8][2] = {
    { 1, 0 },  { 1, -1 }, { 0, -1 }, { -1, -1 },
    { -1, 0 }, { -1, 1 }, { 0, 1 },  { 1, 1 },
};
static uint32_t sparkWrite, sparkSpawned; /* [0x1AF8] cursor, [0x1AFC] count */
static uint32_t sparkOX, sparkOY; /* [0x1AFE] / [0x1B00]            */
static uint8_t sparkShade[16][16]; /* 0x19EA, from the recipe sprite */
static int32_t sparkOn;

static const OSystem *level_osys;

static uint8_t *atlas; /* 0x400 + 169*256                            */
static uint8_t *bgImage; /* 512*320 + 768                              */
static uint8_t *world; /* the 512x320 composite (BG + terrain)       */

/* DEFS\BD<style>.BIN field M = the palette-rotation table, one 4-byte entry
 * per range: {timer, reload, first DAC entry, entry count}.  It is the table
 * palette_rotate_table (1000:8C78) walks from 202F:0x1FC6; only BD3 has a
 * range (4 entries at 0x71, every 4th tick). */
/* DEFS\BD<style>.BIN's page tables (the terrain-page system behind
 * page_offset_to_ptr, 1000:F123): 256 x 16 mask bytes per tile id and the
 * 16-word item records field B offsets into.  Built by the same loader that
 * `load_terrain_pages` (1000:505C) implements. */
static uint8_t pageMask[256][16];
static uint8_t pageOpacity[32]; /* 0x1FA4: the 32-colour opacity flags */
/* BD field I, copied by copy_level_row (0x4EE9) into the level palette's
 * 0x80..0xBF bank: the level background's 64-colour gradient. */
static uint8_t bdRowBlock[0xC0];
static uint8_t bdCBlock[0x600]; /* field C: the 0xA9 background art */
static uint8_t bdGBlock[3u * 4u * 0x100u]; /* field G: 3 layers x 4 max    */
static uint8_t bdEBlock[0x80]; /* field E: the A9 log sprite   */
static uint8_t bdHBlock[0x200]; /* field H: frames 0x26/0x27    */
static uint8_t slopeProfiles[0x400]; /* field A: the slope profiles  */
static uint32_t slopeSize;
static uint32_t bdAnimFrames; /* field F -> [0x6532]          */
static int32_t bdCHave;
static uint8_t bdPBlock[0x1000]; /* field P: the exit/igloo art (0xB9) */
static int32_t bdPHave;
static uint32_t levelBgIndex; /* the BD page's field L (test probe) */
static uint8_t *pageItems; /* field B + its record area          */
static uint32_t pageItemsSize;
static int32_t pageValid;
static uint8_t kBytes[4]; /* field K: [0x6F20], [0x6F0B]        */
static uint8_t *animTable;
static uint32_t animCount;

/* DS:0x1413 — the shipped 64-entry gradient that draw_level_text copies into
 * palette entries 0x80..0xBF (0x33F2, 0xC0 bytes).  The runtime buffer was
 * read live from the running game (DS 0x123A:0x1413) on 2026-11-01. */
static const uint8_t levelEndGradient[0xC0] = {
    0x3Cu, 0x28u, 0x3Cu, 0x3Cu, 0x26u, 0x3Cu, 0x3Cu, 0x24u, 0x3Cu, 0x3Cu, 0x22u,
    0x3Cu, 0x3Cu, 0x20u, 0x3Cu, 0x3Cu, 0x1Eu, 0x3Cu, 0x3Cu, 0x1Cu, 0x3Cu, 0x3Cu,
    0x1Au, 0x3Cu, 0x3Cu, 0x18u, 0x3Cu, 0x3Cu, 0x16u, 0x3Cu, 0x3Cu, 0x14u, 0x3Cu,
    0x3Cu, 0x12u, 0x3Cu, 0x3Cu, 0x10u, 0x3Cu, 0x3Cu, 0x0Eu, 0x3Cu, 0x3Cu, 0x0Cu,
    0x3Cu, 0x3Cu, 0x0Au, 0x3Cu, 0x3Cu, 0x0Cu, 0x3Cu, 0x3Cu, 0x0Bu, 0x38u, 0x3Cu,
    0x0Bu, 0x35u, 0x3Cu, 0x0Bu, 0x32u, 0x3Cu, 0x0Bu, 0x2Fu, 0x3Cu, 0x0Au, 0x2Bu,
    0x3Cu, 0x0Au, 0x28u, 0x3Cu, 0x0Au, 0x25u, 0x3Cu, 0x0Au, 0x22u, 0x3Cu, 0x09u,
    0x1Eu, 0x3Cu, 0x09u, 0x1Bu, 0x3Cu, 0x09u, 0x18u, 0x3Cu, 0x09u, 0x15u, 0x3Cu,
    0x08u, 0x11u, 0x3Cu, 0x08u, 0x0Eu, 0x3Cu, 0x08u, 0x0Bu, 0x3Cu, 0x08u, 0x08u,
    0x3Cu, 0x07u, 0x07u, 0x3Cu, 0x06u, 0x06u, 0x3Cu, 0x05u, 0x05u, 0x3Cu, 0x04u,
    0x04u, 0x3Cu, 0x03u, 0x03u, 0x3Cu, 0x02u, 0x02u, 0x3Cu, 0x01u, 0x01u, 0x3Cu,
    0x00u, 0x00u, 0x39u, 0x00u, 0x00u, 0x37u, 0x00u, 0x00u, 0x34u, 0x00u, 0x00u,
    0x32u, 0x00u, 0x00u, 0x2Fu, 0x00u, 0x00u, 0x2Du, 0x00u, 0x00u, 0x2Au, 0x00u,
    0x00u, 0x28u, 0x00u, 0x00u, 0x25u, 0x00u, 0x00u, 0x23u, 0x00u, 0x00u, 0x20u,
    0x00u, 0x00u, 0x1Eu, 0x00u, 0x00u, 0x1Bu, 0x00u, 0x00u, 0x19u, 0x00u, 0x00u,
    0x16u, 0x00u, 0x00u, 0x14u, 0x00u, 0x00u, 0x11u, 0x00u, 0x00u, 0x0Fu, 0x00u,
    0x00u, 0x0Cu, 0x00u, 0x00u, 0x0Au, 0x00u, 0x00u, 0x07u, 0x00u, 0x00u, 0x05u,
    0x00u, 0x00u, 0x02u, 0x00u, 0x00u,
};

/* The in-level per-tick render (draw_map_digit 1000:4F22 + draw_terrain
 * 1000:4FEF): the BG fills the 512x320 screen buffer, draw_terrain blits the
 * map's tiles over it (tile ids >= 0xA9 are empty and let the BG through;
 * every non-zero tile byte gains the +0x20 bank bias and a zero byte is not
 * stored, so the BG shows through the tile's transparent pixels), and the
 * visible 320x200 window reads the buffer's left edge (the CRTC start at
 * scroll 0,0), the status bar, and the working palette 202F:0x14D3 built at
 * load.  The entity / logic systems land with entity.c.  Returns 1 while
 * the level is running. */
static uint8_t levelJoy; /* [0x5ACE]: this tick's input word  */

static int32_t diskPhase = DISK_OFF;
static uint32_t diskDisc;
static uint16_t diskHold;
static uint8_t diskPal[768];
static const uint8_t diskBlack[768];
static uint8_t *diskFont;

/* 1000:3E73: clear the list, take the shade table and the origin (its tail is
 * the view set the claim already does). */
void level_sparkBegin(uint32_t ox, uint32_t oy, const uint8_t *shade)
{
    memset(spark, 0, sizeof spark);
    if (shade) memcpy(sparkShade, shade, sizeof sparkShade);
    sparkWrite = 0u;
    sparkSpawned = 0u;
    sparkOX = ox;
    sparkOY = oy;
    sparkOn = 1;
}

int32_t level_sparkActive(void) { return sparkOn; }

/* the same queue bytes the original keeps at 0x48BC, for object.c's DS window */
uint8_t level_sparkByte(uint32_t off)
{ return off < sizeof spark ? ((const uint8_t *)spark)[off] : 0u; }

void level_sparkSetByte(uint32_t off, uint8_t value)
{
    if (off < sizeof spark) ((uint8_t *)spark)[off] = value;
}

/* the first live slot, for the tests: writes its world position and value. */
int32_t level_sparkPeek(uint32_t *x, uint32_t *y, uint32_t *value)
{
    for (uint32_t i = 0; i < 0x58u; i++)
        if (spark[i].live) {
            if (x) *x = spark[i].x;
            if (y) *y = spark[i].y;
            if (value) *value = spark[i].value;
            return 1;
        }
    return 0;
}

/* 1000:3FB3: spawn one cell (while fewer than 0x100 have been spawned and the
 * write slot is free), then move every live cell by path[age] and drop it at
 * age 0x53.  Stops when the spawns are done and nothing is live ([0x1B02]). */
static void sparkStep(void)
{
    uint32_t live = 0;

    if (!sparkOn) return;
    if (sparkSpawned < 0x100u && !spark[sparkWrite].live) {
        uint8_t b = sparkStream[sparkSpawned & 0xFFu];
        uint8_t dx = (uint8_t)(b & 0x0Fu), dy = (uint8_t)(b >> 4);

        spark[sparkWrite].live = 1;
        spark[sparkWrite].value = sparkShade[dy][dx];
        spark[sparkWrite].x = (uint16_t)(sparkOX + dx);
        spark[sparkWrite].y = (uint16_t)(sparkOY + dy);
        spark[sparkWrite].age = 0u;
        sparkWrite = (sparkWrite + 1u) % 0x58u;
        sparkSpawned++;
    }
    for (uint32_t i = 0; i < 0x58u; i++) {
        uint32_t d;

        if (!spark[i].live) continue;
        live = 1;
        d = sparkPath[spark[i].age] & 7u;
        spark[i].x = (uint16_t)(spark[i].x + sparkDir[d][0]);
        spark[i].y = (uint16_t)(spark[i].y + sparkDir[d][1]);
        if (++spark[i].age >= 0x53u) spark[i].live = 0;
    }
    if (sparkSpawned >= 0x100u && !live) sparkOn = 0;
}

/* `1000:3F4A`'s per-frame particle draw, in the flat model: `DI = y*0x58 +
 * (x>>2) + [0x5AC4]`, plane `x & 3`, one `STOSB` per cell.  The original
 * writes into its 512x320 page with no clip; the flat page is 320x200, so this
 * clips to the window.  `0x4073` (the expired cell's paint into the 0x12-wide
 * scratch strip) is a separate routine - see D-132 in port_deviations.md. */
static void sparkDraw(void)
{
    for (uint32_t i = 0; i < 0x58u; i++) {
        int32_t sx, sy;
        uint32_t plane, addr, base;

        if (!spark[i].live || spark[i].value == 0u) continue;
        sx = (int32_t)spark[i].x - (int32_t)viewX;
        sy = (int32_t)spark[i].y - (int32_t)viewY;
        if (sx < 0 || sx >= (int32_t)VIEW_W || sy < 0 || sy >= (int32_t)VIEW_H)
            continue;
        plane = (uint32_t)sx & 3u;
        base = (uint32_t)sx & ~3u;
        addr = (uint32_t)sy * 88u + ((uint32_t)sx >> 2);
        for (uint32_t k = 0; k < 4u; k++) {
            uint8_t bit = (uint8_t)(((spark[i].value >> k) & 1u) << plane);
            uint8_t *px = &lvlPlane[addr][(base + k) & 3u];

            *px = (uint8_t)((*px & (uint8_t)~(1u << plane)) | bit);
        }
    }
}

static void visuals_free(void)
{
    free(atlas);
    atlas = NULL;
    free(bgImage);
    bgImage = NULL;
    free(world);
    world = NULL;
    free(animTable);
    animTable = NULL;
    free(pageItems);
    pageItems = NULL;
    pageItemsSize = 0;
    pageValid = 0;
    animCount = 0;
}

static uint32_t bd_walk(const uint8_t *b, uint32_t size, uint32_t *bgIndex,
                        const uint8_t **anim, uint32_t *animEntries,
                        BdPage *page)
{
    uint32_t si = 0;

    /* A / B: 256-word offset tables whose largest entry is the record's
     * size sentinel; the strides 0x10 / 0x20 pad up afterwards. */
    for (uint32_t pass = 0; pass < 2u; pass++) {
        if (si + 512u > size) return 0;
        uint32_t m = 0;
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t v = rd_le_u16(b + si + i * 2u);
            if (v > m) m = v;
        }
        if (pass == 0u) { /* A: the per-tile slope profiles */
            page->aTable = b + si;
            page->aSize = m;
        }
        if (pass == 1u) { /* B: the item offset table */
            page->items = b + si;
            page->itemsSize = m + 0x20u;
        }
        si += m + (pass ? 0x20u : 0x10u);
    }

    page->cBlock = b + si; /* C: the 0xA9 background art   */
    si += 0x600u;
    page->eBlock = b + si; /* E: the A9 log sprite (D points here) */
    si += 0x80u;
    if (si + 2u > size) return 0;
    uint32_t n = rd_le_u16(b + si);
    page->gBlock = b + si + 2u;
    page->gCount = n; /* F: n; G: 3 layers x n blocks */
    si += 2u + 3u * n * 0x100u;
    if (si + 0x200u + 0xC0u > size) return 0;
    page->hBlock = b + si; /* H: two 0x100 sprites (frames 0x26/0x27) */
    page->rowBlock = b + si + 0x200u; /* I: copy_level_row's source   */
    si += 0x200u + 0xC0u; /* H + I                        */
    if (si + 2u > size) return 0;
    {
        uint32_t skip = rd_le_u16(b + si);
        if (si + skip + 8u > size) return 0;
        page->jBytes = b + si + skip; /* J: the 8 bytes at 0x1E98     */
        si += skip + 8u;
    }
    if (si + 4u > size) return 0;
    kBytes[0] = b[si];
    kBytes[1] = b[si + 1]; /* K: [0x6F20] = two tile ids */
    kBytes[2] = b[si + 2];
    kBytes[3] = b[si + 3]; /* K2: [0x6F0B]            */
    si += 4u;
    if (si + 2u > size) return 0;
    *bgIndex = rd_le_u16(b + si); /* L                            */
    si += 2u;
    if (si + 2u > size) return 0;
    uint32_t m = rd_le_u16(b + si);
    if (si + 2u + 4u * m > size) return 0;
    *anim = b + si + 2u; /* M: the rotation table        */
    *animEntries = m;
    si += 2u + 4u * m;
    if (si + 1u > size) return 0;
    n = b[si];
    page->nList = b + si;
    page->nCount = n;
    si += 1u + 17u * n; /* N                            */
    if (si + 1u > size) return 0;
    n = b[si];
    page->oList = b + si;
    page->oCount = n;
    si += 1u + n; /* O                            */
    if (si + 0x1000u > size) return 0;
    page->pBlock = b + si; /* P: the 0xB9 exit/igloo art   */
    page->pSize = 0x1000u;
    si += 0x1000u;
    if (si + 2u > size) return 0;
    return si + 2u; /* Q                            */
}

/* 1000:5137: expand four mask bytes into 32 opacity flags, bit 0 first. */
static void page_mask_build(const uint8_t *four)
{
    uint32_t bit = 0;

    memset(pageOpacity, 0, sizeof pageOpacity);
    for (uint32_t b = 0; b < 4u; b++)
        for (uint32_t k = 0; k < 8u; k++)
            pageOpacity[bit++] = (uint8_t)((four[b] >> k) & 1u ? 0xFFu : 0u);
}

/* draw_terrain2 (1000:505C): build the 256x16 table at 0x92A0 from the tile
 * atlas.  `page->jBytes + 4` is the second four-byte opacity mask (the first
 * four drive draw_objects' item records, 1000:558B).  ids > 0xA8 and the
 * field-O ids are wholly solid; every other column holds the row of its
 * first opaque pixel (0xFF when the column is empty); the field-N records
 * then overwrite the 16 bytes. */
static void page_table_build(const BdPage *page)
{
    memset(pageMask, 0xFF, sizeof pageMask);
    if (!atlas || !page->jBytes) return;
    page_mask_build(page->jBytes + 4u);
    for (uint32_t id = 0; id <= 0xA8u; id++) {
        int32_t blocked = 0;

        for (uint32_t k = 0; k < page->oCount; k++)
            if (page->oList[1u + k] == id) {
                blocked = 1;
                break;
            }
        if (blocked) continue;
        for (uint32_t col = 0; col < 16u; col++) {
            const uint8_t *tile = atlas + 0x400u + id * 0x100u;

            for (uint32_t row = 0; row < 16u; row++) {
                if (pageOpacity[tile[row * 16u + col]]) {
                    pageMask[id][col] = (uint8_t)row;
                    break;
                }
            }
        }
    }
    for (uint32_t k = 0; k < page->nCount; k++) {
        const uint8_t *e = page->nList + 1u + 17u * k;
        memcpy(pageMask[e[0]], e + 1u, 16u);
    }
}

/* draw_objects (1000:558B): field B's item records are the terrain's per-row
 * collision masks, NOT stored - the shipped descriptors are all zero and this
 * pass fills them from the tile art.  `1000:5137` builds the `0x1FA4` flags
 * from the J block's FIRST four bytes (0x1E98; the page table above uses the
 * second four), then the pass walks the 0xA9 offset words:
 *
 *   55AF  a negative offset ends the walk;
 *   55B7  a word equal to tab[0] is skipped (the shared "no collision" record)
 *         and the art cursor SI advances one tile;
 *   55C8  any word already non-zero is left alone - and that path does NOT
 *         advance SI, so a tile whose descriptor an earlier tile already
 *         computed reuses whatever art SI sits on.  Kept as the listing has it
 *         (the live table is the check);
 *   55DB  a zero word is 16 pixels of `ES:SI`, bit 0x8000 >> col for every
 *         opaque one, and SI advances 16.
 */
static void item_masks_build(const BdPage *page)
{
    uint32_t si = 0x400u; /* 1000:5595: ES:SI, the tiles    */
    uint32_t tab0;

    if (!pageItems || !atlas || !page->jBytes || !pageValid) return;
    page_mask_build(page->jBytes); /* 0x1FA4 from 0x1E98's first 4   */
    tab0 = rd_le_u16(pageItems);
    for (uint32_t id = 0; id <= 0xA8u; id++) {
        uint32_t off = rd_le_u16(pageItems + 2u * id);

        if (off & 0x8000u) break; /* 1000:55AF JS     */
        if (off == tab0) { /* 1000:55B7        */
            si += 0x100u;
            continue;
        }
        if ((uint32_t)off + 32u > pageItemsSize) continue;
        for (uint32_t row = 0; row < 16u; row++) {
            uint32_t o = off + 2u * row;
            uint32_t word = rd_le_u16(pageItems + o);

            if (word != 0u) continue; /* 1000:55CC JNZ    */
            word = 0u;
            for (uint32_t col = 0; col < 16u; col++) { /* 1000:55D9        */
                uint8_t px =
                    si < 0x400u + ATLAS_TILES * 0x100u ? atlas[si] : 0u;

                si++;
                if (px < 32u && pageOpacity[px]) word |= 0x8000u >> col;
            }
            wr_le_u16(pageItems + o, (uint16_t)word);
        }
    }
}

/* draw_map_digit (1000:4F22) + the BD walk: the style's defs, atlas and
 * background, all off the current disc (everything ships on HUMANS.001). */
static int32_t visuals_load(uint32_t style)
{
    visuals_free();

    char name[32];
    snprintf(name, sizeof name, "DEFS\\BD%u.BIN", style);
    uint32_t bdSize = 0;
    uint16_t chk = 0;
    uint8_t *bd = res_loadChunk(name, &bdSize, &chk);
    if (!bd) return -1;
    uint32_t bgIdx = 0, animN = 0;
    const uint8_t *anim = NULL;
    BdPage page;
    memset(&page, 0, sizeof page);
    uint32_t end = bd_walk(bd, bdSize, &bgIdx, &anim, &animN, &page);
    memset(bdRowBlock, 0, sizeof bdRowBlock);
    if (end == bdSize && page.rowBlock)
        memcpy(bdRowBlock, page.rowBlock, sizeof bdRowBlock); /* 0x4EE9 */
    /* 1000:4D58's add_delta_nonzero(0x20) applies to E and H too. */
    memset(bdEBlock, 0, sizeof bdEBlock);
    memset(bdHBlock, 0, sizeof bdHBlock);
    if (end == bdSize && page.eBlock && page.hBlock) {
        for (uint32_t i = 0; i < sizeof bdEBlock; i++)
            bdEBlock[i] =
                page.eBlock[i] ? (uint8_t)(page.eBlock[i] + 0x20u) : 0u;
        for (uint32_t i = 0; i < sizeof bdHBlock; i++)
            bdHBlock[i] =
                page.hBlock[i] ? (uint8_t)(page.hBlock[i] + 0x20u) : 0u;
        object_publishDEHSprites(bdEBlock, bdHBlock);
    }
    memset(bdGBlock, 0, sizeof bdGBlock);
    if (end == bdSize && page.gBlock && page.gCount && page.gCount <= 4u) {
        uint32_t blocks = 3u * page.gCount;

        /* 1000:4D58's add_delta_nonzero(0x20) applies to every G block. */
        for (uint32_t i = 0; i < blocks * 0x100u; i++) {
            uint8_t v = page.gBlock[i];
            bdGBlock[i] = v ? (uint8_t)(v + 0x20u) : 0u;
        }
        object_publishGSprites(bdGBlock, blocks, page.gCount);
    }
    bdAnimFrames = (end == bdSize) ? page.gCount : 0u;
    /* field A (the per-tile slope profiles) is read by behaviour 4's
     * projectile; copy it before `bd` is freed. */
    memset(slopeProfiles, 0, sizeof slopeProfiles);
    slopeSize = 0;
    if (end == bdSize && page.aTable && page.aSize &&
        page.aSize <= sizeof slopeProfiles) {
        memcpy(slopeProfiles, page.aTable, page.aSize);
        slopeSize = page.aSize;
    }
    memset(bdCBlock, 0, sizeof bdCBlock);
    bdCHave = 0;
    if (end == bdSize && page.cBlock) {
        /* 1000:4D58-0x4D68: level_setup_screen's add_delta_nonzero(0x600,
         * 0x20) moves the C block into the 0x20..0x3F art palette. */
        for (uint32_t i = 0; i < sizeof bdCBlock; i++) {
            uint8_t v = page.cBlock[i];
            bdCBlock[i] = v ? (uint8_t)(v + 0x20u) : 0u;
        }
        bdCHave = 1;
    }
    memset(bdPBlock, 0, sizeof bdPBlock);
    bdPHave = 0;
    if (end == bdSize && page.pBlock && page.pSize >= sizeof bdPBlock) {
        memcpy(bdPBlock, page.pBlock, sizeof bdPBlock); /* 0x54A9 */
        bdPHave = 1;
    }
    if (end == bdSize && animN) {
        animTable = malloc(4u * animN);
        if (animTable) {
            memcpy(animTable, anim, 4u * animN);
            animCount = animN;
        }
    }
    /* The terrain page table (draw_terrain2 1000:505C) is built from the
     * tile atlas once that has loaded - page_table_build below. */
    if (page.items && page.itemsSize <= bdSize) {
        free(pageItems);
        pageItems = malloc(page.itemsSize);
        if (pageItems) {
            memcpy(pageItems, page.items, page.itemsSize);
            pageItemsSize = page.itemsSize;
            pageValid = 1;
        }
    }
    if (end != bdSize) {
        free(bd);
        return -1;
    }

    snprintf(name, sizeof name, "LEVELS\\BS%u.BLK", style);
    atlas = res_loadChunk(name, &bdSize, &chk);
    if (!atlas || bdSize != 0x400u + ATLAS_TILES * 0x100u) {
        free(bd);
        visuals_free();
        return -1;
    }
    /* the page table is built from the atlas while the BD image (J/O/N)
     * is still alive; the pointers in `page` point into it. */
    page_table_build(&page);
    /* draw_objects (1000:558B) then fills field B's item records; it needs
     * the same J bytes, so it runs before the BD image is released. */
    item_masks_build(&page);
    free(bd);

    levelBgIndex = bgIdx;
    if (bgIdx != 0u) {
        snprintf(name, sizeof name, "BACKS\\BG%u.RAW", bgIdx);
        bgImage = res_loadChunk(name, &bdSize, &chk);
        if (!bgImage || bdSize != WORLD_W * WORLD_H + 768u) {
            visuals_free();
            return -1;
        }
        /* draw_map_digit 0x4F3C -> 0x1593 puts this file's palette at the
         * level palette's 0x40..0x7F (the art), and load_level_assets'
         * 0x4BE2 transform rewrites the image: v (0..0x3F) becomes 0x40+v,
         * and the 0xFF empty fill becomes the BD row block's 64-colour
         * gradient: load_level_assets' 0x4BE2 loop walks the image in 64
         * chunks of 0xA00 bytes and uses 0x80 + the chunk as the fill index,
         * so the entry steps every 5 rows of the 512-wide image - exactly
         * one ramp over the 320-row world (0x80 + i / 0xA00). */
        for (uint32_t i = 0; i < WORLD_W * WORLD_H; i++) {
            uint8_t v = bgImage[i];
            if (v == 0xFFu) bgImage[i] = (uint8_t)(0x80u + (i / 0xA00u) % 64u);
            else bgImage[i] = (uint8_t)(v < 0x40u ? 0x40u + v : v);
        }
    }

    world = malloc(WORLD_W * WORLD_H);
    if (!world) {
        visuals_free();
        return -1;
    }
    return 0;
}

/* 1000:5159 load_terrain_a + 1000:54A9: every map cell 0xB9 stamps the BD
 * page's P block (64 x 64, zero transparent) into the background image at
 * (col*16, row*16 - 0x30) — the level's exit/igloo art. */
static void level_stampExits(void)
{
    if (!bgImage || !bdPHave) return;
    for (uint32_t row = 0; row < MAP_ROWS; row++)
        for (uint32_t col = 0; col < MAP_COLS; col++) {
            int32_t x, y;

            if (level_data_cur.map[row * MAP_COLS + col] != 0xB9u) continue;
            x = (int32_t)(col * 16u);
            y = (int32_t)(row * 16u) - 0x30;
            for (uint32_t r = 0; r < 64u; r++)
                for (uint32_t c = 0; c < 64u; c++) {
                    uint8_t v = bdPBlock[r * 64u + c];
                    int32_t px = x + (int32_t)c, py = y + (int32_t)r;

                    if (!v || px < 0 || py < 0 || px >= (int32_t)WORLD_W ||
                        py >= (int32_t)WORLD_H)
                        continue;
                    /* level_setup_screen's add_delta_nonzero(0x1000, 0x20) pass
                     * moves the P block into the 0x20..0x3F art palette. */
                    bgImage[py * (int32_t)WORLD_W + px] = (uint8_t)(v + 0x20u);
                }
        }
}

/* 1000:5F54 — render_objects' tail: for each 0x32B9 entry it blits the two
 * 32 x 24 mode-2 C-block sprites at (x, y) and (x + 0x20, y) (0x5FBE ADDs
 * 0x300 to the source and 0x20 to x for the second).  The queue holds one
 * entry per 0xA9 cell, and all 14 shipped levels with a 0xA9 have exactly
 * one.  The original's drawer steps DI by 8 (not the record's 12) at
 * 1000:5FD2, so with more than one entry it reads the first record's +8/+A
 * as x/y; nothing shipped reaches that.  The page here is the flat-compose
 * window, so the view is subtracted exactly as the object passes do. */
void level_drawBgSprites(uint8_t *page, int32_t scrollX, int32_t scrollY)
{
    if (!bdCHave) return;
    for (uint32_t i = 0; i < object_bgCount(); i++) {
        uint32_t x, y;

        object_bgAt(i, &x, &y);
        object_blitSprite(page, bdCBlock, 24u, 32u, (int32_t)x - scrollX,
                          (int32_t)y - scrollY);
        object_blitSprite(page, bdCBlock + 0x300u, 24u, 32u,
                          (int32_t)x + 0x20 - scrollX, (int32_t)y - scrollY);
    }
}

/* the stamped background and the P block, for the exit/igloo test */
const uint8_t *level_bgForProbe(void) { return bgImage; }
uint32_t level_animFrames(void) { return bdAnimFrames; }

/* 1000:1FD4's per-tile slope profile at `sub` (signed -4..+4). */
int32_t level_slopeByte(uint32_t id, uint32_t sub)
{
    uint32_t off;

    if (!slopeSize || id > 0xFFu || sub > 0xFu) return 0;
    off = rd_le_u16(slopeProfiles + 2u * id);
    if (off + sub >= slopeSize) return 0;
    return (int8_t)slopeProfiles[off + sub];
}
const uint8_t *level_pBlockForProbe(void) { return bdPHave ? bdPBlock : NULL; }
const uint8_t *level_cBlockForProbe(void) { return bdCHave ? bdCBlock : NULL; }
uint32_t level_bgIndexForProbe(void) { return levelBgIndex; }

int32_t level_init(const OSystem *osys)
{
    level_osys = osys; /* the render goes through videoSetPalette  */
    object_attachOsys(osys); /* the ACTION machine's fatal route         */
    return 0;
}

const LevelData *level_data(void)
{ return level_loaded ? &level_data_cur : NULL; }

/* The level text carries literal CR and tab from the authoring tool — the
 * census's _TEXT set (tools/level_census.py); rejecting them silently
 * truncates the walk (level 9 of LA.BIN is the first casualty). */
static int32_t isTextByte(uint8_t b)
{ return (b >= 0x20u && b < 0x7Fu) || b == 0x09u || b == 0x0Au || b == 0x0Du; }

/* A record plausibly begins at p: the header + the goal ASCIIZ (>= 4) +
 * the author ASCIIZ (>= 2) + the 6-byte signature.  Returns the map's
 * offset, or 0. */
static uint32_t recIsStart(const uint8_t *b, uint32_t size, uint32_t p)
{
    if (p + REC_HDR + SIG_LEN + MAP_LEN > size) return 0;
    const uint8_t *h = b + p;
    if (h[0] < 1u || h[0] > 6u) return 0;
    if (h[2] < 1u || h[2] > 3u) return 0;

    uint32_t q = p + REC_HDR;
    uint32_t goalStart = q;
    while (q < size && isTextByte(b[q])) q++;
    if (q - goalStart < 4u) return 0;
    if (q >= size || b[q] != 0u) return 0;
    q++; /* the goal's NUL */

    uint32_t authorStart = q;
    while (q < size && isTextByte(b[q])) q++;
    if (q - authorStart < 2u) return 0;
    if (q >= size || b[q] != 0u) return 0;
    q++; /* the author's NUL */

    if (q + SIG_LEN > size || memcmp(b + q, recSig, SIG_LEN) != 0) return 0;
    return q + FIELD_LEN; /* the map's start */
}

/* The next record's start: the first candidate at or after `from`. */
static uint32_t recNextStart(const uint8_t *b, uint32_t size, uint32_t from)
{
    uint32_t p = from;
    while (p + REC_HDR + SIG_LEN + MAP_LEN <= size) {
        uint32_t mo = recIsStart(b, size, p);
        if (mo) return p;
        p++;
    }
    return 0; /* no further record */
}

int32_t level_load(uint32_t levelId)
{
    if (levelId >= 160u) return -1;

    uint32_t disc = 1u; /* the L*.BIN files ship on HUMANS.001 */
    res_setDisc(disc);

    char name[32];
    snprintf(name, sizeof name, "LEVELS\\L%c.BIN", 'A' + levelId / 10u);

    uint32_t fileSize = 0;
    uint16_t chk = 0;
    uint8_t *image = res_loadChunk(name, &fileSize, &chk);
    if (!image) return -1;

    /* walk to the record levelId%10 */
    uint32_t p = 0;
    uint32_t rec = 0;
    LevelData *d = &level_data_cur;
    memset(d, 0, sizeof *d);

    int32_t ok = 0;
    while (p + REC_HDR <= fileSize) {
        uint32_t mapOff = recIsStart(image, fileSize, p);
        if (mapOff == 0) break; /* the walk: the candidates tile the file */

        if (rec == levelId % 10u) {
            const uint8_t *h = image + p;
            d->levelId = levelId;
            d->style = h[0];
            d->introIndex = h[1];
            d->goalCount = h[2];
            d->entityLife = h[3];
            d->objectTest = rd_le_u16(h + 4);
            d->unused06 = h[6];
            d->timeA = rd_le_u16(h + 7);
            d->timeB = rd_le_u16(h + 9);
            d->timeC = rd_le_u16(h + 11);
            memcpy(d->map, image + mapOff, MAP_LEN);
            d->mapFileOff = mapOff;

            /* the goal/author: the ASCIIZ strings between the header and the SIG */
            {
                uint32_t q = p + REC_HDR;
                uint32_t n = 0;
                while (q < fileSize && image[q] != 0u &&
                       n + 1 < sizeof d->goal) {
                    d->goal[n++] = (char)image[q++];
                }
                d->goal[n] = 0;
                q++; /* the NUL */
                n = 0;
                while (q < fileSize && image[q] != 0u &&
                       n + 1 < sizeof d->author) {
                    d->author[n++] = (char)image[q++];
                }
                d->author[n] = 0;
            }
            ok = 1;
        }

        rec++;
        p = recNextStart(image, fileSize, mapOff + MAP_LEN);
        if (p == 0) break; /* the file's records exhausted */
    }

    free(image);
    if (!ok) return -1;

    /* the header's style selects the visual set; the census across all 160
     * levels doubles as the BD-walk byte-exactness check (every style 1..6
     * walks to EOF or the load fails) */
    if (visuals_load(d->style) != 0) return -1;
    level_stampExits(); /* 1000:4F73 load_terrain_a */

    /* load_level_assets (1000:3468): the in-level dialog's box tiles
     * (apl\box.spr, 9 x 16x16 at [0x13B1]) load with the level's assets. */
    free(boxSpr);
    boxSpr = res_loadChunk("APL\\BOX.SPR", &boxSize, NULL);
    textpage_publishBox(boxSpr && boxSize >= 0x900u ? boxSpr : NULL);

    /* load_level_assets (1000:3488): APL\SPRITES.DAT is the object sprite
     * image (81744 = 0x13F50 bytes); the object pass draws from it. */
    free(objBank);
    objBank = NULL;
    objBank = res_loadChunk("APL\\SPRITES.DAT", &objBankSize, NULL);
    object_publishBank(objBank, objBankSize);

    /* load_level_assets 1000:4F82: apl\rex.dat into the entity slot [0x13AD]
     * (the entity pass's bank, 1000:5823). */
    free(entBank);
    entBank = NULL;
    entBankSize = 0;
    entBank = res_loadChunk("APL\\REX.DAT", &entBankSize, NULL);
    entity_publishBank(entBank, entBankSize);

    /* load_level_assets 1000:4F89: apl\TER.dat into [0x13AF] (the 0x649A
     * slots' draw pass, 1000:5C2F). */
    free(terBank);
    terBank = NULL;
    terBankSize = 0;
    terBank = res_loadChunk("APL\\TER.DAT", &terBankSize, NULL);
    object_publishTer(terBank, terBankSize);

    /* Publish the level before the placed-object pass: the spawn's y snap
     * (1000:5639 -> map_get) and the style-6 nudge (1000:5662) read the map
     * and the style, so level_map()/level_style() must stop returning NULL/0
     * here.  Without this the torch (0xAD) keeps its raw row*16 y and floats
     * 4 px above the ground (the reported elevated torch). */
    level_loaded = 1;

    /* the level's placed objects (load_terrain_b 1000:5168): the array is
     * cleared, then the map's 0xAA/0xAB cells become the humans and the spear. */
    object_init();
    /* the loaded record is the writable map: 1000:51F9 marks two cells a
     * row below (the original works on its [0x1FA0] copy). */
    memset(levelMapCopy, 0, sizeof levelMapCopy);
    memcpy(levelMapCopy, level_data_cur.map, sizeof levelMapCopy);
    object_loadTerrainObjects(levelMapCopy);

    /* the bar (level_start 1000:957B + build_statusbar_layout 1000:961B):
     * the time limit and the tribe pair.  On a fresh game both come from the
     * LEVEL board's counter [0x5E65] read at entry: render_level_frame
     * (1000:9589) selects [0x17F5]/[0x17F7]/[0x17F9] — this record's
     * timeA/B/C — by 0/1/else, and entry (1000:009F) maps the same byte to
     * the starting tribe [0x11FC] := 12/10/8.  With a save loaded ([0x34],
     * render_level_frame 1000:959E) the save's own +0x07 word and +0x0B
     * members replace both.  The required = the map's 0xAA-tile count
     * (load_level 1000:315F counts the first 640-byte block). */
    {
        uint32_t required = 0;
        for (uint32_t i = 0; i < MAP_LEN; i++)
            if (d->map[i] == 0xAAu) required++;
        if (save_present()) {
            timeLimit = save_time();
            tribeMembers = save_members();
        } else {
            uint32_t skill = frontend_option(FRONTEND_OPT_LEVEL);
            timeLimit = (skill == 0u)   ? d->timeA
                        : (skill == 1u) ? d->timeB
                                        : d->timeC;
            tribeMembers = (skill == 0u) ? 12u : (skill == 1u) ? 10u : 8u;
        }
        levelRequired = required;
        membersAtStart = tribeMembers; /* [0x1970] (306D)         */
        statusbar_start(timeLimit, tribeMembers, required);
        memset(palBarOnly, 0, sizeof palBarOnly);

        /* The working palette 0x14D3 (the level's DAC base): entries
         * 0..31 = APL\SPRPAL.APL+0x18 (the UI, verified live), 32..63 =
         * LEVELS\BS<style>.BLK+0x18 (the tiles), 64..127 = the BG's own
         * palette, 128..191 = zero, 192..255 = panel4's (level_video_init's
         * 0xC0-byte 0x1713 copy).  Then the level's fade-in (level_fade_in
         * 1000:337E/338A: the final 256-entry fade, div 0x32). */
        memset(levelPal, 0, sizeof levelPal);
        {
            uint32_t spSize = 0;
            uint8_t *sp = res_loadChunk("APL\\SPRPAL.APL", &spSize, NULL);
            if (sp && spSize >= 0x18u + 0x60u)
                memcpy(levelPal, sp + 0x18, 0x60u);
            free(sp);
        }
        if (atlas) memcpy(levelPal + 32u * 3u, atlas + 0x18, 0x60u);
        if (bgImage)
            memcpy(levelPal + 64u * 3u, bgImage + WORLD_W * WORLD_H, 0xC0u);
        /* copy_level_row 0x4EE9: the BD row block fills 0x80..0xBF. */
        memcpy(levelPal + 0x80u * 3u, bdRowBlock, sizeof bdRowBlock);
        {
            const uint8_t *bank = statusbar_palette();
            if (bank) {
                memcpy(levelPal + 0xC0u * 3u, bank, 0xC0u);
                memcpy(palBarOnly + 0xC0u * 3u, bank, 0xC0u);
            }
        }
        /* level_fade_in runs after the party (level_run 30AB): level_begin
         * starts the chain once the play begins, not here. */
    }

    /* a fresh load starts the chain again (level_fade_in runs per level) */
    startPhase = LVSTART_IDLE;
    free(loadingBank);
    loadingBank = NULL;
    loadingSize = 0;
    loadPhase = 0; /* the LOADING screen repeats */

    endRequested = 0; /* [0x138E] = 0xFF in play */
    endSoundSent = 0;
    playExitHold = 0;
    endWon = 1;
    goalHit = 0;
    level_loaded = 1;
    return 0;
}

/* --- load_sprite_pal's LOADING screen (1000:1DC0) ---------------------- */

/* load_sprite_pal's palette step (1000:1DCF..1DF2): BACKS\DICON.APL's
 * 0x180 bytes at +0x18 are the screen's DAC (entries 0..0x7F). */
void level_loadingBegin(void)
{
    if (!level_loaded || loadPhase) return;

    /* level_video_init (1000:329d) calls reset_display_mode (1000:82c6)
     * before load_sprite_pal's screen: the level's 320x192 field */
    video_setMode(VIDEO_MODE_X);

    /* 1000:1DCF..1E3F: dicon+0x18 -> 0x14D3 entries 0..0x7F, then the DAC
     * FADES to them (0x80 entries, div 0x19) before the frames run */
    memset(loadPal, 0, sizeof loadPal);
    free(loadDic);
    loadDic = NULL;
    loadDicSize = 0;
    /* 1000:1E9C's style table f,s,c,w,d,p indexed by style-1; 0xEA8 is
     * the 'd' in "backs\dicon.apl" — the level's own preview card */
    {
        static const char letter[6] = { 'f', 's', 'c', 'w', 'd', 'p' };
        uint32_t st = level_data_cur.style;
        char icon[32];
        char l = (st >= 1u && st <= 6u) ? letter[st - 1u] : 'd';
        snprintf(icon, sizeof icon, "BACKS\\%cICON.APL", l);
        loadDic = res_loadChunk(icon, &loadDicSize, NULL);
    }
    if (loadDic && loadDicSize >= 0x18u + 0x180u) {
        memcpy(loadPal, loadDic + 0x18, 0x180u);
        palette_buildFade(0u, 0x80u, 0x19u, loadPal);
    }
    {
        const uint8_t *bank = statusbar_palette();
        if (bank) palette_setDac(bank, 0xC0u, 0x40u);
    }
    loadingBank = res_loadChunk("APL\\LOADING.APL", &loadingSize, NULL);
    if (!loadingBank) {
        loadPhase = 3;
        return;
    }

    memcpy(loadList, loadWalkers + 2, sizeof loadList);
    loadEntries = rd_le_u16(loadWalkers);
    loadCx = 3; /* particle_frame(3, 0xA0)       */
    loadReload = 3;
    loadLeft = 0xA0u;
    loadPhase = 1;
}

/* level_run (1000:3054) runs level_video_init's two fades (3083) BEFORE
 * load_sprite_pal (3086), so the loading screen exists only once the chain has
 * poured the palette to black. */
int32_t level_loadingScreen(void)
{
    if (startPhase != LVSTART_LOADING) return 0;
    if (!loadPhase) level_loadingBegin();
    return loadPhase == 1 || loadPhase == 2;
}

/* The background: a clean black page plus BACKS\DICON.APL's 80x56 card at
 * (0x50,0x60), above the level's 40-row status bar (152..191).  The 3310
 * blit writes 20 bytes per plane from the 80-pixel source with the page
 * row stride 0x58; the image is 4 pixels per byte (80 px, not 160 — the
 * party compose's x>>2 law), so it must NOT be doubled.  The card is
 * opaque (index 0 = its black frame) — the level's DESERT preview. */
static void load_drawBackground(uint8_t *page)
{
    /* load_sprite_pal sets the vertical display scroll [0x2] := 0x32
     * (0x31F2), so every page-relative y loses 0x32: the card's source
     * DI=0x324E row 0x92 becomes 0x60, exactly above the 152-row bar. */
    memset(page, 0, 320u * 200u);
    if (loadDic && loadDicSize >= 0x400u + 80u * 56u) {
        const uint8_t *src = loadDic + 0x400u;
        for (uint32_t r = 0; r < 56u; r++) {
            uint32_t y = 0x60u + r;
            if (y >= 192u) break;
            memcpy(page + y * 320u + 0x78u, src + r * 80u, 80u);
        }
    }
    statusbar_setActive(0);
    statusbar_tick();
}

/* blit_sprite_scaled (1000:1F5A): the list x is in the 416-px source
 * coordinate space; the visible 320-px window starts 0x50 pixels in
 * (x/4 - 0x14 bytes), so screen x = list x - 0x50.  The frames are the
 * 16x24 8bpp cells at bank + 0x400 + id*0x180; 0 = transparent. */
static void load_drawOne(uint8_t *page, const uint8_t *list, uint32_t entries,
                         int32_t lo, int32_t hi, int32_t yoff)
{
    for (uint32_t e = 0; e < entries; e++) {
        const uint8_t *p = list + e * 10u;
        int32_t x = (int32_t)rd_le_u16(p) - 0x50;
        int32_t y = (int32_t)rd_le_u16(p + 2) + yoff;
        uint32_t spr = rd_le_u16(p + 8);
        if (x <= lo || x >= hi) continue; /* the screen window */
        uint32_t off = 0x400u + spr * 0x180u;
        if (off + 0x180u > loadingSize) continue; /* the 0x25 sentinel  */
        if (y < 0 || y + 24 > 200) continue;
        for (uint32_t r = 0; r < 24u; r++)
            for (uint32_t c = 0; c < 16u; c++) {
                int32_t px = x + (int32_t)c;
                if (px < 0 || px >= 320) continue;
                uint8_t v = loadingBank[off + r * 16u + c];
                if (v) page[(y + r) * 320u + (uint32_t)px] = v;
            }
    }
}

static void load_drawList(uint8_t *page)
{
    /* the arms-up group is the eight under the eight letters (the two edge
     * walkers at screen x=0x50/0xE0 are outside the LOADING! span) */
    if (loadPhase == 2 && loadWalkerCount)
        load_drawOne(page, loadWalkerSnap, loadWalkerCount, 0x60 - 1, 0xD0 + 1,
                     -0x32);
    load_drawOne(page, loadList, loadEntries, -16, 320, -0x32);
}

/* particle_frame (1000:1E78) + update_particles (1000:1E98): one frame of
 * the list — the counter reload, the per-entry step and sprite cycle — then
 * the draw and the present. */
void level_loadingTick(void)
{
    uint8_t *page;

    if (loadPhase != 1 && loadPhase != 2) return;
    page = level_osys->videoPage(0);

    if (--loadCx < 0) loadCx = loadReload;
    for (uint32_t e = 0; e < loadEntries; e++) {
        uint8_t *p = loadList + e * 10u;
        int32_t x = (int32_t)rd_le_u16(p), y = (int32_t)rd_le_u16(p + 2);
        int32_t dx = (int32_t)(int16_t)rd_le_u16(p + 4),
                dy = (int32_t)(int16_t)rd_le_u16(p + 6);
        uint32_t spr = rd_le_u16(p + 8);
        x += dx;
        if (dy) {
            y += dy;
            if (y > 0x56) y = 0x56;
        }
        if (loadCx == 0 && spr < 0x10u) {
            spr++;
            if (spr == 8u) spr = 0u;
            else if (spr == 0x10u) spr = 8u;
        }
        wr_le_u16(p, (uint32_t)x);
        wr_le_u16(p + 2, (uint32_t)y);
        wr_le_u16(p + 8, spr);
    }
    if (page) {
        load_drawBackground(page);
        load_drawList(page);
    }

    if (--loadLeft == 0) {
        if (loadPhase == 1) {
            if (page) {
                load_drawBackground(page);
                load_drawList(page);
            } /* the extra 1000:1EF8 draw  */
            memcpy(loadWalkerSnap, loadList, sizeof loadWalkerSnap);
            loadWalkerCount = loadEntries;
            for (uint32_t e = 0; e < loadWalkerCount; e++)
                wr_le_u16(loadWalkerSnap + e * 10u + 8u,
                          0x10u); /* arms-up hold */
            memcpy(loadList, loadLetters + 2, sizeof loadList);
            loadEntries = rd_le_u16(loadLetters);
            loadCx = 1;
            loadReload = 1; /* particle_frame(1, 0x56)   */
            loadLeft = 0x56u;
            loadPhase = 2;
        } else {
            loadPhase = 3;
            free(loadDic);
            loadDic = NULL;
            loadDicSize = 0;
        }
    }
}

/* level_fade_in (1000:3334), first half: after the party and the level
 * intro, fade the DAC out of the pre-level screen — entries 0..0xBF toward
 * the zeroed 0x48BC ramp, div 0x19.  The rest of the chain (the eight
 * update_world frames, the fade in to 0x14D3, the tick hook) advances in
 * level_tick. */
/* [0x0]/[0x2]: the display window into the 512x320 level page, in pixels and
 * rows — `set_display_ptr` (1000:882D) starts the CRTC at
 * `pagebase + 0x58*[0x2] + [0x0]/4`, and the object pass subtracts the same
 * pair (`draw_object_scaled_x1` 1000:66EC clips `x - [0x0]` against
 * [-0x10,0x140]).  The two stores are level_reset_view's (`1000:319D`), and in
 * play `level_followView` below is their only writer. */
void level_setView(uint32_t x, uint32_t y)
{
    viewX = x;
    viewY = y;
}

/* The pair the follower adds to the record's position ([0x2D90] indexes
 * DS:0x2D92..0x2DA4).  Outside that range the original would read whatever
 * follows in DS; the pointer cannot leave it (see the walk below), so the port
 * returns zero there (port_deviations.md D-25). */
static void level_offsetPair(int32_t *dx, int32_t *dy)
{
    uint32_t i = (uint32_t)(levelOffsetPtr - 0x2D92u) / 2u;

    if (i + 1u < sizeof levelOffsetPairs / sizeof levelOffsetPairs[0]) {
        *dx = levelOffsetPairs[i];
        *dy = levelOffsetPairs[i + 1u];
    } else {
        *dx = 0;
        *dy = 0;
    }
}

/* 1000:5D37: the walk at the end of clip_object_x.  While [0x2D8E]'s bit 15 is
 * clear the pointer advances one pair per call (4 bytes); on the last pair
 * (0x2DA2) the original stores `0x2D92` into [0x2D8E] and then decrements that
 * word - i.e. it writes 0x2D91 into the GATE and leaves the pointer on the last
 * pair, so the camera keeps the (1, 1) offset and the gate stays 0x2D91 from
 * then on.  Transcribed as it stands. */
static void level_offsetWalk(void)
{
    if (levelOffsetWord & 0x8000u) return;
    if (levelOffsetPtr != 0x2DA2u) {
        levelOffsetPtr = (uint16_t)(levelOffsetPtr + 4u);
        return;
    }
    levelOffsetWord = 0x2D92u;
    levelOffsetWord = (uint16_t)(levelOffsetWord - 1u); /* 0x2D91 */
}

/* 1000:5CB6 — the first call of `update_world` (`1000:80AD`), guarded there by
 * `[0xC] != 0`: the camera follows the **acting record** (`get_object_ptr`,
 * `1000:79F4`, walks the 29-word table at `0x3E24` with `[0x3E22]`, i.e.
 * `object_slot(object_actionEntity())`).
 *
 *   target = record + the offset pair at `[0x2D90]` - (0x88, 0x44)
 *   clamped to the pan range: 0xC0 = 512-320 px, 0xA8 = 320-152 rows (the
 *   play area; the bar owns the last 40 rows), then each axis moves at most
 *   `[0x2DA6]` = 4 px per frame (0x5D09 / 0x5D21), stored at 0x5D2F/0x5D33.
 *
 * The offset pair is **{0,0} at rest**: `[0x2D90]` = 0x2D92 (the first pair of
 * the 0x2D92..0x2DA2 table) and `[0x2D8E]` = 0xFFFF, whose bit 15 idles the
 * 4-entry +-1 px walk at 0x5D37.  Both enablers (`0xDAC7` the entity path and
 * `0xF8CE` the fall machine) are ported; level_offsetWalk advances the pair.
 * `update_view` (1000:80F1) then scrolls the page by the difference; the flat
 * compose reads the window directly, which is the same visible result. */
void level_followView(void)
{
    uint8_t *rec = object_slot(object_actionEntity());
    int32_t x, y, tx, ty, dx, dy;

    if (!rec) return;
    x = rd_le_i16(rec);
    y = rd_le_i16(rec + 2u);
    {
        int32_t ox, oy;

        level_offsetPair(&ox, &oy); /* 1000:5CCB/5CCD */
        x += ox;
        y += oy;
    }
    tx = x - 0x88;
    ty = y - 0x44;
    tx = tx < 0 ? 0 : (tx > 0xC0 ? 0xC0 : tx);
    ty = ty < 0 ? 0 : (ty > 0xA8 ? 0xA8 : ty);
    dx = tx - (int32_t)viewX;
    dy = ty - (int32_t)viewY;
    if (dx > 4) dx = 4;
    else if (dx < -4) dx = -4;
    if (dy > 4) dy = 4;
    else if (dy < -4) dy = -4;
    viewX = (uint32_t)((int32_t)viewX + dx);
    viewY = (uint32_t)((int32_t)viewY + dy);
    level_offsetWalk(); /* 1000:5D37 */
}

uint32_t level_viewX(void) { return viewX; }
uint32_t level_viewY(void) { return viewY; }

/* [0xC]: the claim clears it at its start (1000:3B19) and restores it before
 * the closing page frames (1000:3CAB). */
void level_setUpdates(int32_t on) { worldUpdates = on ? 1 : 0; }
int32_t level_updates(void) { return worldUpdates; }
void level_set_offsetWord(uint16_t v) { levelOffsetWord = v; }
uint16_t level_offsetWord(void) { return levelOffsetWord; }
/* test seam: the walk's pointer back to the table's first pair */
void level_reset_offsetWalk(void) { levelOffsetPtr = 0x2D92u; }

void level_begin(void)
{
    if (!level_loaded || startPhase != LVSTART_IDLE) return;
    /* level_video_init's two component fades, in its own order:
     *   32EE/32F1  two frames of wait (CALL 8B56 twice)
     *   32F4       all 0x100 entries to 0x14D3, div 2, then 8B8B + one frame
     *   3309       entries 0xC0..0xFF to 0x1713, div 2, then 8B8B + one frame
     *   then level_fade_in (0x3334): entries 0..0xBF to 0x48BC, div 0x19.
     * Each wait is one display period; the phase switch below walks them. */
    /* The pre-level screen fades itself to black (the party's 1000:2711,
     * 0x100 entries div 0x23).  In the original level_video_init's fades start
     * from that black; entering the level state before it has finished must
     * not override it - palette_buildFade replaces the ramp, so overriding
     * recoloured the still-lit party art in the level's palette.  Wait here
     * (the caller retries next tick) and let it finish. */
    if (palette_fadeActive()) return;
    /* [0x5B12] is level_fade_in's gate and is 0 until the chain completes, so
     * the bar's countdown cannot run during the fades (1000:95B5 reseeds the
     * clock but leaves the gate clear). */
    statusbar_setActive(0);
    /* 1000:32F4: all 0x100 entries to 0x14D3, which 1000:3170 (level_setup)
     * has just zeroed - a fade to BLACK over 2 steps.  level_run (1000:3054)
     * puts it BEFORE the loading screen (level_video_init 1000:3083,
     * load_sprite_pal 1000:3086). */
    palette_buildFade(0u, 0x100u, 2u, zeroPal); /* 1000:32F4 */
    startPhase = LVSTART_VIDEO_BASE;
}

/* still inside level_fade_in's chain?  game_main holds the goal page and the
 * play loop until it is done, exactly like the original's foreground block. */
int32_t level_starting(void)
{ return level_loaded && startPhase != LVSTART_PLAYING; }

/* --- level_run's end and the entry state machine's spine -------------- */
uint32_t level_required(void) { return levelRequired; }
uint32_t level_members(void) { return tribeMembers; }
/* 1000:DE1F: the join tail increments the tribe count ([0x11FC]). */
void level_addMember(void)
{
    if (tribeMembers < 0xFFu) tribeMembers++;
}

/* level_restore (1000:35AF): the members go back to the level's own start
 * value, then level_start re-picks the time (the port re-runs level_load, so
 * this is called after it). */
void level_restoreMembers(void)
{
    tribeMembers = membersAtStart;
    statusbar_start(timeLimit, tribeMembers, levelRequired);
}

/* the level-end test: object_test_dispatch (1000:35D6) returning negative
 * ends the play loop; [0x138E] stays 0xFF for a win and is cleared for the
 * extinct/time-out paths.  The six handlers all exist now: object_testDispatch
 * maps the level's own mode word onto them (the array predicates, the entity
 * table and the two data conditions this file owns — the goal hook and the
 * tribe count).  level_endTest runs from level_tick's play phase, like the
 * original's call in input_dispatch_loop. */
void level_goalReached(void) { goalHit = 1; }

void level_memberLost(void)
{
    if (tribeMembers > 0u) tribeMembers--;
    statusbar_armOdometer(); /* the bar rolls down (1000:F682) */
    if (tribeMembers == 0u) {
        endRequested = 1;
        endWon = 0;
    }
}

int32_t level_endTest(void)
{
    if (endRequested) return 1;
    if (goalHit) {
        level_requestEnd(1);
        return 1;
    }
    /* object_test_dispatch (1000:35D6): the level record's +0x04 word is the
     * goal test.  A level with no test byte (0x0000) relies on the hook. */
    if (level_loaded && level_data_cur.objectTest &&
        object_testDispatch(level_data_cur.objectTest)) {
        level_requestEnd(1);
        return 1;
    }
    if (tribeMembers == 0u) {
        level_requestEnd(0);
        return 1;
    }
    return 0;
}

void level_requestEnd(int32_t won)
{
    endRequested = 1;
    endWon = won ? 1 : 0;
}
int32_t level_ended(void) { return endRequested && playExitHold == 0u; }
/* 1000:34E6: the play loop's exit - after the end test's negative return the
 * loop plays sound 0xC (1000:83B7) and waits 15 vsyncs (1000:8B56) on the
 * frozen world before returning to draw_level_text. */
int32_t level_endHolding(void) { return playExitHold > 0u; }
/* 1000:F66D: the goal-record tail's "no survivor" exit sets [0x138E] = 0 and
 * ends the play loop (the same state object_test_dispatch's negative return
 * produces, reached from the object machine instead). */
void level_setEnd(int32_t won)
{
    endRequested = 1;
    endWon = won ? 1 : 0;
}
int32_t level_outcome(void) { return endWon; }

/* page_offset_to_ptr (1000:F123): 0 when the tile id's 16-byte page mask is
 * all-negative (every sub-cell empty) AND its 16-word item record sums to 0
 * (16-bit wrap, as DX).  The caller (op 3) bails on a non-zero return. */
int32_t level_pageFree(uint32_t id)
{
    uint32_t off, k;
    uint16_t sum = 0;

    if (!pageValid || id > 0xFFu) return 0;
    for (k = 0; k < 16u; k++)
        if (!(pageMask[id][k] & 0x80u)) return 0; /* a byte >= 0 */
    off = rd_le_u16(pageItems + 2u * id); /* word[B + 2*id] */
    if (off + 32u > pageItemsSize) return 0;
    for (k = 0; k < 16u; k++)
        sum = (uint16_t)(sum + rd_le_u16(pageItems + off + 2u * k));
    return sum == 0u;
}

/* The `0x92A0` page mask's byte for a tile id and sub-column: bit 7 = solid,
 * the low 7 bits are the step 1000:E389 follows.  `level_pageFree` reads the
 * same table for op 3. */
uint32_t level_pageByte(uint32_t id, uint32_t subcol)
{
    if (!pageValid || id > 0xFFu || subcol > 0xFu) return 0x80u;
    return pageMask[id][subcol];
}

/* The `[0x1FD6]` item record's `row` word: `B + word[B + 2*id]` then `2*row`,
 * i.e. the 16-bit sub-cell occupancy mask 1000:F25F tests. 0xFFFF when absent. */
/* The item record's raw byte at `off` from the tile's offset, i.e. from field
 * B.  NOTE: `1000:EC27` does NOT read this table - it reads field A
 * (`[0x1FD4]`, the slope profiles) with the same `offset + sub` shape; the
 * path-advance step that uses it is port_deviations.md D-110. */
uint32_t level_itemByte(uint32_t id, uint32_t off)
{
    uint32_t o;

    if (!pageValid || id > 0xFFu) return 0x80u;
    o = rd_le_u16(pageItems + 2u * id);
    if (o + off >= pageItemsSize) return 0x80u;
    return pageItems[o + off];
}

uint32_t level_itemWord(uint32_t id, uint32_t row)
{
    uint32_t off;

    if (!pageValid || id > 0xFFu || row > 0xFu) return 0xFFFFu;
    off = rd_le_u16(pageItems + 2u * id);
    if (off + 32u > pageItemsSize) return 0xFFFFu;
    return rd_le_u16(pageItems + off + 2u * row);
}

/* Field K's first word, the level's two special tile ids (`[0x6F20]` and
 * `[0x6F21]`): the walk states compare the cell under the feet with them. */
uint32_t level_specialTile(uint32_t i) { return i < 2u ? kBytes[i] : 0xFFFFu; }

/* The record's style byte (header +0, `[0x17EE]`): 1000:5662 tests it against 6. */
uint32_t level_style(void) { return level_loaded ? level_data_cur.style : 0u; }

const uint8_t *level_map(void) { return level_loaded ? levelMapCopy : NULL; }

/* the level's working palette 202F:0x14D3 (levelPal above) */
const uint8_t *level_palette(void) { return levelPal; }

/* draw_level_text 0x33E2 + 0x33F2: the level palette's 0..0x7F becomes grey
 * (seal_triples), then the shipped gradient fills 0x80..0xBF.  The 0x33FD
 * fade targets this palette. */
void level_endPalette(void)
{
    palette_sealTriples(levelPal, 0u, 0x80u);
    memcpy(levelPal + 0x80u * 3u, levelEndGradient, sizeof levelEndGradient);
}

/* [0x5D31] and [0x11FC] after the LEVEL board's pick (the bar's seed; the
 * party screen reads the members too). */
uint16_t level_timeLimit(void) { return timeLimit; }
uint32_t level_tribeMembers(void) { return tribeMembers; }

/* The play loop's input (1000:8526's output; the backend already produces the
 * same shape in InputState.joyBits).  Kept out of level_tick's signature so the
 * front-end screens call it the same way as the original's shared sampler. */
void level_noteInput(const InputState *in) { levelJoy = in ? in->joyBits : 0u; }

/* The world pass: `render_level_frame`'s terrain + `render_objects` + the bar,
 * i.e. everything 1000:4C28's pre-draw and every play frame put on the page.
 * The original's `blit_bitmap_call` (`1000:4C28`) draws the level bitmap into
 * BOTH pages before level_fade_in's eight `update_world` frames:
 *   DI = 0x1200 -> CALL 0x4C4E     (page A)
 *   DI = 0x8100 -> CALL 0x4C4E     (page B)
 * The flat port has one page, so a single pass is the whole of that pre-draw,
 * and the same function serves the frames after it. */
static void renderWorld(void)
{
    if (bgImage) memcpy(world, bgImage, WORLD_W * WORLD_H);
    else memset(world, 0, WORLD_W * WORLD_H);

    for (uint32_t r = 0; r < MAP_ROWS; r++) {
        for (uint32_t c = 0; c < MAP_COLS; c++) {
            uint32_t tile = level_data_cur.map[r * MAP_COLS + c];
            if (tile >= 0xA9u) continue;
            const uint8_t *src = atlas + 0x400u + tile * 0x100u;
            uint8_t *dst = world + (r * 16u) * WORLD_W + c * 16u;
            for (uint32_t y = 0; y < 16u; y++) {
                for (uint32_t x = 0; x < 16u; x++) {
                    uint8_t v = src[y * 16u + x];
                    /* draw_terrain 0x502d: a zero tile byte is NOT stored -
                     * the background (BG art / 0x80 sky) stays. */
                    if (v) dst[y * WORLD_W + x] = (uint8_t)(v + 0x20u);
                }
            }
        }
    }

    /* 1000:3D5E's four-plane presented frame, as a flat compose: the visible
     * window is the world at the display window ([0x0], [0x2] — see
     * `level_followView`), written through the chained page and read back.
     * ([0x1AEA]/[0x1AEC] are 1000:3DC0's *strip save* coordinate, not the
     * window: that routine copies a 64x18-byte page strip to A000:0xF000 and
     * never scrolls.) */
    for (uint32_t y = 0; y < VIEW_H; y++)
        for (uint32_t x = 0; x < VIEW_W; x++) {
            uint32_t wx = viewX + x, wy = viewY + y;

            lvlPlane[y * 88u + (x >> 2)][x & 3u] =
                (wx < WORLD_W && wy < WORLD_H) ? world[wy * WORLD_W + wx] : 0u;
        }
    if (sparkOn) sparkDraw();
    uint8_t *page = level_osys->videoPage(0);
    for (uint32_t y = 0; y < VIEW_H; y++)
        for (uint32_t x = 0; x < VIEW_W; x++)
            page[y * VIEW_W + x] = lvlPlane[y * 88u + (x >> 2)][x & 3u];

    /* the object pass (render_objects 1000:5D56): the placed records' base
     * sprites over the terrain, before the bar */
    object_render(page, (int32_t)viewX, (int32_t)viewY);

    /* the entity pass (render_objects 1000:5D56: 0x6421 then 0x5823) */
    entity_render(page, (int32_t)viewX, (int32_t)viewY);

    /* update_object_list's tail: the two TER.DAT slots (1000:5C2F), off on
     * shipped data because 1000:4CAB disables them */
    object_renderTer(page, (int32_t)viewX, (int32_t)viewY);

    /* render_objects' tail (1000:5F54): the 0xA9 background sprites */
    level_drawBgSprites(page, (int32_t)viewX, (int32_t)viewY);

    /* the bar over the world rows (the original's band lives below the
     * 192-row visible window via the line compare; the flat page redraws
     * band + marks per tick instead) */
    statusbar_tick();
}

/* draw_level_text 0x344C/0x3450: blit_bitmap_planes + render_objects render
 * the world into the page after the caption has been dismissed, so the page
 * behind check_highscore's card is the world, not the caption. */
void level_redrawPage(void)
{
    if (!level_loaded || !level_osys) return;
    renderWorld();
}

/* draw_level_text 1000:3437: the end page clamps [0x2] to 0x78 before its two
 * page frames; the clamped value persists in the original's view. */
void level_endRedrawPage(void)
{
    if (!level_loaded || !level_osys) return;
    if (level_viewY() > 0x78u) level_setView(level_viewX(), 0x78u);
    renderWorld();
}

/* 1000:3D5E: the claim's pan frames recolour every page byte below 0x80 to
 * 0x1E before the present (the original's four plane passes are one flat page
 * here). */
void level_claimRecolour(void)
{
    uint8_t *page = video_page(0);

    if (!page) return;
    for (uint32_t i = 0; i < VIEW_W * 200u; i++)
        if (page[i] < 0x80u) page[i] = 0x1Eu;
}

int32_t level_tick(void)
{
    if (!level_loaded) return 0;

    /* 1000:34E6: the exit hold - the world is frozen and the last page is
     * presented until the 15 waits expire. */
    if (playExitHold > 0u) {
        playExitHold--;
        return 1;
    }

    /* input_dispatch_loop 1000:34B8's object_test_dispatch call, after the
     * world tick: a negative return ends the play loop */
    if (startPhase == LVSTART_PLAYING) {
        (void)level_endTest();
        if (endRequested && !endSoundSent) { /* 1000:34E6 */
            object_driverCall(0x0Cu); /* 1000:83B7 with AX = 0xC */
            endSoundSent = 1;
            playExitHold = 0x0Fu; /* MOV CX,0xF / LOOP 8B56 */
        }
    }

    if (!world || !level_osys) return 1;

    /* level_fade_in (1000:3334) — the chain: the fade-out waits for its
     * steps, then the eight update_world (1000:80AD) frames, then the final
     * 256-entry fade to 0x14D3 (div 0x32), then the hook is installed (the
     * bar's ticking gate [0x5B12] := 0xFF).  The world is kept drawn behind
     * each phase. */
    /* The play loop leaves at 1000:34E6 and never calls update_world
     * (1000:80AD) again; the end caption (draw_level_text 0x33A3) only
     * renders the frozen world.  `endRequested` is that exit. */
    int32_t doUpdate = (startPhase == LVSTART_PLAYING && !endRequested) ? 1 : 0;

    switch (startPhase) {
    case LVSTART_IDLE: level_begin(); break;
    case LVSTART_VIDEO_BASE:
        if (!palette_fadeActive()) {
            /* 1000:3309: the `0xC0..0xFF` block to 0x1713, filled at 1000:32CA
             * with panel4's palette (1000:4BBC) - the bar's colours. */
            palette_buildFade(0xC0u, 0x40u, 2u, statusbar_palette());
            startPhase = LVSTART_VIDEO_BAR;
        }
        break;
    case LVSTART_VIDEO_BAR:
        if (!palette_fadeActive())
            startPhase = LVSTART_LOADING; /* 1000:3086 runs the loading */
        break;
    case LVSTART_LOADING:
        if (level_loadingScreen()) break; /* 1000:1DC0's screen is up */
        /* 1000:3334: the loading art fades to black (0..0xBF -> 0x48BC, div 0x19) */
        palette_buildFade(0u, 0xC0u, 0x19u, blackPal);
        startPhase = LVSTART_FADE_OUT;
        break;
    case LVSTART_FADE_OUT:
        if (!palette_fadeActive()) {
            /* 1000:3334's order after the fade-out: the scroll seeds (set in
             * the LOADING case above), slot 8's init, then 1000:4C28
             * blit_bitmap_call - the world drawn into BOTH pages (DI = 0x1200
             * then 0x8100) before the eight update_world frames.  The port has
             * one page, so its first WARMUP tick is that pre-draw. */
            /* 1000:3346/334C/334F: the display window's seeds - the eight
             * update_world frames run with the camera at (0x60,0x50), and
             * 1000:5CB6 walks it onto the acting record at <=4 px a frame. */
            level_setView(0x60u, 0x50u);
            object_levelStartRecord(); /* 1000:3355 */
            worldUpdates = 1; /* 1000:336C: [0xC] = 0xFF */
            renderWorld(); /* 1000:4C28's pre-draw (one page) */
            startPhase = LVSTART_WARMUP;
            warmupLeft = 8u; /* level_fade_in's LOOP CX=8 */
        }
        break;
    case LVSTART_WARMUP:
        doUpdate = 1; /* 1000:3374's eight frames */
        if (--warmupLeft == 0u) {
            palette_buildFade(0, 256, 0x32, levelPal);
            startPhase = LVSTART_FADE_IN;
        }
        break;
    case LVSTART_FADE_IN:
        if (!palette_fadeActive()) {
            startPhase = LVSTART_PLAYING;
            statusbar_setActive(1); /* level_fade_in's [0x5B12] */
        }
        break;
    default: break;
    }

    /* palette_rotate_table (1000:8C78), from the level's ISR hook
     * (1000:339D): per range, a signed countdown reloaded from `reload`;
     * when it wraps negative the range rotates toward the higher entries
     * (entry 0 takes the last one) and those DAC entries are re-sent.
     * The original reads the table from 202F:0x1FC6, i.e. field M.  The
     * hook is installed only after level_fade_in's fade completes (the
     * fade chases this same buffer), so no rotation while one is running.
     * The claim also freezes it (`[0x410C] = 0xFF` at 1000:3B16). */
    for (uint32_t e = 0; e < animCount && startPhase == LVSTART_PLAYING &&
                         !palette_fadeActive() && !object_claimActive();
         e++) {
        uint8_t *a = animTable + 4u * e;
        uint32_t first = a[2], n = a[3];
        if (n < 2u || first + n > 256u) continue;
        a[0] = (uint8_t)(a[0] - 1u);
        if (!(a[0] & 0x80u)) continue; /* still non-negative     */
        a[0] = a[1];
        uint8_t *p = levelPal + first * 3u;
        uint8_t last[3];
        memcpy(last, p + (n - 1u) * 3u, 3);
        for (uint32_t i = n - 1u; i > 0u; i--)
            memcpy(p + i * 3u, p + (i - 1u) * 3u, 3);
        memcpy(p, last, 3);
        palette_setDac(p, first, n);
    }

    /* the object machine's frame (the claim's sequence and its fades); the
     * original runs it from the level's per-frame body, so not during the
     * start chain. */
    /* 1000:80AD update_world runs for level_fade_in's eight frames and then
     * from the play loop; it does NOT run during the palette fade, where
     * level_fade_in spins in `wait_palette_fade_done` (1000:8B8B).  The
     * marker's lifetime shows it: the live record 8 reads type 0x18 /
     * +0xA 0x1001 when play starts - exactly eight updates after the
     * 0x1006 init (the fade-out tick that seeds the view does not count). */
    if (doUpdate) {
        /* get_object_ptr (1000:79F4) then the input command (1000:E3F6) for
         * the acting record, before the object tick.  `joyBits` is the port's
         * [0x5ACE] (backend -> 1000:8526's shape).  The whole chain is behind
         * `[0xC]` (1000:80AD's first test), which level_fade_in sets before
         * its eight frames, and the claim holds it at 0. */
        uint8_t *actor = object_slot(object_actionEntity());

        if (worldUpdates && !object_claimActive()) {
            level_followView(); /* 1000:5CB6, first in update_world */
            if (actor) (void)object_commandRun(actor, levelJoy);
            object_updateList(); /* 1000:C295, 1000:80BD            */
            entity_tick(); /* 1000:C93C, inside the [0xC] gate */
            object_markerTick(); /* 1000:D64F, after the dispatch */
            object_worldTick(); /* 1000:80CC/0x80CF, update_world's tails */
        }
        /* The claim's own frames only: the original sequence never calls
         * update_world, even after 1000:3CAB sets [0xC] = 0xFF. */
        object_claimFrame(level_palette());
    }

    /* The world is not drawn during level_video_init's two component fades
     * (1000:32F4 / 1000:3309): in the original the screen still holds the
     * pre-level art there, so those fades recolour IT while the DAC walks to
     * 0x14D3 - the world's first frames are the eight update_world passes of
     * level_fade_in, drawn while the DAC is at black (the fade-out of 0..0xBF
     * runs first, 1000:3334).  Drawing here made the level appear at full
     * brightness for those frames and then fade to black and in again. */
    /* 0x3FB3 is the play frame's flush; the chain's fades and the loading come
     * before the first play frame, so the effect is not advanced there. */
    if (startPhase == LVSTART_PLAYING) sparkStep();
    if (startPhase != LVSTART_WARMUP && startPhase != LVSTART_FADE_IN &&
        startPhase != LVSTART_PLAYING) {
        /* the bar is up from the mode reset onward (1000:952C/957B), so it is
         * redrawn here while the world waits for its black frames */
        statusbar_tick();
        return 1;
    }

    renderWorld();
    if (object_claimPanActive()) /* 1000:3D5E's recolour on both pan frames */
        level_claimRecolour();

    /* the pending-event dispatch ([0x1979] -> CS:0x197B) and, when event 4 is
     * running, its per-frame share (1000:3702): the world frame has run, so
     * redraw the jump/throw bar at the record's new +0xE */
    object_pendingRun();
    (void)object_chargeTick();

    /* the colours are the working palette 202F:0x14D3, built in level_load
     * and faded in by the first tick above (re_map.md "The level's working
     * palette"). */
    return 1;
}

static void disk_draw(uint32_t disc)
{
    uint8_t *page = video_page(0);
    memset(page, 0, VIEW_W * 200u);

    uint8_t *img = NULL;
    uint32_t isz = 0;
    uint32_t used = disc;
    for (uint32_t k = 0; k < 3u && !img; k++) {
        used = (k == 0u) ? disc : (k == 1u ? 0u : (disc == 1u ? 2u : 1u));
        res_setDisc(used);
        img = res_loadChunk("APL\\DISKIMG1.DAT", &isz, NULL);
    }
    if (img) {
        if (isz >= 0x18u + 768u) memcpy(diskPal, img + 0x18, 768u);
        /* blit_sprite_to_page, 1000:2105: source byte 4g+p -> plane p of
         * destination byte g; each byte is 8 pixels, MSB left */
        for (uint32_t g = 0; 4u * g + 3u < isz && g * 8u < VIEW_W * 200u; g++) {
            const uint8_t *q = img + 4u * g;
            for (uint32_t bit = 0; bit < 8u; bit++) {
                uint32_t px = ((q[0] >> (7u - bit)) & 1u) |
                              (((q[1] >> (7u - bit)) & 1u) << 1) |
                              (((q[2] >> (7u - bit)) & 1u) << 2) |
                              (((q[3] >> (7u - bit)) & 1u) << 3);
                page[g * 8u + bit] = (uint8_t)px;
            }
        }
        free(img);
    }

    free(diskFont);
    diskFont = res_loadChunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(diskFont);

    char line[32];
    textpage_drawLine(page, "PUT THE DISK", 0x50u, 0x0Fu);
    snprintf(line, sizeof line, "INTO DRIVE %c", (char)('1' + disc));
    textpage_drawLine(page, line, 0x60u, 0x0Fu);
    textpage_drawLine(page, "PRESS FIRE", 0x88u, 0x0Fu);
    textpage_drawLine(page, "WHEN READY", 0x98u, 0x0Fu);
}

int32_t level_diskStatusBegin(uint32_t disc)
{
    if (diskPhase != DISK_OFF) return 1;
    if (res_volumePresent(disc)) return 0; /* 1000:2005: the disc is in */

    /* draw_status_indicator (1000:1fef) sets mode 13h on the missing-disc
     * page (its flat 320x200 image) */
    video_setMode(VIDEO_MODE_13H);
    diskDisc = disc;
    memset(diskPal, 0, sizeof diskPal);
    disk_draw(disc);
    palette_buildFade(0u, 0x100u, 0x1Eu, diskPal); /* 1000:2057 */
    diskPhase = DISK_FADE_IN;
    return 1;
}

void level_diskStatusReset(void)
{
    diskPhase = DISK_OFF;
    diskHold = 0;
    free(diskFont);
    diskFont = NULL;
    font_publish(NULL);
}

int32_t level_diskStatusTick(const InputState *in)
{
    switch (diskPhase) {
    case DISK_FADE_IN:
        if (palette_fadeActive()) return 1;
        diskPhase = DISK_WAIT;
        return 1;
    case DISK_WAIT:
        if (in->lastKey || (in->joyBits & 0x80u)) {
            if (res_volumePresent(diskDisc)) {
                palette_buildFade(0u, 0x100u, 0x1Eu, diskBlack);
                diskPhase = DISK_FADE_OUT;
            }
            /* else: still the wrong disc — stay on the prompt */
        }
        return 1;
    case DISK_FADE_OUT:
        if (palette_fadeActive()) return 1;
        diskPhase = DISK_HOLD;
        diskHold = 0x4Bu; /* present_frames_75 */
        return 1;
    case DISK_HOLD:
        if (diskHold > 0u) {
            diskHold--;
            return 1;
        }
        diskPhase = DISK_OFF;
        free(diskFont);
        diskFont = NULL;
        font_publish(NULL);
        return 0;
    default: return 0;
    }
}
