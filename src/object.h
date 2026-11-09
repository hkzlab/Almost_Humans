/* object.h — the interface of src/object.c: the placed-object array (0x3E75)
 *
 * 8 records of 0x12 bytes (check_highscore 1000:41D7 walks all eight); the
 * fields and the remaining machine are in object.c's header.  The predicates
 * are object_test_dispatch's (1000:35D6): -1 = true, 0 = false.
 */

#ifndef HUMANSRE_OBJECT_H
#define HUMANSRE_OBJECT_H

#include "osystem.h"

/* 1000:E4B0's outcome for one command. */
#define OBJ_CMD_NONE     0 /* neither a handler nor a tail ran          */
#define OBJ_CMD_HANDLER  1 /* the type row's handler ran                */
#define OBJ_CMD_TAIL     2 /* the row's tail handler ran                */
#define OBJ_CMD_UNPORTED (-1) /* no port */

/* The adjacent-object op of the entity bytecode (1000:3A6A): the gate and the
 * scan around the *acting* record (`actor`, the entity's own slot), then the
 * red/white flash, the blinks, the animation, the append and the settle.
 * `recipe` = 1/0/3 for the three handlers.  object_tick drives the sequence
 * and supplies the level's palette (the fades return to it). */
/* 1000:E3F6's command dispatch: the acting record's handler for one input word
 * (`[0x5ACE]`).  `handler` is the in-segment address the original calls
 * (0 = none, so the row's `tail` applies); `command` is the 0..7 number the
 * type row was indexed by.  Index 2 is 1000:E609, whose tail holds the human
 * stack's push.  Every handler body is ported (the former `0xE7C0` and
 * `0xEBCA` gaps were closed). */
typedef struct {
    uint16_t handler;
    uint16_t tail;
    uint16_t hook; /* the row's +0xA: run 1000:D8BD before the tail */
    uint8_t command;
} ObjCommand;

/** @brief The program header `1000:98F8` resolves for `group`: the six table
 * words at `0x5B2B` select the overlapping runs at `0x5B37..0x5B53`.  An id
 * outside 1..6 is bounded (D-186).
 *
 * The program header `1000:98F8` resolves for a raw id: the six word pointers
 * at `202F:0x5B2B` select the overlapping runs at 0x5B37..0x5B53, read as
 * {count, cursor, icon...}.
 *
 * @param group the raw program id
 * @return the 12-byte header, or NULL outside 1..6
 */
const uint8_t *object_program_header(uint32_t group);

/**
 * @brief Accessor: the action program header.
 *
 * @return the action program header, or NULL outside 1..6
 */
const uint8_t *object_action_header(void);

/** @brief One byte of the acting program's header sequence, `prog[k]` (one header
 * byte, 0 past the region).
 *
 * @param k the byte offset
 * @return the byte, or 0 outside the region
 */
uint8_t object_action_byte(uint32_t k);

/** @brief The resolved header pointer (SI) for the current raw id (image address, 0
 * outside 1..6).
 *
 * @return SI, or 0 outside 1..6
 */
uint32_t object_action_program_si(void);

/** @brief 1000:3933's tail (0x3941-0x394D): advance the selected cell with the wrap
 * to 1 past the count. It is the program's cursor byte, so the next ACTION
 * press runs the op the bar's highlight shows (1000:3941: SPACE steps the
 * cursor).
 */
void object_step_action_cell(void);

/**
 * @brief Publish the G sprite blocks.
 *
 * DEFS field G: the per-level 3-layer 0x100 sprites the frame slots 0xC1+4L+i
 * point at (level_setup_screen writes their far pointers into DS:0x3CAC).
 *
 * @param data the data buffer
 * @param blocks the block count
 * @param n the count
 */
void object_publish_g_sprites(const uint8_t *data, uint16_t blocks, uint8_t n);

/**
 * @brief Publish the DEH sprite banks.
 *
 * DEFS fields E and H: the 0xA9 log sprite (frames 0x33..0x37) and the two
 * sprites the frame slots 0x26/0x27 point at.
 *
 * @param e the E sprite bank
 * @param h the H sprite bank
 */
void object_publish_deh_sprites(const uint8_t *e, const uint8_t *h);

/**
 * @brief The placed-object update list.
 *
 * 1000:C295 update_list: the placed records' behaviour walk (torch, stepper,
 * sequence, wanderer).
 */
void object_update_list(void);

/**
 * @brief The goal scan over the placed records.
 *
 * 1000:C2CE count_tile_collisions: the goal scan; sets [0x6530] when the count
 * of records standing on a 0xBA tile reaches the level's goal byte.
 */
void object_goal_scan(void);

/**
 * @brief Accessor: the goal flag ([0x6530]).
 *
 * @return The goal flag.
 */
uint8_t object_goal_flag(void);

/** @brief 1000:D860 collision_scan: update_world's last call (1000:80D5), the
 * per-frame row pass over the eight static records.  The acting record is
 * skipped (it ran its row through 1000:E4B0 already); every other record whose
 * x is not 0xFFFF runs its type row's +0xA settle hook (1000:D8BD) and then the
 * row's +0xC tail - the *first* half of the row, with no input-bit-7
 * selection.  `CALL word ptr [0x6845]` is unconditional, but every shipped
 * row's tail is a real handler (the common one is the bare RET 0xD9C7).
 */
void object_row_pass(void);

/** @brief [0x6531]: menu_flag_tick (1000:3540) raises it after the goal pan's hold
 * and clears it when [0x6530] falls; 1000:C541's mode arm reads it.
 *
 * @param on the on/off flag
 */
void object_set_marker_mode(uint8_t on);

/**
 * @brief Accessor: the marker mode flag.
 *
 * @return The marker mode flag.
 */
uint8_t object_marker_mode(void);

/** @brief The array's initial state (`1000:4CAB`): every one of the 29 records gets
 * `x = y = 0xFFFF` and `+6 = 0x80` (flags bit 7 = free, class 0), the rest 0;
 * `[0x3E20]`/`[0x3E22]`/`[0x32B9]` are cleared and the recipes re-armed.
 * The live level confirms it: slots 4..7 read `x = y = 0xFFFF, flags = 0x80`.
 */
void object_init(void);

/** @brief The one blitter, for the entity pass (1000:5C5F -> 1000:6C74): the same law,
 * mode 2's 32-px cells.
 *
 * @param page the page buffer
 * @param src the source buffer
 * @param rows the row count
 * @param cols the column count
 * @param x the x coordinate
 * @param y the y coordinate
 */
void object_blit_sprite(uint8_t *page, const uint8_t *src, uint32_t rows,
                        uint32_t cols, int32_t x, int32_t y);

/**
 * @brief Initialise the terrain slots.
 *
 * 1000:5C2F draw_objects_pass2: the two TER.DAT slots at DS:0x649A, disabled
 * by object_array_init (0x4CAB) through their +0x10 flag words.
 */
void object_ter_slot_init(void);

/**
 * @brief Accessor: one terrain slot (NULL past the two slots).
 *
 * @param i the index
 * @return One terrain slot.
 */
uint8_t *object_ter_slot_at(uint32_t i);

/**
 * @brief Publish the terrain bank.
 *
 * @param bank the sprite bank
 * @param size the size
 */
void object_publish_ter(const uint8_t *bank, uint32_t size);

/**
 * @brief 1000:5C2F + the 0x649A loop of 1000:5823.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
void object_render_ter(uint8_t *page, int32_t scrollX, int32_t scrollY);

/** @brief 1000:C7A0/0xC7A9 (iterate_camera_records / set_camera_from): run
 * both 0x649A / 0x64AE TER slots for one world frame.
 *
 * 0xC7A0 calls 0xC7A9 for the first slot, `ADD SI,0x14`, and falls into the
 * second (so the second RET returns to 0xC7A0's caller).  Per slot:
 *
 *  - `+0x10` bit 15 (0xFFFF from 0x4CAB) is the disabled state: no motion.
 *  - Otherwise the move target is `+4/+6`, or `+8/+0xA` while `+0x10` is 0.
 *  - Current `+0/+2` steps one unit toward the target on each axis (the
 *    x/y pair is swapped between the two steps by the original, 0xC82E and
 *    0xC868).  On arrival `+0x10` toggles its low byte 0x00 <-> 0xFF, so the
 *    next frame's target is the other corner: the slot patrols the two cells
 *    the 0xAF+0xB0 (or 0xB1+0xB2) tiles name.
 *  - The `+0xE` counter reloads every 6 frames and then bumps `+0xC`; the
 *    `+0xC` word is both that counter and the TER sprite index (0x5823/0x5C2F
 *    pass it straight to the 0x2D7E index table), which is why the sprite
 *    animates while it patrols.
 *  - The slot's own `(new - old)` delta is added to the eight static 0x3E75
 *    records and the 0x6F22 groups under the byte-swapped `+0x12` mask
 *    (`0x80>>i` / `1<<(7-index)`).  The mask's high byte IS the stack mask
 *    0x64AD/0x64C1 (`_obj_ter_slot[s][0x13]`), so a human riding the platform
 *    is moved by exactly this shift.
 *
 * 1000:C7A0: the two 0x649A TER slots' patrol/interpolation (update_world's
 * second call).
 * The handlers of the 0xAF+0xB0 / 0xB1+0xB2 tile pairs fill them; a disabled
 * slot (bit 15 of +0x10) does nothing.
 */
void object_camera_slots_tick(void);

/**
 * @brief Accessor: one record slot (NULL past object_slots()).
 *
 * @param i the index
 * @return One record slot.
 */
uint8_t *object_slot(uint32_t i);

/**
 * @brief Accessor: the record slot count.
 *
 * @return The record slot count.
 */
uint32_t object_slots(void);

/**
 * @brief Accessor: the frame slot count (the 0x11E sprite-table bound).
 *
 * @return The frame slot count.
 */
uint32_t object_frame_slots(void);

/**
 * @brief Accessor: the frame counter ([0x4103]).
 *
 * @return The frame counter.
 */
uint16_t object_frame_counter_probe(void);

/**
 * @brief one sprite-table entry, for the tests (the D-69 tail copies).
 *
 * @param frame the frame index
 * @param off the offset
 * @param rows the row count
 */
void object_frame_for_probe(uint32_t frame, uint32_t *off, uint32_t *rows);

/**
 * @brief 0x32B9's queue for the drawer at 1000:5F54 (level.c's level_draw_bg_sprites).
 *
 * the 0x32B9 background-sprite queue (1000:5F54's input)
 *
 * @return the background record count
 */
uint8_t object_bg_count(void);

/**
 * @brief Accessor: one background decoration's position.
 *
 * @param i the index
 * @param x the output x
 * @param y the output y
 */
void object_bg_at(uint32_t i, uint32_t *x, uint32_t *y);

/** @brief 0x3E20..0x407E: [0x3E20] placed humans, [0x3E22] entity index, the 29
 * fixed pointers, the 20 class bytes, [0x3E73] live count and the 29 0x12
 * records at 0x3E75.  The pointer table is the image's constant run.
 *
 * the SAVE GAME state ranges (re_formats.md §10/§12.3): object table (0x3E20,
 * 0x25F), pools (0x6F20, 0x13CA), programs (0x5B37, 0x20), TER slots (0x649A,
 * 0x28) and the 0x6F92 copy (0x54).
 *
 * @param dst the destination buffer
 */
void object_save_table(uint8_t *dst);

/**
 * @brief Load the behaviour table.
 *
 * @param src the source buffer
 */
void object_load_table(const uint8_t *src);

/** @brief 0x6F20..0x82EA: the special tiles, the 8 group templates at 0x6F22 and the
 * two 0x1C spans at 0x6F92; the image is zero elsewhere, and so is this.
 *
 * @param dst the destination buffer
 */
void object_save_pool(uint8_t *dst);

/**
 * @brief Load the shared sprite pool.
 *
 * @param src the source buffer
 */
void object_load_pool(const uint8_t *src);

/**
 * @brief Save the action programs.
 *
 * @param dst the destination buffer
 */
void object_save_programs(uint8_t *dst);

/**
 * @brief Load the action programs.
 *
 * @param src the source buffer
 */
void object_load_programs(const uint8_t *src);

/**
 * @brief 0x649A..0x64C2: the two TER.DAT slots (2 x 0x14).
 *
 * @param dst the destination buffer
 */
void object_save_ter(uint8_t *dst);

/**
 * @brief Load the terrain table.
 *
 * @param src the source buffer
 */
void object_load_ter(const uint8_t *src);

/**
 * @brief 0x6F92..0x6FE6's second copy: the two spans, then zero.
 *
 * @param dst the destination buffer
 */
void object_save_spans(uint8_t *dst);

/**
 * @brief Load the span table.
 *
 * @param src the source buffer
 */
void object_load_spans(const uint8_t *src);

/**
 * @brief Accessor: the current record index ([0x1AF2], -1 = none).
 *
 * @return The current record index.
 */
int16_t object_current(void);

/**
 * @brief Accessor: set the current record.
 *
 * @param slot the slot
 */
void object_set_current(int16_t slot);

/**
 * @brief Accessor: the player record index ([0x180A], -1 = none).
 *
 * @return The player record index.
 */
int16_t object_player(void);

/**
 * @brief Accessor: set the player record.
 *
 * @param slot the slot
 */
void object_set_player(int16_t slot);

/**
 * @brief 1000:3640 — -1 when NO record's type is 0x1D.
 *
 * @return -1 when no record is type 0x1D, 0 otherwise
 */
int16_t object_none_type1d(void);

/** @brief 1000:3659 — the current record's +7 indexes the source-slot table 0x3E5E;
 * a zero slot is false, and the slot's low six bits are compared with the
 * test's byte.
 *
 * @param want the wanted value
 * @return -1 on a match, 0 otherwise
 */
int16_t object_class_matches(uint32_t want);

/**
 * @brief 1000:3678 — the current record's type equals the test's word.
 *
 * @param type the type
 * @return -1 when the type matches, 0 otherwise
 */
int16_t object_type_equals(uint32_t type);

/** @brief 1000:3688 — the entity table (count 0x64C2, records 0x64C4, stride 0x10):
 * -1 when there are none or every one's +4 is 5 (the knocked/finished state),
 * 0 as soon as one is not.
 *
 * @return -1 when every entity finished, 0 otherwise
 */
int16_t object_entities_finished(void);

/** @brief 1000:36AB — the record's type is <= 2 and the map cell under it equals the
 * test's byte.
 *
 * @param want the wanted value
 * @return -1 when the cell matches, 0 otherwise
 */
int16_t object_map_cell_equals(uint32_t cell);

/**
 * @brief Accessor: the static record count (the 8 the predicates scan).
 *
 * @return The static record count.
 */
uint8_t object_static_slots(void);

/**
 * @brief Accessor: the live record count ([0x3E73]; 9 at the level start).
 *
 * @return The live record count.
 */
uint16_t object_live_count(void);

/**
 * @brief Accessor: one slot's class byte ([0x3E5E + slot]).
 *
 * @param slot the slot
 * @return One slot's class byte.
 */
uint8_t object_slot_class(uint32_t slot);

/**
 * @brief Test seam: resolve a link offset (the +0xE/+0xC link encoding).
 *
 * @param off the offset
 * @return the pointer, or NULL when unavailable
 */
uint8_t *object_link_for_probe(uint32_t off);

/**
 * @brief Accessor: one span entry (the 0x6F92 span entry).
 *
 * @param i the index
 * @return One span entry.
 */
uint8_t *object_span_at_for_probe(uint32_t i);

/** @brief test seam: the stripe's seed row, which 1000:FC81 sets when it seeds a new
 * entry (`+6 == 0`, 1000:FD6C).
 *
 * @param i the index
 * @param top the top value
 */
void object_span_seed_for_probe(uint32_t i, uint32_t top);

/**
 * @brief Test seam: the landing probe.
 *
 * @param x the x coordinate
 * @param y the y coordinate
 * @param cellOut the output map cell
 * @return the landed y, or -1 when off the map
 */
int32_t object_land_y_for_probe(int32_t x, int32_t y, uint32_t *cellOut);

/** @brief 1000:FD7B: the span's +0x12 flag is 0xFFFF when d equals the entry's
 * frame, 0 below it and 1 above it.
 *
 * @param d the stored frame
 * @param frame the frame index
 * @return the span flag (0, 1 or 0xFFFF)
 */
uint16_t object_span_flag_for_probe(int32_t d, uint32_t frame);

/**
 * @brief the throw's 0x5BA4 word and a recipe's parameter, for the tests
 *
 * @param type the type
 * @return the per-type dx value, or 0 when out of range
 */
int32_t object_throw_dx_for_probe(uint32_t type);

/**
 * @brief Accessor: one recipe parameter.
 *
 * @param recipe the recipe index
 * @param k the loop counter
 * @return One recipe parameter.
 */
uint16_t object_recipe_param_for_probe(uint32_t recipe, uint32_t k);

/**
 * @brief Accessor: one pending event's offset (the 0x67FF step table).
 *
 * @param i the index
 * @return One pending event's offset.
 */
uint16_t object_pend_off_for_probe(uint32_t i);

/**
 * @brief Accessor: one pending event's wait.
 *
 * @param i the index
 * @return One pending event's wait.
 */
uint16_t object_pend_wait_for_probe(uint32_t i);

/**
 * @brief Accessor: one byte of the out-of-range DS tail record (0x5105..0x5114
 * (objCmdEnter)).
 *
 * @param off the offset
 * @return One byte of the out-of-range DS tail record.
 */
uint8_t object_ds_tail5105(uint32_t off);

/**
 * @brief the tests' setter for [0x3E5E + slot] (the behaviour bytes).
 *
 * @param slot the slot
 * @param cls the class byte
 */
void object_set_slot_class_for_probe(uint32_t slot, uint8_t cls);

/**
 * @brief Test seam: set one slot's class.
 *
 * @param slot the slot
 * @param cls the class byte
 */
void object_set_slot_class(uint32_t slot, uint32_t cls);

/**
 * @brief 1000:5662: the style-6 +3 row nudge for the cell at (col, row).
 *
 * Every handler that stores a tile-aligned y calls it first: the terrain
 * spawns (0x52A2/0x52C7/0x52EB, 0x5481, 0x53BA ...) and the exit-marker
 * stamper (0x54A9), which runs it before `ADD DX,0x10 / SUB DX,0x40`.  The
 * lookup cell is the one directly below the matched tile; only a level of
 * style 6 has the table.
 *
 * @param col the cell column
 * @param row the cell row
 * @return 3 when the nudge applies, 0 otherwise
 */
uint32_t object_style_y_nudge(uint32_t col, uint32_t row);

/** @brief the 0x6EB6 read window, for the tests (D-160's closure): behaviour -> the
 * state byte `test_class_against_6eb6` compares against (DS:0x6EB6 window).
 *
 * @param beh the behaviour byte
 * @return the behaviour state
 */
uint8_t object_beh_state_for_probe(uint32_t beh);

/** @brief --- object_test_dispatch (1000:35D6) ---------------------------------
 *
 * The level record's `+0x04` word (the 13-byte header copied to `202F:0x17F2`)
 * is the whole goal test: the low byte is the mode, the high byte its
 * argument (the handlers read `DI = 0x17F3`).  The mode's low 7 bits index the
 * CS word table at `0x35F4` = `0x3600, 0x3640, 0x3659, 0x3678, 0x3688,
 * 0x36AB`; bit 7 first runs the entity scan whose result the dispatch
 * discards, so it changes nothing and the port masks it off.
 *
 * Returns 1 = the goal is met (the original's -1).
 *
 * @param mode the mode
 * @return 1 when the active test fires, 0 otherwise
 */
int16_t object_test_dispatch(uint16_t mode);

/**
 * @brief Publish the object sprite bank.
 *
 * render_objects' object pass (1000:5D56 -> 0x653C): the base sprite of every
 * live record, drawn at (x - scrollX, y - scrollY).
 *
 * @param bank the sprite bank
 * @param size the size
 */
void object_publish_bank(const uint8_t *bank, uint32_t size);

/**
 * @brief Spawn the map's placed objects.
 *
 * load_terrain_b's per-tile object walk (1000:5168): [0x3E73] = 8, the 20
 * behaviour bytes cleared, then every map cell of the 25 table tiles in table
 * order (0xAA the human into the first free static slot, frame 0x10, type 2;
 * 0xAB the spear, appended, frame 0x60, +8 = 0xFFFF, behaviour 5; the rest per
 * re_map.md); returns [0x3E73].
 *
 * @param map the 640-cell map
 * @return the live record count after the spawn
 */
uint32_t object_load_terrain_objects(uint8_t *map);

/**
 * @brief 1000:3138/0x3149: seed the buffer tail from a freshly loaded map.
 *
 * 1000:3138/0x3149: seed the [0x1FA0] buffer's 32-byte gap (zero) and second
 * map copy (this map) for the D-235 tail write/read.
 * Call after the map is copied and before object_load_terrain_objects.
 *
 * @param map the 640-byte map (NULL clears the second copy too)
 */
void object_map_tail_reset(const uint8_t *map);

/**
 * @brief Accessor: the placed human count ([0x3E20]).
 *
 * @return The placed human count.
 */
uint16_t object_placed_humans(void);

/** @brief The map cell under a record, for the score (55FA calls 0xF761 with AX = 8,
 * BX = 0xC on each of the eight static records): 0xFFFF for a non-static slot or
 * when no level is loaded.
 *
 * the map cell under a static record (0xF761 with AX = 8, BX = 0xC); the
 * score's object bonus reads it: 0xA8 -> +1000, 0xA7 -> +2000 (1000:55FA).
 *
 * @param slot the slot
 * @return the map cell, or 0xFFFF when there is no record
 */
uint32_t object_record_map_cell(uint32_t slot);

/** @brief render_objects' object pass (1000:5D56 -> draw_object_group 1000:6421):
 * the 0x650C list, then scan_object_rows (slots 8..[0x3E73]), then the
 * 0x651E list.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
void object_render(uint8_t *page, int32_t scrollX, int32_t scrollY);

/** @brief 1000:E3F6 dispatch_actor_command: the acting record's command for one input
 * word.  The original calls the returned handler, or the row's tail handler when
 * it is 0 and the handler did not change the record's type; the handlers are the
 * next objective, so only the selection (and the one-shot counter's two writes)
 * is ported.  One global counter, not one per command: [0x6DEA].
 *
 * @param rec the record
 * @param input the input state
 * @return the selected command descriptor
 */
ObjCommand object_command_select(uint8_t *rec, uint8_t input);

/** @brief 1000:E4B0..E4F7: run one command for the acting record.  The type row's
 * handler runs first, and when it changes the record's type the tail is skipped
 * (1000:E4CC); otherwise the row's +0xA hook (1000:D8BD, the terrain settle)
 * and then the tail handler run.
 *
 * This is the ACTING record's path.  Every other static record runs its row
 * from `object_row_pass` (`collision_scan` `1000:D860`, the last call of
 * `update_world`), which the port runs after `object_world_tick`.
 *
 * Only the ported handlers are dispatched (the two movement states, the
 * leaves, the snap, the selector wrappers and the bare-RET tail).
 *
 * @param actor the object record
 * @param input the input state
 * @return the command class that ran, or OBJ_CMD_UNPORTED
 */
int32_t object_command_run(uint8_t *actor, uint8_t input);

/** @brief Test seams for the two handler bodies (the row selector's one-shot gate
 * keeps 1000:E7C0 from being driven directly in the unit test).
 *
 * @param actor the object record
 */
void object_stream_command(uint8_t *actor);

/**
 * @brief Test seam: clear the record's hold.
 *
 * @param actor the object record
 */
void object_clear_countdown(uint8_t *actor);

/**
 * @brief Test seam: set the command timer.
 *
 * @param v the value
 */
void object_set_command_timer(uint32_t v);

/** @brief 0x6F22 entry: word0 = base x (bit 15 free), +4 = direction<<4 | count,
 * +5.. = member list (bit-7 terminated). Every 6th call the count steps and
 * each member record whose frame is 0x20..0x22 gets x = group.word0 +
 * _obj_anim_x[length-3][count][member] (1000:D6B2 phase 3).
 */
void object_animator_tick(void);

/**
 * @brief Accessor: one group record (0x6F22 entry).
 *
 * @param i the index
 * @return One group record.
 */
uint8_t *object_group_at(uint32_t i);

/**
 * @brief Reset the animator's state.
 */
void object_animator_reset(void);

/** @brief 1000:E697 (a fall-through inside 1000:E609, the walk-up state): push the
 * acting record onto the `0x6F22` group it stands under.  The original reaches
 * this with AX = actor.x and BX = actor.y + 0x18, after the `0x6F92` span scan
 * has failed; the port takes the record and derives the same two values.
 * Returns 1 when a group took it (frame 0x25, type 7, `+0xE` = its slot).
 *
 * The human stack (`0x6F22`, 8 entries x 0x0E: +0/+2 x/y, bit 15 = free, +4
 * the animator state, +5 the owner index, +6ff the bit-7-terminated member
 * list).
 * `object_stack_push` is the push block of 1000:E609 (1000:E697) and
 * `object_stack_grab` the one of 1000:E4F8 (1000:E58B), `object_stack_pop` is
 * 1000:FDED, the leaves are 1000:E860 / 1000:E8DD, the snap 1000:E963, the
 * selector 1000:F17C, and `object_stack_scan_dissolve` is 1000:F453 (the scan
 * that finds the group holding a record and dissolves it).
 * The `0x64AD`/`0x64C1` in-a-stack masks (1000:F38F/F3CA) are written by the
 * two pushes, read by the selector, and are ALSO the high bytes of the two TER
 * slots' +0x12 carry masks (1000:C8A4): a marked record rides the patrolling
 * platform.
 * `object_ter_slot_at(i)[0x13]` is that byte.
 *
 * @param actor the object record
 * @return 1 when the record was pushed, 0 otherwise
 */
int32_t object_stack_push(uint8_t *actor);

/** @brief 1000:E58B (the tail of 1000:E4F8, the step-down state): grab the group the
 * actor stands at - within 8 px in x and 4 px in y - and append it to the
 * member list (frame 0x23, type 6).  With no group matching, register the
 * actor's position in the last free `0x6F22` entry (frame 0x20, type 8).
 * Returns 1 = grabbed, 0 = registered, -1 = nothing (no group, no free entry:
 * the original would write through a stale `[0x423C]`, the port refuses).
 *
 * @param actor the object record
 * @return 1 when a group was grabbed, 0 when already grouped, -1 on failure
 */
int32_t object_stack_grab(uint8_t *actor);

/** @brief 1000:FDED object_script_select (the pop): find the acting record in a
 * `0x6F22` list and, if it is the topmost member, remove it.  Returns the
 * original's AX: -1 = not present or not the top, 0 = the record below is within
 * 8 px of `ax` (the caller's x) so the list is left alone, 1 = removed.
 * `*below` = the record the original leaves in DI (NULL when the entry was
 * freed).
 *
 * @param ax the x accumulator
 * @param below the cell below
 * @param entry the entry
 * @return 1 when a member was popped, 0 when aligned, -1 on failure
 */
int32_t object_stack_pop(uint16_t ax, uint8_t **below, uint8_t **entry);

/** @brief 1000:E860 (handler 10): step off the group.  Finds the entry with the actor's
 * x, the actor in its list, and on `entry.y - 0x18 == actor.y` moves the actor
 * down 8 (frame 0x20, type 8) and the list byte below it likewise (frame 0x21).
 * Returns 1 = acted, 0 = not in a group, -1 = the `y` mismatch branch (1000:E8D4
 * -> 1000:FD91, the walk frame step).
 *
 * @param actor the object record
 * @return 1 when the record stepped off, 0 when not grouped, -1 on failure
 */
int32_t object_stack_step_off(uint8_t *actor);

/** @brief 1000:E8DD (handler 11): leave the group.  Same walk, and on `entry.y ==
 * actor.y` the actor becomes a standing human (frame 0x10, type 2) and the list
 * is cut at the slot the push stored in `actor+0xE`.
 * Returns 1 = left, 0 = not in a group, -1 = the `y` mismatch branch (1000:E934
 * -> 1000:FD91).
 *
 * @param actor the object record
 * @return 1 when the group was left, 0 when not grouped, -1 on failure
 */
int32_t object_stack_leave(uint8_t *actor);

/** @brief 1000:E963 (handler 14): snap onto the group's x.  Only the topmost member
 * (the byte after it is a terminator) moves: `actor.x = entry.x`, frame 0x20.
 *
 * @param actor the object record
 * @return 1 when the record snapped, 0 otherwise
 */
int32_t object_stack_snap(uint8_t *actor);

/** @brief 1000:E99D (index 15): stand back on the group.  The topmost member takes the
 * entry's x, `y -= 7`, frame 0x25 and type 7 - and stores its slot in `+0xE` -
 * while the list byte below it moves up 8 (frame 0x20).  Like the pop, its
 * "below" lookup reads `match-1` literally, so for the owner itself that byte is
 * the entry's `+4` state.
 *
 * @param actor the object record
 * @return 1 when the record stood, 0 otherwise
 */
int32_t object_stack_stand(uint8_t *actor);

/** @brief 1000:F17C script_op_select_on_zero: the object selector, and the stack's
 * "walk off" path.  `xoff` = [0x4234], `frame` = [0x4238], `type` = [0x423C] -
 * the two wrappers 1000:E93D (8/1) and 1000:E952 (0/0) set them and jump here.
 * Returns 0 = done, -1 = nothing found (the original's `JS`); the terrain
 * branch (`1000:F1D7`'s probe, `1000:F223`'s special-tile test) is in.
 *
 * @param actor the object record
 * @param xoff the x offset
 * @param frame the frame index
 * @param type the type
 * @return -1 when the selector cannot move, 0 otherwise
 */
int32_t object_selector(uint8_t *actor, uint16_t xoff, uint16_t frame,
                        uint16_t type);

/** @brief 1000:F453 scan_groups_for_record with the callers' `DX = 5`: the first
 * 0x6F22 entry whose member list (from +5) contains `recIndex` gets dissolved,
 * then the `DL & 1` class-transfer tail runs (whether or not a group matched).
 *
 * @param recIndex the record index
 * @return 1 when a group was found, 0 otherwise
 */
int32_t object_stack_scan_dissolve(uint32_t recIndex);

/** @brief 0x6EC3 + type != 0, with the image's full DS window: the static run,
 * then the live register pool, then the spans.
 *
 * 0x6EC3 + type != 0 (the overlap/carry gate, with its full DS window).
 *
 * @param type the record type
 * @return 1 when the gate blocks the type, 0 otherwise
 */
uint8_t object_type_blocked(uint32_t type);

/** @brief Test seam: 1000:F2E0's overlap conversion.
 *
 * Test seam: 1000:F2E0's per-candidate dissolve + conversion.
 *
 * @param actor the object record
 */
void object_find_overlap_for_probe(uint8_t *actor);

/** @brief Test seam: dissolve the group in register slot `slot`.
 *
 * 1000:FA13 object_group_dissolve, by group slot (the 0x6F22 entry the tail
 * holds in BP when it calls it).
 *
 * @param slot the slot
 */
void object_group_dissolve_at(uint32_t slot);

/** @brief 1000:E4F8 (handler 1): the **step-down** state.  The cell at the feet
 * (`x + 8`, `y + 0x16`) decides: a special tile (field K -> `[0x6F20]`/`[0x6F21]`)
 * snaps the actor to the tile grid, a `0x6F92` span at that column takes it
 * onto the span, else the grab (1000:E58B) runs.  Returns 1 tile rule,
 * 2 span, 3 grabbed, 4 registered, -1 nothing.
 *
 * 1000:E4F8 / 1000:E609: the two movement states that feed the stack - the
 * feet cell's special tile (field K), a 0x6F92 span, else the grab / the push.
 *
 * @param actor the object record
 * @return 1-4 for the state reached, or -1 on failure
 */
int32_t object_step_down(uint8_t *actor);

/** @brief 1000:E609 (handler 2): the **walk-up** state.  Same three-way split, with
 * `x + 8, y + 0x19` as the feet cell; the span test uses `y + 0x18` as the line
 * and a 0x10-px column window, and the fall-through is the push (1000:E697).
 * Returns 1 tile rule, 2 span, 3 pushed, 4 nothing.
 *
 * @param actor the object record
 * @return 1-4 for the state reached
 */
int32_t object_step_up(uint8_t *actor);

/** @brief 1000:E93D / 1000:E952: the two wrappers that set the selector's constants and
 * jump into it - left: xoff -1, frame 8, type 1; right: xoff 1, frame 0, type 0.
 *
 * @param actor the object record
 * @param right the right value
 * @return the selector's result
 */
int32_t object_selector_walk(uint8_t *actor, int32_t right);

/**
 * @brief Begin the claim animation for one recipe.
 *
 * @param recipe the recipe index
 * @param actor the object record
 * @return the claim record index, or -1 when it cannot start
 */
int32_t object_claim_begin(uint32_t recipe, int32_t actor);

/**
 * @brief Accessor: the claim is running.
 *
 * @return The claim is running.
 */
uint8_t object_claim_active(void);

/**
 * @brief Accessor: the claim's pan phase (the 1000:3D04 recolour frames).
 *
 * @return The claim's pan phase.
 */
uint8_t object_claim_pan_active(void);

/**
 * @brief Accessor: the claim's animation phase (1000:3BD6, the 56 frames).
 *
 * @return 1 while the claim is animating the neighbour.
 */
uint8_t object_claim_anim_active(void);

/**
 * @brief Accessor: the claim's page re-blit phase (1000:3CB5's re-blits).
 *
 * @return 1 while the world must be re-composed.
 */
uint8_t object_claim_flip_active(void);

/**
 * @brief Accessor: the claim has reached the pan and its page is frozen.
 *
 * The three phases before the pan (`CLAIM_TO_RED`/`RED_BACK`/`TO_WHITE`) only
 * fade the DAC in the original, so the last drawn page must be left alone
 * (1000:3B34..1000:3B8E); from the pan onward the recoloured page is presented.
 *
 * @return 1 from CLAIM_WHITE_BACK through CLAIM_SETTLE.
 */
uint8_t object_claim_page_frozen(void);

/**
 * @brief Accessor: the claim's settle fade (1000:3CDE).
 *
 * The page is already the final `1000:3CB5` frame (world + objects); the
 * settle only fades the DAC, so it must not be presented over the plane.
 *
 * 1 during CLAIM_SETTLE: the page is the final 0x3CB5 frame, leave it.
 *
 * @return 1 while the settle fade runs.
 */
uint8_t object_claim_settle_active(void);

/** @brief 1000:40CA render_frame_objects: the claim's presented frames draw
 * only the selected record (`[0x180A]`) and the claimed one (`[0x1AF2]`) over
 * the frozen page — never the whole world.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
void object_claim_draw_records(uint8_t *page, int32_t scrollX, int32_t scrollY);

/** @brief One frame of the sequence; the fades advance through `palette_tick`, which
 * the level's own tick drives.  Every `palette_fade_active()` wait below is the
 * original's `0x9F8B` (spin while the fade runs).
 *
 * @param levelPal the level palette
 */
void object_claim_tick(const uint8_t *levelPal);

/**
 * @brief The scan at 1000:3A91..1000:3AD9, for the acting record `actor`.
 *
 * the scan at 4E91: the first record beside `actor` (|dx| <= 0x20, |dy| <= 8,
 * type <= 2, active) or -1.
 *
 * @param actor the object record
 * @return the record index, or -1 when none
 */
int32_t object_find_neighbour(int32_t actor);

/**
 * @brief --- object_tick: one level frame for the object machine --------------
 *
 * one level frame of the object machine (the frame counter + the claim)
 *
 * @param levelPal the level palette
 */
void object_tick(const uint8_t *levelPal);

/**
 * @brief One world tick.
 *
 * update_world's animator/bg tails (1000:80CC/0x80CF) and the claim's own
 * frame (frame counter + claim), separately for level_tick's [0xC] gate.
 */
void object_world_tick(void);

/**
 * @brief Accessor: the world tick counter.
 *
 * @return The world tick counter.
 */
uint32_t object_world_ticks_probe(void);

/** @brief The claim's own frame: [0x4103] advances on every page flip, and the claim
 * sequence runs while update_world is frozen (D-163).
 *
 * @param levelPal the level palette
 */
void object_claim_frame(const uint8_t *levelPal);

/** @brief --- pred_type_low_phase_match (1000:3600) ----------------------------
 *
 * The acting record's type must be <= 2 and the test runs once per frame
 * (`[0x4103]`, the frame counter, against its stored copy `[0x1972]`).  It
 * reads the map cell under the record (`1000:E361` with AX = 8, BX = 0xC) and
 * then **the cell one map row below it** (`[DI + 0x20]` - `DI` is still the map
 * cell pointer `1000:E361` left behind, and a map row is 32 bytes).  Either one
 * being the goal tile `0xA8` counts.  A miss clears the counter `[0x1976]`; a
 * hit counts it up and at `0x8C` (140) the goal is met (-1).
 *
 * SI comes from the caller (the dispatch's `0x8DF4`), i.e. the acting entity's
 * record - see `object_current`.
 *
 * @return -1 on the once-per-frame match, 0 otherwise
 */
int16_t object_low_phase_match(void);

/** @brief Test seam: run `1000:F3ED` on `actor`.
 *
 * 1000:F3ED's gate, for the test: clears the mask, applies the 0x6EBE type
 * gate, then tests the two TER slots' attach boxes (1000:F423).
 * A box match sets the record's carry bit in the slot and returns 0 (settle
 * refused).
 *
 * @param actor the object record
 * @return 1 when the settle may run, 0 when a TER slot claimed the actor
 */
uint8_t object_settle_gate_for_probe(uint8_t *actor);

/**
 * @brief script_op_dispatch: one ACTION press.
 *
 * The ACTION program machine (script_op_dispatch, 1000:3979): ACTION with no
 * direction held runs the acting record's program.
 * The acting record is [0x3E22]; object_action_run() performs one press,
 * object_cycle_entity(±1) is the object selector (1000:410A/4147).
 */
void object_action_run(void);

/** @brief The pending-event dispatch (`[0x1979]` -> the table at CS:0x197B).  The port
 * only needs the two gameplay codes: 4 is the jump/throw charge loop and 5 is
 * the program-cursor rewind; 1/2/3/6 are the screen-level dispatch control
 * (lock, "return 0", a bare RET, spin-wait), none of which the port's tick
 * drives.
 */
void object_pending_run(void);

/**
 * @brief The charge loop's per-tick step (the [0x1979] dispatch, per frame).
 *
 * one frame of the event-4 charge loop (1000:3702); call after the frame
 *
 * @return 1 while the charge loop continues, 0 when it ends
 */
uint8_t object_charge_tick(void);

/** @brief One frame of the charge loop (0x3702): the world frame has already run, so
 * this redraws the bar at the record's new +0xE, or leaves when the turn did
 * not advance (0x370C).  The loop's other exit, a pending code, is consumed by
 * `object_pending_run` (0x3705 -> 0x3735/0x373B), which swallows it.
 *
 * the charge loop is running (1000:3702): the status bar must not repaint the
 * band/action row while it is (D-81).
 *
 * @return 1 while the charge loop is active, 0 otherwise
 */
uint8_t object_charge_active(void);

/**
 * @brief 0x410A (resolve_next_eligible_object_sprite): cycle the 8 static records.
 *
 * @param step the step
 * @return the new index, or -1 when none is eligible
 */
int32_t object_cycle_entity(int32_t step);

/** @brief 1000:38C3 (resolve_object_sprite_guarded): the key list's F1..F8 handler.
 * AX is the scancode and the record is `scancode - 0x3B`; a slot with `+6`
 * bit 7 set or type 0x1D is skipped, otherwise 1000:4147 resolves it.
 * Returns 1 when a record was selected.
 *
 * @param index the index
 * @return 1 when the record was selected, 0 otherwise
 */
uint8_t object_select_by_index(uint32_t index);

/** @brief 1000:3933 (the key list's id 8, SPACE).  1000:392D runs the home-state
 * conversion on the acting record (get_object_ptr + 0xEDB7) and the handler
 * returns when it converted; otherwise it plays driver effect 0 and steps the
 * action row's selected cell (1000:3941 -> 0x98F8 + the 0x5B2B header's second
 * byte, wrapping to 1 past the cell count) before the bar redraw.
 *
 * Returns 1 (the make is consumed).
 *
 * @return 1 when the ACTION program ran, 0 otherwise
 */
uint8_t object_space_action(void);

/**
 * @brief Accessor: the acting record's index.
 *
 * @return The acting record's index.
 */
uint16_t object_action_entity(void);

/** @brief 1000:3344..336C (level_fade_in): slot 8 (`0x3F05`) takes frame 0x17, type 0x1A,
 * +0xA = 0x1006 and +0xC = 0xFF.  `scan_object_rows` starts its walk there and
 * counts `[0x3E73]-8+1` records, so this slot is drawn.
 * Slot 8 is the active-human ARROW: 1000:D64F (object_marker_tick, called from
 * update_world) positions and bobs it every frame, and the selector arms it in
 * 1000:4188 (_obj_marker_arm), so the x/y the array init leaves at 0xFFFF are
 * replaced on the first level frame.
 */
void object_level_start_record(void);

/** @brief 1000:D64F (update_world's 1000:80C9): record 8 follows the acting record;
 * its type word is subtracted from y and steps 0x1A,0x18,0x16,0x14,0x12 (two
 * every third frame - the bob); a 32-px acting sprite shifts x by +8; and the
 * word at +0xA is the lifetime - when its high byte underflows the marker is
 * disabled (+6 = 0x80) and scan_object_rows stops drawing it.
 *
 * 1000:D64F: the active-human marker (record 8) follows the acting record,
 * bobs through its type word and expires; update_world calls it every frame.
 */
void object_marker_tick(void);

/**
 * @brief Accessor: set the acting record.
 *
 * @param i the index
 */
void object_set_action_entity(uint16_t index);

/**
 * @brief Accessor: the action program index.
 *
 * @return The action program index.
 */
uint8_t object_action_program(void);

/**
 * @brief Accessor: the pending event.
 *
 * @return The pending event.
 */
uint8_t object_action_pending(void);

/**
 * @brief [0x1988]: the program cursor byte (the pending action 5 writes it, 0x382D).
 *
 * @param cursor the cursor
 */
void object_action_set_cursor(uint8_t cursor);

/** @brief [0x5B56] (a raw byte), for tests and for a program restored from a
 * save.  The shipped writers produce 1..6; the collision tail can store the
 * shared scratch byte (any value; an out-of-table id is bounded, D-186).
 *
 * @param index the index
 */
void object_action_set_program(uint32_t index);

/**
 * @brief Accessor: the last sound effect.
 *
 * 1000:83B7 driver_call: the last sfx number a handler asked for (the audio
 * surface has no sfx entry until M6, so the request is only recorded).
 *
 * @return The last sound effect.
 */
uint8_t object_last_sfx(void);

/**
 * @brief the flag's reader is unported, so expose the store for the test
 *
 * [0x6FE6]: the marker 1000:EAE6/0xF1B2 write (no ported reader yet).
 *
 * @return 1 when the flag is set, 0 otherwise
 */
uint8_t object_flag6f_e6(void);

/**
 * @brief Test seam: call the sound driver (1000:83B7 from another module).
 *
 * @param n the count
 */
void object_driver_call(uint32_t n);

/**
 * @brief Accessor: the ACTION latch.
 *
 * [0x13A4]: the ACTION press's latch as the command handlers see it.
 *
 * @return The ACTION latch.
 */
uint8_t object_action_latched(void);

/**
 * @brief Accessor: set the ACTION latch (7 = op 7 was dispatched).
 *
 * @param on the on/off flag
 */
void object_set_action_latched(uint8_t on);

#endif /* HUMANSRE_OBJECT_H */
