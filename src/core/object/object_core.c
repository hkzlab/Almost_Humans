#include "object_internal.h"
#include "object_command.h"
#include "object_core.h"
#include "audio.h"
#include "entity.h"
#include "level/level.h"
#include "byteorder.h"
#include <string.h>

/* forward declarations */
static uint32_t _obj_core_map_cell_index(const uint8_t *rec);
static uint32_t _obj_core_map_cell(const uint8_t *rec);
static uint32_t _obj_core_goal_cell_index(uint32_t x, uint32_t y);
static int32_t _goal_group(uint32_t i, uint32_t *x, uint32_t *y);
static uint16_t _obj_core_span_flag(int32_t d, uint32_t frame);
static void _obj_core_span_top_reset(void);


uint8_t _obj_array[OBJ_SLOTS * OBJ_STRIDE];

static int16_t _obj_current = -1; // [0x1AF2], -1 = none

int16_t _obj_player = -1; // [0x180A], -1 = none

uint16_t _obj_live_count = OBJ_LIVE0; // [0x3E73]: the array's used slots

uint16_t _obj_placed_humans; // [0x3E20]: the 0xAA handlers' count

uint16_t _obj_entity; // [0x3E22]: the acting record

uint16_t _obj_cmd_timer; // [0x6DEA]: the command one-shot

uint16_t _obj_stream_count; // [0x6DEC]: stream hold countdown

const uint8_t *_obj_stream_ptr; // [0x6DEE]: the script stream

/* 0x64AD / 0x64C1: the "in a stack" bits live in the HIGH byte of the two TER
 * slots' +0x12 words (0x64AC/0x64C0, `_obj_ter_slot[s][0x13]`), so
 * set_camera_from (1000:C8A4) reads the very bits the stack pushes and the
 * 1000:F423 attach box set, and carries the marked records with the patrol. */
int16_t _obj_side; // [0x4230]: which side the settle stepped from

uint8_t _obj_sequence_active; // [0x6F1D]: sequence owns input

uint8_t _obj_pending; // [0x1979]

uint8_t _obj_cursor = 1; // [0x1988]

/* [0x1987] / the port's "inside 0x36DB" state: the turn arms the flag
 * (1000:EE35) and the pending-event 4 charge loop consumes it (1000:370C). */
uint8_t _obj_charge_arm;

uint8_t _obj_charging;

/* [0x5ACE] latched when event 4 starts: the original's 0x36DB loop does not
 * poll input, so every update_world reuses the press frame's word.  The live
 * charge has the fire bit set and [0x13A4] clear. */
uint8_t _obj_charge_joy;

/* [0x42F0], the ACTION edge's guard, is **never set** in this image: the only
 * two references in the whole file are the test and the clear at 0xE3F9-0xE407,
 * so the edge (0xE40C's `CALL 0x392D`) is dead in the shipped build and the port
 * does not run it.  That is what lets a fire press reach the type row's fire
 * half (D-68). */
/* 0x6F22: 8 entries of 7 words (0xE bytes each; the clear is 0x70 bytes at
 * 0x60F4; op 1 walks AX = 7..0); bit 15 = free. */
uint16_t _obj_register[8][7];

uint16_t _obj_span[2][14]; // 0x6F92, 2 x 0x1C: {id, .. +6, +8, +0x12}

/* The row each span stripe starts at.  The original has no field for it: the
 * stripe's already-painted pixels are the page, and 1000:5D65 redraws from
 * them (the 0x2DB0 area only holds the saved background).  The port rebuilds
 * the page every frame, so it keeps the seed row here. */
uint16_t _obj_span_top[2];

/* [0x6530]: count_tile_collisions' level-complete flag (1000:C438). */
static uint8_t _obj_goal_flag;


/* [0x3E5E + b] - the class byte of the object in source slot b (0 = none).
 * `pred_sprite_class_match` (1000:3659) reads it as `[0x3E5E + rec[+7]] & 0x3F`
 * and the claim frees the slot (`[0x3E5E + rec[7]] = 0`, 1000:3B1E); the spawn
 * writes the behaviour id at `[0x3E5F + n]`, i.e. slot (n+1). */
uint8_t _obj_slot_class[0x40];


/** @brief The original's `[0x3E5E + b]` is not an array: it is a byte window of the
 * data segment - the 21 class bytes 0x3E5E..0x3E72, the live-count word at
 * 0x3E73/0x3E74, then the 29 0x12-byte records at 0x3E75.  A record's `+7` is
 * that byte index (1000:3659, 0x4147, 0xF2CF read it directly; 0x653C, 0x9D5D,
 * 0xF603 and 0xDD3C reach it as 0x3E5F + cls - 1), and the byte can be any
 * value a record carries (1000:D12B writes 0x99), so the accessors map the
 * whole window instead of indexing _obj_slot_class past its end.
 *
 * @param b the class byte
 * @return the class byte at b
 */
uint8_t _obj_core_class_byte_at(uint32_t b)
{
    b &= 0xFFu;
    if (b < 0x15u) return _obj_slot_class[b]; // 0x3E5E..0x3E72
    if (b == 0x15u) return (uint8_t)_obj_live_count; // 0x3E73
    if (b == 0x16u) return (uint8_t)(_obj_live_count >> 8); // 0x3E74
    return _obj_array[b - 0x17u]; // 0x3E75.., b <= 0xFF
}


/**
 * @brief Set one slot's class byte.
 *
 * @param b the class byte
 * @param value the value
 */
void _obj_core_class_byte_set(uint32_t b, uint8_t value)
{
    b &= 0xFFu;
    if (b < 0x15u) {
        _obj_slot_class[b] = value;
        return;
    }
    if (b == 0x15u) {
        _obj_live_count = (uint16_t)((_obj_live_count & 0xFF00u) | value);
        return;
    }
    if (b == 0x16u) {
        _obj_live_count =
            (uint16_t)((_obj_live_count & 0x00FFu) | (value << 8));
        return;
    }
    _obj_array[b - 0x17u] = value;
}


/* 1000:EEB8's class-0 record write.  `DEC DX` wraps to 0xFFFF and `MUL DL`
 * uses 0xFF, so the original writes 0x510D..0x5114.  No image instruction
 * reads that range, so the port drops the write (D-271). */

/* The four shipped recipes (202F:0x19BA, 12 bytes each): [uses][behaviour]
 * then the sprite word and the four parameter words, re-armed to one use
 * every level start (level_setup 1000:31C3). */
const uint8_t _obj_recipes[4][12] = {
    { 1, 5, 0x00, 0x60, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0 }, // op 11 -> behaviour 5
    { 1, 7, 0x00, 0x96, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0 }, // op 12 -> behaviour 7
    { 1, 6, 0x00, 0x9C, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0 }, // AX=2, never fired
    { 1, 4, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF },
    /* op 10: the image's 01 04 00 38 00 00 00 00 ff ff ff ff */
};

uint8_t _obj_uses[4]; // the live copy of [DI+0]


/* the frame counter the low-phase predicate gates on ([0x4103]) */
uint16_t _obj_frame;

static uint16_t _low_phase_count; // [0x1976]: 0x8C = 140


/* DS:0x6EC3, the overlap/carry gate (1000:F336/0xF361, 0x5B9F, 0xC4BA and the
 * entity's 0xCAE1).  The original reads `byte[0x6EC3 + type]` with no bound;
 * every caller passes a static slot's `+8`, and shipped play only stores
 * 0x02/0x11/0x16/0x1D/0x1E/0x22 there (the 0xAA spawn 2, the 0xB6 spawn 0x1D,
 * the stack pickup 0x11, the mover hit 0x16, the overlap 0x1E, the entity carry
 * 0x22), so the port keeps the image's 40-byte table and returns 0 past it
 * (D-272).  The original reads on into the 0x6EEB frame table, the 0x6F0D
 * dispatch and the live 0x6F22/0x6F92 state; only a save that stores such a
 * type reaches that, and the game never writes one. */
const uint8_t _obj_overlap_gate[0x28] = {
    0x00u, 0x00u, 0x00u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x00u, 0x01u,
    0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u,
    0x01u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
};


uint8_t object_core_type_blocked(uint32_t type)
{ return type < sizeof _obj_overlap_gate && _obj_overlap_gate[type] != 0u; }


/* 0x6847: the per-record settled position (4 bytes per record). */
uint16_t _obj_settled[OBJ_SLOTS][2];


/* 0x6531: cmd 9's mode flag (zero in the image; the port keeps it too). */
uint8_t _obj_marker_mode;


void object_core_set_marker_mode(uint8_t on)
{ _obj_marker_mode = on ? 1u : 0u; }


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint8_t object_core_marker_mode(void) { return _obj_marker_mode; }
#endif /* HUMANSRE_TEST_PROBES */


/* [0x4234] / [0x4238] / [0x423C] - the selector's three parameters.  The
 * wrappers 1000:E93D/E952 set them and jump, and 1000:E6FB/E764 overwrite the
 * frame/type pair for the walk step, so they are state, not arguments. */
uint16_t _obj_sel_xoff, _obj_sel_frame, _obj_sel_type;


/* [0x4220] / [0x4222]: the settle's footing, read by 1000:DF2C. */
uint16_t _obj_foot_x, _obj_foot_y;


/* [0x6916] / [0x6917]: the per-call output state the handlers may set.  The
 * dispatcher zeroes the word before the handler (1000:E447) and copies it to
 * [0x1979] / [0x1988] when it is non-zero afterwards (1000:E4E4). */
uint8_t _obj_result_pending, _obj_result_cursor;


/* ===================================================================== *
 * The other record types' command handlers (the rest of DS:0x6918)
 * ===================================================================== *
 *
 * `1000:E4BD` calls `[0x6916 + 2*index]`; the index is the byte the type row
 * (`DS:0x698A`, 32 bytes per type: two 16-byte halves, a bit-7 input selecting
 * the second) picks for this input.  Types 0/1/4/5/9/10/15/18/20/23/24/31/32
 * select the handlers below.
 *
 * They exchange arguments through the dispatcher's own scratch words, the same
 * ones the selector uses: `[0x4230]` (_obj_side), `[0x4234]` (_obj_sel_xoff),
 * `[0x4238]` (_obj_sel_frame), `[0x423C]` (_obj_sel_type).  `[0x4224]`/`[0x4226]`
 * are the level-select character's x/y (`frontend.c`'s charX/charY; `[0x4224]`
 * is what `set_scroll_x` `1000:AA05` centres on) and `[0x4228]` a saved
 * argument.
 */

/* [0x13A4]: the ACTION press latch.  pause_toggle sets it (1000:3538) and
 * the home-state transition re-sets it (1000:EE01); poll_input's release and
 * the even-frame scan clear it (1000:85A9 / 1000:FC81).  While set, poll_input
 * masks the ACTION bit (1000:859F) and the record does not turn (1000:EE07).
 * A charge never sets it - pause_toggle is blocked by [0x6F1D] (1000:3530) -
 * so the type-row fire half keeps being selected (D-80). */
static uint8_t _obj_action_latch;


static uint8_t _obj_sfx; // the last `1000:83B7` driver argument

/* [0x4224] / [0x4226]: the door position 1000:EE3B folds into the record.  The
 * same DS cells are the front-end room's character x/y (frontend.c's
 * `_char_x`/`_char_y`); the two users never run in the same flow state, so the
 * port keeps one model per owner. */
uint16_t _obj_door_x, _obj_door_y;


uint8_t _obj_last_result; // [0x197A]: the previous dispatch code


/* [0x6FE6]: set when a record is written into its home stance - the home
 * frame/type pair (the type-12 tail 1000:EB05 and the selector's found-entry
 * branch 1000:F1B2).  The shipped image has no reader: the only other
 * references are the save/restore block lengths (1FE7:0000 / 1FE7:00FD).
 * Kept because the 0x6F20..0x82EA pool carries it across a save. */
static uint8_t _obj_home_stance_flag;


/** @brief 1000:98F8's header for a raw id: the six table words at 0x5B2B point
 * into `_obj_program_region`, so the header is the 12 bytes at that run.  An
 * id outside 1..6 is bounded here (D-186).
 *
 * @param id the raw id
 * @return the header, or NULL outside 1..6
 */
uint8_t *_obj_core_program_slot(uint32_t id)
{
    return (id >= 1u && id <= 6u)
               ? _obj_program_region + _obj_program_start[id - 1u]
               : NULL;
}


/** @brief The acting record: get_object_ptr (1000:8DF4).
 *
 * The 29-word table baked at DS:0x3E24, indexed by [0x3E22] - i.e.
 * _obj_array[_obj_entity].
 *
 * @return the record pointer
 */
uint8_t *_obj_core_actor(void) { return _obj_array + _obj_entity * OBJ_STRIDE; }


uint8_t object_core_static_slots(void) { return OBJ_STATIC; }


void object_core_init(void)
{
    memset(_obj_array, 0, sizeof _obj_array);
    for (uint16_t i = 0; i < OBJ_SLOTS; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        wr_le_u16(r + 0u, 0xFFFFu);
        wr_le_u16(r + 2u, 0xFFFFu);
        wr_le_u16(r + 6u, 0x0080u);
    }
    memset(_obj_slot_class, 0, sizeof _obj_slot_class);
    _obj_current = -1;
    _obj_player = -1;
    _obj_live_count = OBJ_LIVE0;
    _obj_placed_humans = 0u;
    _obj_entity = 0u; // [0x3E22] cleared (1000:4CB7)
    _obj_bg_count = 0u; // [0x32B9] cleared (1000:4CBA)
    _obj_pending = 0u; // [0x1979]
    _obj_last_result = 0u; // [0x197A], cleared with it (1000:31B1)
    _obj_cursor = 1u; // [0x1988]
    _obj_sequence_active = 0; // [0x6F1D]
    _obj_action_latch = 0; // [0x13A4]
    _obj_charging = 0; // the event-4 loop is stopped at load
    _obj_charge_arm = 0u; // [0x1987]
    _obj_charge_joy = 0u; // the latched [0x5ACE]
    _obj_sequence_set_program_index(1u); // [0x5B56]
    _obj_program_region[1] = 1u; // 1000:957B: [SI+1] = AH = 1 (prog 1)
    _obj_cmd_timer = 0u; // [0x6DEA]
    _obj_stream_count = 0u; // [0x6DEC]
    _obj_stream_ptr = _obj_anim_stream; // [0x6DEE] = 0x6DF2
    _obj_anim_div = 0; // [0x66B1]
    _obj_anim_move_tag = _obj_anim_move_count = _obj_anim_move_slot =
        _obj_anim_move_index = _obj_anim_move_cursor = 0u;
    _obj_anim_move_div = 0;
    memset(_obj_register, 0xFF, sizeof _obj_register); // 0x6F22, all free
    memset(_obj_span, 0xFF,
           sizeof _obj_span); // 0x6F92: 0x38 bytes at 0x60FF
    memset(_obj_span_top, 0, sizeof _obj_span_top);
    _obj_goal_flag = 0u; // [0x6530]
    for (uint16_t i = 0; i < 4u; i++) _obj_uses[i] = _obj_recipes[i][0];
    object_render_ter_slot_init(); // 1000:4CC3/4CC6: both slots off
}


uint16_t object_core_placed_humans(void) { return _obj_placed_humans; }


uint8_t *object_core_slot(uint32_t i)
{ return i < OBJ_SLOTS ? _obj_array + i * OBJ_STRIDE : NULL; }


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint32_t object_core_slots(void) { return OBJ_SLOTS; }
#endif /* HUMANSRE_TEST_PROBES */


uint16_t object_core_live_count(void) { return _obj_live_count; }


uint8_t object_core_slot_class(uint32_t slot)
{ return _obj_core_class_byte_at(slot); }


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint8_t *object_core_link_for_probe(uint32_t off)
{ return _obj_core_link_record(off); }
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
uint8_t *object_core_span_at_for_probe(uint32_t i)
{ return i < 2u ? (uint8_t *)_obj_span[i] : NULL; }
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
void object_core_span_seed_for_probe(uint32_t i, uint32_t top)
{
    if (i < 2u) _obj_span_top[i] = (uint16_t)top;
}
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
uint16_t object_core_recipe_param_for_probe(uint32_t recipe, uint32_t k)
{
    return recipe < 4u && k < 4u ? rd_le_u16(_obj_recipes[recipe] + 4u + 2u * k)
                                 : 0u;
}
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
void object_core_set_slot_class_for_probe(uint32_t slot, uint8_t cls)
{
    if (slot < sizeof _obj_slot_class) _obj_slot_class[slot] = cls;
}
#endif /* HUMANSRE_TEST_PROBES */


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
void object_core_set_slot_class(uint32_t slot, uint32_t cls)
{
    if (slot < sizeof _obj_slot_class) _obj_slot_class[slot] = (uint8_t)cls;
}
#endif /* HUMANSRE_TEST_PROBES */


int16_t object_core_current(void) { return _obj_current; }


void object_core_set_current(int16_t slot)
{ _obj_current = (slot >= 0 && (uint32_t)slot < OBJ_SLOTS) ? slot : -1; }


int16_t object_core_player(void) { return _obj_player; }


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
void object_core_set_player(int16_t slot)
{ _obj_player = (slot >= 0 && (uint32_t)slot < OBJ_SLOTS) ? slot : -1; }
#endif /* HUMANSRE_TEST_PROBES */


/* --- object_core_test_dispatch's predicates (1000:35D6) ---------------------
 *
 * The six handlers are reachable from the level's goal-test word at
 * [0x17F2] through the CS word table at 0x35F4.  All six are ported here
 * (0x3600 is object_core_low_phase_match below).
 *
 * 0xF761 is NOT a random generator: it adds
 * the record's x/y to its arguments and turns them into a **map address** -
 * `cell = 32*((y+0xC)/16) + (x+8)/16` - returning the map byte there and a
 * tile-row pointer in BP.  So `pred_map_cell_equals` compares the map cell
 * under the current object with the test's byte.
 *
 * The convention is the original's: -1 = true, 0 = false.  Every handler runs
 * against SI = the current record, i.e. what `object_core_current()` selects. */

int16_t object_core_none_type1d(void)
{
    for (uint16_t i = 0; i < OBJ_STATIC; i++)
        if (rd_le_u16(_obj_array + i * OBJ_STRIDE + 8u) == 0x1Du) return 0;
    return -1;
}


int16_t object_core_class_matches(uint32_t want)
{
    uint8_t b;

    b = _obj_core_actor()[7u];
    if (!b) return 0;
    return ((_obj_core_class_byte_at(b) & 0x3Fu) == (uint8_t)want) ? -1 : 0;
}


int16_t object_core_type_equals(uint32_t type)
{ return rd_le_u16(_obj_core_actor() + 8u) == type ? -1 : 0; }


/**
 * @brief The map byte for an explicit 640-byte map (the spawn handlers get the
 * map they are loading, not necessarily `level_map()`).
 *
 * `1000:E361`/`0xE2B5`/`0xE039` form the address as `[0x1FA0] + cell` with
 * 16-bit arithmetic and no bound, so a cell outside the map reads the gap,
 * the second map copy or the data segment below the copy.  The port keeps the
 * 16-bit cell index but treats every address outside `0..639` as air: tile 0,
 * which the page table maps to "no ground" (D-270).
 *
 * @param map the 640-byte map
 * @param idx the cell index
 * @return the map byte, or 0 outside the map
 */
uint32_t _obj_core_map_byte_at(const uint8_t *map, int32_t idx)
{
    if (!map || idx < 0 || (uint32_t)idx >= OBJ_MAP_CELLS) return 0u;
    return map[idx];
}


/* Read one byte of the live map (outside it is air). */
uint32_t _obj_core_map_buffer_byte(int32_t idx)
{ return _obj_core_map_byte_at(level_map(), idx); }


/** @brief 1000:E361: the map cell at (x + 8, y + 0xC).  Returns the cell's *index* so
 * the callers that read a neighbour (`1000:3600`'s `[DI+0x20]`, one map row
 * below) can address it.  `1000:E369` only aligns y; it does not test the index,
 * so the index may leave the map and read air (D-270).
 *
 * @param rec the record
 * @return the cell index
 */
static uint32_t _obj_core_map_cell_index(const uint8_t *rec)
{
    /* 1000:E361: ADD AX,[SI] / ADD BX,[SI+2] are 16-bit, ADD DI,BX is 16-bit. */
    uint16_t x = (uint16_t)(rd_le_u16(rec + 0u) + 8u);
    uint16_t y = (uint16_t)(rd_le_u16(rec + 2u) + 0xCu);

    if (!level_map()) return 0xFFFFu;
    /* 1000:E369: MOV DI,BX / ADD DI,BX is 16-bit, so 2*y wraps. */
    return (uint16_t)(2u * (y & 0xFFF0u)) + (x >> 4); // no 640 bound
}


/**
 * @brief Accessor: the record's map cell.
 *
 * @param rec the record
 * @return the cell, or 0xFFFF when no level is loaded
 */
static uint32_t _obj_core_map_cell(const uint8_t *rec)
{
    if (!level_map()) return 0xFFFFu;
    return _obj_core_map_buffer_byte((int32_t)_obj_core_map_cell_index(rec));
}


uint32_t object_core_record_map_cell(uint32_t slot)
{
    if (slot >= OBJ_STATIC) return 0xFFFFu;
    return _obj_core_map_cell(_obj_array + slot * OBJ_STRIDE);
}


int16_t object_core_map_cell_equals(uint32_t want)
{
    if (rd_le_u16(_obj_core_actor() + 8u) > 2u) return 0;
    return _obj_core_map_cell(_obj_core_actor()) == (uint16_t)want ? -1 : 0;
}


/** @brief -1 when every live entity's handler is 5 (the map's exit state).
 *
 * @return -1 when all handlers are 5, 0 otherwise
 */
static int16_t _object_core_entities_finished(void)
{
    uint16_t n = entity_count();

    for (uint16_t i = 0; i < n; i++) {
        const Entity *e = entity_at(i);
        if (e && e->handler != 5u) return 0;
    }
    return -1;
}


int16_t object_core_low_phase_match(void)
{
    static uint32_t lastFrame = ~0u;

    if (rd_le_u16(_obj_core_actor() + 8u) > 2u) return 0;
    if (lastFrame == _obj_frame) return 0; // once per frame
    lastFrame = _obj_frame;
    {
        /* 1000:361F..3629: the feet cell, then the cell one map row *below* it
         * (`[DI+0x20]`, which is `map[cell + 32]`).  Either being the goal tile
         * 0xA8 counts. */
        const uint8_t *map = level_map();
        uint16_t cell = (uint16_t)(_obj_core_map_cell_index(_obj_core_actor()));
        uint8_t goal =
            map != NULL && _obj_core_map_buffer_byte((int32_t)cell) == 0xA8u;

        /* 1000:3624: the original reads `[DI+0x20]` with no wrap; on the last
         * map row that is outside the map, which the port reads as air
         * (D-270). */
        if (!goal && map &&
            _obj_core_map_buffer_byte((int32_t)(cell + 0x20u)) == 0xA8u)
            goal = 1;
        if (!goal) {
            _low_phase_count = 0u; // 1000:362C
            return 0;
        }
    }
    if (++_low_phase_count != 0x8Cu) return 0;
    return -1;
}


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint16_t object_core_frame_counter_probe(void) { return _obj_frame; }
#endif /* HUMANSRE_TEST_PROBES */


int16_t object_core_test_dispatch(uint16_t mode)
{
    uint8_t op = (uint8_t)(mode & 0x7Fu);
    uint8_t arg = (uint8_t)((mode >> 8) & 0xFFu);

    switch (op) {
    case 0: return object_core_low_phase_match() == -1; // 1000:3600
    case 1: return object_core_none_type1d() == -1; // 1000:3640
    case 2: return object_core_class_matches(arg) == -1; // 1000:3659
    case 3: return object_core_type_equals(arg) == -1; // 1000:3678
    case 4: return _object_core_entities_finished() == -1; // 1000:3688
    case 5: return object_core_map_cell_equals(arg) == -1; // 1000:36AB
    default: return 0;
    }
}


/* --- 1000:C2CE count_tile_collisions: the goal scan --------------------
 *
 * update_world's seventh call (1000:80C6), inside the [0xC] gate: clear
 * [0x6530], count the enabled static records standing on the goal tile 0xBA,
 * add the command-4 live records (twice when their +0xE has bit 15 clear),
 * and raise [0x6530] when the count is non-zero and reaches the level header's
 * goal byte [0x17F0].  menu_flag_tick (1000:3540) is the flag's only reader.
 */

/* 0x6549: the five-byte id exclusion mask (nothing writes it). */
static const uint8_t _obj_goal_mask[5] = { 0x1F, 0x99, 0x02, 0x1C, 0x20 };


/** @brief The 0xBA test's map address: 32*((y + 0xC) & 0xFFF0)/16 + (x + 8)/16, the
 * 16-bit adds wrapping as the original's do.
 *
 * @param x the x coordinate
 * @param y the y coordinate
 * @return the map cell index
 */
static uint32_t _obj_core_goal_cell_index(uint32_t x, uint32_t y)
{
    uint16_t yy = (uint16_t)(y + 0x0Cu);
    uint16_t xx = (uint16_t)(x + 8u);

    return 32u * ((yy & 0xFFF0u) >> 4) + (xx >> 4);
}


/**
 * @brief Find the group that lists record i; its base point goes to x/y.
 *
 * @param i the index
 * @param x the output x
 * @param y the output y
 * @return the group index, or -1 when none
 */
static int32_t _goal_group(uint32_t i, uint32_t *x, uint32_t *y)
{
    for (uint16_t g = 0; g < 8u; g++) { // 1000:C319
        uint8_t *e = (uint8_t *)_obj_register[g];
        uint8_t *p;
        uint8_t hit = 0;

        if (rd_le_u16(e + 0u) & 0x8000u) continue; // 1000:C32B
        p = e + 5;
        while (1) { // 1000:C339
            if (*p == (uint8_t)i) {
                hit = 1;
                break;
            }
            p++;
            if (*p & 0x80u) break; // 1000:C347
        }
        if (!hit) continue;
        *x = rd_le_u16(e + 0u); // 1000:C325
        *y = rd_le_u16(e + 2u); // 1000:C31E
        return (int32_t)g;
    }
    return -1;
}


void object_core_goal_scan(void)
{
    uint16_t count = 0;

    _obj_goal_flag = 0; // 1000:C2CE

    for (uint16_t i = 0; i < OBJ_STATIC; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        uint32_t x, y, type;

        if (r[6] & 0x80u) continue; // 1000:C2E7
        x = rd_le_u16(r + 0u);
        y = rd_le_u16(r + 2u);
        type = rd_le_u16(r + 8u);
        if (type == 8u) { // 1000:C2F7 -> C300: a group base's own point
            if (_goal_group(i, &x, &y) < 0) continue; // 1000:C35B
        } else {
            /* 1000:C389 test_goal_cell_occupancy: the id's mask bit skips. */
            uint8_t bit;

            if (type >= 40u) continue; // the mask read is past its table
            bit = (uint8_t)(1u << (7u - (type & 7u)));
            if (_obj_goal_mask[type >> 3] & bit) continue; // 1000:C3A5
        }
        if (_obj_core_map_buffer_byte(
                (int32_t)_obj_core_goal_cell_index(x, y)) == 0xBAu)
            count++; // 1000:C386
    }

    /* 1000:C3D6: the live records whose behaviour byte is 4. */
    for (uint16_t k = 0; OBJ_LIVE0 + 1u + k <= _obj_live_count; k++) {
        uint8_t *r = _obj_array + (OBJ_LIVE0 + 1u + k) * OBJ_STRIDE;

        if (r[6] & 0x80u) continue; // 1000:C3F1
        if ((_obj_slot_class[1u + k] & 0x3Fu) != 4u) continue; // 1000:C3FC
        if (_obj_core_map_buffer_byte((int32_t)_obj_core_goal_cell_index(
                rd_le_u16(r + 0u), rd_le_u16(r + 2u))) != 0xBAu)
            continue; // 1000:C417
        count++;
        if (!(rd_le_u16(r + 0xEu) & 0x8000u)) count++; // 1000:C421
    }

    /* 1000:C434: a non-zero count that reaches the level's goal byte. */
    if (count != 0u && count >= level_goal_count()) _obj_goal_flag = 0xFFu;
}


uint8_t object_core_goal_flag(void) { return _obj_goal_flag != 0u; }


/* [0x4238] / [0x423C]: the scratch position `advance_position_snap_row`
 * accumulates, and [0x4244]-1: the acting record's class byte. */
int16_t _obj_scan_x, _obj_scan_y;

uint8_t *_obj_act_class;


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
void object_core_tick(const uint8_t *levelPal)
{
    _obj_frame++;
    object_command_animator_tick();
    object_sequence_claim_tick(levelPal);
    _obj_spawn_bg_tick();
}
#endif /* HUMANSRE_TEST_PROBES */


/* update_world's two tails (1000:80CC/0x80CF): the pending-move animator and
 * the bg-log settle.  level_tick runs them only inside its [0xC] gate. */
#if defined(HUMANSRE_TEST_PROBES)
static uint32_t _obj_world_ticks; // test probe counter
#endif

void object_core_world_tick(void)
{
#if defined(HUMANSRE_TEST_PROBES)
    _obj_world_ticks++;
#endif
    object_command_animator_tick();
    _obj_spawn_bg_tick();
}


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint32_t object_core_world_ticks_probe(void) { return _obj_world_ticks; }
#endif /* HUMANSRE_TEST_PROBES */


void object_core_claim_frame(const uint8_t *levelPal)
{
    _obj_frame++;
    object_sequence_claim_tick(levelPal);
}


/** @brief 1000:E361 map_get, the law shared with _obj_core_map_cell: the map cell at
 * (x, y) is `map[2*(y & 0xFFF0) + (x >> 4)]`; `*sub` returns `x & 0xF`, the 0x92A0
 * sub-column the step test indexes.  The cell index may leave the map; every
 * read outside `0..639` is air (D-270).
 *
 * @param x the x coordinate
 * @param y the y coordinate
 * @param sub the sub-column
 * @return the map cell
 */
uint32_t _obj_core_map_at_abs(int32_t x, int32_t y, uint32_t *sub)
{
    const uint8_t *map = level_map();
    uint16_t cell;

    if (sub) *sub = (uint32_t)x & 0xFu; // the fine x, when the caller wants it
    if (!map) return 0xFFu;
    /* 1000:E363..0xE376: the y add and `ADD DI,BX` are 16-bit, so 2*y wraps. */
    cell = (uint16_t)(2u * ((uint32_t)y & 0xFFF0u)) +
           (((uint32_t)x & 0xFFFFu) >> 4);
    return _obj_core_map_buffer_byte((int32_t)cell);
}


/* The 1000:E361 ground probe: the surface row under (x,y) and its cell.  The
 * sides are hard walls; below the map is a floor at OBJ_MAP_H (D-270). */
int32_t _obj_core_land_y(int32_t x, int32_t y, uint32_t *cellOut)
{
    int32_t row;

    if (x < 0 || x >= (int32_t)OBJ_MAP_W) {
        *cellOut = 0xFFFFu;
        return -1; // side wall
    }
    if (y >= (int32_t)OBJ_MAP_H) {
        *cellOut = 0xFFFFu;
        return (int32_t)OBJ_MAP_H; // below the map: hard floor
    }
    /* 1000:E361 does AND BX,0xFFF0 on the probe y, so every return below is
     * relative to the row start, not the raw probe line. */
    row = y & ~0xF;
    {
        uint32_t sub, id = _obj_core_map_at_abs(x, row, &sub);
        uint8_t b = (uint8_t)(level_page_byte(id, sub));

        *cellOut = id;
        if (!(b & 0x80u)) {
            if (b != 0u) return row + (int32_t)(int8_t)b;
            {
                uint32_t above =
                    _obj_core_map_at_abs(x, row - 0x10,
                                         &sub); // 1000:E3BA, DI unmoved

                b = (uint8_t)(level_page_byte(above, sub));
                if (b & 0x80u) {
                    *cellOut = id; // 1000:E3CA: DI is still the point's cell
                    return row; // AX = 0
                }
                *cellOut = above;
                return row - 0x10 + (int32_t)(int8_t)b;
            }
        }
        {
            uint32_t below = _obj_core_map_at_abs(
                x, row + 0x10, &sub); // 1000:E3E7, DI unmoved

            b = (uint8_t)(level_page_byte(below, sub));
            if (b & 0x80u) {
                *cellOut = id; // 1000:E3F1: DI is still the point's cell
                return -1;
            }
            *cellOut = below;
        }
        return row + 0x10 + (int32_t)(int8_t)b;
    }
}


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
int32_t object_core_land_y_for_probe(int32_t x, int32_t y, uint32_t *cellOut)
{ return _obj_core_land_y(x, y, cellOut); }
#endif /* HUMANSRE_TEST_PROBES */


/* Probe the ground one step (dx,dy) from the actor. */
int32_t _obj_core_step_probe_at(const uint8_t *actor, int32_t dx, int32_t dy,
                                uint32_t *cellOut)
{
    return _obj_core_land_y((int32_t)(int16_t)rd_le_u16(actor + 0u) + dx,
                            (int32_t)(int16_t)rd_le_u16(actor + 2u) + dy,
                            cellOut);
}


/**
 * @brief Probe the step below the actor.
 *
 * @param actor the object record
 * @param cellOut the output map cell
 * @return the landing row, or -1 when blocked
 */
int32_t _obj_core_step_probe(const uint8_t *actor, uint32_t *cellOut)
{ return _obj_core_step_probe_at(actor, 8, 0x18, cellOut); }


uint8_t object_core_action_latched(void) { return _obj_action_latch != 0; }


void object_core_set_action_latched(uint8_t on)
{ _obj_action_latch = on ? 0xFFu : 0u; }


void _obj_core_driver_call(uint32_t n)
{
    _obj_sfx = (uint8_t)(n);
    audio_sfx(n); // 1000:83B7: the gate lives in audio.c
}


void object_core_driver_call(uint32_t n) { _obj_core_driver_call(n); }


uint8_t object_core_last_sfx(void) { return _obj_sfx; }


/** @brief 1000:F23A: apply the frame/type pair from [0x4234]/[0x4238].
 *
 * Only when the landing probe at ([0x4230], 0x18) lands 0..7 px below that
 * row.  The probe is 1000:E389's: its 0xE361 prologue adds the record's own
 * x/y, so the arguments are offsets from the actor.
 *
 * @param actor the object record
 */
void _obj_core_place_at(uint8_t *actor)
{
    uint32_t cell;
    int32_t bx =
        _obj_core_step_probe_at(actor, (int32_t)_obj_side, 0x18, &cell);
    int32_t d;

    if (bx < 0) return; // 1000:F243
    d = bx - 0x18 - (int32_t)(int16_t)rd_le_u16(actor + 2u);
    if (d < 0) return; // 1000:F24B
    if (d - 8 >= 0) return; // 1000:F250
    wr_le_u16(actor + 4u, _obj_sel_xoff); // 1000:F255
    wr_le_u16(actor + 8u, _obj_sel_frame);
}


/** @brief 1000:FD7B: the span's +0x12 flag (0xFFFF = same frame, 0 below,
 * 1 above).
 *
 * @param d the stored frame
 * @param frame the frame index
 * @return the span flag
 */
static uint16_t _obj_core_span_flag(int32_t d, uint32_t frame)
{
    if (d == (int32_t)frame) return 0xFFFFu;
    return d < (int32_t)frame ? 0u : 1u;
}


/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint16_t object_core_span_flag_for_probe(int32_t d, uint32_t frame)
{ return _obj_core_span_flag(d, frame); }
#endif /* HUMANSRE_TEST_PROBES */


/** @brief 1000:FC81: the point scan.  Runs only on even frames (on odd ones it just
 * clears the ACTION latch).  From (xoff + record.x, record.y + 0x10) it walks
 * the map down a row at a time until a cell's page byte is a surface (bit 7
 * clear); the surface's height gives the distance `d` (clamped to 0x5F) to the
 * floor below.  It then fills one of the two 0x1C-byte span entries at 0x6F92 -
 * the one owned by the acting entity, else the first whose word 0 still has bit
 * 15 set - with the point, the distance and a direction word (1 when the found
 * extent is above the requested y), and copies `[0x423C]` into the record's
 * type.  An entry whose +6 is 0 copies its own point into +0xA/+0xC.
 *
 * @param actor the object record
 * @param xoff the x offset
 */
void _obj_core_scan_point(uint8_t *actor, uint32_t xoff)
{
    int32_t x = (int32_t)(int16_t)rd_le_u16(actor + 0u) + (int32_t)xoff;
    int32_t y = (int32_t)(int16_t)rd_le_u16(actor + 2u) + 0x10;
    int32_t top = y, d;
    uint8_t b;
    uint8_t *span = NULL;
    int8_t fallback = -1;

    if (_obj_frame & 1u) { // 1000:FC81
        object_core_set_action_latched(0);
        return;
    }
    while (1) { // 1000:FCBA
        uint32_t idx = 2u * ((uint32_t)y & 0xFFF0u) + ((uint32_t)x >> 4);

        b = (uint8_t)(level_page_byte(_obj_core_map_buffer_byte(idx),
                                      (uint32_t)x & 0xFu));
        if (!(b & 0x80u)) { // 1000:FCE0: a surface
            y += (int32_t)(int8_t)b;
            break;
        }
        y += 0x10;
        if ((uint32_t)y >= 0x140u) {
            y = 0x140;
            break;
        } // 1000:FCC8
    }
    if (y > 0x140) y = 0x140; // 1000:FCE9
    d = y - 1 - top; // 1000:FCF2
    if (d <= 0) return;
    if (d > 0x5F) d = 0x5F;
    _obj_sel_xoff = 0; // 1000:FD0E
    _obj_sel_frame = 0;
    _obj_side = 1; // [0x4230] = the entry count - 1
    for (uint16_t i = 0; i < 2u; i++) { // 1000:FD20
        uint8_t *e = (uint8_t *)_obj_span[i];

        if (rd_le_u16(e + 0u) & 0x8000u) fallback = (int8_t)((int32_t)i);
        if (rd_le_u16(e + 0u) == _obj_entity) {
            _obj_sel_frame = rd_le_u16(e + 6u);
            span = e;
            break;
        }
        _obj_side = (int16_t)(_obj_side - 1); // 1000:FD3B DEC [0x4230]
    }
    if (!span) { // 1000:FD41
        if (fallback < 0) return;
        span = (uint8_t *)_obj_span[fallback];
    }
    wr_le_u16(span + 0u, _obj_entity); // 1000:FD4D
    wr_le_u16(span + 2u, (uint16_t)x);
    wr_le_u16(actor + 8u, _obj_sel_type); // 1000:FD52
    wr_le_u16(span + 4u, (uint16_t)top);
    wr_le_u16(span + 6u, _obj_sel_frame);
    wr_le_u16(span + 8u, (uint16_t)d);
    if (_obj_sel_frame == 0u) { // 1000:FD6C
        wr_le_u16(span + 6u, 0u);
        wr_le_u16(span + 0xAu, rd_le_u16(span + 2u));
        wr_le_u16(span + 0xCu, (uint16_t)top);
        _obj_span_top[((uint16_t *)span - _obj_span[0]) / 14u] = (uint16_t)top;
    }
    wr_le_u16(span + 0x12u,
              _obj_core_span_flag(d, (uint32_t)_obj_sel_frame)); // 1000:FD7B
}


/**
 * @brief Set [0x6FE6]: the record has just been written into its home stance.
 */
void _obj_core_home_stance_set(void) { _obj_home_stance_flag = 0xFFu; }


/**
 * @brief The home-stance flag: the only reader of [0x6FE6].
 *
 * @return 1 when set, 0 otherwise
 */
uint8_t object_core_home_stance_flag(void)
{ return _obj_home_stance_flag != 0u; }

uint8_t *_obj_core_link_record(uint32_t off)
{
    uint16_t idx;

    if (off < 4u) return NULL;
    idx = (uint16_t)((off - 4u) / OBJ_STRIDE);
    return idx < OBJ_SLOTS ? _obj_array + idx * OBJ_STRIDE : NULL;
}


/* --- SAVE GAME state ranges -------------------------------------------- */

void object_core_save_table(uint8_t *dst)
{
    if (!dst) return;
    memset(dst, 0, 0x25Fu);
    wr_le_u16(dst + 0u, _obj_placed_humans);
    wr_le_u16(dst + 2u, _obj_entity);
    for (uint16_t i = 0; i < OBJ_SLOTS; i++)
        wr_le_u16(dst + 4u + 2u * i, (uint16_t)(0x3E75u + 0x12u * i));
    for (uint16_t i = 0; i < 21u; i++) dst[0x3Eu + i] = _obj_slot_class[i];
    wr_le_u16(dst + 0x53u, _obj_live_count); // [0x3E73] word
    memcpy(dst + 0x55u, _obj_array, OBJ_SLOTS * OBJ_STRIDE);
}


void object_core_load_table(const uint8_t *src)
{
    if (!src) return;
    _obj_placed_humans = rd_le_u16(src + 0u);
    _obj_entity = rd_le_u16(src + 2u);
    if (_obj_entity >= OBJ_SLOTS) _obj_entity = 0u;
    memset(_obj_slot_class, 0, sizeof _obj_slot_class);
    for (uint16_t i = 0; i < 21u; i++) _obj_slot_class[i] = src[0x3Eu + i];
    _obj_live_count = rd_le_u16(src + 0x53u); // [0x3E73] word
    if (_obj_live_count > OBJ_SLOTS) _obj_live_count = OBJ_SLOTS;
    memcpy(_obj_array, src + 0x55u, OBJ_SLOTS * OBJ_STRIDE);
}


void object_core_save_programs(uint8_t *dst)
{
    if (!dst) return;
    /* 0x5B37..0x5B56: the six overlapping programs and the raw id byte. */
    memcpy(dst, _obj_program_region, 0x20u);
}


void object_core_load_programs(const uint8_t *src)
{
    if (!src) return;
    /* 0x5B37..0x5B56 only: program 6's bytes past the id (0x5B57..) are not
     * in the save and stay static. */
    memcpy(_obj_program_region, src, 0x20u);
}


void object_core_save_ter(uint8_t *dst)
{
    if (!dst) return;
    memcpy(dst, _obj_ter_slot[0], 0x14u);
    memcpy(dst + 0x14u, _obj_ter_slot[1], 0x14u);
}


void object_core_load_ter(const uint8_t *src)
{
    if (!src) return;
    memcpy(_obj_ter_slot[0], src, 0x14u);
    memcpy(_obj_ter_slot[1], src + 0x14u, 0x14u);
}


void object_core_save_pool(uint8_t *dst)
{
    if (!dst) return;
    memset(dst, 0, 0x13CAu);
    /* 1FE7:0000 copies the whole [0x6F20]..[0x82EA] region, which starts
     * with the two single-byte tile ids (DEFS\BD<n>.BIN field K's first
     * word; D-143 keeps them bytes). */
    dst[0] = (uint8_t)level_special_tile(0u); // [0x6F20]
    dst[1] = (uint8_t)level_special_tile(1u); // [0x6F21]
    memcpy(dst + 2u, _obj_register, sizeof _obj_register);
    memcpy(dst + 0x72u, _obj_span, sizeof _obj_span);
}


/** @brief The stripe's pixels live on the page in the original and the page is not part
 * of any save block, so a restore starts the stripe empty: the port puts its
 * seed row at the entry's current top (1000:5D65 then draws nothing until the
 * next +0xC step).
 */
static void _obj_core_span_top_reset(void)
{
    for (uint16_t i = 0; i < 2u; i++)
        _obj_span_top[i] = rd_le_u16((uint8_t *)_obj_span[i] + 0xCu);
}


void object_core_load_pool(const uint8_t *src)
{
    if (!src) return;
    memcpy(_obj_register, src + 2u, sizeof _obj_register);
    memcpy(_obj_span, src + 0x72u, sizeof _obj_span);
    _obj_core_span_top_reset();
}


void object_core_save_spans(uint8_t *dst)
{
    if (!dst) return;
    memset(dst, 0, 0x54u);
    memcpy(dst, _obj_span, sizeof _obj_span);
}


void object_core_load_spans(const uint8_t *src)
{
    if (!src) return;
    memcpy(_obj_span, src, sizeof _obj_span);
    _obj_core_span_top_reset();
}
