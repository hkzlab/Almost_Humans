#include "entity.h"
#include "frontend.h"
#include "game_resources.h"
#include "level/level.h"
#include "level/level_internal.h"
#include "object/object.h"
#include "osystem.h"
#include "res.h"
#include "save.h"
#include "statusbar.h"
#include "textpage.h"
#include "byteorder.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t _rec_sig[SIG_LEN] = { 0x00, 0x20, 0x00, 0x14, 0x00, 0x00 };

/* The loaded record and the level's own state. */
static LevelData _level_data_cur;
/* The writable map the game plays on: 1000:3138 copies the record's 640 bytes
 * to [0x1FA0], and 1000:51F9 (the 0xA9 handler) marks two cells of it.  Keeping
 * the copy means the parsed record stays byte-exact (tests/test_level). */
static uint8_t _level_map_copy[640];
static uint8_t _level_loaded;
static uint8_t *_box_spr;           // apl\box.spr (the dialog tiles)
static uint8_t *_obj_bank;          // apl\sprites.dat (the object sprites)
static uint8_t *_ent_bank;          // apl\rex.dat (the entity sprites)
static uint32_t _ent_bank_size;
static uint8_t *_ter_bank;          // apl\TER.dat (the 0x649A slots)
static uint32_t _ter_bank_size;
static uint32_t _obj_bank_size;
static uint32_t _box_size;

uint32_t level_style(void)
{ return _level_loaded ? _level_data_cur.style : 0u; }

const uint8_t *level_map(void)
{ return _level_loaded ? _level_map_copy : NULL; }

uint8_t _level_is_loaded(void) { return _level_loaded; }

const LevelData *_level_record(void) { return &_level_data_cur; }

int32_t level_init(void) { return 0; }

const LevelData *level_data(void)
{ return _level_loaded ? &_level_data_cur : NULL; }

/** @brief The level text carries literal CR and tab from the authoring tool - the
 * census's _TEXT set; rejecting them silently
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
    q++; // the goal's NUL

    uint32_t authorStart = q;
    while (q < size && _is_text_byte(b[q])) q++;
    if (q - authorStart < 2u) return 0;
    if (q >= size || b[q] != 0u) return 0;
    q++; // the author's NUL

    if (q + SIG_LEN > size || memcmp(b + q, _rec_sig, SIG_LEN) != 0) return 0;
    return q + FIELD_LEN; // the map's start
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
    return 0; // no further record
}

int32_t level_load(uint16_t levelId)
{
    if (levelId >= 160u) return -1;

    uint16_t disc = 1u; // the L*.BIN files ship on HUMANS.001
    res_set_disc(disc);

    char name[32];
    snprintf(name, sizeof name, game_resource_name(GAME_RES_LEVELS_L_FMT),
             'A' + levelId / 10u);

    uint32_t fileSize = 0;
    uint16_t chk = 0;
    uint8_t *image = res_load_chunk(name, &fileSize, &chk);
    if (!image) return -1;

    /* walk to the record levelId%10 */
    uint32_t p = 0;
    uint8_t rec = 0;
    LevelData *d = &_level_data_cur;
    memset(d, 0, sizeof *d);

    uint8_t ok = 0;
    while (p + REC_HDR <= fileSize) {
        uint32_t mapOff = _rec_is_start(image, fileSize, p);
        if (mapOff == 0) break; // the walk: the candidates tile the file

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
                uint8_t n = 0;
                while (q < fileSize && image[q] != 0u &&
                       n + 1 < sizeof d->goal) {
                    d->goal[n++] = (char)image[q++];
                }
                d->goal[n] = 0;
                q++; // the NUL
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
        if (p == 0) break; // the file's records exhausted
    }

    g_osys->memFree(image);
    if (!ok) return -1;

    /* the header's style selects the visual set; the census across all 160
     * levels doubles as the BD-walk byte-exactness check (every style 1..6
     * walks to EOF or the load fails) */
    if (_level_assets_load(d->style) != 0) return -1;
    _level_stamp_exits(); // 1000:4F73 load_terrain_a
    _level_world_build(); // draw_terrain 1000:502D: once per load, not per frame

    /* load_level_assets (1000:3468): the in-level dialog's box tiles
     * (apl\box.spr, 9 x 16x16 at [0x13B1]) load with the level's assets. */
    g_osys->memFree(_box_spr);
    _box_spr = res_load_chunk(game_resource_name(GAME_RES_APL_BOX_SPR),
                              &_box_size, NULL);
    textpage_publish_box(_box_spr && _box_size >= 0x900u ? _box_spr : NULL);

    /* load_level_assets (1000:3488): APL\SPRITES.DAT is the object sprite
     * image (81744 = 0x13F50 bytes); the object pass draws from it. */
    g_osys->memFree(_obj_bank);
    _obj_bank = NULL;
    _obj_bank = res_load_chunk(game_resource_name(GAME_RES_APL_SPRITES_DAT),
                               &_obj_bank_size, NULL);
    object_render_publish_bank(_obj_bank, _obj_bank_size);

    /* load_level_assets 1000:4F82: apl\rex.dat into the entity slot [0x13AD]
     * (the entity pass's bank, 1000:5823). */
    g_osys->memFree(_ent_bank);
    _ent_bank = NULL;
    _ent_bank_size = 0;
    _ent_bank = res_load_chunk(game_resource_name(GAME_RES_APL_REX_DAT),
                               &_ent_bank_size, NULL);
    entity_publish_bank(_ent_bank, _ent_bank_size);

    /* load_level_assets 1000:4F89: apl\TER.dat into [0x13AF] (the 0x649A
     * slots' draw pass, 1000:5C2F). */
    g_osys->memFree(_ter_bank);
    _ter_bank = NULL;
    _ter_bank_size = 0;
    _ter_bank = res_load_chunk(game_resource_name(GAME_RES_APL_TER_DAT),
                               &_ter_bank_size, NULL);
    object_render_publish_ter(_ter_bank, _ter_bank_size);

    /* Publish the level before the placed-object pass: the spawn's y snap
     * (1000:5639 -> map_get) and the style-6 nudge (1000:5662) read the map
     * and the style, so level_map()/level_style() must stop returning NULL/0
     * here.  Without this the torch (0xAD) keeps its raw row*16 y and floats
     * 4 px above the ground (the reported elevated torch). */
    _level_loaded = 1;

    /* the level's placed objects (load_terrain_b 1000:5168): the array is
     * cleared, then the map's 0xAA/0xAB cells become the humans and the spear. */
    object_core_init();
    /* the loaded record is the writable map: 1000:51F9 marks two cells a
     * row below (the original works on its [0x1FA0] copy). */
    memset(_level_map_copy, 0, sizeof _level_map_copy);
    memcpy(_level_map_copy, _level_data_cur.map, sizeof _level_map_copy);
    object_spawn_load_terrain_objects(_level_map_copy);

    /* the bar (level_start 1000:957B + build_statusbar_layout 1000:961B):
     * the time limit and the tribe pair.  The time comes from the LEVEL
     * board's counter [0x5E65] read per level: render_level_frame
     * (1000:9589) selects [0x17F5]/[0x17F7]/[0x17F9] - this record's
     * timeA/B/C - by 0/1/else.  With a save loaded ([0x34], render_level_frame
     * 1000:959E) the save's own +0x07 word and +0x0B members replace both.
     * The tribe count is NOT re-seeded here: entry's 1000:009C..0x00B1 seed
     * (level_seed_tribe) runs once per game, and the count then carries from
     * level to level (load_level 1000:30BC only reads [0x11FC] at the gate).
     * The required = the map's 0xAA-tile count (load_level 1000:315F counts
     * the first 640-byte block). */
    {
        uint16_t required = 0;
        for (uint16_t i = 0; i < MAP_LEN; i++)
            if (d->map[i] == 0xAAu) required++;
        uint16_t time, members;

        if (save_present()) {
            time = save_time();
            members = save_members();
        } else {
            uint8_t skill = frontend_option(FRONTEND_OPT_LEVEL);

            time = (skill == 0u)   ? d->timeA
                   : (skill == 1u) ? d->timeB
                                   : d->timeC;
            members = level_members();
        }
        _level_tribe_configure(time, members, required);
        statusbar_start(time, members, required);

        _level_palette_build();

        /* level_fade_in runs after the party (level_run 30AB): level_begin
         * starts the chain once the play begins, not here. */
    }

    /* a fresh load starts the chain again (level_fade_in runs per level) */
    _level_phase_reset();
    _level_loading_reset(); // the LOADING screen repeats

    _level_tribe_reset(); // [0x138E] = 0xFF in play
    /* the goal flag/pan and its follow override ([0x6530]/[0x6531]/[0x2D8C]):
     * 1000:3540's state is rebuilt by the level's own scan */
    _level_view_reset();
    _level_loaded = 1;
    return 0;
}
