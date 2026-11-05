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
 * LEVELS\BS<style>.BLK: the 169x16x16 tile _atlas behind a 0x400 APL header.
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
} _spark[0x58];

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

static const uint8_t _rec_sig[SIG_LEN] = { 0x00, 0x20, 0x00, 0x14, 0x00, 0x00 };

/* The 202F:0x6287-equivalent: the intro cue state lives in script.c; the
 * level's own state: */
static LevelData _level_data_cur;
/* The writable map the game plays on: 1000:3138 copies the record's 640 bytes
 * to [0x1FA0], and 1000:51F9 (the 0xA9 handler) marks two cells of it.  Keeping
 * the copy means the parsed record stays byte-exact (tests/test_level). */
static uint8_t _level_map_copy[640];
static int32_t _level_loaded;
static uint8_t *_box_spr;           /* apl\box.spr (the dialog tiles)      */
static uint8_t *_obj_bank;          /* apl\sprites.dat (the object sprites) */
static uint8_t *_ent_bank;          /* apl\rex.dat (the entity sprites)     */
static uint32_t _ent_bank_size;
static uint8_t *_ter_bank;          /* apl\TER.dat (the 0x649A slots)       */
static uint32_t _ter_bank_size;
static uint32_t _obj_bank_size;
static uint32_t _box_size;
static uint8_t _level_pal[768];     /* the working palette 202F:0x14D3     */

/* --- load_sprite_pal's LOADING screen (1000:1DC0) ----------------------
 * 0xA0 frames of ten walkers (APL\LOADING.APL blocks 0..0x10, two 5-strong
 * groups converging at y=0x6E), then 0x56 frames of the eight "LOADING!"
 * letters (blocks 0x11..0x18) falling to y=0x56.  Both lists are the static
 * templates at 202F:0xD7E and 202F:0xDE4: [u16 count][{u16 x, u16 y,
 * s16 dx, s16 dy, u16 sprite}].  The sprite blit skips x outside
 * 0x40..0x1A0 (blit_sprite_scaled 1000:1F5A) and the templates' 0x25
 * sprite ids are past the 26-frame bank, so those entries draw nothing. */
static uint8_t *_loading_bank;
static uint32_t _loading_size;
static uint8_t _load_list[10u * 10u];
static uint8_t
    _load_walker_snap[10u * 10u];   /* the walkers at the letters switch */
static uint32_t _load_walker_count;
static uint8_t _load_pal[768];          /* dicon+0x18: the screen's DAC    */
static uint8_t *_load_dic; /* BACKS\DICON.APL: the preview card */
static uint32_t _load_dic_size;
static uint32_t _load_entries;
static int32_t _load_cx,
    _load_reload; /* the particle counter [0xD7A]/[0xD7C] */
static uint32_t _load_left; /* particle_frame's CX             */
static int32_t _load_phase; /* 0 idle, 1 walkers, 2 letters, 3 done */

static const uint8_t _load_walkers[2u + 10u * 10u] = {
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
static const uint8_t _load_letters[2u + 8u * 10u] = {
    0x08, 0x00, 0xB0, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x02, 0x00, 0x11, 0x00,
    0xC0, 0x00, 0x19, 0x00, 0x00, 0x00, 0x02, 0x00, 0x12, 0x00, 0xD0, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x02, 0x00, 0x13, 0x00, 0xE0, 0x00, 0x17, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x14, 0x00, 0xF0, 0x00, 0x16, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x15, 0x00, 0x00, 0x01, 0x15, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x16, 0x00, 0x10, 0x01, 0x14, 0x00, 0x00, 0x00, 0x02, 0x00, 0x17, 0x00,
    0x20, 0x01, 0x13, 0x00, 0x00, 0x00, 0x02, 0x00, 0x18, 0x00
};
static int32_t _start_phase = LVSTART_IDLE;
static uint32_t _warmup_left;
/* the fade-to-black target: 0x48BC, the ramp the boot leaves zeroed */
static const uint8_t _black_pal[768];
/* level_video_init's palette state at its fades (32F4/3309): 0x14D3 holds
 * only the bar's bank (0xC0..0xFF) at that point, everything else still
 * zeroed by level_setup — the screen goes black except the bar before
 * level_fade_in runs.  Installing it at the chain's start keeps the port's
 * first level frame off the pre-level screen's palette. */
static uint8_t _pal_bar_only[768];
static uint16_t _time_limit; /* [0x5D31]: the difficulty's BCD pick  */
static uint32_t _tribe_members; /* [0x11FC]: 12/10/8 by the difficulty  */
static const uint8_t _zero_pal[768]; /* the zeroed 0x14D3 (1000:3170) */
static uint32_t _view_x, _view_y;
/* [0xC]: update_world's (1000:80AD) gate.  level_fade_in sets it at 1000:336C,
 * before its eight frames; the claim clears it (1000:3B19) and restores it
 * (1000:3CAB), so the _world is frozen while the claim's blocking sequence
 * runs.  Zero until the level's own chain reaches that store. */
static int32_t _world_updates;
/* [0x2D8E]: the follower's +-1 px offset walk enabler.  Its two writers are
 * 1000:DAC7 (the entity/collision path) and 0xF8CE (the fall machine), both
 * ported; the walk itself over 0x2D92..0x2DA2 is _level_offset_walk below. */
static uint16_t _level_offset_word =
    0xFFFFu; /* [0x2D8E]: bit 15 idles the walk */
/* DS:0x2D92..0x2DA4 - the follower's five offset pairs, read through the
 * pointer at [0x2D90] (0x2D92 at rest).  [0x2DA2]'s pair is (1, 1). */
static const int16_t _level_offset_pairs[10] = {
    0, 0, 1, -1, -1, 1, -1, -1, 1, 1,
};
static uint16_t _level_offset_ptr = 0x2D92u; /* [0x2D90] */
static uint32_t _level_required; /* [0x17FB]: the map's 0xAA count        */
static uint32_t _members_at_start; /* [0x1970]: the members at level start  */
static int32_t _end_requested; /* the play loop's exit (35D6's -1)      */
static int32_t _end_sound_sent; /* 1000:34E6's 0x83B7 call, once          */
static uint32_t _play_exit_hold; /* 1000:34E6: the 15 x 1000:8B56 waits    */
static int32_t _goal_hit; /* the discovery/claim win hook          */
static int32_t _end_won; /* [0x138E]: 1 won, 0 extinct            */
/* [0x2D8C]: the follower's record override while menu_flag_tick's goal pan
 * runs (a pointer in the original); -1 = 0, the acting record. */
static int32_t _goal_follow_record = -1;
/* menu_flag_tick's own state (1000:3540): the original blocks in it, running
 * update_world until the view stops, then 0x23 frames, then raising [0x6531]
 * and 0x1E more.  Every one of those is a presented frame, so the port runs
 * one per level tick. */
enum {
    GOALPAN_NONE = 0,
    GOALPAN_TRACK,
    GOALPAN_HOLD,
    GOALPAN_TAIL,
    GOALPAN_DONE
};
static int32_t _goal_pan_state;
static uint32_t _goal_pan_left;
static uint32_t _goal_pan_view_x, _goal_pan_view_y;
static uint8_t _goal_pan_joy; /* [0x5ACE] frozen: the pan never polls again */
/* The chained screen the original draws into: one byte = 4 pixels, 88 bytes per
 * row (352 px, of which the visible window is 320 — that slack is what lets the
 * CRTC start sit on a 4-px column boundary, 1000:3DC0's `y*88 + x/4`). */
static uint8_t _lvl_plane[VIEW_H * 88u][4];

/* --- the claim's particles (1000:3E73 init, 0x3FB3 step, 0x3F4A flush) ------
 * The list at 0x48BC is 0x58 slots of 8 bytes {live, value, x, y, age}.  The
 * tables are the image's: the age -> direction walk at 0x1B03, the eight
 * directions at 0x1B56 and the spawn nibbles at 0x1B76. */
static const uint8_t _spark_path[83] = {
    0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x01u, 0x01u,
    0x01u, 0x01u, 0x02u, 0x01u, 0x02u, 0x02u, 0x01u, 0x02u, 0x02u, 0x02u, 0x02u,
    0x02u, 0x02u, 0x03u, 0x02u, 0x02u, 0x03u, 0x02u, 0x03u, 0x03u, 0x03u, 0x03u,
    0x03u, 0x04u, 0x03u, 0x04u, 0x04u, 0x03u, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u,
    0x04u, 0x05u, 0x04u, 0x04u, 0x05u, 0x04u, 0x05u, 0x05u, 0x05u, 0x05u, 0x05u,
    0x06u, 0x05u, 0x06u, 0x06u, 0x05u, 0x06u, 0x06u, 0x06u, 0x06u, 0x06u, 0x06u,
    0x07u, 0x06u, 0x06u, 0x07u, 0x06u, 0x07u, 0x07u, 0x07u, 0x07u, 0x07u, 0x00u,
    0x07u, 0x00u, 0x00u, 0x07u, 0x00u, 0x00u,
};

static const uint8_t _spark_stream[256] = {
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
static const int8_t _spark_dir[8][2] = {
    { 1, 0 },  { 1, -1 }, { 0, -1 }, { -1, -1 },
    { -1, 0 }, { -1, 1 }, { 0, 1 },  { 1, 1 },
};
static uint32_t _spark_write,
    _spark_spawned; /* [0x1AF8] cursor, [0x1AFC] count */
static uint32_t _spark_ox, _spark_oy; /* [0x1AFE] / [0x1B00]            */
static uint8_t _spark_shade[16][16]; /* 0x19EA, from the recipe sprite */
static int32_t _spark_on;

static const OSystem *_level_osys;

static uint8_t *_atlas; /* 0x400 + 169*256                            */
static uint8_t *_bg_image; /* 512*320 + 768                              */
static uint8_t *_world; /* the 512x320 composite (BG + terrain)       */

/* DEFS\BD<style>.BIN field M = the palette-rotation table, one 4-byte entry
 * per range: {timer, reload, first DAC entry, entry count}.  It is the table
 * palette_rotate_table (1000:8C78) walks from 202F:0x1FC6; only BD3 has a
 * range (4 entries at 0x71, every 4th tick). */
/* DEFS\BD<style>.BIN's page tables (the terrain-page system behind
 * page_offset_to_ptr, 1000:F123): 256 x 16 mask bytes per tile id and the
 * 16-word item records field B offsets into.  Built by the same loader that
 * `load_terrain_pages` (1000:505C) implements. */
static uint8_t _page_mask[256][16];
static uint8_t _page_opacity[32]; /* 0x1FA4: the 32-colour opacity flags */
/* BD field I, copied by copy_level_row (0x4EE9) into the level palette's
 * 0x80..0xBF bank: the level background's 64-colour gradient. */
static uint8_t _bd_row_block[0xC0];
static uint8_t _bd_c_block[0x600]; /* field C: the 0xA9 background art */
static uint8_t _bd_g_block[3u * 4u * 0x100u]; /* field G: 3 layers x 4 max    */
static uint8_t _bd_e_block[0x80]; /* field E: the A9 log sprite   */
static uint8_t _bd_h_block[0x200]; /* field H: frames 0x26/0x27    */
static uint8_t _slope_profiles[0x400]; /* field A: the slope profiles  */
static uint32_t _slope_size;
static uint32_t _bd_anim_frames; /* field F -> [0x6532]          */
static int32_t _bd_c_have;
static uint8_t _bd_p_block[0x1000]; /* field P: the exit/igloo art (0xB9) */
static int32_t _bd_p_have;
static uint32_t _level_bg_index; /* the BD page's field L (test probe) */
static uint8_t *_page_items; /* field B + its record area          */
static uint32_t _page_items_size;
static int32_t _page_valid;
static uint8_t _k_bytes[4]; /* field K: [0x6F20], [0x6F0B]        */
static uint8_t *_anim_table;
static uint32_t _anim_count;

/* DS:0x1413 — the shipped 64-entry gradient that draw_level_text copies into
 * palette entries 0x80..0xBF (0x33F2, 0xC0 bytes).  The runtime buffer was
 * read live from the running game (DS 0x123A:0x1413) on 2026-11-01. */
static const uint8_t _level_end_gradient[0xC0] = {
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
static uint8_t _level_joy; /* [0x5ACE]: this tick's input word  */

static int32_t _disk_phase = DISK_OFF;
static uint32_t _disk_disc;
static uint16_t _disk_hold;
static uint8_t _disk_pal[768];
static const uint8_t _disk_black[768];
static uint8_t *_disk_font;

/** @brief 1000:3E73: clear the list, take the shade table and the origin (its tail is
 * the view set the claim already does).
 *
 * @param ox the x origin
 * @param oy the y origin
 * @param shade the shade table
 */
void level_spark_begin(uint32_t ox, uint32_t oy, const uint8_t *shade)
{
    memset(_spark, 0, sizeof _spark);
    if (shade) memcpy(_spark_shade, shade, sizeof _spark_shade);
    _spark_write = 0u;
    _spark_spawned = 0u;
    _spark_ox = ox;
    _spark_oy = oy;
    _spark_on = 1;
}

/**
 * @brief Accessor: the spark effect is running.
 *
 * @return The spark effect is running.
 */
int32_t level_spark_active(void) { return _spark_on; }

/**
 * @brief the same queue bytes the original keeps at 0x48BC, for object.c's DS window
 *
 * @param off the offset
 * @return the spark byte
 */
uint8_t level_spark_byte(uint32_t off)
{ return off < sizeof _spark ? ((const uint8_t *)_spark)[off] : 0u; }

/**
 * @brief Test seam: poke one spark buffer byte.
 *
 * @param off the offset
 * @param value the value
 */
void level_spark_set_byte(uint32_t off, uint8_t value)
{
    if (off < sizeof _spark) ((uint8_t *)_spark)[off] = value;
}

/**
 * @brief the first live slot, for the tests: writes its _world position and value.
 *
 * @param x the x coordinate
 * @param y the y coordinate
 * @param value the value
 * @return 1 when a live spark slot was found, 0 otherwise
 */
int32_t level_spark_peek(uint32_t *x, uint32_t *y, uint32_t *value)
{
    for (uint32_t i = 0; i < 0x58u; i++)
        if (_spark[i].live) {
            if (x) *x = _spark[i].x;
            if (y) *y = _spark[i].y;
            if (value) *value = _spark[i].value;
            return 1;
        }
    return 0;
}

/** @brief 1000:3FB3: spawn one cell (while fewer than 0x100 have been spawned and the
 * write slot is free), then move every live cell by path[age] and drop it at
 * age 0x53.  Stops when the spawns are done and nothing is live ([0x1B02]).
 */
static void _spark_step(void)
{
    uint32_t live = 0;

    if (!_spark_on) return;
    if (_spark_spawned < 0x100u && !_spark[_spark_write].live) {
        uint8_t b = _spark_stream[_spark_spawned & 0xFFu];
        uint8_t dx = (uint8_t)(b & 0x0Fu), dy = (uint8_t)(b >> 4);

        _spark[_spark_write].live = 1;
        _spark[_spark_write].value = _spark_shade[dy][dx];
        _spark[_spark_write].x = (uint16_t)(_spark_ox + dx);
        _spark[_spark_write].y = (uint16_t)(_spark_oy + dy);
        _spark[_spark_write].age = 0u;
        _spark_write = (_spark_write + 1u) % 0x58u;
        _spark_spawned++;
    }
    for (uint32_t i = 0; i < 0x58u; i++) {
        uint32_t d;

        if (!_spark[i].live) continue;
        live = 1;
        d = _spark_path[_spark[i].age] & 7u;
        _spark[i].x = (uint16_t)(_spark[i].x + _spark_dir[d][0]);
        _spark[i].y = (uint16_t)(_spark[i].y + _spark_dir[d][1]);
        if (++_spark[i].age >= 0x53u) _spark[i].live = 0;
    }
    if (_spark_spawned >= 0x100u && !live) _spark_on = 0;
}

/** @brief `1000:3F4A`'s per-frame particle draw, in the flat model: `DI = y*0x58 +
 * (x>>2) + [0x5AC4]`, plane `x & 3`, one `STOSB` per cell.  The original
 * writes into its 512x320 page with no clip; the flat page is 320x200, so this
 * clips to the window.  `0x4073` (the expired cell's paint into the 0x12-wide
 * scratch strip) is a separate routine - see D-132 in port_deviations.md.
 */
static void _spark_draw(void)
{
    for (uint32_t i = 0; i < 0x58u; i++) {
        int32_t sx, sy;
        uint32_t plane, addr, base;

        if (!_spark[i].live || _spark[i].value == 0u) continue;
        sx = (int32_t)_spark[i].x - (int32_t)_view_x;
        sy = (int32_t)_spark[i].y - (int32_t)_view_y;
        if (sx < 0 || sx >= (int32_t)VIEW_W || sy < 0 || sy >= (int32_t)VIEW_H)
            continue;
        plane = (uint32_t)sx & 3u;
        base = (uint32_t)sx & ~3u;
        addr = (uint32_t)sy * 88u + ((uint32_t)sx >> 2);
        for (uint32_t k = 0; k < 4u; k++) {
            uint8_t bit = (uint8_t)(((_spark[i].value >> k) & 1u) << plane);
            uint8_t *px = &_lvl_plane[addr][(base + k) & 3u];

            *px = (uint8_t)((*px & (uint8_t)~(1u << plane)) | bit);
        }
    }
}

/**
 * @brief Release the loaded level's visual banks.
 */
static void _visuals_free(void)
{
    free(_atlas);
    _atlas = NULL;
    free(_bg_image);
    _bg_image = NULL;
    free(_world);
    _world = NULL;
    free(_anim_table);
    _anim_table = NULL;
    free(_page_items);
    _page_items = NULL;
    _page_items_size = 0;
    _page_valid = 0;
    _anim_count = 0;
}

/**
 * @brief Walk the BD record's streams and fill the level's terrain blocks.
 *
 * @param b the BD record bytes
 * @param size the size
 * @param bgIndex the background index
 * @param anim the animation cursor
 * @param animEntries the animation entry count
 * @param page the page buffer
 * @return the end offset, or 0 on a short record
 */
static uint32_t _bd_walk(const uint8_t *b, uint32_t size, uint32_t *bgIndex,
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
    _k_bytes[0] = b[si];
    _k_bytes[1] = b[si + 1]; /* K: [0x6F20] = two tile ids */
    _k_bytes[2] = b[si + 2];
    _k_bytes[3] = b[si + 3]; /* K2: [0x6F0B]            */
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

/**
 * @brief 1000:5137: expand four mask bytes into 32 opacity flags, bit 0 first.
 *
 * @param four the four-pixel value
 */
static void _page_mask_build(const uint8_t *four)
{
    uint32_t bit = 0;

    memset(_page_opacity, 0, sizeof _page_opacity);
    for (uint32_t b = 0; b < 4u; b++)
        for (uint32_t k = 0; k < 8u; k++)
            _page_opacity[bit++] = (uint8_t)((four[b] >> k) & 1u ? 0xFFu : 0u);
}

/** draw_terrain2 (1000:505C): build the 256x16 table at 0x92A0 from the tile
 * _atlas.  `page->jBytes + 4` is the second four-byte opacity mask (the first
 * four drive draw_objects' item records, 1000:558B).  ids > 0xA8 and the
 * field-O ids are wholly solid; every other column holds the row of its
 * first opaque pixel (0xFF when the column is empty); the field-N records
 * then overwrite the 16 bytes. */
/**
 * @brief The first opaque row of one 16-px tile column (0xFF when empty).
 *
 * @param tile the tile byte
 * @param col the column
 * @return the first opaque row, or 0xFF when empty
 */
static uint8_t _page_column_top(const uint8_t *tile, uint32_t col)
{
    for (uint32_t row = 0; row < 16u; row++)
        if (_page_opacity[tile[row * 16u + col]]) return (uint8_t)row;
    return 0xFFu;
}

/**
 * @brief Build the per-tile column masks from the page's J and N blocks.
 *
 * @param page the page buffer
 */
static void _page_table_build(const BdPage *page)
{
    memset(_page_mask, 0xFF, sizeof _page_mask);
    if (!_atlas || !page->jBytes) return;
    _page_mask_build(page->jBytes + 4u);
    for (uint32_t id = 0; id <= 0xA8u; id++) {
        int32_t blocked = 0;
        const uint8_t *tile;
        uint32_t col;

        for (uint32_t k = 0; k < page->oCount; k++)
            if (page->oList[1u + k] == id) {
                blocked = 1;
                break;
            }
        if (blocked) continue;
        for (col = 0; col < 16u; col++) {
            tile = _atlas + 0x400u + id * 0x100u;
            _page_mask[id][col] = _page_column_top(tile, col);
        }
    }
    for (uint32_t k = 0; k < page->nCount; k++) {
        const uint8_t *e = page->nList + 1u + 17u * k;
        memcpy(_page_mask[e[0]], e + 1u, 16u);
    }
}

/**
 * @brief Build one 16-bit item row from the atlas cursor and advance it.
 *
 * @param si the atlas cursor (in/out)
 * @return the 16-bit item row
 */
static uint32_t _item_row_word(uint32_t *si)
{
    uint32_t word = 0;

    for (uint32_t col = 0; col < 16u; col++) { /* 1000:55D9 */
        uint8_t px = *si < 0x400u + ATLAS_TILES * 0x100u ? _atlas[*si] : 0u;

        (*si)++;
        if (px < 32u && _page_opacity[px]) word |= 0x8000u >> col;
    }
    return word;
}

/** @brief draw_objects (1000:558B): field B's item records are the terrain's per-row
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
 *
 * @param page the page buffer
 */
static void _item_masks_build(const BdPage *page)
{
    uint32_t si = 0x400u; /* 1000:5595: ES:SI, the tiles    */
    uint32_t tab0;

    if (!_page_items || !_atlas || !page->jBytes || !_page_valid) return;
    _page_mask_build(page->jBytes); /* 0x1FA4 from 0x1E98's first 4   */
    tab0 = rd_le_u16(_page_items);
    for (uint32_t id = 0; id <= 0xA8u; id++) {
        uint32_t off = rd_le_u16(_page_items + 2u * id);

        if (off & 0x8000u) break; /* 1000:55AF JS     */
        if (off == tab0) { /* 1000:55B7        */
            si += 0x100u;
            continue;
        }
        if ((uint32_t)off + 32u > _page_items_size) continue;
        for (uint32_t row = 0; row < 16u; row++) {
            uint32_t o = off + 2u * row;

            if (rd_le_u16(_page_items + o) != 0u) continue; /* 1000:55CC */
            wr_le_u16(_page_items + o, (uint16_t)_item_row_word(&si));
        }
    }
}

/** @brief draw_map_digit (1000:4F22) + the BD walk: the style's defs, _atlas and
 * background, all off the current disc (everything ships on HUMANS.001).
 *
 * @param style the level style
 * @return 0 on success, -1 when an asset is missing
 */
static int32_t _visuals_load(uint32_t style)
{
    _visuals_free();

    char name[32];
    snprintf(name, sizeof name, "DEFS\\BD%u.BIN", style);
    uint32_t bdSize = 0;
    uint16_t chk = 0;
    uint8_t *bd = res_load_chunk(name, &bdSize, &chk);
    if (!bd) return -1;
    uint32_t bgIdx = 0, animN = 0;
    const uint8_t *anim = NULL;
    BdPage page;
    memset(&page, 0, sizeof page);
    uint32_t end = _bd_walk(bd, bdSize, &bgIdx, &anim, &animN, &page);
    memset(_bd_row_block, 0, sizeof _bd_row_block);
    if (end == bdSize && page.rowBlock)
        memcpy(_bd_row_block, page.rowBlock, sizeof _bd_row_block); /* 0x4EE9 */
    /* 1000:4D58's add_delta_nonzero(0x20) applies to E and H too. */
    memset(_bd_e_block, 0, sizeof _bd_e_block);
    memset(_bd_h_block, 0, sizeof _bd_h_block);
    if (end == bdSize && page.eBlock && page.hBlock) {
        for (uint32_t i = 0; i < sizeof _bd_e_block; i++)
            _bd_e_block[i] =
                page.eBlock[i] ? (uint8_t)(page.eBlock[i] + 0x20u) : 0u;
        for (uint32_t i = 0; i < sizeof _bd_h_block; i++)
            _bd_h_block[i] =
                page.hBlock[i] ? (uint8_t)(page.hBlock[i] + 0x20u) : 0u;
        object_publish_deh_sprites(_bd_e_block, _bd_h_block);
    }
    memset(_bd_g_block, 0, sizeof _bd_g_block);
    if (end == bdSize && page.gBlock && page.gCount && page.gCount <= 4u) {
        uint32_t blocks = 3u * page.gCount;

        /* 1000:4D58's add_delta_nonzero(0x20) applies to every G block. */
        for (uint32_t i = 0; i < blocks * 0x100u; i++) {
            uint8_t v = page.gBlock[i];
            _bd_g_block[i] = v ? (uint8_t)(v + 0x20u) : 0u;
        }
        object_publish_g_sprites(_bd_g_block, blocks, page.gCount);
    }
    _bd_anim_frames = (end == bdSize) ? page.gCount : 0u;
    /* field A (the per-tile slope profiles) is read by behaviour 4's
     * projectile; copy it before `bd` is freed. */
    memset(_slope_profiles, 0, sizeof _slope_profiles);
    _slope_size = 0;
    if (end == bdSize && page.aTable && page.aSize &&
        page.aSize <= sizeof _slope_profiles) {
        memcpy(_slope_profiles, page.aTable, page.aSize);
        _slope_size = page.aSize;
    }
    memset(_bd_c_block, 0, sizeof _bd_c_block);
    _bd_c_have = 0;
    if (end == bdSize && page.cBlock) {
        /* 1000:4D58-0x4D68: level_setup_screen's add_delta_nonzero(0x600,
         * 0x20) moves the C block into the 0x20..0x3F art palette. */
        for (uint32_t i = 0; i < sizeof _bd_c_block; i++) {
            uint8_t v = page.cBlock[i];
            _bd_c_block[i] = v ? (uint8_t)(v + 0x20u) : 0u;
        }
        _bd_c_have = 1;
    }
    memset(_bd_p_block, 0, sizeof _bd_p_block);
    _bd_p_have = 0;
    if (end == bdSize && page.pBlock && page.pSize >= sizeof _bd_p_block) {
        memcpy(_bd_p_block, page.pBlock, sizeof _bd_p_block); /* 0x54A9 */
        _bd_p_have = 1;
    }
    if (end == bdSize && animN) {
        _anim_table = malloc(4u * animN);
        if (_anim_table) {
            memcpy(_anim_table, anim, 4u * animN);
            _anim_count = animN;
        }
    }
    /* The terrain page table (draw_terrain2 1000:505C) is built from the
     * tile _atlas once that has loaded - _page_table_build below. */
    if (page.items && page.itemsSize <= bdSize) {
        free(_page_items);
        _page_items = malloc(page.itemsSize);
        if (_page_items) {
            memcpy(_page_items, page.items, page.itemsSize);
            _page_items_size = page.itemsSize;
            _page_valid = 1;
        }
    }
    if (end != bdSize) {
        free(bd);
        return -1;
    }

    snprintf(name, sizeof name, "LEVELS\\BS%u.BLK", style);
    _atlas = res_load_chunk(name, &bdSize, &chk);
    if (!_atlas || bdSize != 0x400u + ATLAS_TILES * 0x100u) {
        free(bd);
        _visuals_free();
        return -1;
    }
    /* the page table is built from the _atlas while the BD image (J/O/N)
     * is still alive; the pointers in `page` point into it. */
    _page_table_build(&page);
    /* draw_objects (1000:558B) then fills field B's item records; it needs
     * the same J bytes, so it runs before the BD image is released. */
    _item_masks_build(&page);
    free(bd);

    _level_bg_index = bgIdx;
    if (bgIdx != 0u) {
        snprintf(name, sizeof name, "BACKS\\BG%u.RAW", bgIdx);
        _bg_image = res_load_chunk(name, &bdSize, &chk);
        if (!_bg_image || bdSize != WORLD_W * WORLD_H + 768u) {
            _visuals_free();
            return -1;
        }
        /* draw_map_digit 0x4F3C -> 0x1593 puts this file's palette at the
         * level palette's 0x40..0x7F (the art), and load_level_assets'
         * 0x4BE2 transform rewrites the image: v (0..0x3F) becomes 0x40+v,
         * and the 0xFF empty fill becomes the BD row block's 64-colour
         * gradient: load_level_assets' 0x4BE2 loop walks the image in 64
         * chunks of 0xA00 bytes and uses 0x80 + the chunk as the fill index,
         * so the entry steps every 5 rows of the 512-wide image - exactly
         * one ramp over the 320-row _world (0x80 + i / 0xA00). */
        for (uint32_t i = 0; i < WORLD_W * WORLD_H; i++) {
            uint8_t v = _bg_image[i];
            if (v == 0xFFu)
                _bg_image[i] = (uint8_t)(0x80u + (i / 0xA00u) % 64u);
            else _bg_image[i] = (uint8_t)(v < 0x40u ? 0x40u + v : v);
        }
    }

    _world = malloc(WORLD_W * WORLD_H);
    if (!_world) {
        _visuals_free();
        return -1;
    }
    return 0;
}

/**
 * @brief Stamp one 64x64 P-block into the background at (x, y).
 *
 * @param x the x coordinate
 * @param y the y coordinate
 */
static void _stamp_p_block(int32_t x, int32_t y)
{
    for (uint32_t r = 0; r < 64u; r++)
        for (uint32_t c = 0; c < 64u; c++) {
            uint8_t v = _bd_p_block[r * 64u + c];
            int32_t px = x + (int32_t)c, py = y + (int32_t)r;

            if (!v || px < 0 || py < 0 || px >= (int32_t)WORLD_W ||
                py >= (int32_t)WORLD_H)
                continue;
            /* level_setup_screen's add_delta_nonzero(0x1000, 0x20) pass
             * moves the P block into the 0x20..0x3F art palette. */
            _bg_image[py * (int32_t)WORLD_W + px] = (uint8_t)(v + 0x20u);
        }
}

/** @brief 1000:5159 load_terrain_a + 1000:54A9: every map cell 0xB9 stamps the BD
 * page's P block (64 x 64, zero transparent) into the background image at
 * (col*16, row*16 - 0x30) — the level's exit/igloo art.
 */
static void _level_stamp_exits(void)
{
    if (!_bg_image || !_bd_p_have) return;
    for (uint32_t row = 0; row < MAP_ROWS; row++)
        for (uint32_t col = 0; col < MAP_COLS; col++) {
            if (_level_data_cur.map[row * MAP_COLS + col] != 0xB9u) continue;
            _stamp_p_block((int32_t)(col * 16u), (int32_t)(row * 16u) - 0x30);
        }
}

/** @brief 1000:5F54 — render_objects' tail: for each 0x32B9 entry it blits the two
 * 32 x 24 mode-2 C-block sprites at (x, y) and (x + 0x20, y) (0x5FBE ADDs
 * 0x300 to the source and 0x20 to x for the second).  The queue holds one
 * entry per 0xA9 cell, and all 14 shipped levels with a 0xA9 have exactly
 * one.  The original's drawer steps DI by 8 (not the record's 12) at
 * 1000:5FD2, so with more than one entry it reads the first record's +8/+A
 * as x/y; nothing shipped reaches that.  The page here is the flat-compose
 * window, so the view is subtracted exactly as the object passes do.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
void level_draw_bg_sprites(uint8_t *page, int32_t scrollX, int32_t scrollY)
{
    if (!_bd_c_have) return;
    for (uint32_t i = 0; i < object_bg_count(); i++) {
        uint32_t x, y;

        object_bg_at(i, &x, &y);
        object_blit_sprite(page, _bd_c_block, 24u, 32u, (int32_t)x - scrollX,
                           (int32_t)y - scrollY);
        object_blit_sprite(page, _bd_c_block + 0x300u, 24u, 32u,
                           (int32_t)x + 0x20 - scrollX, (int32_t)y - scrollY);
    }
}

/**
 * @brief the stamped background and the P block, for the exit/igloo test
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *level_bg_for_probe(void) { return _bg_image; }
/**
 * @brief Accessor: the loaded record's animation frame count.
 *
 * @return The loaded record's animation frame count.
 */
uint32_t level_anim_frames(void) { return _bd_anim_frames; }

/**
 * @brief 1000:1FD4's per-tile slope profile at `sub` (signed -4..+4).
 *
 * @param id the identifier
 * @param sub the sub-column
 * @return the signed slope byte
 */
int32_t level_slope_byte(uint32_t id, uint32_t sub)
{
    uint32_t off;

    if (!_slope_size || id > 0xFFu || sub > 0xFu) return 0;
    off = rd_le_u16(_slope_profiles + 2u * id);
    if (off + sub >= _slope_size) return 0;
    return (int8_t)_slope_profiles[off + sub];
}
/**
 * @brief Accessor: the record's P block (exit art).
 *
 * @return The record's P block (exit art).
 */
const uint8_t *level_p_block_for_probe(void)
{ return _bd_p_have ? _bd_p_block : NULL; }
/**
 * @brief Accessor: the record's C block (0xA9 background art).
 *
 * @return The record's C block (0xA9 background art).
 */
const uint8_t *level_c_block_for_probe(void)
{ return _bd_c_have ? _bd_c_block : NULL; }
/**
 * @brief Accessor: the loaded background index.
 *
 * @return The loaded background index.
 */
uint32_t level_bg_index_for_probe(void) { return _level_bg_index; }

/**
 * @brief Bind the level module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t level_init(const OSystem *osys)
{
    _level_osys = osys; /* the render goes through videoSetPalette  */
    object_attach_osys(osys); /* the ACTION machine's fatal route         */
    return 0;
}

/**
 * @brief Accessor: the loaded LevelData record.
 *
 * @return The loaded LevelData record.
 */
const LevelData *level_data(void)
{ return _level_loaded ? &_level_data_cur : NULL; }

/** @brief The level text carries literal CR and tab from the authoring tool — the
 * census's _TEXT set (tools/level_census.py); rejecting them silently
 * truncates the walk (level 9 of LA.BIN is the first casualty).
 *
 * @param b the byte
 * @return 1 when the byte is printable or CR/LF/tab, 0 otherwise
 */
static int32_t _is_text_byte(uint8_t b)
{ return (b >= 0x20u && b < 0x7Fu) || b == 0x09u || b == 0x0Au || b == 0x0Du; }

/** @brief A record plausibly begins at p: the header + the goal ASCIIZ (>= 4) +
 * the author ASCIIZ (>= 2) + the 6-byte signature.  Returns the map's
 * offset, or 0.
 *
 * @param b the record bytes
 * @param size the size
 * @param p the candidate offset
 * @return 1 when a record starts at p, 0 otherwise
 */
static uint32_t _rec_is_start(const uint8_t *b, uint32_t size, uint32_t p)
{
    if (p + REC_HDR + SIG_LEN + MAP_LEN > size) return 0;
    const uint8_t *h = b + p;
    if (h[0] < 1u || h[0] > 6u) return 0;
    if (h[2] < 1u || h[2] > 3u) return 0;

    uint32_t q = p + REC_HDR;
    uint32_t goalStart = q;
    while (q < size && _is_text_byte(b[q])) q++;
    if (q - goalStart < 4u) return 0;
    if (q >= size || b[q] != 0u) return 0;
    q++; /* the goal's NUL */

    uint32_t authorStart = q;
    while (q < size && _is_text_byte(b[q])) q++;
    if (q - authorStart < 2u) return 0;
    if (q >= size || b[q] != 0u) return 0;
    q++; /* the author's NUL */

    if (q + SIG_LEN > size || memcmp(b + q, _rec_sig, SIG_LEN) != 0) return 0;
    return q + FIELD_LEN; /* the map's start */
}

/**
 * @brief The next record's start: the first candidate at or after `from`.
 *
 * @param b the record bytes
 * @param size the size
 * @param from the start index
 * @return the next record offset, or 0 when none
 */
static uint32_t _rec_next_start(const uint8_t *b, uint32_t size, uint32_t from)
{
    uint32_t p = from;
    while (p + REC_HDR + SIG_LEN + MAP_LEN <= size) {
        uint32_t mo = _rec_is_start(b, size, p);
        if (mo) return p;
        p++;
    }
    return 0; /* no further record */
}

/**
 * @brief Load one level record and its terrain from the volume.
 *
 * @param levelId the level id
 * @return 0 on success, -1 on failure
 */
int32_t level_load(uint32_t levelId)
{
    if (levelId >= 160u) return -1;

    uint32_t disc = 1u; /* the L*.BIN files ship on HUMANS.001 */
    res_set_disc(disc);

    char name[32];
    snprintf(name, sizeof name, "LEVELS\\L%c.BIN", 'A' + levelId / 10u);

    uint32_t fileSize = 0;
    uint16_t chk = 0;
    uint8_t *image = res_load_chunk(name, &fileSize, &chk);
    if (!image) return -1;

    /* walk to the record levelId%10 */
    uint32_t p = 0;
    uint32_t rec = 0;
    LevelData *d = &_level_data_cur;
    memset(d, 0, sizeof *d);

    int32_t ok = 0;
    while (p + REC_HDR <= fileSize) {
        uint32_t mapOff = _rec_is_start(image, fileSize, p);
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
        p = _rec_next_start(image, fileSize, mapOff + MAP_LEN);
        if (p == 0) break; /* the file's records exhausted */
    }

    free(image);
    if (!ok) return -1;

    /* the header's style selects the visual set; the census across all 160
     * levels doubles as the BD-walk byte-exactness check (every style 1..6
     * walks to EOF or the load fails) */
    if (_visuals_load(d->style) != 0) return -1;
    _level_stamp_exits(); /* 1000:4F73 load_terrain_a */

    /* load_level_assets (1000:3468): the in-level dialog's box tiles
     * (apl\box.spr, 9 x 16x16 at [0x13B1]) load with the level's assets. */
    free(_box_spr);
    _box_spr = res_load_chunk("APL\\BOX.SPR", &_box_size, NULL);
    textpage_publish_box(_box_spr && _box_size >= 0x900u ? _box_spr : NULL);

    /* load_level_assets (1000:3488): APL\SPRITES.DAT is the object sprite
     * image (81744 = 0x13F50 bytes); the object pass draws from it. */
    free(_obj_bank);
    _obj_bank = NULL;
    _obj_bank = res_load_chunk("APL\\SPRITES.DAT", &_obj_bank_size, NULL);
    object_publish_bank(_obj_bank, _obj_bank_size);

    /* load_level_assets 1000:4F82: apl\rex.dat into the entity slot [0x13AD]
     * (the entity pass's bank, 1000:5823). */
    free(_ent_bank);
    _ent_bank = NULL;
    _ent_bank_size = 0;
    _ent_bank = res_load_chunk("APL\\REX.DAT", &_ent_bank_size, NULL);
    entity_publish_bank(_ent_bank, _ent_bank_size);

    /* load_level_assets 1000:4F89: apl\TER.dat into [0x13AF] (the 0x649A
     * slots' draw pass, 1000:5C2F). */
    free(_ter_bank);
    _ter_bank = NULL;
    _ter_bank_size = 0;
    _ter_bank = res_load_chunk("APL\\TER.DAT", &_ter_bank_size, NULL);
    object_publish_ter(_ter_bank, _ter_bank_size);

    /* Publish the level before the placed-object pass: the spawn's y snap
     * (1000:5639 -> map_get) and the style-6 nudge (1000:5662) read the map
     * and the style, so level_map()/level_style() must stop returning NULL/0
     * here.  Without this the torch (0xAD) keeps its raw row*16 y and floats
     * 4 px above the ground (the reported elevated torch). */
    _level_loaded = 1;

    /* the level's placed objects (load_terrain_b 1000:5168): the array is
     * cleared, then the map's 0xAA/0xAB cells become the humans and the spear. */
    object_init();
    /* the loaded record is the writable map: 1000:51F9 marks two cells a
     * row below (the original works on its [0x1FA0] copy). */
    memset(_level_map_copy, 0, sizeof _level_map_copy);
    memcpy(_level_map_copy, _level_data_cur.map, sizeof _level_map_copy);
    object_load_terrain_objects(_level_map_copy);

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
            _time_limit = save_time();
            _tribe_members = save_members();
        } else {
            uint32_t skill = frontend_option(FRONTEND_OPT_LEVEL);
            _time_limit = (skill == 0u)   ? d->timeA
                          : (skill == 1u) ? d->timeB
                                          : d->timeC;
            _tribe_members = (skill == 0u) ? 12u : (skill == 1u) ? 10u : 8u;
        }
        _level_required = required;
        _members_at_start = _tribe_members; /* [0x1970] (306D)         */
        statusbar_start(_time_limit, _tribe_members, required);
        memset(_pal_bar_only, 0, sizeof _pal_bar_only);

        /* The working palette 0x14D3 (the level's DAC base): entries
         * 0..31 = APL\SPRPAL.APL+0x18 (the UI, verified live), 32..63 =
         * LEVELS\BS<style>.BLK+0x18 (the tiles), 64..127 = the BG's own
         * palette, 128..191 = zero, 192..255 = panel4's (level_video_init's
         * 0xC0-byte 0x1713 copy).  Then the level's fade-in (level_fade_in
         * 1000:337E/338A: the final 256-entry fade, div 0x32). */
        memset(_level_pal, 0, sizeof _level_pal);
        {
            uint32_t spSize = 0;
            uint8_t *sp = res_load_chunk("APL\\SPRPAL.APL", &spSize, NULL);
            if (sp && spSize >= 0x18u + 0x60u)
                memcpy(_level_pal, sp + 0x18, 0x60u);
            free(sp);
        }
        if (_atlas) memcpy(_level_pal + 32u * 3u, _atlas + 0x18, 0x60u);
        if (_bg_image)
            memcpy(_level_pal + 64u * 3u, _bg_image + WORLD_W * WORLD_H, 0xC0u);
        /* copy_level_row 0x4EE9: the BD row block fills 0x80..0xBF. */
        memcpy(_level_pal + 0x80u * 3u, _bd_row_block, sizeof _bd_row_block);
        {
            const uint8_t *bank = statusbar_palette();
            if (bank) {
                memcpy(_level_pal + 0xC0u * 3u, bank, 0xC0u);
                memcpy(_pal_bar_only + 0xC0u * 3u, bank, 0xC0u);
            }
        }
        /* level_fade_in runs after the party (level_run 30AB): level_begin
         * starts the chain once the play begins, not here. */
    }

    /* a fresh load starts the chain again (level_fade_in runs per level) */
    _start_phase = LVSTART_IDLE;
    free(_loading_bank);
    _loading_bank = NULL;
    _loading_size = 0;
    _load_phase = 0; /* the LOADING screen repeats */

    _end_requested = 0; /* [0x138E] = 0xFF in play */
    _end_sound_sent = 0;
    _play_exit_hold = 0;
    _end_won = 1;
    _goal_hit = 0;
    /* the goal flag/pan and its follow override ([0x6530]/[0x6531]/[0x2D8C]):
     * 1000:3540's state is rebuilt by the level's own scan */
    _goal_follow_record = -1;
    _goal_pan_state = GOALPAN_NONE;
    _goal_pan_left = 0;
    _level_loaded = 1;
    return 0;
}

/* --- load_sprite_pal's LOADING screen (1000:1DC0) ---------------------- */

/** @brief load_sprite_pal's palette step (1000:1DCF..1DF2): BACKS\DICON.APL's
 * 0x180 bytes at +0x18 are the screen's DAC (entries 0..0x7F).
 */
void level_loading_begin(void)
{
    if (!_level_loaded || _load_phase) return;

    /* level_video_init (1000:329d) calls reset_display_mode (1000:82c6)
     * before load_sprite_pal's screen: the level's 320x192 field */
    video_set_mode(VIDEO_MODE_X);

    /* 1000:1DCF..1E3F: dicon+0x18 -> 0x14D3 entries 0..0x7F, then the DAC
     * FADES to them (0x80 entries, div 0x19) before the frames run */
    memset(_load_pal, 0, sizeof _load_pal);
    free(_load_dic);
    _load_dic = NULL;
    _load_dic_size = 0;
    /* 1000:1E9C's style table f,s,c,w,d,p indexed by style-1; 0xEA8 is
     * the 'd' in "backs\dicon.apl" — the level's own preview card */
    {
        static const char letter[6] = { 'f', 's', 'c', 'w', 'd', 'p' };
        uint32_t st = _level_data_cur.style;
        char icon[32];
        char l = (st >= 1u && st <= 6u) ? letter[st - 1u] : 'd';
        snprintf(icon, sizeof icon, "BACKS\\%cICON.APL", l);
        _load_dic = res_load_chunk(icon, &_load_dic_size, NULL);
    }
    if (_load_dic && _load_dic_size >= 0x18u + 0x180u) {
        memcpy(_load_pal, _load_dic + 0x18, 0x180u);
        palette_build_fade(0u, 0x80u, 0x19u, _load_pal);
    }
    {
        const uint8_t *bank = statusbar_palette();
        if (bank) palette_set_dac(bank, 0xC0u, 0x40u);
    }
    _loading_bank = res_load_chunk("APL\\LOADING.APL", &_loading_size, NULL);
    if (!_loading_bank) {
        _load_phase = 3;
        return;
    }

    memcpy(_load_list, _load_walkers + 2, sizeof _load_list);
    _load_entries = rd_le_u16(_load_walkers);
    _load_cx = 3; /* particle_frame(3, 0xA0)       */
    _load_reload = 3;
    _load_left = 0xA0u;
    _load_phase = 1;
}

/** @brief level_run (1000:3054) runs level_video_init's two fades (3083) BEFORE
 * load_sprite_pal (3086), so the loading screen exists only once the chain has
 * poured the palette to black.
 *
 * @return 1 while the loading screen is up, 0 otherwise
 */
int32_t level_loading_screen(void)
{
    if (_start_phase != LVSTART_LOADING) return 0;
    if (!_load_phase) level_loading_begin();
    return _load_phase == 1 || _load_phase == 2;
}

/** @brief The background: a clean black page plus BACKS\DICON.APL's 80x56 card at
 * (0x50,0x60), above the level's 40-row status bar (152..191).  The 3310
 * blit writes 20 bytes per plane from the 80-pixel source with the page
 * row stride 0x58; the image is 4 pixels per byte (80 px, not 160 — the
 * party compose's x>>2 law), so it must NOT be doubled.  The card is
 * opaque (index 0 = its black frame) — the level's DESERT preview.
 *
 * @param page the page buffer
 */
static void _load_draw_background(uint8_t *page)
{
    /* load_sprite_pal sets the vertical display scroll [0x2] := 0x32
     * (0x31F2), so every page-relative y loses 0x32: the card's source
     * DI=0x324E row 0x92 becomes 0x60, exactly above the 152-row bar. */
    memset(page, 0, 320u * 200u);
    if (_load_dic && _load_dic_size >= 0x400u + 80u * 56u) {
        const uint8_t *src = _load_dic + 0x400u;
        for (uint32_t r = 0; r < 56u; r++) {
            uint32_t y = 0x60u + r;
            if (y >= 192u) break;
            memcpy(page + y * 320u + 0x78u, src + r * 80u, 80u);
        }
    }
    statusbar_set_active(0);
    statusbar_tick();
}

/**
 * @brief Blit one 16x24 loading-screen sprite at (x, y).
 *
 * @param page the page buffer
 * @param off the offset
 * @param x the x coordinate
 * @param y the y coordinate
 */
static void _load_sprite(uint8_t *page, uint32_t off, int32_t x, int32_t y)
{
    for (uint32_t r = 0; r < 24u; r++)
        for (uint32_t c = 0; c < 16u; c++) {
            int32_t px = x + (int32_t)c;
            uint8_t v = _loading_bank[off + r * 16u + c];

            if (px < 0 || px >= 320 || !v) continue;
            page[(y + r) * 320u + (uint32_t)px] = v;
        }
}

/** @brief blit_sprite_scaled (1000:1F5A): the list x is in the 416-px source
 * coordinate space; the visible 320-px window starts 0x50 pixels in
 * (x/4 - 0x14 bytes), so screen x = list x - 0x50.  The frames are the
 * 16x24 8bpp cells at bank + 0x400 + id*0x180; 0 = transparent.
 *
 * @param page the page buffer
 * @param list the list
 * @param entries the entry count
 * @param lo the low word
 * @param hi the high word
 * @param yoff the y offset
 */
static void _load_draw_one(uint8_t *page, const uint8_t *list, uint32_t entries,
                           int32_t lo, int32_t hi, int32_t yoff)
{
    for (uint32_t e = 0; e < entries; e++) {
        const uint8_t *p = list + e * 10u;
        int32_t x = (int32_t)rd_le_u16(p) - 0x50;
        int32_t y = (int32_t)rd_le_u16(p + 2) + yoff;
        uint32_t spr = rd_le_u16(p + 8);
        if (x <= lo || x >= hi) continue; /* the screen window */
        uint32_t off = 0x400u + spr * 0x180u;
        if (off + 0x180u > _loading_size) continue; /* the 0x25 sentinel  */
        if (y < 0 || y + 24 > 200) continue;
        _load_sprite(page, off, x, y);
    }
}

/**
 * @brief Draw the loading screen's progress list.
 *
 * @param page the page buffer
 */
static void _load_draw_list(uint8_t *page)
{
    /* the arms-up group is the eight under the eight letters (the two edge
     * walkers at screen x=0x50/0xE0 are outside the LOADING! span) */
    if (_load_phase == 2 && _load_walker_count)
        _load_draw_one(page, _load_walker_snap, _load_walker_count, 0x60 - 1,
                       0xD0 + 1, -0x32);
    _load_draw_one(page, _load_list, _load_entries, -16, 320, -0x32);
}

/** @brief particle_frame (1000:1E78) + update_particles (1000:1E98): one frame of
 * the list — the counter reload, the per-entry step and sprite cycle — then
 * the draw and the present.
 */
void level_loading_tick(void)
{
    uint8_t *page;

    if (_load_phase != 1 && _load_phase != 2) return;
    page = _level_osys->videoPage(0);

    if (--_load_cx < 0) _load_cx = _load_reload;
    for (uint32_t e = 0; e < _load_entries; e++) {
        uint8_t *p = _load_list + e * 10u;
        int32_t x = (int32_t)rd_le_u16(p), y = (int32_t)rd_le_u16(p + 2);
        int32_t dx = (int32_t)(int16_t)rd_le_u16(p + 4),
                dy = (int32_t)(int16_t)rd_le_u16(p + 6);
        uint32_t spr = rd_le_u16(p + 8);
        x += dx;
        if (dy) {
            y += dy;
            if (y > 0x56) y = 0x56;
        }
        if (_load_cx == 0 && spr < 0x10u) {
            spr++;
            if (spr == 8u) spr = 0u;
            else if (spr == 0x10u) spr = 8u;
        }
        wr_le_u16(p, (uint32_t)x);
        wr_le_u16(p + 2, (uint32_t)y);
        wr_le_u16(p + 8, spr);
    }
    if (page) {
        _load_draw_background(page);
        _load_draw_list(page);
    }

    if (--_load_left == 0) {
        if (_load_phase == 1) {
            if (page) {
                _load_draw_background(page);
                _load_draw_list(page);
            } /* the extra 1000:1EF8 draw  */
            memcpy(_load_walker_snap, _load_list, sizeof _load_walker_snap);
            _load_walker_count = _load_entries;
            for (uint32_t e = 0; e < _load_walker_count; e++)
                wr_le_u16(_load_walker_snap + e * 10u + 8u,
                          0x10u); /* arms-up hold */
            memcpy(_load_list, _load_letters + 2, sizeof _load_list);
            _load_entries = rd_le_u16(_load_letters);
            _load_cx = 1;
            _load_reload = 1; /* particle_frame(1, 0x56)   */
            _load_left = 0x56u;
            _load_phase = 2;
        } else {
            _load_phase = 3;
            free(_load_dic);
            _load_dic = NULL;
            _load_dic_size = 0;
        }
    }
}

/* level_fade_in (1000:3334), first half: after the party and the level
 * intro, fade the DAC out of the pre-level screen — entries 0..0xBF toward
 * the zeroed 0x48BC ramp, div 0x19.  The rest of the chain (the eight
 * update_world frames, the fade in to 0x14D3, the tick hook) advances in
 * level_tick. */
/** @brief [0x0]/[0x2]: the display window into the 512x320 level page, in pixels and
 * rows — `set_display_ptr` (1000:882D) starts the CRTC at
 * `pagebase + 0x58*[0x2] + [0x0]/4`, and the object pass subtracts the same
 * pair (`draw_object_scaled_x1` 1000:66EC clips `x - [0x0]` against
 * [-0x10,0x140]).  The two stores are level_reset_view's (`1000:319D`), and in
 * play `level_follow_view` below is their only writer.
 *
 * @param x the x coordinate
 * @param y the y coordinate
 */
void level_set_view(uint32_t x, uint32_t y)
{
    _view_x = x;
    _view_y = y;
}

/** @brief The pair the follower adds to the record's position ([0x2D90] indexes
 * DS:0x2D92..0x2DA4).  Outside that range the original would read whatever
 * follows in DS; the pointer cannot leave it (see the walk below), so the port
 * returns zero there (port_deviations.md D-25).
 *
 * @param dx the x delta
 * @param dy the y delta
 */
static void _level_offset_pair(int32_t *dx, int32_t *dy)
{
    uint32_t i = (uint32_t)(_level_offset_ptr - 0x2D92u) / 2u;

    if (i + 1u < sizeof _level_offset_pairs / sizeof _level_offset_pairs[0]) {
        *dx = _level_offset_pairs[i];
        *dy = _level_offset_pairs[i + 1u];
    } else {
        *dx = 0;
        *dy = 0;
    }
}

/** @brief 1000:5D37: the walk at the end of clip_object_x.  While [0x2D8E]'s bit 15 is
 * clear the pointer advances one pair per call (4 bytes); on the last pair
 * (0x2DA2) the original stores `0x2D92` into [0x2D8E] and then decrements that
 * word - i.e. it writes 0x2D91 into the GATE and leaves the pointer on the last
 * pair, so the camera keeps the (1, 1) offset and the gate stays 0x2D91 from
 * then on.  Transcribed as it stands.
 */
static void _level_offset_walk(void)
{
    if (_level_offset_word & 0x8000u) return;
    if (_level_offset_ptr != 0x2DA2u) {
        _level_offset_ptr = (uint16_t)(_level_offset_ptr + 4u);
        return;
    }
    _level_offset_word = 0x2D92u;
    _level_offset_word = (uint16_t)(_level_offset_word - 1u); /* 0x2D91 */
}

/** @brief 1000:5CB6 — the first call of `update_world` (`1000:80AD`), guarded there by
 * `[0xC] != 0`: the camera follows the **acting record** (`get_object_ptr`,
 * `1000:79F4`, walks the 29-word table at `0x3E24` with `[0x3E22]`, i.e.
 * `object_slot(object_action_entity())`), or the record `[0x2D8C]` names while
 * menu_flag_tick's goal pan holds it (`1000:5CB9..5CC0`).
 *
 *   target = record + the offset pair at `[0x2D90]` - (0x88, 0x44)
 *   clamped to the pan range: 0xC0 = 512-320 px, 0xA8 = 320-152 rows (the
 *   play area; the bar owns the last 40 rows), then each axis moves at most
 *   `[0x2DA6]` = 4 px per frame (0x5D09 / 0x5D21), stored at 0x5D2F/0x5D33.
 *
 * The offset pair is **{0,0} at rest**: `[0x2D90]` = 0x2D92 (the first pair of
 * the 0x2D92..0x2DA2 table) and `[0x2D8E]` = 0xFFFF, whose bit 15 idles the
 * 4-entry +-1 px walk at 0x5D37.  Both enablers (`0xDAC7` the entity path and
 * `0xF8CE` the fall machine) are ported; _level_offset_walk advances the pair.
 * `update_view` (1000:80F1) then scrolls the page by the difference; the flat
 * compose reads the window directly, which is the same visible result.
 */
void level_follow_view(void)
{
    uint8_t *rec =
        object_slot(_goal_follow_record >= 0 ? (uint32_t)_goal_follow_record
                                             : object_action_entity());
    int32_t x, y, tx, ty, dx, dy;

    if (!rec) return;
    x = rd_le_i16(rec);
    y = rd_le_i16(rec + 2u);
    {
        int32_t ox, oy;

        _level_offset_pair(&ox, &oy); /* 1000:5CCB/5CCD */
        x += ox;
        y += oy;
    }
    tx = x - 0x88;
    ty = y - 0x44;
    tx = tx < 0 ? 0 : (tx > 0xC0 ? 0xC0 : tx);
    ty = ty < 0 ? 0 : (ty > 0xA8 ? 0xA8 : ty);
    dx = tx - (int32_t)_view_x;
    dy = ty - (int32_t)_view_y;
    if (dx > 4) dx = 4;
    else if (dx < -4) dx = -4;
    if (dy > 4) dy = 4;
    else if (dy < -4) dy = -4;
    _view_x = (uint32_t)((int32_t)_view_x + dx);
    _view_y = (uint32_t)((int32_t)_view_y + dy);
    _level_offset_walk(); /* 1000:5D37 */
}

/**
 * @brief Accessor: the camera's x.
 *
 * @return The camera's x.
 */
uint32_t level_view_x(void) { return _view_x; }
/**
 * @brief Accessor: the camera's y.
 *
 * @return The camera's y.
 */
uint32_t level_view_y(void) { return _view_y; }

/** @brief --- menu_flag_tick (1000:3540): the goal pan -------------------------
 *
 * input_dispatch_loop calls it before scroll_map/update_world.  When the goal
 * scan has raised [0x6530] and the pan has not run, the original finds the
 * first of the 20 live records whose class byte is 9, stores its pointer in
 * [0x2D8C] (the follower's override), then blocks: update_world until the view
 * stops moving, then 0x23 more frames, then [0x6531] = 0xFF, then 0x1E more,
 * then [0x2D8C] = 0.  When [0x6530] falls it only clears [0x6531].
 *
 * The port advances one of those update_world frames per level tick (the tick's
 * own _world update is that frame), so the pan is presented for the same number
 * of frames as the original's blocking loop.  The input word is frozen for the
 * same reason: the original polls only once per outer frame, and it is blocked
 * inside the pan for all of them.
 *
 * @return 1 while the goal pan runs, 0 otherwise
 */
static int32_t _level_goal_pan_running(void)
{
    return _goal_pan_state == GOALPAN_TRACK ||
           _goal_pan_state == GOALPAN_HOLD || _goal_pan_state == GOALPAN_TAIL;
}

/**
 * @brief The goal-text camera pan: one step per tick.
 */
static void _level_goal_pan_tick(void)
{
    if (!object_goal_flag()) { /* 1000:35A2 */
        if (_goal_pan_state != GOALPAN_NONE) {
            object_set_marker_mode(0); /* 1000:35A9 */
            _goal_follow_record = -1;
            _goal_pan_state = GOALPAN_NONE;
        }
        return;
    }
    if (_goal_pan_state == GOALPAN_NONE) { /* 1000:354E */
        for (uint32_t i = 0; i < 20u; i++) {
            if (object_slot_class(1u + i) == 9u) {
                _goal_follow_record = (int32_t)(9u + i);
                break;
            }
        }
        if (_goal_follow_record < 0) { /* 1000:3563: no goal record, no pan */
            _goal_pan_state = GOALPAN_DONE;
            return;
        }
        _goal_pan_joy = _level_joy; /* the pan does not poll again */
        _goal_pan_state = GOALPAN_TRACK;
    } else if (_goal_pan_state == GOALPAN_TRACK) {
        _goal_pan_view_x = _view_x; /* 1000:3568 */
        _goal_pan_view_y = _view_y;
    }
}

/** @brief the pan's accounting, after the tick's update_world (= one of the original's
 * nested frames).
 */
static void _level_goal_pan_after(void)
{
    switch (_goal_pan_state) {
    case GOALPAN_TRACK: /* 1000:3576: the view stopped moving */
        if (_view_x == _goal_pan_view_x && _view_y == _goal_pan_view_y) {
            _goal_pan_state = GOALPAN_HOLD;
            _goal_pan_left = 0x23u;
        }
        break;
    case GOALPAN_HOLD: /* 1000:3582's LOOP CX = 0x23 */
        if (--_goal_pan_left == 0u) {
            object_set_marker_mode(1); /* 1000:358C */
            _goal_pan_state = GOALPAN_TAIL;
            _goal_pan_left = 0x1Eu;
        }
        break;
    case GOALPAN_TAIL: /* 1000:3591's LOOP CX = 0x1E */
        if (--_goal_pan_left == 0u) {
            _goal_follow_record = -1; /* 1000:359B */
            _goal_pan_state = GOALPAN_DONE;
        }
        break;
    default: break;
    }
}

/** @brief [0xC]: the claim clears it at its start (1000:3B19) and restores it before
 * the closing page frames (1000:3CAB).
 *
 * @param on the on/off flag
 */
void level_set_updates(int32_t on) { _world_updates = on ? 1 : 0; }
/**
 * @brief Accessor: the world-update gate.
 *
 * @return The world-update gate.
 */
int32_t level_updates(void) { return _world_updates; }
/**
 * @brief Accessor: set the record's offset word.
 *
 * @param v the value
 */
void level_set_offset_word(uint16_t v) { _level_offset_word = v; }
/**
 * @brief Accessor: the record's offset word.
 *
 * @return The record's offset word.
 */
uint16_t level_offset_word(void) { return _level_offset_word; }
/**
 * @brief test seam: the walk's pointer back to the table's first pair
 */
void level_reset_offset_walk(void) { _level_offset_ptr = 0x2D92u; }

/**
 * @brief level_fade_in: start the level's fade and first play frame.
 */
void level_begin(void)
{
    if (!_level_loaded || _start_phase != LVSTART_IDLE) return;
    /* level_video_init's two component fades, in its own order:
     *   32EE/32F1  two frames of wait (CALL 8B56 twice)
     *   32F4       all 0x100 entries to 0x14D3, div 2, then 8B8B + one frame
     *   3309       entries 0xC0..0xFF to 0x1713, div 2, then 8B8B + one frame
     *   then level_fade_in (0x3334): entries 0..0xBF to 0x48BC, div 0x19.
     * Each wait is one display period; the phase switch below walks them. */
    /* The pre-level screen fades itself to black (the party's 1000:2711,
     * 0x100 entries div 0x23).  In the original level_video_init's fades start
     * from that black; entering the level state before it has finished must
     * not override it - palette_build_fade replaces the ramp, so overriding
     * recoloured the still-lit party art in the level's palette.  Wait here
     * (the caller retries next tick) and let it finish. */
    if (palette_fade_active()) return;
    /* [0x5B12] is level_fade_in's gate and is 0 until the chain completes, so
     * the bar's countdown cannot run during the fades (1000:95B5 reseeds the
     * clock but leaves the gate clear). */
    statusbar_set_active(0);
    /* 1000:32F4: all 0x100 entries to 0x14D3, which 1000:3170 (level_setup)
     * has just zeroed - a fade to BLACK over 2 steps.  level_run (1000:3054)
     * puts it BEFORE the loading screen (level_video_init 1000:3083,
     * load_sprite_pal 1000:3086). */
    palette_build_fade(0u, 0x100u, 2u, _zero_pal); /* 1000:32F4 */
    _start_phase = LVSTART_VIDEO_BASE;
}

/** @brief still inside level_fade_in's chain?  game_main holds the goal page and the
 * play loop until it is done, exactly like the original's foreground block.
 *
 * @return 1 while the fade-in chain runs, 0 otherwise
 */
int32_t level_starting(void)
{ return _level_loaded && _start_phase != LVSTART_PLAYING; }

/**
 * @brief --- level_run's end and the entry state machine's spine --------------
 *
 * @return the members the level needs
 */
uint32_t level_required(void) { return _level_required; }
/**
 * @brief [0x17F0]: the level header's +0x02 low byte, the goal scan's threshold.
 *
 * @return the level's goal count
 */
uint32_t level_goal_count(void)
{ return _level_loaded ? _level_data_cur.goalCount : 0u; }
/**
 * @brief Accessor: the tribe's current member count.
 *
 * @return The tribe's current member count.
 */
uint32_t level_members(void) { return _tribe_members; }
/**
 * @brief 1000:DE1F: the join tail increments the tribe count ([0x11FC]).
 */
void level_add_member(void)
{
    if (_tribe_members < 0xFFu) _tribe_members++;
}

/** @brief level_restore (1000:35AF): the members go back to the level's own start
 * value, then level_start re-picks the time (the port re-runs level_load, so
 * this is called after it).
 */
void level_restore_members(void)
{
    _tribe_members = _members_at_start;
    statusbar_start(_time_limit, _tribe_members, _level_required);
}

/** @brief the level-end test: object_test_dispatch (1000:35D6) returning negative
 * ends the play loop; [0x138E] stays 0xFF for a win and is cleared for the
 * extinct/time-out paths.  The six handlers all exist now: object_test_dispatch
 * maps the level's own mode word onto them (the array predicates, the entity
 * table and the two data conditions this file owns — the goal hook and the
 * tribe count).  level_end_test runs from level_tick's play phase, like the
 * original's call in input_dispatch_loop.
 */
void level_goal_reached(void) { _goal_hit = 1; }

/**
 * @brief One tribe member was lost: decrement the counter.
 */
void level_member_lost(void)
{
    if (_tribe_members > 0u) _tribe_members--;
    statusbar_arm_odometer(); /* the bar rolls down (1000:F682) */
    if (_tribe_members == 0u) {
        _end_requested = 1;
        _end_won = 0;
    }
}

/**
 * @brief The level-end test (object_test_dispatch's negative return).
 *
 * @return 1 when the level-end test fires, 0 otherwise
 */
int32_t level_end_test(void)
{
    if (_end_requested) return 1;
    if (_goal_hit) {
        level_request_end(1);
        return 1;
    }
    /* object_test_dispatch (1000:35D6): the level record's +0x04 word is the
     * goal test.  A level with no test byte (0x0000) relies on the hook. */
    if (_level_loaded && _level_data_cur.objectTest &&
        object_test_dispatch(_level_data_cur.objectTest)) {
        level_request_end(1);
        return 1;
    }
    if (_tribe_members == 0u) {
        level_request_end(0);
        return 1;
    }
    return 0;
}

/**
 * @brief The level's end request with the win/lose outcome.
 *
 * @param won the won flag
 */
void level_request_end(int32_t won)
{
    _end_requested = 1;
    _end_won = won ? 1 : 0;
}
/**
 * @brief Accessor: the play loop's end reached and its hold finished.
 *
 * @return The play loop's end reached and its hold finished.
 */
int32_t level_ended(void) { return _end_requested && _play_exit_hold == 0u; }
/** @brief 1000:34E6: the play loop's exit - after the end test's negative return the
 * loop plays sound 0xC (1000:83B7) and waits 15 vsyncs (1000:8B56) on the
 * frozen _world before returning to draw_level_text.
 *
 * @return 1 while the exit hold runs, 0 otherwise
 */
int32_t level_end_holding(void) { return _play_exit_hold > 0u; }
/** @brief 1000:F66D: the goal-record tail's "no survivor" exit sets [0x138E] = 0 and
 * ends the play loop (the same state object_test_dispatch's negative return
 * produces, reached from the object machine instead).
 *
 * @param won the won flag
 */
void level_set_end(int32_t won)
{
    _end_requested = 1;
    _end_won = won ? 1 : 0;
}
/**
 * @brief Accessor: 1 when the level was won.
 *
 * @return 1 when the level was won.
 */
int32_t level_outcome(void) { return _end_won; }

/** @brief page_offset_to_ptr (1000:F123): 0 when the tile id's 16-byte page mask is
 * all-negative (every sub-cell empty) AND its 16-word item record sums to 0
 * (16-bit wrap, as DX).  The caller (op 3) bails on a non-zero return.
 *
 * @param id the identifier
 * @return 1 when the tile's page bytes are free, 0 otherwise
 */
int32_t level_page_free(uint32_t id)
{
    uint32_t off, k;
    uint16_t sum = 0;

    if (!_page_valid || id > 0xFFu) return 0;
    for (k = 0; k < 16u; k++)
        if (!(_page_mask[id][k] & 0x80u)) return 0; /* a byte >= 0 */
    off = rd_le_u16(_page_items + 2u * id); /* word[B + 2*id] */
    if (off + 32u > _page_items_size) return 0;
    for (k = 0; k < 16u; k++)
        sum = (uint16_t)(sum + rd_le_u16(_page_items + off + 2u * k));
    return sum == 0u;
}

/** @brief The `0x92A0` page mask's byte for a tile id and sub-column: bit 7 = solid,
 * the low 7 bits are the step 1000:E389 follows.  `level_page_free` reads the
 * same table for op 3.
 *
 * @param id the identifier
 * @param subcol the sub-column
 * @return the page mask byte
 */
uint32_t level_page_byte(uint32_t id, uint32_t subcol)
{
    if (!_page_valid || id > 0xFFu || subcol > 0xFu) return 0x80u;
    return _page_mask[id][subcol];
}

/* The `[0x1FD6]` item record's `row` word: `B + word[B + 2*id]` then `2*row`,
 * i.e. the 16-bit sub-cell occupancy mask 1000:F25F tests. 0xFFFF when absent. */
/** @brief The item record's raw byte at `off` from the tile's offset, i.e. from field
 * B.  (`1000:EC27` reads field A, the slope profiles, through
 * `level_slope_byte`; this accessor is the item record's own field-B read.)
 *
 * @param id the identifier
 * @param off the offset
 * @return the item byte, or 0x80 when out of range
 */
uint32_t level_item_byte(uint32_t id, uint32_t off)
{
    uint32_t o;

    if (!_page_valid || id > 0xFFu) return 0x80u;
    o = rd_le_u16(_page_items + 2u * id);
    if (o + off >= _page_items_size) return 0x80u;
    return _page_items[o + off];
}

/**
 * @brief The map cell's item word (tile row and sub-column).
 *
 * @param id the identifier
 * @param row the row
 * @return the item word, or 0xFFFF when out of range
 */
uint32_t level_item_word(uint32_t id, uint32_t row)
{
    uint32_t off;

    if (!_page_valid || id > 0xFFu || row > 0xFu) return 0xFFFFu;
    off = rd_le_u16(_page_items + 2u * id);
    if (off + 32u > _page_items_size) return 0xFFFFu;
    return rd_le_u16(_page_items + off + 2u * row);
}

/** @brief Field K, the level's four special tile ids: `[0x6F20]`/`[0x6F21]` (the walk
 * states compare the cell under the feet with them) and the second word
 * `[0x6F0B]`/`[0x6F0C]` (`1000:4E99`, which the fall machine's `1000:F7CD`
 * type compare indexes with `type - 0x19`, and `1000:E30E` reads directly).
 *
 * @param i the index
 * @return the special tile id, or 0xFFFF when out of range
 */
uint32_t level_special_tile(uint32_t i)
{ return i < 4u ? _k_bytes[i] : 0xFFFFu; }

/**
 * @brief The record's style byte (header +0, `[0x17EE]`): 1000:5662 tests it against 6.
 *
 * @return the level style
 */
uint32_t level_style(void)
{ return _level_loaded ? _level_data_cur.style : 0u; }

/**
 * @brief Accessor: the 640-cell map buffer.
 *
 * @return The 640-cell map buffer.
 */
const uint8_t *level_map(void)
{ return _level_loaded ? _level_map_copy : NULL; }

/**
 * @brief the level's working palette 202F:0x14D3 (_level_pal above)
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *level_palette(void) { return _level_pal; }

/** @brief draw_level_text 0x33E2 + 0x33F2: the level palette's 0..0x7F becomes grey
 * (seal_triples), then the shipped gradient fills 0x80..0xBF.  The 0x33FD
 * fade targets this palette.
 */
void level_end_palette(void)
{
    palette_seal_triples(_level_pal, 0u, 0x80u);
    memcpy(_level_pal + 0x80u * 3u, _level_end_gradient,
           sizeof _level_end_gradient);
}

/** @brief [0x5D31] and [0x11FC] after the LEVEL board's pick (the bar's seed; the
 * party screen reads the members too).
 *
 * @return the time limit word
 */
uint16_t level_time_limit(void) { return _time_limit; }
/**
 * @brief Accessor: the tribe's member count.
 *
 * @return The tribe's member count.
 */
uint32_t level_tribe_members(void) { return _tribe_members; }

/** @brief The play loop's input (1000:8526's output; the backend already produces the
 * same shape in InputState.joyBits).  Kept out of level_tick's signature so the
 * front-end screens call it the same way as the original's shared sampler.
 *
 * @param in the input state
 */
void level_note_input(const InputState *in)
{ _level_joy = in ? in->joyBits : 0u; }

/**
 * @brief Stamp one 16x16 terrain tile into the world page.
 *
 * @param dst the destination buffer
 * @param src the source buffer
 */
static void _stamp_tile(uint8_t *dst, const uint8_t *src)
{
    for (uint32_t y = 0; y < 16u; y++) {
        for (uint32_t x = 0; x < 16u; x++) {
            uint8_t v = src[y * 16u + x];
            /* draw_terrain 0x502d: a zero tile byte is NOT stored - the
             * background (BG art / 0x80 sky) stays. */
            if (v) dst[y * WORLD_W + x] = (uint8_t)(v + 0x20u);
        }
    }
}

/** @brief The _world pass: `render_level_frame`'s terrain + `render_objects` + the bar,
 * i.e. everything 1000:4C28's pre-draw and every play frame put on the page.
 * The original's `blit_bitmap_call` (`1000:4C28`) draws the level bitmap into
 * BOTH pages before level_fade_in's eight `update_world` frames:
 *   DI = 0x1200 -> CALL 0x4C4E     (page A)
 *   DI = 0x8100 -> CALL 0x4C4E     (page B)
 * The flat port has one page, so a single pass is the whole of that pre-draw,
 * and the same function serves the frames after it.
 */
static void _render_world(void)
{
    if (_bg_image) memcpy(_world, _bg_image, WORLD_W * WORLD_H);
    else memset(_world, 0, WORLD_W * WORLD_H);

    for (uint32_t r = 0; r < MAP_ROWS; r++) {
        for (uint32_t c = 0; c < MAP_COLS; c++) {
            uint32_t tile = _level_data_cur.map[r * MAP_COLS + c];
            if (tile >= 0xA9u) continue;
            _stamp_tile(_world + (r * 16u) * WORLD_W + c * 16u,
                        _atlas + 0x400u + tile * 0x100u);
        }
    }

    /* 1000:3D5E's four-plane presented frame, as a flat compose: the visible
     * window is the _world at the display window ([0x0], [0x2] — see
     * `level_follow_view`), written through the chained page and read back.
     * ([0x1AEA]/[0x1AEC] are 1000:3DC0's *strip save* coordinate, not the
     * window: that routine copies a 64x18-byte page strip to A000:0xF000 and
     * never scrolls.) */
    for (uint32_t y = 0; y < VIEW_H; y++)
        for (uint32_t x = 0; x < VIEW_W; x++) {
            uint32_t wx = _view_x + x, wy = _view_y + y;

            _lvl_plane[y * 88u + (x >> 2)][x & 3u] =
                (wx < WORLD_W && wy < WORLD_H) ? _world[wy * WORLD_W + wx] : 0u;
        }
    if (_spark_on) _spark_draw();
    uint8_t *page = _level_osys->videoPage(0);
    for (uint32_t y = 0; y < VIEW_H; y++)
        for (uint32_t x = 0; x < VIEW_W; x++)
            page[y * VIEW_W + x] = _lvl_plane[y * 88u + (x >> 2)][x & 3u];

    /* the object pass (render_objects 1000:5D56): the placed records' base
     * sprites over the terrain, before the bar */
    object_render(page, (int32_t)_view_x, (int32_t)_view_y);

    /* the entity pass (render_objects 1000:5D56: 0x6421 then 0x5823) */
    entity_render(page, (int32_t)_view_x, (int32_t)_view_y);

    /* update_object_list's tail: the two TER.DAT slots (1000:5C2F), off on
     * shipped data because 1000:4CAB disables them */
    object_render_ter(page, (int32_t)_view_x, (int32_t)_view_y);

    /* render_objects' tail (1000:5F54): the 0xA9 background sprites */
    level_draw_bg_sprites(page, (int32_t)_view_x, (int32_t)_view_y);

    /* the bar over the _world rows (the original's band lives below the
     * 192-row visible window via the line compare; the flat page redraws
     * band + marks per tick instead) */
    statusbar_tick();
}

/** @brief draw_level_text 0x344C/0x3450: blit_bitmap_planes + render_objects render
 * the _world into the page after the caption has been dismissed, so the page
 * behind check_highscore's card is the _world, not the caption.
 */
void level_redraw_page(void)
{
    if (!_level_loaded || !_level_osys) return;
    _render_world();
}

/** @brief draw_level_text 1000:3437: the end page clamps [0x2] to 0x78 before its two
 * page frames; the clamped value persists in the original's view.
 */
void level_end_redraw_page(void)
{
    if (!_level_loaded || !_level_osys) return;
    if (level_view_y() > 0x78u) level_set_view(level_view_x(), 0x78u);
    _render_world();
}

/** @brief 1000:3D5E: the claim's pan frames recolour every page byte below 0x80 to
 * 0x1E before the present (the original's four plane passes are one flat page
 * here).
 */
void level_claim_recolour(void)
{
    uint8_t *page = video_page(0);

    if (!page) return;
    for (uint32_t i = 0; i < VIEW_W * 200u; i++)
        if (page[i] < 0x80u) page[i] = 0x1Eu;
}

/**
 * @brief One play frame: update the world, render and pump the effects.
 *
 * @return 1 while the level is running
 */
int32_t level_tick(void)
{
    if (!_level_loaded) return 0;

    /* 1000:34E6: the exit hold - the _world is frozen and the last page is
     * presented until the 15 waits expire. */
    if (_play_exit_hold > 0u) {
        _play_exit_hold--;
        return 1;
    }

    if (!_world || !_level_osys) return 1;

    /* level_fade_in (1000:3334) — the chain: the fade-out waits for its
     * steps, then the eight update_world (1000:80AD) frames, then the final
     * 256-entry fade to 0x14D3 (div 0x32), then the hook is installed (the
     * bar's ticking gate [0x5B12] := 0xFF).  The _world is kept drawn behind
     * each phase. */
    /* The play loop leaves at 1000:34E6 and never calls update_world
     * (1000:80AD) again; the end caption (draw_level_text 0x33A3) only
     * renders the frozen _world.  `_end_requested` is that exit. */
    int32_t doUpdate = (_start_phase == LVSTART_PLAYING &&
                        (!_end_requested || _level_goal_pan_running()))
                           ? 1
                           : 0;

    switch (_start_phase) {
    case LVSTART_IDLE: level_begin(); break;
    case LVSTART_VIDEO_BASE:
        if (!palette_fade_active()) {
            /* 1000:3309: the `0xC0..0xFF` block to 0x1713, filled at 1000:32CA
             * with panel4's palette (1000:4BBC) - the bar's colours. */
            palette_build_fade(0xC0u, 0x40u, 2u, statusbar_palette());
            _start_phase = LVSTART_VIDEO_BAR;
        }
        break;
    case LVSTART_VIDEO_BAR:
        if (!palette_fade_active())
            _start_phase = LVSTART_LOADING; /* 1000:3086 runs the loading */
        break;
    case LVSTART_LOADING:
        if (level_loading_screen()) break; /* 1000:1DC0's screen is up */
        /* 1000:3334: the loading art fades to black (0..0xBF -> 0x48BC, div 0x19) */
        palette_build_fade(0u, 0xC0u, 0x19u, _black_pal);
        _start_phase = LVSTART_FADE_OUT;
        break;
    case LVSTART_FADE_OUT:
        if (!palette_fade_active()) {
            /* 1000:3334's order after the fade-out: the scroll seeds (set in
             * the LOADING case above), slot 8's init, then 1000:4C28
             * blit_bitmap_call - the _world drawn into BOTH pages (DI = 0x1200
             * then 0x8100) before the eight update_world frames.  The port has
             * one page, so its first WARMUP tick is that pre-draw. */
            /* 1000:3346/334C/334F: the display window's seeds - the eight
             * update_world frames run with the camera at (0x60,0x50), and
             * 1000:5CB6 walks it onto the acting record at <=4 px a frame. */
            level_set_view(0x60u, 0x50u);
            object_level_start_record(); /* 1000:3355 */
            _world_updates = 1; /* 1000:336C: [0xC] = 0xFF */
            _render_world(); /* 1000:4C28's pre-draw (one page) */
            _start_phase = LVSTART_WARMUP;
            _warmup_left = 8u; /* level_fade_in's LOOP CX=8 */
        }
        break;
    case LVSTART_WARMUP:
        doUpdate = 1; /* 1000:3374's eight frames */
        if (--_warmup_left == 0u) {
            palette_build_fade(0, 256, 0x32, _level_pal);
            _start_phase = LVSTART_FADE_IN;
        }
        break;
    case LVSTART_FADE_IN:
        if (!palette_fade_active()) {
            _start_phase = LVSTART_PLAYING;
            statusbar_set_active(1); /* level_fade_in's [0x5B12] */
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
    for (uint32_t e = 0; e < _anim_count && _start_phase == LVSTART_PLAYING &&
                         !palette_fade_active() && !object_claim_active();
         e++) {
        uint8_t *a = _anim_table + 4u * e;
        uint32_t first = a[2], n = a[3];
        if (n < 2u || first + n > 256u) continue;
        a[0] = (uint8_t)(a[0] - 1u);
        if (!(a[0] & 0x80u)) continue; /* still non-negative     */
        a[0] = a[1];
        uint8_t *p = _level_pal + first * 3u;
        uint8_t last[3];
        memcpy(last, p + (n - 1u) * 3u, 3);
        for (uint32_t i = n - 1u; i > 0u; i--)
            memcpy(p + i * 3u, p + (i - 1u) * 3u, 3);
        memcpy(p, last, 3);
        palette_set_dac(p, first, n);
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
        uint8_t *actor = object_slot(object_action_entity());

        if (_world_updates && !object_claim_active()) {
            _level_goal_pan_tick(); /* 1000:3540, before update_world */
            level_follow_view(); /* 1000:5CB6, first in update_world */
            if (actor)
                (void)object_command_run(actor, _level_goal_pan_running()
                                                    ? _goal_pan_joy
                                                    : _level_joy);
            object_update_list(); /* 1000:C295, 1000:80BD            */
            entity_tick(); /* 1000:C93C, inside the [0xC] gate */
            object_goal_scan(); /* 1000:C2CE, the goal count        */
            object_marker_tick(); /* 1000:D64F, after the dispatch */
            object_world_tick(); /* 1000:80CC/0x80CF, update_world's tails */
            object_row_pass(); /* 1000:D860, the static records. rows */
            _level_goal_pan_after(); /* the frame the pan just ran     */
        }
        /* The claim's own frames only: the original sequence never calls
         * update_world, even after 1000:3CAB sets [0xC] = 0xFF. */
        object_claim_frame(level_palette());
    }

    /* The _world is not drawn during level_video_init's two component fades
     * (1000:32F4 / 1000:3309): in the original the screen still holds the
     * pre-level art there, so those fades recolour IT while the DAC walks to
     * 0x14D3 - the _world's first frames are the eight update_world passes of
     * level_fade_in, drawn while the DAC is at black (the fade-out of 0..0xBF
     * runs first, 1000:3334).  Drawing here made the level appear at full
     * brightness for those frames and then fade to black and in again. */
    /* 0x3FB3 is the play frame's flush; the chain's fades and the loading come
     * before the first play frame, so the effect is not advanced there. */
    if (_start_phase == LVSTART_PLAYING) _spark_step();
    if (_start_phase != LVSTART_WARMUP && _start_phase != LVSTART_FADE_IN &&
        _start_phase != LVSTART_PLAYING) {
        /* the bar is up from the mode reset onward (1000:952C/957B), so it is
         * redrawn here while the _world waits for its black frames */
        statusbar_tick();
        return 1;
    }

    _render_world();
    if (object_claim_pan_active()) /* 1000:3D5E's recolour on both pan frames */
        level_claim_recolour();

    /* the pending-event dispatch ([0x1979] -> CS:0x197B) and, when event 4 is
     * running, its per-frame share (1000:3702): the _world frame has run, so
     * redraw the jump/throw bar at the record's new +0xE */
    object_pending_run();
    (void)object_charge_tick();

    /* input_dispatch_loop 1000:34B8's object_test_dispatch call, after the
     * _world tick: a negative return ends the play loop.  menu_flag_tick's pan
     * blocks the outer loop, so the test cannot run while it is up. */
    if (_start_phase == LVSTART_PLAYING && !_level_goal_pan_running()) {
        (void)level_end_test();
        if (_end_requested && !_end_sound_sent) { /* 1000:34E6 */
            object_driver_call(0x0Cu); /* 1000:83B7 with AX = 0xC */
            _end_sound_sent = 1;
            _play_exit_hold = 0x0Fu; /* MOV CX,0xF / LOOP 8B56 */
        }
    }

    /* the colours are the working palette 202F:0x14D3, built in level_load
     * and faded in by the first tick above (re_map.md "The level's working
     * palette"). */
    return 1;
}

/**
 * @brief Draw the insert-disc prompt for one disc.
 *
 * @param disc the disc number
 */
static void _disk_draw(uint32_t disc)
{
    uint8_t *page = video_page(0);
    memset(page, 0, VIEW_W * 200u);

    uint8_t *img = NULL;
    uint32_t isz = 0;
    uint32_t used = disc;
    for (uint32_t k = 0; k < 3u && !img; k++) {
        used = (k == 0u) ? disc : (k == 1u ? 0u : (disc == 1u ? 2u : 1u));
        res_set_disc(used);
        img = res_load_chunk("APL\\DISKIMG1.DAT", &isz, NULL);
    }
    if (img) {
        if (isz >= 0x18u + 768u) memcpy(_disk_pal, img + 0x18, 768u);
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

    free(_disk_font);
    _disk_font = res_load_chunk("APL\\TEXT.SPR", NULL, NULL);
    font_publish(_disk_font);

    char line[32];
    textpage_draw_line(page, "PUT THE DISK", 0x50u, 0x0Fu);
    snprintf(line, sizeof line, "INTO DRIVE %c", (char)('1' + disc));
    textpage_draw_line(page, line, 0x60u, 0x0Fu);
    textpage_draw_line(page, "PRESS FIRE", 0x88u, 0x0Fu);
    textpage_draw_line(page, "WHEN READY", 0x98u, 0x0Fu);
}

/**
 * @brief Start the disc-presence indicator for one disc.
 *
 * @param disc the disc number
 * @return 1 when the prompt is up, 0 when the disc is present
 */
int32_t level_disk_status_begin(uint32_t disc)
{
    if (_disk_phase != DISK_OFF) return 1;
    if (res_volume_present(disc)) return 0; /* 1000:2005: the disc is in */

    /* draw_status_indicator (1000:1fef) sets mode 13h on the missing-disc
     * page (its flat 320x200 image) */
    video_set_mode(VIDEO_MODE_13H);
    _disk_disc = disc;
    memset(_disk_pal, 0, sizeof _disk_pal);
    _disk_draw(disc);
    palette_build_fade(0u, 0x100u, 0x1Eu, _disk_pal); /* 1000:2057 */
    _disk_phase = DISK_FADE_IN;
    return 1;
}

/**
 * @brief Clear the disc indicator's state.
 */
void level_disk_status_reset(void)
{
    _disk_phase = DISK_OFF;
    _disk_hold = 0;
    free(_disk_font);
    _disk_font = NULL;
    font_publish(NULL);
}

/**
 * @brief Tick the disc indicator; 1 = still waiting.
 *
 * @param in the input state
 * @return 1 while the prompt is still up
 */
int32_t level_disk_status_tick(const InputState *in)
{
    switch (_disk_phase) {
    case DISK_FADE_IN:
        if (palette_fade_active()) return 1;
        _disk_phase = DISK_WAIT;
        return 1;
    case DISK_WAIT:
        if (in->lastKey || (in->joyBits & 0x80u)) {
            if (res_volume_present(_disk_disc)) {
                palette_build_fade(0u, 0x100u, 0x1Eu, _disk_black);
                _disk_phase = DISK_FADE_OUT;
            }
            /* else: still the wrong disc — stay on the prompt */
        }
        return 1;
    case DISK_FADE_OUT:
        if (palette_fade_active()) return 1;
        _disk_phase = DISK_HOLD;
        _disk_hold = 0x4Bu; /* present_frames_75 */
        return 1;
    case DISK_HOLD:
        if (_disk_hold > 0u) {
            _disk_hold--;
            return 1;
        }
        _disk_phase = DISK_OFF;
        free(_disk_font);
        _disk_font = NULL;
        font_publish(NULL);
        return 0;
    default: return 0;
    }
}
