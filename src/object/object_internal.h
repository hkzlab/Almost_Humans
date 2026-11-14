/* The object module's shared declarations: the record model, the state
 * cells every translation unit of the module reads, and the helpers that
 * cross a theme boundary.  Nothing here is part of the port's public API;
 * include object.h for that.
 */
#ifndef HUMANSRE_OBJECT_INTERNAL_H
#define HUMANSRE_OBJECT_INTERNAL_H

#include <stdint.h>

#define OBJ_STATIC 8u // the records 0x3640/check_highscore scan
/* 0x3E75..0x407F: the array's initialiser (`1000:4CAB`) clears 29 records. */
#define OBJ_SLOTS  29u
#define OBJ_STRIDE 0x12u
/* `load_terrain_b` (`1000:516F`) sets [0x3E73] to 8; its per-tile walk appends
 * one live record per placed object (`0x3F17 + 0x12*([0x3E73]-8)`), so the
 * count is 8 + the walk's appends (level 1: one 0xAB cell = the spear). */
#define OBJ_LIVE0 8u
/* --- the level's object walk (load_terrain_b, 1000:5168) -------------
 *
 * The level start clears the array (1000:4CAB), sets [0x3E73] = 8 and zeroes
 * the 20 behaviour bytes at 0x3E5F (1000:516F), then walks the map once per
 * tile in the 25-entry table 202F:0x1EB3 (1000:5184), handing every match to
 * its handler with CX = col*16, DX = row*16.  All 25 handlers are ported.
 *
 *   0xAA  1000:524B  the human, into the first free static slot (+6 bit 7):
 *                   x = col*16, y = row*16 - 7, +4 = 0x10, +8 = 2,
 *                   [0x3E20]++.
 *   0xAB  1000:527D  the spear, appended at the live end: x = col*16,
 *                   y = row*16, +4 = 0x60, +8 = 0xFFFF, behaviour 5,
 *                   [0x3E73]++.
 */
#define OBJ_COLS        32u
#define OBJ_ROWS        20u
#define OBJ_FRAME_SLOTS 0x11Eu // apply_relocations: 0x39A8..0x3E20

#define OBJ_PAIRB_FRAMES 0xF00Fu
#define OBJ_MAP_CELLS    (OBJ_COLS * OBJ_ROWS)
#define OBJ_MAP_W        (OBJ_COLS * 16u)
#define OBJ_MAP_H        (OBJ_ROWS * 16u)

/* --- the renderer (render_objects 1000:5D56 -> 0x7829 -> 0x653C) -------
 *
 * Base sprite: select_object_tile (1000:6625), gated by the record's +6 bit 7
 * and DS:0x3570[frame] bit 7.  Storage law (measured, pixel-exact), 0 =
 * transparent:
 *
 *   value(col,row) = SPRITES.DAT[off + (col % 4)*(h*4) + row*4 + (col/4)]
 *
 * The descriptor table DS:0x39A8 is built at boot by apply_relocations
 * (1000:569E) from six manifests at DS:0x252C (verified 248/248):
 *
 *   id   = word at DS:0x39A8 + 4F    rank = earlier slots with the same id
 *   off  = bswap(word at t1[id] + 4*rank + 2)
 *   X    = (w1<<11) | (hi(t2)<<6) | hi(t3)
 *   rows = X & 0x3F   ybias = (X>>6) & 0x1F   mode = (X>>11) & 3
 *
 * The manifests tile APL\SPRITES.DAT (bankbase 0, 0x3080, 0xD3F0 x3, 0xE130).
 * Mode 1 -> 16 cols (1000:66EC), mode 2 -> 32 (1000:6C74), 0/3 -> nothing,
 * drawn at y + ybias.  The table below is generated; do not hand-edit it.
 */
typedef struct {
    uint32_t off;
    uint8_t rows, ybias, mode;
} ObjSprite;

/* --- shared state (defined in object_core.c) --------------------------- */
extern uint8_t _obj_array[OBJ_SLOTS * OBJ_STRIDE];
extern int16_t _obj_player;
extern uint16_t _obj_live_count;
extern uint16_t _obj_placed_humans;
extern uint16_t _obj_entity;
extern uint16_t _obj_cmd_timer;
extern uint16_t _obj_stream_count;
extern const uint8_t *_obj_stream_ptr;
/* [0x4230]..[0x423C]: one shared argument block.  The original reuses these
 * four words as the calling convention of the machinery routines, so each
 * caller's meaning is documented at the call site (the settle's side, the
 * selector's xoff/frame/type, the fall machine's frame/type, ...). */
extern int16_t _obj_side;      // [0x4230]
extern uint16_t _obj_sel_xoff; // [0x4234]
extern uint16_t _obj_sel_frame; // [0x4238]
extern uint16_t _obj_sel_type;  // [0x423C]
extern uint8_t _obj_sequence_active;
extern uint8_t _obj_bg_count;
extern uint8_t _obj_pending;
extern uint8_t _obj_cursor;
extern uint8_t _obj_charge_arm;
extern uint8_t _obj_charging;
extern uint8_t _obj_charge_joy;
extern uint16_t _obj_register[8][7];
extern uint16_t _obj_span[2][14];
extern uint16_t _obj_span_top[2];
extern uint8_t _obj_anim_div;
extern uint16_t _obj_anim_move_tag;
extern uint16_t _obj_anim_move_count;
extern uint16_t _obj_anim_move_slot;
extern uint16_t _obj_anim_move_index;
extern uint16_t _obj_anim_move_cursor;
extern uint8_t _obj_anim_move_div;
extern const uint8_t _obj_anim_stream[];
extern uint8_t _obj_slot_class[0x40];
extern const uint8_t _obj_recipes[4][12];
extern uint8_t _obj_uses[4];
extern uint16_t _obj_frame;
extern const ObjSprite _obj_sprites[OBJ_FRAME_SLOTS];
extern uint8_t _obj_ter_slot[2][0x14];
extern const uint8_t _obj_overlap_gate_run[0x5F];
extern uint16_t _obj_settled[OBJ_SLOTS][2];
extern uint8_t _obj_marker_mode;
extern uint16_t _obj_foot_x;
extern uint16_t _obj_foot_y;
extern uint8_t _obj_result_pending;
extern uint8_t _obj_result_cursor;
extern uint16_t _obj_door_x;
extern uint16_t _obj_door_y;
extern uint8_t _obj_last_result;
extern uint8_t _obj_program_region[0x28];
extern const uint8_t _obj_program_start[6];
extern int16_t _obj_scan_x;
extern int16_t _obj_scan_y;
extern uint8_t *_obj_act_class;

/* --- shared helpers (defined in their theme's translation unit) --- */
uint8_t _obj_core_class_byte_at(uint32_t b);
void _obj_core_class_byte_set(uint32_t b, uint8_t value);
void _obj_sequence_set_program_index(uint32_t index);
uint8_t *_obj_core_program_slot(uint32_t id);
uint8_t *_obj_core_actor(void);
uint32_t _obj_core_map_byte_at(const uint8_t *map, int32_t idx);
uint32_t _obj_core_map_buffer_byte(int32_t idx);
void _obj_spawn_bg_tick(void);
void _obj_sequence_resolve_sprite(uint32_t index);
int32_t _obj_sequence_state_is(uint8_t *actor, uint32_t state);
uint8_t _obj_sequence_normalise_input(uint8_t in);
uint32_t _obj_units_index_bit(uint32_t idx);
void _obj_units_stack_unmark(uint8_t *actor);
uint32_t _obj_core_map_at_abs(int32_t x, int32_t y, uint32_t *sub);
int32_t _obj_core_land_y(int32_t x, int32_t y, uint32_t *cellOut);
int32_t _obj_core_step_probe_at(const uint8_t *actor, int32_t dx, int32_t dy,
                                uint32_t *cellOut);
int32_t _obj_core_step_probe(const uint8_t *actor, uint32_t *cellOut);
void _obj_command_walk(uint8_t *actor, int32_t left);
int32_t _obj_units_settle_gate(uint8_t *actor);
int32_t _obj_spawn_bg_mark(const uint8_t *actor);
void _obj_render_build_shade(uint32_t frame, uint8_t *out);
void _obj_core_driver_call(uint32_t n);
int32_t _obj_units_stack_masked(void);
void _obj_core_place_at(uint8_t *actor);
void _obj_core_scan_point(uint8_t *actor, uint32_t xoff);
void _obj_core_home_stance_set(void);
void _obj_units_tail_type28(uint8_t *actor);
void _obj_units_tail_type30(uint8_t *actor);
void _obj_units_tail_type34(uint8_t *actor);
void _obj_units_tail_type22(uint8_t *actor);
void _obj_units_tail_frame_bound(uint8_t *actor, uint32_t bound);
void _obj_units_tail_type3(uint8_t *actor);
void _obj_units_tail_span_owner(uint8_t *actor, uint32_t frame, uint32_t f8,
                                uint32_t bad);
void _obj_units_tail_type16(uint8_t *actor);
void _obj_units_tail_walk_countdown(uint8_t *actor, int32_t left);
void _obj_units_tail_special_tile(uint8_t *actor, uint32_t type, int32_t yoff,
                                  int32_t down);
void _obj_units_tail_type17(uint8_t *actor);
void _obj_units_tail_type29(uint8_t *actor);
void _obj_units_tail_type11(uint8_t *actor);
void _obj_units_tail_type12(uint8_t *actor);
void _obj_units_tail_type33(uint8_t *actor);
uint8_t *_obj_core_link_record(uint32_t off);
void _obj_command_ride(uint8_t *actor, uint32_t frame, uint32_t xoff,
                       uint32_t bit);
int32_t _obj_command_home_state(uint8_t *actor);
void _obj_units_fall_machine(uint8_t *actor);

/* module-internal entry points the theme files share (not in object.h) */
void object_sequence_claim_tick(const uint8_t *levelPal);
void object_render_ter_slot_init(void);
uint8_t object_core_home_stance_flag(void);

#endif /* HUMANSRE_OBJECT_INTERNAL_H */
